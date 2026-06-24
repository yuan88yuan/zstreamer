/*=============================================================================
    mp4_demuxer.c — FFmpeg libavformat MP4 demuxer with dynamic pads

    Refactored for Phase F of the Adaptive Stream Demuxing Plan:
      - Dynamic source pads (video_%u, audio_%u) created at stream discovery
      - Stable stream table with IDs, bus events on add/remove/change
      - Sticky in-band pad events (STREAM_START, CAPS) on new pads
      - Unlinked-pad policy support (default BLOCK for file demuxing)
      - Stream query API (get_stream_count, get_stream_info, get_stream_pad)

    Supports two modes:
      1) Direct file mode — set the "location" property to an MP4 file path;
         the element opens the file via avformat_open_input() at start().
      2) Push/streaming mode — feed buffers into the sink pad; data is
         buffered until enough is available to probe format and read packets
         (fragmented MP4 / fMP4 friendly via custom AVIO).
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_log.h"
#include "zst_bus.h"
#include "zst_pipeline.h"
#include "zst_clock.h"
#include "zst_stream.h"
#include "zst_pad_event.h"
#include "zstreamer/elements/zst_mp4_demuxer.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define MP4DEMUX_MAX_STREAMS 16
#define MP4DEMUX_INIT_PROBE_BYTES 4096

/* ── Internal buffer queue node ────────────────────────────────────── */

typedef struct zst_buffer_node {
    zst_buffer_t* buf;
    struct zst_buffer_node* next;
} zst_buffer_node_t;

/* ── Per-stream descriptor ─────────────────────────────────────────── */

typedef struct {
    zst_stream_id_t    id;
    int                av_stream_idx;
    int                av_stream_idx_valid;

    zst_media_kind_t   kind;
    zst_stream_status_t status;

    zst_caps_t*        caps;
    zst_pad_t*         pad;

    enum AVCodecID     codec_id;
    uint64_t           last_seen_pts_ns;
} mp4demux_stream_t;

/* ── Element private data ───────────────────────────────────────────── */

typedef struct {
    AVFormatContext* fc;
    uint8_t*         avio_buf;
    size_t           avio_buf_size;

    /* Stream table */
    mp4demux_stream_t streams[MP4DEMUX_MAX_STREAMS];
    uint32_t          nb_streams;
    zst_stream_id_t   next_stream_id;

    /* Push-mode buffer queue */
    zst_buffer_node_t* queue_head;
    zst_buffer_node_t* queue_tail;
    size_t             current_buf_offset;
    pthread_mutex_t    lock;
    int                eos_received;
    int                eos_sent;
    int                probing;

    /* Direct-file mode */
    char               location[256];
    int                direct_file;

    /* Pacing */
    bool               real_time_pacing;
    zst_time_t         base_time;

    /* Signal/status tracking */
    int                signal_present;
} mp4_demuxer_t;

/* ── Temporary read context for initial probe ──────────────────────── */

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} temp_read_ctx_t;

/* ── Forward declarations ──────────────────────────────────────────── */

static uint32_t     mp4demux_get_stream_count(zst_element_t* el);
static zst_result_t mp4demux_get_stream_info(zst_element_t* el, uint32_t index, zst_stream_info_t* info_out);
static zst_pad_t*   mp4demux_get_stream_pad(zst_element_t* el, zst_stream_id_t stream_id);
static void         mp4demux_send_eos(zst_element_t* el);

/* =====================================================================
    Push-mode buffer queue helpers
   ===================================================================== */

static void
mp4demux_enqueue(mp4_demuxer_t* s, zst_buffer_t* buf)
{
    pthread_mutex_lock(&s->lock);
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->eos_received = 1;
        pthread_mutex_unlock(&s->lock);
        return;
    }

    zst_buffer_node_t* node = malloc(sizeof(*node));
    if (node) {
        node->buf = zst_buffer_ref(buf);
        node->next = NULL;
        if (s->queue_tail) {
            s->queue_tail->next = node;
            s->queue_tail = node;
        } else {
            s->queue_head = node;
            s->queue_tail = node;
            s->current_buf_offset = 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
}

static void
mp4demux_clear_queue(mp4_demuxer_t* s)
{
    pthread_mutex_lock(&s->lock);
    zst_buffer_node_t* curr = s->queue_head;
    while (curr) {
        zst_buffer_node_t* next = curr->next;
        zst_buffer_unref(curr->buf);
        free(curr);
        curr = next;
    }
    s->queue_head = NULL;
    s->queue_tail = NULL;
    s->current_buf_offset = 0;
    pthread_mutex_unlock(&s->lock);
}

static size_t
mp4demux_get_queue_bytes(mp4_demuxer_t* s)
{
    pthread_mutex_lock(&s->lock);
    size_t total = 0;
    zst_buffer_node_t* curr = s->queue_head;
    while (curr) {
        total += curr->buf->memory.size;
        curr = curr->next;
    }
    if (s->queue_head) {
        total -= s->current_buf_offset;
    }
    pthread_mutex_unlock(&s->lock);
    return total;
}

static void
mp4demux_advance_queue(mp4_demuxer_t* s, size_t bytes)
{
    pthread_mutex_lock(&s->lock);
    size_t remaining = bytes;
    while (remaining > 0 && s->queue_head) {
        zst_buffer_t* head_buf = s->queue_head->buf;
        size_t available = head_buf->memory.size - s->current_buf_offset;
        if (remaining >= available) {
            remaining -= available;
            zst_buffer_node_t* next = s->queue_head->next;
            zst_buffer_unref(head_buf);
            free(s->queue_head);
            s->queue_head = next;
            if (!s->queue_head) {
                s->queue_tail = NULL;
            }
            s->current_buf_offset = 0;
        } else {
            s->current_buf_offset += remaining;
            remaining = 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
}

/* ── AVIO read callback for push mode ──────────────────────────────── */

static int
mp4demux_read_packet(void* opaque, uint8_t* buf, int buf_size)
{
    mp4_demuxer_t* s = opaque;
    pthread_mutex_lock(&s->lock);

    int bytes_read = 0;
    while (bytes_read < buf_size) {
        if (!s->queue_head) {
            if (s->eos_received) {
                pthread_mutex_unlock(&s->lock);
                return bytes_read > 0 ? bytes_read : AVERROR_EOF;
            }
            pthread_mutex_unlock(&s->lock);
            return bytes_read > 0 ? bytes_read : AVERROR(EAGAIN);
        }

        zst_buffer_t* head_buf = s->queue_head->buf;
        int available = (int)head_buf->memory.size - (int)s->current_buf_offset;
        int to_copy = buf_size - bytes_read;
        if (to_copy > available) to_copy = available;

        memcpy(buf + bytes_read, (uint8_t*)head_buf->memory.data + s->current_buf_offset, to_copy);
        bytes_read += to_copy;
        s->current_buf_offset += to_copy;

        if (s->current_buf_offset >= head_buf->memory.size) {
            zst_buffer_node_t* next = s->queue_head->next;
            zst_buffer_unref(s->queue_head->buf);
            free(s->queue_head);
            s->queue_head = next;
            if (!s->queue_head) {
                s->queue_tail = NULL;
            }
            s->current_buf_offset = 0;
        }
    }

    pthread_mutex_unlock(&s->lock);
    return bytes_read;
}

/* ── Temp read callback for initial probe ──────────────────────────── */

static int
mp4demux_temp_read(void* opaque, uint8_t* buf, int buf_size)
{
    temp_read_ctx_t* ctx = opaque;
    if (ctx->pos >= ctx->size) return AVERROR_EOF;
    int to_read = buf_size;
    if (ctx->pos + to_read > ctx->size) {
        to_read = (int)(ctx->size - ctx->pos);
    }
    memcpy(buf, ctx->data + ctx->pos, to_read);
    ctx->pos += to_read;
    return to_read;
}

/* ── Stream table helpers ──────────────────────────────────────────── */

static mp4demux_stream_t*
mp4demux_find_stream_by_id(mp4_demuxer_t* s, zst_stream_id_t id)
{
    for (uint32_t i = 0; i < s->nb_streams; i++) {
        if (s->streams[i].id == id)
            return &s->streams[i];
    }
    return NULL;
}

static mp4demux_stream_t*
mp4demux_find_stream_by_av_idx(mp4_demuxer_t* s, int av_idx)
{
    for (uint32_t i = 0; i < s->nb_streams; i++) {
        if (s->streams[i].av_stream_idx_valid && s->streams[i].av_stream_idx == av_idx)
            return &s->streams[i];
    }
    return NULL;
}

static zst_stream_id_t
mp4demux_next_id(mp4_demuxer_t* s)
{
    return s->next_stream_id++;
}

/* ── Build caps from AVCodecParameters ─────────────────────────────── */

static const char*
mp4demux_video_media_type(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264:  return "video/x-h264";
    case AV_CODEC_ID_HEVC:  return "video/x-h265";
    case AV_CODEC_ID_MPEG4: return "video/mpeg4";
    case AV_CODEC_ID_VP8:   return "video/x-vp8";
    case AV_CODEC_ID_VP9:   return "video/x-vp9";
    case AV_CODEC_ID_AV1:   return "video/x-av1";
    default:                return "video/x-unknown";
    }
}

static zst_caps_t*
mp4demux_build_video_caps(AVCodecParameters* par)
{
    const char* mt = mp4demux_video_media_type(par->codec_id);
    zst_caps_t* caps = zst_caps_new_simple(mt);
    if (caps) {
        zst_caps_set_int(caps, "width", par->width);
        zst_caps_set_int(caps, "height", par->height);
        if (par->extradata && par->extradata_size > 0)
            zst_caps_set_buffer(caps, "codec_data", par->extradata, par->extradata_size);
    }
    return caps;
}

static const char*
mp4demux_audio_media_type(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AAC:      return "audio/aac";
    case AV_CODEC_ID_MP3:      return "audio/mpeg";
    case AV_CODEC_ID_OPUS:     return "audio/opus";
    case AV_CODEC_ID_VORBIS:   return "audio/vorbis";
    case AV_CODEC_ID_FLAC:     return "audio/flac";
    case AV_CODEC_ID_PCM_S16LE: return "audio/x-raw";
    default:                   return "audio/x-unknown";
    }
}

static zst_caps_t*
mp4demux_build_audio_caps(AVCodecParameters* par)
{
    const char* mt = mp4demux_audio_media_type(par->codec_id);
    int channels = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    channels = par->ch_layout.nb_channels;
#else
    channels = par->channels;
#endif
    zst_caps_t* caps = zst_caps_new_simple(mt);
    if (caps) {
        zst_caps_set_int(caps, "channels", channels);
        zst_caps_set_int(caps, "sample-rate", par->sample_rate);
        if (par->extradata && par->extradata_size > 0)
            zst_caps_set_buffer(caps, "codec_data", par->extradata, par->extradata_size);
    }
    return caps;
}

/* ── Push sticky events on a pad ───────────────────────────────────── */

static void
mp4demux_push_sticky_on_pad(zst_pad_t* pad, zst_stream_id_t stream_id)
{
    if (!pad) return;

    zst_pad_event_t* ss = zst_pad_event_new_stream_start(stream_id);
    if (ss) {
        zst_pad_push_event(pad, ss);
        zst_pad_event_unref(ss);
    }

    if (pad->caps) {
        zst_caps_t* caps_copy = zst_caps_copy(pad->caps);
        if (caps_copy) {
            zst_pad_event_t* ce = zst_pad_event_new_caps(caps_copy);
            if (ce) {
                zst_pad_push_event(pad, ce);
                zst_pad_event_unref(ce);
            }
            zst_caps_destroy(caps_copy);
        }
    }

    zst_segment_t segment = zst_segment_default();
    zst_pad_event_t* se = zst_pad_event_new_segment(&segment);
    if (se) {
        zst_pad_push_event(pad, se);
        zst_pad_event_unref(se);
    }
}

static uint32_t
mp4demux_kind_pad_index(mp4_demuxer_t* s, zst_media_kind_t kind)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < s->nb_streams; i++) {
        if (s->streams[i].kind == kind) count++;
    }
    return count;
}

static int
mp4demux_caps_equal(const zst_caps_t* a, const zst_caps_t* b)
{
    if (a == b) return 1;
    if (!a || !b || !a->structs || !b->structs) return 0;
    const zst_caps_struct_t* as = a->structs;
    const zst_caps_struct_t* bs = b->structs;
    if (as->next || bs->next) return 0;
    if (strcmp(as->media_type, bs->media_type) != 0) return 0;
    if (as->type != bs->type) return 0;
    if (as->video.width != bs->video.width ||
        as->video.height != bs->video.height ||
        as->video.framerate != bs->video.framerate ||
        strcmp(as->video.pixel_format, bs->video.pixel_format) != 0) return 0;
    if (as->audio.channels != bs->audio.channels ||
        as->audio.sample_rate != bs->audio.sample_rate ||
        strcmp(as->audio.format, bs->audio.format) != 0) return 0;
    if (as->nb_fields != bs->nb_fields) return 0;
    for (uint32_t i = 0; i < as->nb_fields; i++) {
        const zst_caps_field_t* af = &as->fields[i];
        const zst_caps_field_t* bf = &bs->fields[i];
        if (strcmp(af->key, bf->key) != 0 || af->type != bf->type) return 0;
        switch (af->type) {
        case ZST_CAPS_FIELD_INT:
            if (af->value.i_val != bf->value.i_val) return 0;
            break;
        case ZST_CAPS_FIELD_UINT:
            if (af->value.u_val != bf->value.u_val) return 0;
            break;
        case ZST_CAPS_FIELD_DOUBLE:
            if (af->value.d_val != bf->value.d_val) return 0;
            break;
        case ZST_CAPS_FIELD_STRING:
            if (strcmp(af->value.s_val ? af->value.s_val : "",
                       bf->value.s_val ? bf->value.s_val : "") != 0) return 0;
            break;
        case ZST_CAPS_FIELD_FRACTION:
            if (af->value.f_val.num != bf->value.f_val.num ||
                af->value.f_val.denom != bf->value.f_val.denom) return 0;
            break;
        case ZST_CAPS_FIELD_BUFFER:
            if (af->value.b_val.size != bf->value.b_val.size) return 0;
            if (af->value.b_val.size > 0 &&
                memcmp(af->value.b_val.data, bf->value.b_val.data, af->value.b_val.size) != 0) return 0;
            break;
        }
    }
    return 1;
}

static void
mp4demux_fill_stream_info(mp4demux_stream_t* ms, uint32_t index, zst_stream_info_t* info)
{
    memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->id = ms->id;
    info->program_id = 0;
    info->index = index;
    info->kind = ms->kind;
    info->status = ms->status;
    info->caps = ms->caps;
    info->first_pts = 0;
    info->last_seen_pts = ms->last_seen_pts_ns;
    info->name = ms->pad ? (char*)ms->pad->name : NULL;
}

static void
mp4demux_update_stream_caps(zst_element_t* el, mp4demux_stream_t* ms, uint32_t index,
                            zst_caps_t* new_caps)
{
    if (!new_caps) return;
    if (mp4demux_caps_equal(ms->caps, new_caps)) {
        zst_caps_destroy(new_caps);
        return;
    }

    zst_caps_t* old_caps = ms->caps ? zst_caps_copy(ms->caps) : NULL;
    if (ms->caps) zst_caps_destroy(ms->caps);
    ms->caps = new_caps;
    ms->status = ZST_STREAM_STATUS_CHANGED;
    if (ms->pad) {
        zst_pad_set_caps(ms->pad, new_caps);
        zst_pad_event_t* ce = zst_pad_event_new_caps(new_caps);
        if (ce) {
            zst_pad_push_event(ms->pad, ce);
            zst_pad_event_unref(ce);
        }
    }

    if (el->bus) {
        if (ms->pad) {
            zst_event_t* caps_ev = zst_event_new_caps_changed(el, ms->pad, old_caps, new_caps);
            if (caps_ev) zst_bus_post(el->bus, caps_ev);
        }
        zst_stream_info_t info;
        mp4demux_fill_stream_info(ms, index, &info);
        zst_event_t* stream_ev = zst_event_new_stream_changed(el, &info);
        if (stream_ev) zst_bus_post(el->bus, stream_ev);
    }
    ms->status = ZST_STREAM_STATUS_PRESENT;
    if (old_caps) zst_caps_destroy(old_caps);
}

static zst_result_t
mp4demux_add_stream_from_av(zst_element_t* el, unsigned int av_index)
{
    mp4_demuxer_t* s = el->priv;
    if (!s->fc || av_index >= s->fc->nb_streams || s->nb_streams >= MP4DEMUX_MAX_STREAMS) return ZST_ERROR;
    if (mp4demux_find_stream_by_av_idx(s, (int)av_index)) return ZST_OK;

    AVStream* st = s->fc->streams[av_index];
    enum AVMediaType codec_type = st->codecpar->codec_type;
    if (codec_type != AVMEDIA_TYPE_VIDEO && codec_type != AVMEDIA_TYPE_AUDIO)
        return ZST_OK;

    zst_caps_t* caps = NULL;
    zst_media_kind_t kind = ZST_MEDIA_UNKNOWN;
    const char* pad_prefix = "data";
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        caps = mp4demux_build_video_caps(st->codecpar);
        kind = ZST_MEDIA_VIDEO;
        pad_prefix = "video";
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        caps = mp4demux_build_audio_caps(st->codecpar);
        kind = ZST_MEDIA_AUDIO;
        pad_prefix = "audio";
    }
    if (!caps) return ZST_OK;

    zst_stream_id_t sid = mp4demux_next_id(s);
    uint32_t table_index = s->nb_streams;
    mp4demux_stream_t* ms = &s->streams[table_index];
    memset(ms, 0, sizeof(*ms));
    ms->id = sid;
    ms->av_stream_idx = (int)av_index;
    ms->av_stream_idx_valid = 1;
    ms->kind = kind;
    ms->status = ZST_STREAM_STATUS_PRESENT;
    ms->caps = caps;
    ms->codec_id = st->codecpar->codec_id;

    char pad_name[64];
    snprintf(pad_name, sizeof(pad_name), "%s_%u", pad_prefix, mp4demux_kind_pad_index(s, kind));

    zst_pad_t* src_pad = zst_pad_create(pad_name, ZST_PAD_SRC);
    if (!src_pad) {
        zst_caps_destroy(caps);
        memset(ms, 0, sizeof(*ms));
        return ZST_ERROR;
    }
    zst_pad_set_caps(src_pad, caps);
    if (s->direct_file) {
        /* File demuxers default to BLOCK so lossless late linking is possible. */
        zst_pad_set_unlinked_policy(src_pad, ZST_PAD_UNLINKED_BLOCK, 0);
    } else {
        zst_pad_set_unlinked_policy(src_pad, ZST_PAD_UNLINKED_DROP, 0);
    }
    ms->pad = src_pad;

    zst_stream_info_t info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    info.id = sid;
    info.index = table_index;
    info.kind = kind;
    info.status = ZST_STREAM_STATUS_PRESENT;
    info.caps = caps;

    zst_result_t add_ret = zst_element_add_dynamic_pad(el, src_pad, &info);
    if (add_ret != ZST_OK) {
        ZST_LOG_ERROR("mp4demux", "failed to add dynamic pad %s", pad_name);
        zst_pad_destroy(src_pad);
        zst_caps_destroy(caps);
        memset(ms, 0, sizeof(*ms));
        return add_ret;
    }

    mp4demux_push_sticky_on_pad(src_pad, sid);
    s->nb_streams++;
    return ZST_OK;
}

/* ── Create/update dynamic source pads from AVFormatContext streams ─── */

static zst_result_t
mp4demux_create_pads_from_streams(zst_element_t* el, int* video_stream_idx_out, int* audio_stream_idx_out)
{
    mp4_demuxer_t* s = el->priv;
    if (!s->fc) return ZST_ERROR;

    if (video_stream_idx_out) *video_stream_idx_out = -1;
    if (audio_stream_idx_out) *audio_stream_idx_out = -1;

    uint32_t before = s->nb_streams;
    for (unsigned int i = 0; i < s->fc->nb_streams; i++) {
        AVStream* st = s->fc->streams[i];
        enum AVMediaType codec_type = st->codecpar->codec_type;
        if (codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx_out && *video_stream_idx_out < 0)
            *video_stream_idx_out = (int)i;
        if (codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_idx_out && *audio_stream_idx_out < 0)
            *audio_stream_idx_out = (int)i;

        mp4demux_stream_t* existing = mp4demux_find_stream_by_av_idx(s, (int)i);
        if (existing) {
            zst_caps_t* caps = NULL;
            if (codec_type == AVMEDIA_TYPE_VIDEO) caps = mp4demux_build_video_caps(st->codecpar);
            else if (codec_type == AVMEDIA_TYPE_AUDIO) caps = mp4demux_build_audio_caps(st->codecpar);
            uint32_t idx = (uint32_t)(existing - s->streams);
            mp4demux_update_stream_caps(el, existing, idx, caps);
            continue;
        }
        if (s->nb_streams < MP4DEMUX_MAX_STREAMS) {
            mp4demux_add_stream_from_av(el, i);
        }
    }

    if (s->nb_streams > before && el->bus) {
        if (!s->signal_present) {
            zst_event_t* present = zst_event_new_signal_present(el);
            if (present) zst_bus_post(el->bus, present);
        }
        zst_event_t* nmp = zst_event_new_no_more_pads(el);
        if (nmp) zst_bus_post(el->bus, nmp);
    }
    if (s->nb_streams > 0) s->signal_present = 1;

    return ZST_OK;
}

/* ── Push-mode: probe and open format from buffered data ───────────── */

static int
mp4demux_streams_ready(AVFormatContext* fc)
{
    if (fc->nb_streams == 0) return 0;
    for (unsigned int i = 0; i < fc->nb_streams; i++) {
        AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (st->codecpar->width <= 0 || st->codecpar->height <= 0 ||
                st->codecpar->format == AV_PIX_FMT_NONE) {
                return 0;
            }
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            int channels = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
            channels = st->codecpar->ch_layout.nb_channels;
#else
            channels = st->codecpar->channels;
#endif
            if (st->codecpar->sample_rate <= 0 || channels <= 0) {
                return 0;
            }
        }
    }
    return 1;
}

static void
mp4demux_close_temp_context(AVFormatContext* fc)
{
    if (fc) {
        AVIOContext* pb = fc->pb;
        avformat_close_input(&fc);
        if (pb) {
            av_freep(&pb->buffer);
            avio_context_free(&pb);
        }
    }
}

static zst_result_t
mp4demux_try_init(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    if (s->fc) return ZST_OK;
    if (s->probing) return ZST_AGAIN;
    s->probing = 1;

    size_t q_bytes = mp4demux_get_queue_bytes(s);
    if (q_bytes < MP4DEMUX_INIT_PROBE_BYTES && !s->eos_received) {
        s->probing = 0;
        return ZST_OK; /* Wait for more data */
    }

    uint8_t* flat = malloc(q_bytes);
    if (!flat) { s->probing = 0; return ZST_ERROR; }

    pthread_mutex_lock(&s->lock);
    size_t flat_pos = 0;
    zst_buffer_node_t* curr = s->queue_head;
    size_t offset = s->current_buf_offset;
    while (curr) {
        size_t to_copy = curr->buf->memory.size - offset;
        memcpy(flat + flat_pos, (uint8_t*)curr->buf->memory.data + offset, to_copy);
        flat_pos += to_copy;
        offset = 0;
        curr = curr->next;
    }
    pthread_mutex_unlock(&s->lock);

    AVFormatContext* temp_fc = avformat_alloc_context();
    if (!temp_fc) { free(flat); s->probing = 0; return ZST_ERROR; }

    temp_read_ctx_t read_ctx = {
        .data = flat,
        .size = q_bytes,
        .pos = 0
    };

    size_t avio_buf_size = 8192;
    uint8_t* avio_buf = av_malloc(avio_buf_size);
    AVIOContext* pb = avio_alloc_context(
        avio_buf, (int)avio_buf_size,
        0, &read_ctx, mp4demux_temp_read, NULL, NULL
    );
    if (!pb) {
        av_free(avio_buf);
        avformat_free_context(temp_fc);
        free(flat);
        s->probing = 0;
        return ZST_ERROR;
    }
    pb->seekable = 0;
    temp_fc->pb = pb;
    temp_fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    temp_fc->probesize = 5000000;
    temp_fc->max_analyze_duration = 5000000;

    const AVInputFormat* mp4_fmt = av_find_input_format("mov,mp4,m4a,3gp,3g2,mj2");
    if (avformat_open_input(&temp_fc, NULL, mp4_fmt, NULL) < 0) {
        if (temp_fc) avformat_free_context(temp_fc);
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        free(flat);
        s->probing = 0;
        return ZST_OK; /* Will retry when more data arrives */
    }

    int ret = avformat_find_stream_info(temp_fc, NULL);
    if (ret < 0 && !s->eos_received) {
        mp4demux_close_temp_context(temp_fc);
        free(flat);
        s->probing = 0;
        return ZST_OK;
    }

    if (!mp4demux_streams_ready(temp_fc) && !s->eos_received) {
        mp4demux_close_temp_context(temp_fc);
        free(flat);
        s->probing = 0;
        return ZST_OK;
    }

    s->fc = temp_fc;

    size_t consumed = read_ctx.pos;
    mp4demux_advance_queue(s, consumed);

    av_freep(&s->fc->pb->buffer);
    avio_context_free(&s->fc->pb);

    s->avio_buf_size = 8192;
    s->avio_buf = av_malloc(s->avio_buf_size);
    s->fc->pb = avio_alloc_context(
        s->avio_buf, (int)s->avio_buf_size,
        0, s, mp4demux_read_packet, NULL, NULL
    );
    s->fc->pb->seekable = 0;
    s->fc->flags |= AVFMT_FLAG_CUSTOM_IO;

    /* Create dynamic pads from discovered streams */
    int video_idx = -1, audio_idx = -1;
    mp4demux_create_pads_from_streams(el, &video_idx, &audio_idx);

    free(flat);
    s->probing = 0;
    return ZST_OK;
}

/* ── H.264 AVCC to Annex B conversion ──────────────────────────────── */

static uint8_t*
mp4demux_avcc_to_annexb(const uint8_t* avcc_data, int avcc_size,
                         const uint8_t* extradata, int extra_size,
                         int* out_size, int is_keyframe)
{
    int alloc_size = avcc_size + 1024;
    uint8_t* out = malloc(alloc_size);
    if (!out) return NULL;

    int pos = 0;

    if (is_keyframe && extradata && extra_size >= 7 && extradata[0] == 1) {
        int num_sps = extradata[5] & 0x1F;
        int p = 6;
        for (int i = 0; i < num_sps; i++) {
            if (p + 2 > extra_size) break;
            int sps_len = (extradata[p] << 8) | extradata[p+1];
            p += 2;
            if (p + sps_len > extra_size) break;
            out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = 1;
            memcpy(out + pos, extradata + p, sps_len);
            pos += sps_len;
            p += sps_len;
        }
        if (p < extra_size) {
            int num_pps = extradata[p];
            p++;
            for (int i = 0; i < num_pps; i++) {
                if (p + 2 > extra_size) break;
                int pps_len = (extradata[p] << 8) | extradata[p+1];
                p += 2;
                if (p + pps_len > extra_size) break;
                out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = 1;
                memcpy(out + pos, extradata + p, pps_len);
                pos += pps_len;
                p += pps_len;
            }
        }
    }

    int i = 0;
    while (i + 4 <= avcc_size) {
        uint32_t nal_len = ((uint32_t)avcc_data[i] << 24) |
                           ((uint32_t)avcc_data[i+1] << 16) |
                           ((uint32_t)avcc_data[i+2] << 8)  |
                           (uint32_t)avcc_data[i+3];
        i += 4;
        if (i + nal_len > (uint32_t)avcc_size) break;

        if (pos + 4 + nal_len > alloc_size) {
            alloc_size = pos + 4 + nal_len + 1024;
            uint8_t* new_out = realloc(out, alloc_size);
            if (!new_out) { free(out); return NULL; }
            out = new_out;
        }

        out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = 1;
        memcpy(out + pos, avcc_data + i, nal_len);
        pos += nal_len;
        i += nal_len;
    }

    *out_size = pos;
    return out;
}

/* ── Stream cleanup ─────────────────────────────────────────────────── */

static void
mp4demux_reset_streams(zst_element_t* el)
{
    if (!el) return;
    mp4_demuxer_t* s = el->priv;
    if (!s) return;

    for (int i = (int)s->nb_streams - 1; i >= 0; i--) {
        mp4demux_stream_t* ms = &s->streams[i];
        if (ms->pad) {
            zst_element_remove_dynamic_pad(el, ms->pad);
            ms->pad = NULL;
        }
        if (ms->caps) {
            zst_caps_destroy(ms->caps);
            ms->caps = NULL;
        }
    }
    memset(s->streams, 0, sizeof(s->streams));
    s->nb_streams = 0;
    s->signal_present = 0;
}

/* ── Send EOS on all active pads ───────────────────────────────────── */

static void
mp4demux_send_eos(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    if (s->eos_sent) return;
    s->eos_sent = 1;

    for (uint32_t i = 0; i < s->nb_streams; i++) {
        mp4demux_stream_t* ms = &s->streams[i];
        if (ms->pad && ms->pad->peer) {
            zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
            if (eos_buf) {
                eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_push(ms->pad, eos_buf);
                zst_buffer_unref(eos_buf);
            }
        }
    }
    if (el->bus) {
        zst_bus_post(el->bus, zst_event_new_eos(el));
    }
}

/* ── Process: read packets and push to output pads ─────────────────── */

static zst_result_t
mp4demux_process(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    if (!s->fc) return ZST_ERROR;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    zst_result_t process_result = ZST_OK;

    while (1) {
        int ret = av_read_frame(s->fc, pkt);
        if (ret == AVERROR(EAGAIN)) {
            process_result = ZST_AGAIN;
            break;
        }
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            ZST_LOG_INFO("mp4demux", "av_read_frame returned: %d (%s)", ret, errbuf);
            if (ret == AVERROR_EOF || s->eos_received) {
                mp4demux_send_eos(el);
                process_result = ZST_EOF;
            } else {
                if (s->signal_present && el->bus) {
                    zst_event_t* lost = zst_event_new_signal_lost(el);
                    if (lost) zst_bus_post(el->bus, lost);
                }
                s->signal_present = 0;
                process_result = ZST_ERROR;
            }
            break;
        }

        /* FFmpeg may discover late streams (for example fragmented MP4). */
        mp4demux_create_pads_from_streams(el, NULL, NULL);

        /* Find matching stream */
        mp4demux_stream_t* ms = NULL;
        for (uint32_t i = 0; i < s->nb_streams; i++) {
            if (s->streams[i].av_stream_idx_valid &&
                s->streams[i].av_stream_idx == pkt->stream_index &&
                s->streams[i].status == ZST_STREAM_STATUS_PRESENT) {
                ms = &s->streams[i];
                break;
            }
        }

        if (ms && ms->pad) {
            int btype = ZST_BUFFER_USER;
            if (ms->kind == ZST_MEDIA_VIDEO)
                btype = ZST_BUFFER_VIDEO_PACKET;
            else if (ms->kind == ZST_MEDIA_AUDIO)
                btype = ZST_BUFFER_AUDIO_PACKET;

            zst_buffer_t* out = zst_buffer_create(btype);
            if (out) {
                if (btype == ZST_BUFFER_VIDEO_PACKET) {
                    int annexb_size = 0;
                    AVCodecParameters* codecpar = s->fc->streams[pkt->stream_index]->codecpar;
                    int is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY);
                    uint8_t* annexb_data = mp4demux_avcc_to_annexb(
                        pkt->data, pkt->size,
                        codecpar->extradata, codecpar->extradata_size,
                        &annexb_size, is_keyframe);
                    if (annexb_data) {
                        out->memory.data = annexb_data;
                        out->memory.size = annexb_size;
                        out->memory.priv = annexb_data;
                        out->memory.release = free;
                    } else {
                        ZST_LOG_ERROR("mp4demux", "Annex B conversion failed, falling back");
                        out->memory.data = malloc(pkt->size);
                        out->memory.size = pkt->size;
                        out->memory.priv = out->memory.data;
                        out->memory.release = free;
                        memcpy(out->memory.data, pkt->data, pkt->size);
                    }
                } else {
                    out->memory.data = malloc(pkt->size);
                    out->memory.size = pkt->size;
                    out->memory.priv = out->memory.data;
                    out->memory.release = free;
                    memcpy(out->memory.data, pkt->data, pkt->size);
                }

                AVRational tb = s->fc->streams[pkt->stream_index]->time_base;
                zst_time_t file_pts = av_rescale_q(pkt->pts, tb, (AVRational){1, 1000000000});
                zst_time_t file_dts = av_rescale_q(pkt->dts, tb, (AVRational){1, 1000000000});
                out->duration = av_rescale_q(pkt->duration, tb, (AVRational){1, 1000000000});

                if ((s->direct_file || s->real_time_pacing) && el->clock) {
                    if (s->base_time == 0) s->base_time = zst_clock_get_time(el->clock);
                    out->pts = s->base_time + file_pts;
                    out->dts = s->base_time + file_dts;
                } else {
                    out->pts = file_pts;
                    out->dts = file_dts;
                }

                ms->last_seen_pts_ns = out->pts;

                bool should_wait = s->real_time_pacing || (s->direct_file && el->pipeline && el->pipeline->clock_sync);
                if (should_wait && el->clock && out->pts > 0) {
                    zst_time_t current = zst_clock_get_time(el->clock);
                    if (out->pts > current) {
                        zst_clock_wait(el->clock, out->pts - current);
                    }
                }

                zst_pad_push(ms->pad, out);
                zst_buffer_unref(out);
            }
        }

        av_packet_unref(pkt);

        if (s->direct_file) {
            process_result = ZST_OK;
            break;
        }
    }

    av_packet_free(&pkt);
    return process_result;
}

/* ── Sink pad push callback (push/streaming mode) ──────────────────── */

static zst_result_t
mp4demux_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mp4_demuxer_t* s = el->priv;

    mp4demux_enqueue(s, buf);

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->eos_received = 1;
    }

    if (!s->fc) {
        zst_result_t res = mp4demux_try_init(el);
        if (res != ZST_OK) return res;
    }

    if (s->fc) {
        return mp4demux_process(el);
    } else if (s->eos_received) {
        mp4demux_send_eos(el);
    }

    return ZST_OK;
}

/* ── Element lifecycle: open / close / start / stop ────────────────── */

static zst_result_t
mp4demux_open(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    pthread_mutex_init(&s->lock, NULL);
    return ZST_OK;
}

static zst_result_t
mp4demux_close(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    pthread_mutex_destroy(&s->lock);
    return ZST_OK;
}

static zst_result_t
mp4demux_start(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    s->fc = NULL;
    s->queue_head = NULL;
    s->queue_tail = NULL;
    s->current_buf_offset = 0;
    s->eos_received = 0;
    s->eos_sent = 0;
    s->probing = 0;
    mp4demux_reset_streams(el);
    s->next_stream_id = 1;
    s->base_time = 0;

    /* Direct-file mode: open immediately */
    if (s->direct_file && s->location[0] != '\0') {
        AVFormatContext* fc = NULL;
        int err = avformat_open_input(&fc, s->location, NULL, NULL);
        if (err < 0) {
            char errbuf[256];
            av_strerror(err, errbuf, sizeof(errbuf));
            ZST_LOG_ERROR("mp4demux", "Failed to open input '%s': %d (%s)", s->location, err, errbuf);
            return ZST_ERROR;
        }
        err = avformat_find_stream_info(fc, NULL);
        if (err < 0) {
            avformat_close_input(&fc);
            ZST_LOG_ERROR("mp4demux", "Failed to find stream info for '%s'", s->location);
            return ZST_ERROR;
        }
        s->fc = fc;
        ZST_LOG_INFO("mp4demux", "Successfully opened direct file '%s'", s->location);

        int video_idx = -1, audio_idx = -1;
        mp4demux_create_pads_from_streams(el, &video_idx, &audio_idx);
    }

    return ZST_OK;
}

static zst_result_t
mp4demux_stop(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    if (s->fc) {
        if (!s->direct_file && s->fc->pb) {
            av_freep(&s->fc->pb->buffer);
            avio_context_free(&s->fc->pb);
        }
        avformat_close_input(&s->fc);
        s->fc = NULL;
    }
    mp4demux_clear_queue(s);
    mp4demux_reset_streams(el);
    s->eos_received = 0;
    s->eos_sent = 0;
    s->probing = 0;
    return ZST_OK;
}

/* ── Process callback for pull/scheduler mode (direct file) ────────── */

static zst_result_t
mp4demux_process_op(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    (void)out;
    mp4_demuxer_t* s = el->priv;
    if (!s->fc || !s->direct_file) return ZST_OK;
    return mp4demux_process(el);
}

/* ── Stream query callbacks ─────────────────────────────────────────── */

static uint32_t
mp4demux_get_stream_count(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    return s ? s->nb_streams : 0;
}

static zst_result_t
mp4demux_get_stream_info(zst_element_t* el, uint32_t index, zst_stream_info_t* info_out)
{
    mp4_demuxer_t* s = el->priv;
    if (!s || index >= s->nb_streams || !info_out) return ZST_ERROR;

    mp4demux_stream_t* ms = &s->streams[index];
    memset(info_out, 0, sizeof(*info_out));
    info_out->struct_size = sizeof(*info_out);
    info_out->id = ms->id;
    info_out->program_id = 0;
    info_out->index = index;
    info_out->kind = ms->kind;
    info_out->status = ms->status;
    info_out->caps = ms->caps ? zst_caps_copy(ms->caps) : NULL;
    info_out->first_pts = 0;
    info_out->last_seen_pts = ms->last_seen_pts_ns;

    if (ms->pad && ms->pad->name) {
        info_out->name = strdup(ms->pad->name);
    }

    return ZST_OK;
}

static zst_pad_t*
mp4demux_get_stream_pad(zst_element_t* el, zst_stream_id_t stream_id)
{
    mp4_demuxer_t* s = el->priv;
    if (!s) return NULL;
    mp4demux_stream_t* ms = mp4demux_find_stream_by_id(s, stream_id);
    return ms ? ms->pad : NULL;
}

/* ── Properties ─────────────────────────────────────────────────────── */

static zst_result_t
mp4demux_set_property(zst_element_t* el, const char* name, const char* value)
{
    mp4_demuxer_t* s = el->priv;
    if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(s->location, sizeof(s->location), "%s", value);
        s->direct_file = s->location[0] != '\0';
        return ZST_OK;
    } else if (strcmp(name, "real-time-pacing") == 0) {
        s->real_time_pacing = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
mp4demux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    mp4_demuxer_t* s = el->priv;
    if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(value_out, max_len, "%s", s->location);
        return ZST_OK;
    } else if (strcmp(name, "real-time-pacing") == 0) {
        snprintf(value_out, max_len, "%s", s->real_time_pacing ? "true" : "false");
        return ZST_OK;
    }
    return ZST_ERROR;
}

/* ── Ops vtable ─────────────────────────────────────────────────────── */

static zst_element_ops_t g_ops = {
    .name  = "mp4demux",
    .open  = mp4demux_open,
    .close = mp4demux_close,
    .start = mp4demux_start,
    .stop  = mp4demux_stop,
    .process = mp4demux_process_op,
    .set_property = mp4demux_set_property,
    .get_property = mp4demux_get_property,
    .get_stream_count = mp4demux_get_stream_count,
    .get_stream_info = mp4demux_get_stream_info,
    .get_stream_pad = mp4demux_get_stream_pad,
};

/* ── Public constructors ────────────────────────────────────────────── */

zst_element_t*
zst_mp4_demuxer_create(void)
{
    mp4_demuxer_t* priv = calloc(1, sizeof(*priv));
    if (!priv) return NULL;

    zst_element_t* el = zst_element_create(&g_ops, priv);
    if (!el) {
        free(priv);
        return NULL;
    }

    zst_pad_t* sink = zst_pad_create("sink", ZST_PAD_SINK);
    if (!sink) {
        zst_element_destroy(el);
        return NULL;
    }
    sink->push = mp4demux_sink_push;
    zst_element_add_pad(el, sink);

    /* Set template caps on sink pad */
    zst_caps_t* sink_caps = zst_caps_new_simple("video/quicktime");
    if (sink_caps) {
        zst_pad_set_template_caps(sink, sink_caps);
        zst_caps_destroy(sink_caps);
    }

    return el;
}

zst_element_t*
zst_mp4_demuxer_create_with_config(const zst_mp4_demuxer_config_t* config)
{
    if (!config || config->struct_size < sizeof(size_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("mp4demux");
    if (!el) return NULL;

    if (config->struct_size >= offsetof(zst_mp4_demuxer_config_t, location) + sizeof(config->location)) {
        if (config->location) {
            zst_element_set_property_string(el, "location", config->location);
        }
    }
    if (config->struct_size >= sizeof(zst_mp4_demuxer_config_t)) {
        zst_element_set_property_bool(el, "real-time-pacing", config->real_time_pacing);
    }

    return el;
}

/* ── Public extradata accessor ─────────────────────────────────────── */

const uint8_t*
zst_mp4_demuxer_get_video_extradata(zst_element_t* el, int* size_out)
{
    if (size_out) *size_out = 0;
    if (!el) return NULL;
    mp4_demuxer_t* s = el->priv;

    /* Find the first video stream */
    for (uint32_t i = 0; i < s->nb_streams; i++) {
        if (s->streams[i].kind == ZST_MEDIA_VIDEO && s->streams[i].av_stream_idx_valid) {
            if (s->fc && s->streams[i].av_stream_idx >= 0 &&
                (unsigned)s->streams[i].av_stream_idx < s->fc->nb_streams) {
                AVCodecParameters* par = s->fc->streams[s->streams[i].av_stream_idx]->codecpar;
                if (par->extradata && par->extradata_size > 0) {
                    if (size_out) *size_out = par->extradata_size;
                    return par->extradata;
                }
            }
            break;
        }
    }
    return NULL;
}

/* ── Dynamic plugin boilerplate ─────────────────────────────────────── */

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "mp4demux") == 0) {
        return zst_mp4_demuxer_create();
    }
    return NULL;
}

static const zst_property_spec_t g_mp4demux_properties[] = {
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Input MP4 file path (optional; enables direct-file mode)" },
    { "real-time-pacing", ZST_PROPERTY_BOOL, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "false", "Make stream output at pts timing" }
};

static const zst_pad_template_t g_mp4demux_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/quicktime;video/mp4;ANY" },
    { "video_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "video/x-h264;video/x-h265;video/mpeg4;video/x-vp9;video/x-av1;ANY" },
    { "audio_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "audio/aac;audio/mpeg;audio/opus;audio/vorbis;audio/flac;ANY" },
    { "data_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "ANY" }
};

static const zst_element_desc_t g_mp4demux_elements[] = {
    {
        .name = "mp4demux",
        .long_name = "MP4 Demuxer",
        .category = "Demuxer/File",
        .description = "Demuxes MP4 (.mp4/.mov/.m4a/.m4v) into encoded audio/video with dynamic pads",
        .author = "zstreamer",
        .properties = g_mp4demux_properties,
        .nb_properties = sizeof(g_mp4demux_properties) / sizeof(g_mp4demux_properties[0]),
        .pads = g_mp4demux_pads,
        .nb_pads = sizeof(g_mp4demux_pads) / sizeof(g_mp4demux_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "mp4demuxer_plugin",
        .author = "zstreamer",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

ZST_PLUGIN_EXPORT
const zst_element_desc_t*
zst_get_plugin_elements(uint32_t* nb_elements_out)
{
    if (nb_elements_out) {
        *nb_elements_out = sizeof(g_mp4demux_elements) / sizeof(g_mp4demux_elements[0]);
    }
    return g_mp4demux_elements;
}

ZST_PLUGIN_EXPORT
zst_plugin_t*
zst_get_plugin(void)
{
    zst_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif
