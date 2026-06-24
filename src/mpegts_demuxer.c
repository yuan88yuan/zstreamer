/*=============================================================================
    mpegts_demuxer.c — FFmpeg libavformat MPEG-TS demuxer with dynamic pads

    Refactored for Phase F of the Adaptive Stream Demuxing Plan:
      - Dynamic source pads (video_%u, audio_%u) created at stream discovery
      - Stable stream table with IDs, bus events on add/remove/change
      - Sticky in-band pad events (STREAM_START, CAPS) on new pads
      - Unlinked-pad policy support (default DROP for live TS)
      - Stream query API (get_stream_count, get_stream_info, get_stream_pad)
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
#include "zst_stream.h"
#include "zst_pad_event.h"
#include "zstreamer/elements/zst_mpegts_demuxer.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define TSDEMUX_MAX_STREAMS 16
#define TSDEMUX_INIT_PROBE_BYTES 4096

/* ── Buffer queue node (push-mode byte accumulation) ───────────────── */

typedef struct zst_buffer_node {
    zst_buffer_t* buf;
    struct zst_buffer_node* next;
} zst_buffer_node_t;

/* ── Per-stream descriptor ─────────────────────────────────────────── */

typedef struct {
    zst_stream_id_t    id;
    int                av_stream_idx;    /* index in AVFormatContext->streams[] */
    int                av_stream_idx_valid;

    zst_media_kind_t   kind;            /* ZST_MEDIA_VIDEO or ZST_MEDIA_AUDIO */
    zst_stream_status_t status;         /* PRESENT / LOST / REMOVED */

    zst_caps_t*        caps;            /* negotiated caps for this stream */
    zst_pad_t*         pad;             /* the dynamic source pad */

    enum AVCodecID     codec_id;
    uint32_t           generation;      /* bumped on each caps change */
    uint64_t           last_seen_pts_ns;
} tsdemux_stream_t;

/* ── Element private data ──────────────────────────────────────────── */

typedef struct {
    AVFormatContext*   fc;
    uint8_t*           avio_buf;
    size_t             avio_buf_size;

    /* Stream table */
    tsdemux_stream_t   streams[TSDEMUX_MAX_STREAMS];
    uint32_t           nb_streams;
    zst_stream_id_t    next_stream_id;   /* monotonically increasing */

    /* Push-mode buffer queue */
    zst_buffer_node_t* queue_head;
    zst_buffer_node_t* queue_tail;
    size_t             current_buf_offset;
    pthread_mutex_t    lock;
    int                eos_received;
    int                eos_sent;

    /* Direct-file mode */
    char               location[256];
    int                direct_file;
    int                file_opened;

    /* Signal tracking */
    int                signal_present;
    int64_t            signal_lost_at_us;
} mpegts_demuxer_t;

/* ── Temporary read context for initial probe ──────────────────────── */

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} temp_read_ctx_t;

/* ── Forward declarations ──────────────────────────────────────────── */

static zst_result_t tsdemux_create_stream_pads(zst_element_t* el);
static void         tsdemux_update_stream_status(zst_element_t* el, zst_stream_status_t status);
static uint32_t     tsdemux_get_stream_count(zst_element_t* el);
static zst_result_t tsdemux_get_stream_info(zst_element_t* el, uint32_t index, zst_stream_info_t* info_out);
static zst_pad_t*   tsdemux_get_stream_pad(zst_element_t* el, zst_stream_id_t stream_id);

/* =====================================================================
    Push-mode buffer queue helpers
   ===================================================================== */

static void
tsdemux_enqueue(mpegts_demuxer_t* s, zst_buffer_t* buf)
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
tsdemux_clear_queue(mpegts_demuxer_t* s)
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
tsdemux_get_queue_bytes(mpegts_demuxer_t* s)
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
tsdemux_advance_queue(mpegts_demuxer_t* s, size_t bytes)
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
tsdemux_read_packet(void* opaque, uint8_t* buf, int buf_size)
{
    mpegts_demuxer_t* s = opaque;
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
tsdemux_temp_read(void* opaque, uint8_t* buf, int buf_size)
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

static tsdemux_stream_t*
tsdemux_find_stream_by_av_idx(mpegts_demuxer_t* s, int av_idx)
{
    for (uint32_t i = 0; i < s->nb_streams; i++) {
        if (s->streams[i].av_stream_idx == av_idx && s->streams[i].av_stream_idx_valid)
            return &s->streams[i];
    }
    return NULL;
}

static tsdemux_stream_t*
tsdemux_find_stream_by_id(mpegts_demuxer_t* s, zst_stream_id_t id)
{
    for (uint32_t i = 0; i < s->nb_streams; i++) {
        if (s->streams[i].id == id)
            return &s->streams[i];
    }
    return NULL;
}

static zst_stream_id_t
tsdemux_next_id(mpegts_demuxer_t* s)
{
    return s->next_stream_id++;
}

/* ── Build caps from AVCodecParameters ─────────────────────────────── */

static zst_caps_t*
tsdemux_build_video_caps(AVCodecParameters* par)
{
    const char* mt = "video/x-h264";
    switch (par->codec_id) {
    case AV_CODEC_ID_HEVC:  mt = "video/x-h265"; break;
    case AV_CODEC_ID_MPEG2VIDEO: mt = "video/mpeg2"; break;
    case AV_CODEC_ID_VP9:   mt = "video/x-vp9"; break;
    case AV_CODEC_ID_AV1:   mt = "video/x-av1"; break;
    default:
        if (par->codec_id != AV_CODEC_ID_H264 && par->codec_id != AV_CODEC_ID_HEVC)
            mt = "video/x-unknown";
        break;
    }

    zst_caps_t* caps = zst_caps_new_simple(mt);
    if (caps) {
        zst_caps_set_int(caps, "width", par->width);
        zst_caps_set_int(caps, "height", par->height);
        if (par->extradata && par->extradata_size > 0)
            zst_caps_set_buffer(caps, "codec_data", par->extradata, par->extradata_size);
        /* Set stream-format for H.264/5 */
        if (par->codec_id == AV_CODEC_ID_H264)
            zst_caps_set_string(caps, "stream-format", "byte-stream");
        else if (par->codec_id == AV_CODEC_ID_HEVC)
            zst_caps_set_string(caps, "stream-format", "byte-stream");
    }
    return caps;
}

static zst_caps_t*
tsdemux_build_audio_caps(AVCodecParameters* par)
{
    const char* mt = "audio/aac";
    switch (par->codec_id) {
    case AV_CODEC_ID_MP3:   mt = "audio/mpeg"; break;
    case AV_CODEC_ID_AC3:   mt = "audio/ac3"; break;
    default: break;
    }

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

/* ── Push sticky events on a newly linked pad ──────────────────────── */

static void
tsdemux_push_sticky_on_pad(zst_pad_t* pad, zst_stream_id_t stream_id)
{
    if (!pad) return;

    /* Push STREAM_START */
    zst_pad_event_t* ss = zst_pad_event_new_stream_start(stream_id);
    if (ss) {
        zst_pad_push_event(pad, ss);
        zst_pad_event_unref(ss);
    }

    /* Push CAPS */
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

    /* Push default SEGMENT as a sticky event for late links */
    zst_segment_t segment = zst_segment_default();
    zst_pad_event_t* se = zst_pad_event_new_segment(&segment);
    if (se) {
        zst_pad_push_event(pad, se);
        zst_pad_event_unref(se);
    }
}

static uint32_t
tsdemux_kind_pad_index(mpegts_demuxer_t* s, zst_media_kind_t kind)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < s->nb_streams; i++) {
        if (s->streams[i].kind == kind) count++;
    }
    return count;
}

static int
tsdemux_caps_equal(const zst_caps_t* a, const zst_caps_t* b)
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
tsdemux_fill_stream_info(tsdemux_stream_t* ts, uint32_t index, zst_stream_info_t* info)
{
    memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->id = ts->id;
    info->program_id = 0;
    info->index = index;
    info->kind = ts->kind;
    info->status = ts->status;
    info->caps = ts->caps;
    info->first_pts = 0;
    info->last_seen_pts = ts->last_seen_pts_ns;
    info->name = ts->pad ? (char*)ts->pad->name : NULL;
}

static void
tsdemux_update_stream_caps(zst_element_t* el, tsdemux_stream_t* ts, uint32_t index,
                           zst_caps_t* new_caps)
{
    if (!new_caps) return;
    if (tsdemux_caps_equal(ts->caps, new_caps)) {
        zst_caps_destroy(new_caps);
        return;
    }

    zst_caps_t* old_caps = ts->caps ? zst_caps_copy(ts->caps) : NULL;
    if (ts->caps) zst_caps_destroy(ts->caps);
    ts->caps = new_caps;
    ts->generation++;
    ts->status = ZST_STREAM_STATUS_CHANGED;
    if (ts->pad) {
        zst_pad_set_caps(ts->pad, new_caps);
        zst_pad_event_t* ce = zst_pad_event_new_caps(new_caps);
        if (ce) {
            zst_pad_push_event(ts->pad, ce);
            zst_pad_event_unref(ce);
        }
    }

    if (el->bus) {
        if (ts->pad) {
            zst_event_t* caps_ev = zst_event_new_caps_changed(el, ts->pad, old_caps, new_caps);
            if (caps_ev) zst_bus_post(el->bus, caps_ev);
        }
        zst_stream_info_t info;
        tsdemux_fill_stream_info(ts, index, &info);
        zst_event_t* stream_ev = zst_event_new_stream_changed(el, &info);
        if (stream_ev) zst_bus_post(el->bus, stream_ev);
    }
    ts->status = ZST_STREAM_STATUS_PRESENT;
    if (old_caps) zst_caps_destroy(old_caps);
}

static zst_result_t
tsdemux_add_stream_from_av(zst_element_t* el, unsigned int av_index)
{
    mpegts_demuxer_t* s = el->priv;
    if (!s->fc || av_index >= s->fc->nb_streams || s->nb_streams >= TSDEMUX_MAX_STREAMS) return ZST_ERROR;
    if (tsdemux_find_stream_by_av_idx(s, (int)av_index)) return ZST_OK;

    AVStream* st = s->fc->streams[av_index];
    enum AVMediaType codec_type = st->codecpar->codec_type;
    if (codec_type != AVMEDIA_TYPE_VIDEO && codec_type != AVMEDIA_TYPE_AUDIO)
        return ZST_OK;

    zst_caps_t* caps = NULL;
    zst_media_kind_t kind = ZST_MEDIA_UNKNOWN;
    const char* pad_prefix = "data";
    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        caps = tsdemux_build_video_caps(st->codecpar);
        kind = ZST_MEDIA_VIDEO;
        pad_prefix = "video";
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        caps = tsdemux_build_audio_caps(st->codecpar);
        kind = ZST_MEDIA_AUDIO;
        pad_prefix = "audio";
    }
    if (!caps) return ZST_OK;

    zst_stream_id_t sid = tsdemux_next_id(s);
    uint32_t table_index = s->nb_streams;
    tsdemux_stream_t* ts = &s->streams[table_index];
    memset(ts, 0, sizeof(*ts));
    ts->id = sid;
    ts->av_stream_idx = (int)av_index;
    ts->av_stream_idx_valid = 1;
    ts->kind = kind;
    ts->status = ZST_STREAM_STATUS_PRESENT;
    ts->caps = caps;
    ts->codec_id = st->codecpar->codec_id;

    char pad_name[64];
    snprintf(pad_name, sizeof(pad_name), "%s_%u", pad_prefix, tsdemux_kind_pad_index(s, kind));

    zst_pad_t* src_pad = zst_pad_create(pad_name, ZST_PAD_SRC);
    if (!src_pad) {
        zst_caps_destroy(caps);
        memset(ts, 0, sizeof(*ts));
        return ZST_ERROR;
    }
    zst_pad_set_caps(src_pad, caps);
    zst_pad_set_unlinked_policy(src_pad, ZST_PAD_UNLINKED_DROP, 0);
    ts->pad = src_pad;

    zst_stream_info_t info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    info.id = sid;
    info.program_id = 0;
    info.index = table_index;
    info.kind = kind;
    info.status = ZST_STREAM_STATUS_PRESENT;
    info.caps = caps;

    zst_result_t add_ret = zst_element_add_dynamic_pad(el, src_pad, &info);
    if (add_ret != ZST_OK) {
        ZST_LOG_ERROR("tsdemux", "failed to add dynamic pad %s", pad_name);
        zst_pad_destroy(src_pad);
        zst_caps_destroy(caps);
        memset(ts, 0, sizeof(*ts));
        return add_ret;
    }

    tsdemux_push_sticky_on_pad(src_pad, sid);
    s->nb_streams++;

    ZST_LOG_INFO("tsdemux", "discovered stream %s [id=%lu kind=%d]", pad_name, (unsigned long)sid, kind);
    return ZST_OK;
}

static zst_result_t
tsdemux_sync_streams(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    if (!s || !s->fc) return ZST_ERROR;

    uint32_t before = s->nb_streams;
    for (unsigned int i = 0; i < s->fc->nb_streams; i++) {
        AVStream* st = s->fc->streams[i];
        enum AVMediaType codec_type = st->codecpar->codec_type;
        tsdemux_stream_t* existing = tsdemux_find_stream_by_av_idx(s, (int)i);
        if (existing) {
            zst_caps_t* caps = NULL;
            if (codec_type == AVMEDIA_TYPE_VIDEO) caps = tsdemux_build_video_caps(st->codecpar);
            else if (codec_type == AVMEDIA_TYPE_AUDIO) caps = tsdemux_build_audio_caps(st->codecpar);
            uint32_t idx = (uint32_t)(existing - s->streams);
            tsdemux_update_stream_caps(el, existing, idx, caps);
            continue;
        }
        if (s->nb_streams < TSDEMUX_MAX_STREAMS) {
            tsdemux_add_stream_from_av(el, i);
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

/* ── Stream discovery: probe AVFormatContext and create dynamic pads ── */

static zst_result_t
tsdemux_try_init(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    if (s->fc) return ZST_OK;

    size_t q_bytes = tsdemux_get_queue_bytes(s);
    if (q_bytes < TSDEMUX_INIT_PROBE_BYTES && !s->eos_received) {
        return ZST_OK; /* Wait for more data */
    }

    uint8_t* flat = malloc(q_bytes);
    if (!flat) return ZST_ERROR;

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
    if (!temp_fc) {
        free(flat);
        return ZST_ERROR;
    }

    temp_read_ctx_t read_ctx = {
        .data = flat,
        .size = q_bytes,
        .pos = 0
    };

    size_t avio_buf_size = 4096;
    uint8_t* avio_buf = av_malloc(avio_buf_size);
    AVIOContext* pb = avio_alloc_context(
        avio_buf, (int)avio_buf_size,
        0, &read_ctx, tsdemux_temp_read, NULL, NULL
    );
    if (!pb) {
        av_free(avio_buf);
        avformat_free_context(temp_fc);
        free(flat);
        return ZST_ERROR;
    }
    pb->seekable = 0;
    temp_fc->pb = pb;
    temp_fc->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&temp_fc, NULL, NULL, NULL) < 0) {
        av_freep(&temp_fc->pb->buffer);
        avio_context_free(&temp_fc->pb);
        avformat_free_context(temp_fc);
        free(flat);
        return ZST_OK; /* Will retry when more data arrives */
    }

    if (avformat_find_stream_info(temp_fc, NULL) < 0) {
        avformat_close_input(&temp_fc);
        free(flat);
        return ZST_OK;
    }

    /* Replace temp FC with persistent one */
    s->fc = temp_fc;

    size_t consumed = read_ctx.pos;
    tsdemux_advance_queue(s, consumed);

    /* Replace the temporary PB with the persistent queue-based PB */
    av_freep(&s->fc->pb->buffer);
    avio_context_free(&s->fc->pb);

    s->avio_buf_size = 4096;
    s->avio_buf = av_malloc(s->avio_buf_size);
    s->fc->pb = avio_alloc_context(
        s->avio_buf, (int)s->avio_buf_size,
        0, s, tsdemux_read_packet, NULL, NULL
    );
    s->fc->pb->seekable = 0;
    s->fc->flags |= AVFMT_FLAG_CUSTOM_IO;

    /* Build/update stream table from discovered streams. */
    tsdemux_sync_streams(el);

    free(flat);
    return ZST_OK;
}

/* ── Stream cleanup ─────────────────────────────────────────────────── */

static void
tsdemux_reset_streams(zst_element_t* el)
{
    if (!el) return;
    mpegts_demuxer_t* s = el->priv;
    if (!s) return;

    for (int i = (int)s->nb_streams - 1; i >= 0; i--) {
        tsdemux_stream_t* ts = &s->streams[i];
        if (ts->pad) {
            zst_element_remove_dynamic_pad(el, ts->pad);
            ts->pad = NULL;
        }
        if (ts->caps) {
            zst_caps_destroy(ts->caps);
            ts->caps = NULL;
        }
    }
    memset(s->streams, 0, sizeof(s->streams));
    s->nb_streams = 0;
    s->signal_present = 0;
}

/* ── Send EOS on all active pads ───────────────────────────────────── */

static void
tsdemux_send_eos(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    if (s->eos_sent) return;
    s->eos_sent = 1;

    for (uint32_t i = 0; i < s->nb_streams; i++) {
        tsdemux_stream_t* ts = &s->streams[i];
        if (ts->pad && ts->pad->peer) {
            zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
            if (eos_buf) {
                eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_push(ts->pad, eos_buf);
                zst_buffer_unref(eos_buf);
            }
        }
    }
    if (el->bus) {
        zst_bus_post(el->bus, zst_event_new_eos(el));
    }
}

/* ── Process: read packets from FFmpeg and route to pads ───────────── */

static zst_result_t
tsdemux_process(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    if (!s->fc) return ZST_ERROR;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    while (1) {
        int ret = av_read_frame(s->fc, pkt);
        if (ret == AVERROR(EAGAIN)) {
            break;
        }
        if (ret < 0) {
            if (ret == AVERROR_EOF || s->eos_received) {
                tsdemux_send_eos(el);
            } else {
                if (s->signal_present && el->bus) {
                    zst_event_t* lost = zst_event_new_signal_lost(el);
                    if (lost) zst_bus_post(el->bus, lost);
                }
                s->signal_present = 0;
            }
            break;
        }

        /* MPEG-TS PMT changes may cause FFmpeg to discover late streams. */
        tsdemux_sync_streams(el);

        /* Find matching stream */
        tsdemux_stream_t* ts = NULL;
        for (uint32_t i = 0; i < s->nb_streams; i++) {
            if (s->streams[i].av_stream_idx_valid &&
                s->streams[i].av_stream_idx == pkt->stream_index &&
                s->streams[i].status == ZST_STREAM_STATUS_PRESENT) {
                ts = &s->streams[i];
                break;
            }
        }

        if (ts && ts->pad) {
            /* Don't push if pad has no peer and policy is DROP */
            if (!ts->pad->peer) {
                if (ts->pad->unlinked_policy == ZST_PAD_UNLINKED_DROP) {
                    av_packet_unref(pkt);
                    continue;
                }
            }

            int btype = ZST_BUFFER_USER;
            if (ts->kind == ZST_MEDIA_VIDEO)
                btype = ZST_BUFFER_VIDEO_PACKET;
            else if (ts->kind == ZST_MEDIA_AUDIO)
                btype = ZST_BUFFER_AUDIO_PACKET;

            zst_buffer_t* out = zst_buffer_create(btype);
            if (out) {
                out->memory.data = malloc(pkt->size);
                out->memory.size = pkt->size;
                out->memory.priv = out->memory.data;
                out->memory.release = free;
                memcpy(out->memory.data, pkt->data, pkt->size);

                AVRational tb = s->fc->streams[pkt->stream_index]->time_base;
                out->pts = av_rescale_q(pkt->pts, tb, (AVRational){1, 1000000000});
                out->dts = av_rescale_q(pkt->dts, tb, (AVRational){1, 1000000000});
                out->duration = av_rescale_q(pkt->duration, tb, (AVRational){1, 1000000000});

                ts->last_seen_pts_ns = out->pts;

                zst_pad_push(ts->pad, out);
                zst_buffer_unref(out);
            }
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return ZST_OK;
}

/* ── Sink pad push callback ─────────────────────────────────────────── */

static zst_result_t
tsdemux_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mpegts_demuxer_t* s = el->priv;

    tsdemux_enqueue(s, buf);

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->eos_received = 1;
    }

    if (!s->fc) {
        zst_result_t res = tsdemux_try_init(el);
        if (res != ZST_OK) return res;
    }

    if (s->fc) {
        return tsdemux_process(el);
    } else if (s->eos_received) {
        tsdemux_send_eos(el);
    }

    return ZST_OK;
}

/* ── Direct-file mode: open via location ────────────────────────────── */

static zst_result_t
tsdemux_open_file(mpegts_demuxer_t* s, zst_element_t* el)
{
    AVFormatContext* fc = NULL;
    int err = avformat_open_input(&fc, s->location, NULL, NULL);
    if (err < 0) {
        char errbuf[256];
        av_strerror(err, errbuf, sizeof(errbuf));
        ZST_LOG_ERROR("tsdemux", "Failed to open file '%s': %d (%s)", s->location, err, errbuf);
        return ZST_ERROR;
    }

    err = avformat_find_stream_info(fc, NULL);
    if (err < 0) {
        avformat_close_input(&fc);
        ZST_LOG_ERROR("tsdemux", "Failed to find stream info for '%s'", s->location);
        return ZST_ERROR;
    }

    s->fc = fc;
    s->file_opened = 1;

    /* Build/update stream table from discovered streams. */
    tsdemux_sync_streams(el);

    ZST_LOG_INFO("tsdemux", "opened file '%s' with %u streams", s->location, s->nb_streams);
    return ZST_OK;
}

/* ── Element lifecycle: open / close / start / stop ────────────────── */

static zst_result_t
tsdemux_open(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    pthread_mutex_init(&s->lock, NULL);
    return ZST_OK;
}

static zst_result_t
tsdemux_close(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    pthread_mutex_destroy(&s->lock);
    return ZST_OK;
}

static zst_result_t
tsdemux_start(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    s->fc = NULL;
    s->queue_head = NULL;
    s->queue_tail = NULL;
    s->current_buf_offset = 0;
    s->eos_received = 0;
    s->eos_sent = 0;
    s->file_opened = 0;
    s->signal_present = 0;
    s->signal_lost_at_us = 0;
    tsdemux_reset_streams(el);
    s->next_stream_id = 1;

    if (s->direct_file && s->location[0] != '\0') {
        return tsdemux_open_file(s, el);
    }

    return ZST_OK;
}

static zst_result_t
tsdemux_stop(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    if (s->fc) {
        if (!s->direct_file && s->fc->pb) {
            av_freep(&s->fc->pb->buffer);
            avio_context_free(&s->fc->pb);
        }
        avformat_close_input(&s->fc);
        s->fc = NULL;
    }
    tsdemux_clear_queue(s);
    tsdemux_reset_streams(el);
    s->eos_received = 0;
    s->eos_sent = 0;
    s->signal_present = 0;
    return ZST_OK;
}

/* ── Process callback for pull/scheduler mode (direct file) ────────── */

static zst_result_t
tsdemux_process_op(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    (void)out;
    mpegts_demuxer_t* s = el->priv;
    if (s->direct_file && s->fc && s->file_opened) {
        return tsdemux_process(el);
    }
    return ZST_OK;
}

/* ── Stream query callbacks ─────────────────────────────────────────── */

static uint32_t
tsdemux_get_stream_count(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    return s ? s->nb_streams : 0;
}

static zst_result_t
tsdemux_get_stream_info(zst_element_t* el, uint32_t index, zst_stream_info_t* info_out)
{
    mpegts_demuxer_t* s = el->priv;
    if (!s || index >= s->nb_streams || !info_out) return ZST_ERROR;

    tsdemux_stream_t* ts = &s->streams[index];
    memset(info_out, 0, sizeof(*info_out));
    info_out->struct_size = sizeof(*info_out);
    info_out->id = ts->id;
    info_out->program_id = 0;
    info_out->index = index;
    info_out->kind = ts->kind;
    info_out->status = ts->status;
    info_out->caps = ts->caps ? zst_caps_copy(ts->caps) : NULL;
    info_out->first_pts = 0;
    info_out->last_seen_pts = ts->last_seen_pts_ns;

    /* Allocate name */
    if (ts->pad && ts->pad->name) {
        info_out->name = strdup(ts->pad->name);
    }

    return ZST_OK;
}

static zst_pad_t*
tsdemux_get_stream_pad(zst_element_t* el, zst_stream_id_t stream_id)
{
    mpegts_demuxer_t* s = el->priv;
    if (!s) return NULL;
    tsdemux_stream_t* ts = tsdemux_find_stream_by_id(s, stream_id);
    return ts ? ts->pad : NULL;
}

/* ── Properties ─────────────────────────────────────────────────────── */

static zst_result_t
tsdemux_set_property(zst_element_t* el, const char* name, const char* value)
{
    mpegts_demuxer_t* s = el->priv;
    if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(s->location, sizeof(s->location), "%s", value);
        s->direct_file = s->location[0] != '\0';
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
tsdemux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    mpegts_demuxer_t* s = el->priv;
    if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(value_out, max_len, "%s", s->location);
        return ZST_OK;
    }
    return ZST_ERROR;
}

/* ── Ops vtable ─────────────────────────────────────────────────────── */

static zst_element_ops_t g_ops = {
    .name  = "tsdemux",
    .open  = tsdemux_open,
    .close = tsdemux_close,
    .start = tsdemux_start,
    .stop  = tsdemux_stop,
    .process = tsdemux_process_op,
    .set_property = tsdemux_set_property,
    .get_property = tsdemux_get_property,
    .get_stream_count = tsdemux_get_stream_count,
    .get_stream_info = tsdemux_get_stream_info,
    .get_stream_pad = tsdemux_get_stream_pad,
};

/* ── Public constructors ────────────────────────────────────────────── */

zst_element_t*
zst_mpegts_demuxer_create(void)
{
    mpegts_demuxer_t* priv = calloc(1, sizeof(*priv));
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
    sink->push = tsdemux_sink_push;
    zst_element_add_pad(el, sink);

    /* Set template caps on sink pad */
    zst_caps_t* sink_caps = zst_caps_new_simple("video/mpegts");
    if (sink_caps) {
        zst_pad_set_template_caps(sink, sink_caps);
        zst_caps_destroy(sink_caps);
    }

    return el;
}

zst_element_t*
zst_mpegts_demuxer_create_with_config(const zst_mpegts_demuxer_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_mpegts_demuxer_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("tsdemux");
    if (!el) return NULL;

    if (config->location) {
        zst_element_set_property_string(el, "location", config->location);
    }

    return el;
}

/* ── Dynamic plugin boilerplate ─────────────────────────────────────── */

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "tsdemux") == 0) {
        return zst_mpegts_demuxer_create();
    }
    return NULL;
}

static const zst_property_spec_t g_tsdemux_properties[] = {
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Input file path (optional)" }
};

static const zst_pad_template_t g_tsdemux_pads[] = {
    { "sink", ZST_PAD_SINK, ZST_PAD_ALWAYS, "video/mpegts" },
    { "video_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "video/x-h264;video/x-h265;video/mpeg2;ANY" },
    { "audio_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "audio/aac;audio/mpeg;audio/ac3;ANY" },
    { "data_%u", ZST_PAD_SRC, ZST_PAD_SOMETIMES, "ANY" }
};

static const zst_element_desc_t g_tsdemux_elements[] = {
    {
        .name = "tsdemux",
        .long_name = "MPEG-TS Demuxer",
        .category = "Demuxer",
        .description = "Demuxes MPEG-TS (.ts) into encoded audio/video with dynamic pads",
        .author = "zstreamer",
        .properties = g_tsdemux_properties,
        .nb_properties = sizeof(g_tsdemux_properties) / sizeof(g_tsdemux_properties[0]),
        .pads = g_tsdemux_pads,
        .nb_pads = sizeof(g_tsdemux_pads) / sizeof(g_tsdemux_pads[0]),
        .create = NULL
    }
};

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "mpegtsdemuxer_plugin",
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
        *nb_elements_out = sizeof(g_tsdemux_elements) / sizeof(g_tsdemux_elements[0]);
    }
    return g_tsdemux_elements;
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
