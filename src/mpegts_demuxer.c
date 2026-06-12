/*=============================================================================
    mpegts_demuxer.c — FFmpeg libavformat MPEG-TS demuxer implementation
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
#include "zstreamer/elements/zst_mpegts_demuxer.h"

typedef struct zst_buffer_node {
    zst_buffer_t* buf;
    struct zst_buffer_node* next;
} zst_buffer_node_t;

typedef struct {
    AVFormatContext* fc;
    uint8_t*         avio_buf;
    size_t           avio_buf_size;
    int              video_stream_idx;
    int              audio_stream_idx;
    
    zst_pad_t*       video_pad;
    zst_pad_t*       audio_pad;

    // Buffer queue
    zst_buffer_node_t* queue_head;
    zst_buffer_node_t* queue_tail;
    size_t             current_buf_offset;
    pthread_mutex_t    lock;
    int                eos_received;
    
    char             location[256];
    int              direct_file;
} mpegts_demuxer_t;

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

static void
mpegts_demux_enqueue(mpegts_demuxer_t* s, zst_buffer_t* buf)
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
mpegts_demux_clear_queue(mpegts_demuxer_t* s)
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
mpegts_demux_get_queue_bytes(mpegts_demuxer_t* s)
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
mpegts_demux_advance_queue(mpegts_demuxer_t* s, size_t bytes)
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

static int
mpegts_demux_read_packet(void* opaque, uint8_t* buf, int buf_size)
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

static zst_result_t
mpegts_demux_try_init(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    if (s->fc) return ZST_OK;
    
    size_t q_bytes = mpegts_demux_get_queue_bytes(s);
    if (q_bytes < 4096 && !s->eos_received) {
        return ZST_OK; // Wait for more data
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
    
    if (avformat_open_input(&temp_fc, NULL, NULL, NULL) < 0) {
        avio_context_free(&temp_fc->pb);
        avformat_free_context(temp_fc);
        free(flat);
        return ZST_OK; // Will retry when more data arrives
    }
    
    if (avformat_find_stream_info(temp_fc, NULL) < 0) {
        avformat_close_input(&temp_fc);
        free(flat);
        return ZST_OK;
    }
    
    s->fc = temp_fc;
    
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    s->video_pad = zst_element_get_pad(el, "video");
    s->audio_pad = zst_element_get_pad(el, "audio");
    
    for (unsigned int i = 0; i < s->fc->nb_streams; i++) {
        AVStream* st = s->fc->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && s->video_stream_idx < 0) {
            s->video_stream_idx = (int)i;
            if (s->video_pad) {
                const char* mt = "video/x-h264";
                if (st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
                    mt = "video/x-h265";
                }
                zst_caps_t* caps = zst_caps_create();
                zst_caps_append(caps, zst_caps_struct_create_video(mt, st->codecpar->width, st->codecpar->height, 0, ""));
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
                zst_caps_t* caps = zst_caps_create();
                zst_caps_append(caps, zst_caps_struct_create_audio("audio/aac", st->codecpar->sample_rate, channels, ""));
                zst_pad_set_caps(s->audio_pad, caps);
                zst_caps_destroy(caps);
            }
        }
    }
    
    size_t consumed = read_ctx.pos;
    mpegts_demux_advance_queue(s, consumed);
    
    avio_context_free(&s->fc->pb);
    s->avio_buf_size = 4096;
    s->avio_buf = av_malloc(s->avio_buf_size);
    s->fc->pb = avio_alloc_context(
        s->avio_buf, (int)s->avio_buf_size,
        0, s, mpegts_demux_read_packet, NULL, NULL
    );
    s->fc->pb->seekable = 0;
    s->fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    
    free(flat);
    return ZST_OK;
}

static void
mpegts_demux_send_eos(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
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

static zst_result_t
mpegts_demux_process(zst_element_t* el)
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
                mpegts_demux_send_eos(el);
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
                out->memory.data = malloc(pkt->size);
                out->memory.size = pkt->size;
                out->memory.priv = out->memory.data;
                out->memory.release = free;
                memcpy(out->memory.data, pkt->data, pkt->size);
                
                AVRational tb = s->fc->streams[pkt->stream_index]->time_base;
                out->pts = av_rescale_q(pkt->pts, tb, (AVRational){1, 1000000000});
                out->dts = av_rescale_q(pkt->dts, tb, (AVRational){1, 1000000000});
                out->duration = av_rescale_q(pkt->duration, tb, (AVRational){1, 1000000000});
                
                zst_pad_push(dest_pad, out);
                zst_buffer_unref(out);
            }
        }
        
        av_packet_unref(pkt);
    }
    
    av_packet_free(&pkt);
    return ZST_OK;
}

static zst_result_t
mpegts_demux_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mpegts_demuxer_t* s = el->priv;
    
    mpegts_demux_enqueue(s, buf);
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->eos_received = 1;
    }
    
    if (!s->fc) {
        zst_result_t res = mpegts_demux_try_init(el);
        if (res != ZST_OK) return res;
    }
    
    if (s->fc) {
        return mpegts_demux_process(el);
    } else if (s->eos_received) {
        mpegts_demux_send_eos(el);
    }
    
    return ZST_OK;
}

static zst_result_t
mpegts_demux_start(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    s->fc = NULL;
    s->queue_head = NULL;
    s->queue_tail = NULL;
    s->current_buf_offset = 0;
    s->eos_received = 0;
    return ZST_OK;
}

static zst_result_t
mpegts_demux_stop(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    if (s->fc) {
        if (s->fc->pb) {
            av_freep(&s->fc->pb->buffer);
            avio_context_free(&s->fc->pb);
        }
        avformat_close_input(&s->fc);
        s->fc = NULL;
    }
    mpegts_demux_clear_queue(s);
    s->eos_received = 0;
    return ZST_OK;
}

static zst_result_t
mpegts_demux_open(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    pthread_mutex_init(&s->lock, NULL);
    return ZST_OK;
}

static zst_result_t
mpegts_demux_close(zst_element_t* el)
{
    mpegts_demuxer_t* s = el->priv;
    pthread_mutex_destroy(&s->lock);
    return ZST_OK;
}

static zst_result_t
mpegts_demux_set_property(zst_element_t* el, const char* name, const char* value)
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
mpegts_demux_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len)
{
    mpegts_demuxer_t* s = el->priv;
    if (strcmp(name, "location") == 0 || strcmp(name, "path") == 0) {
        snprintf(value_out, max_len, "%s", s->location);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_element_ops_t g_ops = {
    .name  = "tsdemux",
    .open  = mpegts_demux_open,
    .close = mpegts_demux_close,
    .start = mpegts_demux_start,
    .stop  = mpegts_demux_stop,
    .set_property = mpegts_demux_set_property,
    .get_property = mpegts_demux_get_property,
};

zst_element_t*
zst_mpegts_demuxer_create(void)
{
    zst_element_t* el;
    mpegts_demuxer_t* priv;
    zst_pad_t* sink;
    zst_pad_t* video;
    zst_pad_t* audio;

    priv = calloc(1, sizeof(*priv));

    el = zst_element_create(&g_ops, priv);

    sink  = zst_pad_create("sink",  ZST_PAD_SINK);
    video = zst_pad_create("video", ZST_PAD_SRC);
    audio = zst_pad_create("audio", ZST_PAD_SRC);

    sink->push = mpegts_demux_sink_push;

    zst_element_add_pad(el, sink);
    zst_element_add_pad(el, video);
    zst_element_add_pad(el, audio);

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
    { "sink", ZST_PAD_SINK, "video/mpegts" },
    { "video", ZST_PAD_SRC, "video/x-h264" },
    { "audio", ZST_PAD_SRC, "audio/x-aac" }
};

static const zst_element_desc_t g_tsdemux_elements[] = {
    {
        .name = "tsdemux",
        .long_name = "MPEG-TS Demuxer",
        .category = "Demuxer",
        .description = "Demuxes MPEG-TS (.ts) into encoded audio/video",
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
