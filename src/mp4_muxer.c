/*=============================================================================
    mp4_muxer.c — FFmpeg libavformat MP4 muxer implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "zst_element.h"
#include "zst_pad.h"
#include "zst_buffer.h"

typedef struct {
    AVFormatContext* fc;
    uint8_t*         avio_buf;
    size_t           avio_buf_size;
    int              video_stream_idx;
    int              audio_stream_idx;
    int              header_written;
    
    int              video_linked;
    int              audio_linked;
    int              video_eos;
    int              audio_eos;
} mp4_muxer_t;

void
mp4_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
}

static int
mp4_mux_write_packet(void* opaque, uint8_t* buf, int buf_size)
{
    zst_element_t* el = opaque;
    
    zst_buffer_t* out_buf = zst_buffer_create(ZST_BUFFER_USER);
    if (!out_buf) return -1;
    
    uint8_t* data = malloc(buf_size);
    if (!data) {
        zst_buffer_unref(out_buf);
        return -1;
    }
    memcpy(data, buf, buf_size);
    
    out_buf->memory.type = ZST_MEMORY_CPU;
    out_buf->memory.data = data;
    out_buf->memory.size = buf_size;
    out_buf->destroy = mp4_buf_free;
    
    zst_pad_t* src_pad = zst_element_get_pad(el, "src");
    if (src_pad && src_pad->peer) {
        zst_pad_push(src_pad, out_buf);
    }
    
    zst_buffer_unref(out_buf);
    return buf_size;
}

static zst_result_t
mp4_mux_write_header(zst_element_t* el)
{
    mp4_muxer_t* s = el->priv;
    
    if (avformat_alloc_output_context2(&s->fc, NULL, "mp4", NULL) < 0) {
        return ZST_ERROR;
    }
    
    s->avio_buf_size = 4096;
    s->avio_buf = av_malloc(s->avio_buf_size);
    s->fc->pb = avio_alloc_context(
        s->avio_buf, s->avio_buf_size,
        1, el, NULL, mp4_mux_write_packet, NULL
    );
    if (!s->fc->pb) {
        avformat_free_context(s->fc);
        s->fc = NULL;
        return ZST_ERROR;
    }
    s->fc->pb->seekable = 0;
    s->fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    
    zst_pad_t* video_pad = zst_element_get_pad(el, "video");
    zst_pad_t* audio_pad = zst_element_get_pad(el, "audio");
    
    int stream_count = 0;
    s->video_linked = (video_pad && video_pad->peer) ? 1 : 0;
    s->audio_linked = (audio_pad && audio_pad->peer) ? 1 : 0;
    
    if (s->video_linked) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_H264;
        st->codecpar->width = 640;
        st->codecpar->height = 480;
        st->time_base = (AVRational){1, 1000000000};
        s->video_stream_idx = stream_count++;
    }
    
    if (s->audio_linked) {
        AVStream* st = avformat_new_stream(s->fc, NULL);
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_AAC;
        st->codecpar->sample_rate = 44100;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
        av_channel_layout_default(&st->codecpar->ch_layout, 2);
#else
        st->codecpar->channels = 2;
        st->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
        st->time_base = (AVRational){1, 1000000000};
        s->audio_stream_idx = stream_count++;
    }
    
    AVDictionary* opts = NULL;
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    if (avformat_write_header(s->fc, &opts) < 0) {
        av_dict_free(&opts);
        return ZST_ERROR;
    }
    av_dict_free(&opts);
    
    s->header_written = 1;
    s->video_eos = 0;
    s->audio_eos = 0;
    return ZST_OK;
}

static zst_result_t
mp4_mux_write(zst_element_t* el, zst_buffer_t* buf, int stream_idx)
{
    mp4_muxer_t* s = el->priv;
    if (!s->header_written) return ZST_ERROR;
    
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return ZST_ERROR;
    
    pkt->data = buf->memory.data;
    pkt->size = buf->memory.size;
    pkt->pts = av_rescale_q(buf->pts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->dts = av_rescale_q(buf->dts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->stream_index = stream_idx;
    
    if (av_interleaved_write_frame(s->fc, pkt) < 0) {
        av_packet_free(&pkt);
        return ZST_ERROR;
    }
    
    av_packet_free(&pkt);
    return ZST_OK;
}

static void
mp4_mux_check_eos(zst_element_t* el)
{
    mp4_muxer_t* s = el->priv;
    int all_eos = 1;
    if (s->video_linked && !s->video_eos) all_eos = 0;
    if (s->audio_linked && !s->audio_eos) all_eos = 0;
    
    if (all_eos) {
        zst_pad_t* src_pad = zst_element_get_pad(el, "src");
        if (src_pad && src_pad->peer) {
            zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_USER);
            if (eos_buf) {
                eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                zst_pad_push(src_pad, eos_buf);
                zst_buffer_unref(eos_buf);
            }
        }
    }
}

static zst_result_t
mp4_mux_video_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mp4_muxer_t* s = el->priv;
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        mp4_mux_check_eos(el);
        return ZST_OK;
    }
    
    if (s->video_stream_idx >= 0) {
        return mp4_mux_write(el, buf, s->video_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
mp4_mux_audio_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    zst_element_t* el = pad->parent;
    mp4_muxer_t* s = el->priv;
    
    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        mp4_mux_check_eos(el);
        return ZST_OK;
    }
    
    if (s->audio_stream_idx >= 0) {
        return mp4_mux_write(el, buf, s->audio_stream_idx);
    }
    return ZST_OK;
}

static zst_result_t
mp4_mux_start(zst_element_t* el)
{
    return mp4_mux_write_header(el);
}

static zst_result_t
mp4_mux_stop(zst_element_t* el)
{
    mp4_muxer_t* s = el->priv;
    if (s->fc && s->header_written) {
        av_write_trailer(s->fc);
    }
    
    if (s->fc) {
        if (s->fc->pb) {
            av_freep(&s->fc->pb->buffer);
            avio_context_free(&s->fc->pb);
        }
        avformat_free_context(s->fc);
        s->fc = NULL;
    }
    s->header_written = 0;
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name  = "mp4mux",
    .start = mp4_mux_start,
    .stop  = mp4_mux_stop,
};

zst_element_t*
zst_mp4_muxer_create(void)
{
    zst_element_t* el;
    mp4_muxer_t* priv;
    zst_pad_t* video;
    zst_pad_t* audio;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    el = zst_element_create(&g_ops, priv);

    video = zst_pad_create("video", ZST_PAD_SINK);
    audio = zst_pad_create("audio", ZST_PAD_SINK);
    src   = zst_pad_create("src",   ZST_PAD_SRC);

    video->push = mp4_mux_video_push;
    audio->push = mp4_mux_audio_push;

    zst_element_add_pad(el, video);
    zst_element_add_pad(el, audio);
    zst_element_add_pad(el, src);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "mp4mux") == 0) {
        return zst_mp4_muxer_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "mp4muxer_plugin",
        .author = "Antigravity",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

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