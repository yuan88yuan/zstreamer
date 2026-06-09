/*=============================================================================
    mp4_muxer.c — FFmpeg libavformat MP4 muxer implementation
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "mm_element.h"
#include "mm_pad.h"
#include "mm_buffer.h"

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
mp4_buf_free(mm_buffer_t* buf)
{
    if (buf && buf->memory.data) {
        free(buf->memory.data);
        buf->memory.data = NULL;
    }
}

static int
mp4_mux_write_packet(void* opaque, uint8_t* buf, int buf_size)
{
    mm_element_t* el = opaque;
    
    mm_buffer_t* out_buf = mm_buffer_create(MM_BUFFER_USER);
    if (!out_buf) return -1;
    
    uint8_t* data = malloc(buf_size);
    if (!data) {
        mm_buffer_unref(out_buf);
        return -1;
    }
    memcpy(data, buf, buf_size);
    
    out_buf->memory.type = MM_MEMORY_CPU;
    out_buf->memory.data = data;
    out_buf->memory.size = buf_size;
    out_buf->destroy = mp4_buf_free;
    
    mm_pad_t* src_pad = mm_element_get_pad(el, "src");
    if (src_pad && src_pad->peer) {
        mm_pad_push(src_pad, out_buf);
    }
    
    mm_buffer_unref(out_buf);
    return buf_size;
}

static mm_result_t
mp4_mux_write_header(mm_element_t* el)
{
    mp4_muxer_t* s = el->priv;
    
    if (avformat_alloc_output_context2(&s->fc, NULL, "mp4", NULL) < 0) {
        return MM_ERROR;
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
        return MM_ERROR;
    }
    s->fc->pb->seekable = 0;
    s->fc->flags |= AVFMT_FLAG_CUSTOM_IO;
    
    s->video_stream_idx = -1;
    s->audio_stream_idx = -1;
    
    mm_pad_t* video_pad = mm_element_get_pad(el, "video");
    mm_pad_t* audio_pad = mm_element_get_pad(el, "audio");
    
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
        return MM_ERROR;
    }
    av_dict_free(&opts);
    
    s->header_written = 1;
    s->video_eos = 0;
    s->audio_eos = 0;
    return MM_OK;
}

static mm_result_t
mp4_mux_write(mm_element_t* el, mm_buffer_t* buf, int stream_idx)
{
    mp4_muxer_t* s = el->priv;
    if (!s->header_written) return MM_ERROR;
    
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return MM_ERROR;
    
    pkt->data = buf->memory.data;
    pkt->size = buf->memory.size;
    pkt->pts = av_rescale_q(buf->pts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->dts = av_rescale_q(buf->dts, (AVRational){1, 1000000000}, s->fc->streams[stream_idx]->time_base);
    pkt->stream_index = stream_idx;
    
    if (av_interleaved_write_frame(s->fc, pkt) < 0) {
        av_packet_free(&pkt);
        return MM_ERROR;
    }
    
    av_packet_free(&pkt);
    return MM_OK;
}

static void
mp4_mux_check_eos(mm_element_t* el)
{
    mp4_muxer_t* s = el->priv;
    int all_eos = 1;
    if (s->video_linked && !s->video_eos) all_eos = 0;
    if (s->audio_linked && !s->audio_eos) all_eos = 0;
    
    if (all_eos) {
        mm_pad_t* src_pad = mm_element_get_pad(el, "src");
        if (src_pad && src_pad->peer) {
            mm_buffer_t* eos_buf = mm_buffer_create(MM_BUFFER_USER);
            if (eos_buf) {
                eos_buf->flags |= MM_BUFFER_FLAG_EOS;
                mm_pad_push(src_pad, eos_buf);
                mm_buffer_unref(eos_buf);
            }
        }
    }
}

static mm_result_t
mp4_mux_video_push(mm_pad_t* pad, mm_buffer_t* buf)
{
    mm_element_t* el = pad->parent;
    mp4_muxer_t* s = el->priv;
    
    if (buf->flags & MM_BUFFER_FLAG_EOS) {
        s->video_eos = 1;
        mp4_mux_check_eos(el);
        return MM_OK;
    }
    
    if (s->video_stream_idx >= 0) {
        return mp4_mux_write(el, buf, s->video_stream_idx);
    }
    return MM_OK;
}

static mm_result_t
mp4_mux_audio_push(mm_pad_t* pad, mm_buffer_t* buf)
{
    mm_element_t* el = pad->parent;
    mp4_muxer_t* s = el->priv;
    
    if (buf->flags & MM_BUFFER_FLAG_EOS) {
        s->audio_eos = 1;
        mp4_mux_check_eos(el);
        return MM_OK;
    }
    
    if (s->audio_stream_idx >= 0) {
        return mp4_mux_write(el, buf, s->audio_stream_idx);
    }
    return MM_OK;
}

static mm_result_t
mp4_mux_start(mm_element_t* el)
{
    return mp4_mux_write_header(el);
}

static mm_result_t
mp4_mux_stop(mm_element_t* el)
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
    return MM_OK;
}

static mm_element_ops_t g_ops = {
    .name  = "mp4mux",
    .start = mp4_mux_start,
    .stop  = mp4_mux_stop,
};

mm_element_t*
mm_mp4_muxer_create(void)
{
    mm_element_t* el;
    mp4_muxer_t* priv;
    mm_pad_t* video;
    mm_pad_t* audio;
    mm_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    el = mm_element_create(&g_ops, priv);

    video = mm_pad_create("video", MM_PAD_SINK);
    audio = mm_pad_create("audio", MM_PAD_SINK);
    src   = mm_pad_create("src",   MM_PAD_SRC);

    video->push = mp4_mux_video_push;
    audio->push = mp4_mux_audio_push;

    mm_element_add_pad(el, video);
    mm_element_add_pad(el, audio);
    mm_element_add_pad(el, src);

    return el;
}

#ifdef BUILDING_PLUGIN
#include "mm_plugin.h"
#include <string.h>

static mm_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "mp4mux") == 0) {
        return mm_mp4_muxer_create();
    }
    return NULL;
}

static mm_plugin_t g_plugin = {
    .desc = {
        .name = "mp4muxer_plugin",
        .author = "Antigravity",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = plugin_create_element
};

MM_PLUGIN_EXPORT
mm_plugin_t*
mm_get_plugin(void)
{
    mm_plugin_t* p = malloc(sizeof(*p));
    if (p) {
        *p = g_plugin;
    }
    return p;
}
#endif