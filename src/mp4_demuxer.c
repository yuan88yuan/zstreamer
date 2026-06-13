/*=============================================================================
    mp4_demuxer.c — FFmpeg libavformat MP4 demuxer implementation

    Supports two modes:
      1) Direct file mode — set the "location" property to an MP4 file path;
         the element opens the file via avformat_open_input() at start().
      2) Push/streaming mode — feed buffers into the sink pad; data is
         buffered until enough is available to probe format and read packets
         (fragmented MP4 / fMP4 friendly via custom AVIO).

    Produces encoded video packets on the "video" src pad and encoded audio
    packets on the "audio" src pad.
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
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
#include "zstreamer/elements/zst_mp4_demuxer.h"

/* ── Internal buffer queue node ──────────────────────────────────────── */

typedef struct zst_buffer_node {
    zst_buffer_t* buf;
    struct zst_buffer_node* next;
} zst_buffer_node_t;

/* ── Private element state ───────────────────────────────────────────── */

typedef struct {
    AVFormatContext* fc;
    uint8_t*         avio_buf;
    size_t           avio_buf_size;
    int              video_stream_idx;
    int              audio_stream_idx;

    zst_pad_t*       video_pad;
    zst_pad_t*       audio_pad;

    /* Push-mode buffer queue */
    zst_buffer_node_t* queue_head;
    zst_buffer_node_t* queue_tail;
    size_t             current_buf_offset;
    pthread_mutex_t    lock;
    int                eos_received;

    /* Direct-file mode */
    char             location[256];
    int              direct_file;
    zst_time_t       base_time;
} mp4_demuxer_t;

/* ── Push-mode buffer queue helpers ──────────────────────────────────── */

static void
mp4_demux_enqueue(mp4_demuxer_t* s, zst_buffer_t* buf)
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
mp4_demux_clear_queue(mp4_demuxer_t* s)
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
mp4_demux_get_queue_bytes(mp4_demuxer_t* s)
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
mp4_demux_advance_queue(mp4_demuxer_t* s, size_t bytes)
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

/* ── AVIO read callback for push mode ────────────────────────────────── */

static int
mp4_demux_read_packet(void* opaque, uint8_t* buf, int buf_size)
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

/* ── Detect media types from codec IDs ───────────────────────────────── */

static const char*
mp4_demux_video_media_type(enum AVCodecID id)
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

static const char*
mp4_demux_audio_media_type(enum AVCodecID id)
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

/* ── Set caps on output pads from discovered streams ─────────────────── */

static void
mp4_demux_set_stream_caps(mp4_demuxer_t* s)
{
    if (!s->fc) return;

    for (unsigned int i = 0; i < s->fc->nb_streams; i++) {
        AVStream* st = s->fc->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && s->video_stream_idx < 0) {
            s->video_stream_idx = (int)i;
            if (s->video_pad) {
                const char* mt = mp4_demux_video_media_type(st->codecpar->codec_id);
                zst_caps_t* caps = zst_caps_create();
                zst_caps_append(caps, zst_caps_struct_create_video(
                    mt, st->codecpar->width, st->codecpar->height, 0, ""));
                zst_pad_set_caps(s->video_pad, caps);
                zst_caps_destroy(caps);
            }
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && s->audio_stream_idx < 0) {
            s->audio_stream_idx = (int)i;
            if (s->audio_pad) {
                int channels = 0;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
                channels = st->codecpar->ch_layout.nb_channels;
#else
                channels = st->codecpar->channels;
#endif
                const char* mt = mp4_demux_audio_media_type(st->codecpar->codec_id);
                zst_caps_t* caps = zst_caps_create();
                zst_caps_append(caps, zst_caps_struct_create_audio(
                    mt, st->codecpar->sample_rate, channels, ""));
                zst_pad_set_caps(s->audio_pad, caps);
                zst_caps_destroy(caps);
            }
        }
    }
}

/* ── Push-mode: probe and open format from buffered data ─────────────── */

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} temp_read_ctx_t;

static int
temp_read(void* opaque, uint8_t* buf, int buf_size)
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

static int
mp4_demux_streams_ready(AVFormatContext* fc)
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
mp4_demux_close_temp_context(AVFormatContext* fc)
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
mp4_demux_try_init(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    if (s->fc) return ZST_OK;

    size_t q_bytes = mp4_demux_get_queue_bytes(s);
    if (q_bytes < 4096 && !s->eos_received) {
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

    size_t avio_buf_size = 8192;
    uint8_t* avio_buf = av_malloc(avio_buf_size);
    AVIOContext* pb = avio_alloc_context(
        avio_buf, (int)avio_buf_size,
        0, &read_ctx, temp_read, NULL, NULL
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
    temp_fc->probesize = 5000000;
    temp_fc->max_analyze_duration = 5000000;

    const AVInputFormat* mp4_fmt = av_find_input_format("mov,mp4,m4a,3gp,3g2,mj2");
    if (avformat_open_input(&temp_fc, NULL, mp4_fmt, NULL) < 0) {
        if (temp_fc) {
            avformat_free_context(temp_fc);
        }
        av_freep(&pb->buffer);
        avio_context_free(&pb);
        free(flat);
        return ZST_OK; /* Will retry when more data arrives */
    }

    int ret = avformat_find_stream_info(temp_fc, NULL);
    if (ret < 0 && !s->eos_received) {
        mp4_demux_close_temp_context(temp_fc);
        free(flat);
        return ZST_OK; /* Will retry when more data arrives */
    }

    /* If we haven't received EOS, ensure all streams are fully detected before completing init */
    if (!mp4_demux_streams_ready(temp_fc) && !s->eos_received) {
        mp4_demux_close_temp_context(temp_fc);
        free(flat);
        return ZST_OK; /* Will retry when more data arrives */
    }

    s->fc = temp_fc;

    size_t consumed = read_ctx.pos;
    mp4_demux_advance_queue(s, consumed);

    /* Replace the temporary PB context with the persistent queue-based PB */
    av_freep(&s->fc->pb->buffer);
    avio_context_free(&s->fc->pb);

    s->avio_buf_size = 8192;
    s->avio_buf = av_malloc(s->avio_buf_size);
    s->fc->pb = avio_alloc_context(
        s->avio_buf, (int)s->avio_buf_size,
        0, s, mp4_demux_read_packet, NULL, NULL
    );
    s->fc->pb->seekable = 0;
    s->fc->flags |= AVFMT_FLAG_CUSTOM_IO;

    /* Discover pads and set caps */
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->video_pad = zst_element_get_pad(el, "video");
    s->audio_pad = zst_element_get_pad(el, "audio");

    mp4_demux_set_stream_caps(s);

    free(flat);
    return ZST_OK;
}

/* ── Send EOS downstream ────────────────────────────────────────────── */

static void
mp4_demux_send_eos(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    if (s->video_pad && s->video_pad->peer) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(s->video_pad, eos_buf);
            zst_buffer_unref(eos_buf);
        }
    }
    if (s->audio_pad && s->audio_pad->peer) {
        zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
        if (eos_buf) {
            eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
            zst_pad_push(s->audio_pad, eos_buf);
            zst_buffer_unref(eos_buf);
        }
    }
    if (el->bus) {
        zst_bus_post(el->bus, zst_event_new_eos(el));
    }
}

static uint8_t*
h264_avcc_to_annexb(const uint8_t* avcc_data, int avcc_size, const uint8_t* extradata, int extra_size, int* out_size, int is_keyframe)
{
    /* Calculate size of Annex B buffer */
    int alloc_size = avcc_size + 1024;
    uint8_t* out = malloc(alloc_size);
    if (!out) return NULL;

    int pos = 0;

    /* 1. Prepend SPS/PPS from extradata if it's a keyframe and extradata is present */
    if (is_keyframe && extradata && extra_size >= 7 && extradata[0] == 1) {
        int num_sps = extradata[5] & 0x1F;
        int p = 6;
        for (int i = 0; i < num_sps; i++) {
            if (p + 2 > extra_size) break;
            int sps_len = (extradata[p] << 8) | extradata[p+1];
            p += 2;
            if (p + sps_len > extra_size) break;
            
            /* Write Annex B start code + SPS */
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
                
                /* Write Annex B start code + PPS */
                out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = 1;
                memcpy(out + pos, extradata + p, pps_len);
                pos += pps_len;
                p += pps_len;
            }
        }
    }

    /* 2. Convert AVCC packets to Annex B */
    int i = 0;
    while (i + 4 <= avcc_size) {
        uint32_t nal_len = (avcc_data[i] << 24) | (avcc_data[i+1] << 16) | (avcc_data[i+2] << 8) | avcc_data[i+3];
        i += 4;
        if (i + nal_len > (uint32_t)avcc_size) {
            break;
        }

        /* Realloc if needed */
        if (pos + 4 + nal_len > alloc_size) {
            alloc_size = pos + 4 + nal_len + 1024;
            uint8_t* new_out = realloc(out, alloc_size);
            if (!new_out) {
                free(out);
                return NULL;
            }
            out = new_out;
        }

        /* Write start code */
        out[pos++] = 0; out[pos++] = 0; out[pos++] = 0; out[pos++] = 1;
        /* Copy NAL unit payload */
        memcpy(out + pos, avcc_data + i, nal_len);
        pos += nal_len;
        i += nal_len;
    }

    *out_size = pos;
    return out;
}

/* ── Process: read packets and push to output pads ───────────────────── */

static zst_result_t
mp4_demux_process(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    if (!s->fc) return ZST_ERROR;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;

    zst_result_t process_result = ZST_OK;

    while (1) {
        int ret = av_read_frame(s->fc, pkt);
        if (ret == AVERROR(EAGAIN)) {
            ZST_LOG_DEBUG("mp4demux", "av_read_frame returned EAGAIN");
            process_result = ZST_AGAIN;
            break;
        }
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            ZST_LOG_INFO("mp4demux", "av_read_frame returned EOF or error: %d (%s)", ret, errbuf);
            if (ret == AVERROR_EOF || s->eos_received) {
                mp4_demux_send_eos(el);
                process_result = ZST_EOF;
            } else {
                process_result = ZST_ERROR;
            }
            break;
        }

        zst_pad_t* dest_pad = NULL;
        int btype = ZST_BUFFER_USER;
        if (pkt->stream_index == s->video_stream_idx) {
            dest_pad = s->video_pad;
            btype = ZST_BUFFER_VIDEO_PACKET;
        } else if (pkt->stream_index == s->audio_stream_idx) {
            dest_pad = s->audio_pad;
            btype = ZST_BUFFER_AUDIO_PACKET;
        }

        if (dest_pad && dest_pad->peer) {
            zst_buffer_t* out = zst_buffer_create(btype);
            if (out) {
                if (btype == ZST_BUFFER_VIDEO_PACKET && s->video_stream_idx != -1) {
                    /* Convert H.264 AVCC to Annex B */
                    int annexb_size = 0;
                    AVCodecParameters* codecpar = s->fc->streams[s->video_stream_idx]->codecpar;
                    int is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY);
                    uint8_t* annexb_data = h264_avcc_to_annexb(pkt->data, pkt->size,
                                                              codecpar->extradata, codecpar->extradata_size,
                                                              &annexb_size, is_keyframe);
                    if (annexb_data) {
                        ZST_LOG_DEBUG("mp4demux", "AVCC->AnnexB: %d bytes -> %d bytes (keyframe=%d)",
                                     pkt->size, annexb_size, is_keyframe);
                        out->memory.data = annexb_data;
                        out->memory.size = annexb_size;
                        out->memory.priv = annexb_data;
                        out->memory.release = free;
                    } else {
                        ZST_LOG_ERROR("mp4demux", "Annex B conversion failed, falling back to raw AVCC copy");
                        /* Fallback to original packet if conversion failed */
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

                if (s->direct_file && el->clock) {
                    if (s->base_time == 0) {
                        s->base_time = zst_clock_get_time(el->clock);
                    }
                    out->pts = s->base_time + file_pts;
                    out->dts = s->base_time + file_dts;
                } else {
                    out->pts = file_pts;
                    out->dts = file_dts;
                }

                /* Clock sync if enabled in direct-file mode */
                if (s->direct_file && el->pipeline && el->pipeline->clock_sync && el->clock && out->dts > 0) {
                    zst_time_t current = zst_clock_get_time(el->clock);
                    if (out->dts > current + 5000000ULL) {
                        zst_clock_wait(el->clock, out->dts - current);
                    }
                }

                ZST_LOG_DEBUG("mp4demux", "Pushing packet on stream %d, size %d, pts %lld", pkt->stream_index, pkt->size, (long long)out->pts);
                zst_pad_push(dest_pad, out);
                zst_buffer_unref(out);
            }
        } else {
            ZST_LOG_DEBUG("mp4demux", "Pushed packet skipped: pad %s has no peer", dest_pad ? dest_pad->name : "NULL");
        }

        av_packet_unref(pkt);

        /* In direct-file mode, process exactly one packet per call to play nice with the scheduler */
        if (s->direct_file) {
            process_result = ZST_OK;
            break;
        }
    }

    av_packet_free(&pkt);
    return process_result;
}

/* ── Sink pad push callback (push / streaming mode) ──────────────────── */

static zst_result_t
mp4_demux_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mp4_demuxer_t* s = el->priv;

    mp4_demux_enqueue(s, buf);

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->eos_received = 1;
    }

    if (!s->fc) {
        zst_result_t res = mp4_demux_try_init(el);
        if (res != ZST_OK) return res;
    }

    if (s->fc) {
        return mp4_demux_process(el);
    } else if (s->eos_received) {
        mp4_demux_send_eos(el);
    }

    return ZST_OK;
}

/* ── Element lifecycle: open / close / start / stop ──────────────────── */

static zst_result_t
mp4_demux_open(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    pthread_mutex_init(&s->lock, NULL);
    return ZST_OK;
}

static zst_result_t
mp4_demux_close(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    pthread_mutex_destroy(&s->lock);
    return ZST_OK;
}

static zst_result_t
mp4_demux_start(zst_element_t* el)
{
    mp4_demuxer_t* s = el->priv;
    s->fc = NULL;
    s->queue_head = NULL;
    s->queue_tail = NULL;
    s->current_buf_offset = 0;
    s->eos_received = 0;
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->base_time = 0;

    /* Cache pad pointers early */
    s->video_pad = zst_element_get_pad(el, "video");
    s->audio_pad = zst_element_get_pad(el, "audio");

    /* Direct-file mode: open the file immediately at start */
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
            char errbuf[256];
            av_strerror(err, errbuf, sizeof(errbuf));
            ZST_LOG_ERROR("mp4demux", "Failed to find stream info for '%s': %d (%s)", s->location, err, errbuf);
            avformat_close_input(&fc);
            return ZST_ERROR;
        }
        s->fc = fc;
        ZST_LOG_INFO("mp4demux", "Successfully opened direct file location '%s'", s->location);
        mp4_demux_set_stream_caps(s);
    }

    return ZST_OK;
}

static zst_result_t
mp4_demux_stop(zst_element_t* el)
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
    mp4_demux_clear_queue(s);
    s->eos_received = 0;
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    return ZST_OK;
}

/* ── Process callback for pull/scheduler mode (direct file) ──────────── */

static zst_result_t
mp4_demux_process_op(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    (void)out;

    mp4_demuxer_t* s = el->priv;
    if (!s->fc || !s->direct_file) return ZST_OK;

    return mp4_demux_process(el);
}

/* ── Properties ──────────────────────────────────────────────────────── */

static zst_result_t
mp4_demux_set_property(zst_element_t* el, const char* name, const char* value)
{
    mp4_demuxer_t* s = el->priv;
    if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(s->location, sizeof(s->location), "%s", value);
        s->direct_file = s->location[0] != '\0';
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
mp4_demux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    mp4_demuxer_t* s = el->priv;
    if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(value_out, max_len, "%s", s->location);
        return ZST_OK;
    }
    return ZST_ERROR;
}

/* ── Ops vtable ──────────────────────────────────────────────────────── */

static zst_element_ops_t g_ops = {
    .name  = "mp4demux",
    .open  = mp4_demux_open,
    .close = mp4_demux_close,
    .start = mp4_demux_start,
    .stop  = mp4_demux_stop,
    .process = mp4_demux_process_op,
    .set_property = mp4_demux_set_property,
    .get_property = mp4_demux_get_property,
};

/* ── Public constructors ─────────────────────────────────────────────── */

zst_element_t*
zst_mp4_demuxer_create(void)
{
    zst_element_t* el;
    mp4_demuxer_t* priv;
    zst_pad_t* sink;
    zst_pad_t* video;
    zst_pad_t* audio;

    priv = calloc(1, sizeof(*priv));

    el = zst_element_create(&g_ops, priv);

    sink  = zst_pad_create("sink",  ZST_PAD_SINK);
    video = zst_pad_create("video", ZST_PAD_SRC);
    audio = zst_pad_create("audio", ZST_PAD_SRC);

    sink->push = mp4_demux_sink_push;

    zst_element_add_pad(el, sink);
    zst_element_add_pad(el, video);
    zst_element_add_pad(el, audio);

    return el;
}

zst_element_t*
zst_mp4_demuxer_create_with_config(const zst_mp4_demuxer_config_t* config)
{
    if (!config || config->struct_size < sizeof(zst_mp4_demuxer_config_t)) return NULL;
    zst_element_t* el = zst_element_factory_make("mp4demux");
    if (!el) return NULL;

    if (config->location) {
        zst_element_set_property_string(el, "location", config->location);
    }

    return el;
}

/* ── Dynamic plugin boilerplate ──────────────────────────────────────── */

/* ── Public extradata accessor ───────────────────────────────────────── */

const uint8_t*
zst_mp4_demuxer_get_video_extradata(zst_element_t* el, int* size_out)
{
    if (size_out) *size_out = 0;
    if (!el) return NULL;
    mp4_demuxer_t* s = el->priv;
    if (!s || !s->fc || s->video_stream_idx < 0) return NULL;
    AVCodecParameters* par = s->fc->streams[s->video_stream_idx]->codecpar;
    if (!par->extradata || par->extradata_size <= 0) return NULL;
    if (size_out) *size_out = par->extradata_size;
    return par->extradata;
}


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
    { "location", ZST_PROPERTY_STRING, ZST_PROPERTY_READABLE | ZST_PROPERTY_WRITABLE, "", "Input MP4 file path (optional; enables direct-file mode)" }
};

static const zst_pad_template_t g_mp4demux_pads[] = {
    { "sink", ZST_PAD_SINK, "video/quicktime" },
    { "video", ZST_PAD_SRC, "video/x-h264" },
    { "audio", ZST_PAD_SRC, "audio/x-aac" }
};

static const zst_element_desc_t g_mp4demux_elements[] = {
    {
        .name = "mp4demux",
        .long_name = "MP4 Demuxer",
        .category = "Demuxer/File",
        .description = "Demuxes MP4 (.mp4/.mov/.m4a/.m4v) into encoded audio/video",
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
