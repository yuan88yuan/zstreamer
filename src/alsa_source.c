/*=============================================================================
    alsa_source.c — ALSA audio source with mock synthetic fallback
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <alsa/asoundlib.h>

#include "zst_element.h"
#include "zst_buffer.h"

typedef struct {
    snd_pcm_t*      handle;
    int             is_mock;
    uint64_t        sample_count;
    uint32_t        sample_rate;
    uint32_t        channels;
} alsa_source_t;

static void
alsa_buf_free(zst_buffer_t* buf)
{
    if (buf) {
        if (buf->memory.data) {
            free(buf->memory.data);
            buf->memory.data = NULL;
        }
        if (buf->payload) {
            free(buf->payload);
            buf->payload = NULL;
        }
    }
}

static zst_result_t
alsa_open(zst_element_t* el)
{
    alsa_source_t* s = el->priv;
    s->sample_rate = 44100;
    s->channels = 2;
    s->sample_count = 0;
    s->handle = NULL;

    int err = snd_pcm_open(&s->handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        printf("[alsasrc] Failed to open default ALSA capture device: %s. Falling back to synthetic source.\n", snd_strerror(err));
        s->is_mock = 1;
        s->handle = NULL;
        return ZST_OK;
    }

    err = snd_pcm_set_params(s->handle,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             s->channels,
                             s->sample_rate,
                             1, // soft resample
                             500000); // 0.5s latency
    if (err < 0) {
        printf("[alsasrc] Failed to set parameters: %s. Falling back to synthetic source.\n", snd_strerror(err));
        snd_pcm_close(s->handle);
        s->handle = NULL;
        s->is_mock = 1;
        return ZST_OK;
    }

    s->is_mock = 0;
    return ZST_OK;
}

static zst_result_t
alsa_close(zst_element_t* el)
{
    alsa_source_t* s = el->priv;
    if (s->handle) {
        snd_pcm_close(s->handle);
        s->handle = NULL;
    }
    return ZST_OK;
}

static zst_result_t
alsa_start(zst_element_t* el)
{
    alsa_source_t* s = el->priv;
    if (s->handle && !s->is_mock) {
        snd_pcm_prepare(s->handle);
    }
    return ZST_OK;
}

static zst_result_t
alsa_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out)
{
    (void)in;
    alsa_source_t* s = el->priv;
    
    zst_buffer_t* buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
    if (!buf) return ZST_ERROR;

    uint32_t nb_samples = 1024;
    size_t data_size = nb_samples * s->channels * sizeof(int16_t);
    uint8_t* raw_data = malloc(data_size);
    if (!raw_data) {
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }

    buf->memory.type = ZST_MEMORY_CPU;
    buf->memory.data = raw_data;
    buf->memory.size = data_size;

    zst_audio_frame_t* frame = calloc(1, sizeof(*frame));
    if (!frame) {
        free(raw_data);
        zst_buffer_unref(buf);
        return ZST_ERROR;
    }
    frame->sample_rate = s->sample_rate;
    frame->channels = s->channels;
    frame->format = 0; // S16_LE
    frame->nb_samples = nb_samples;
    frame->data = raw_data;
    buf->payload = frame;
    buf->destroy = alsa_buf_free;

    if (s->is_mock) {
        /* Generate a synthetic 440 Hz square wave (period = 100 samples @ 44100Hz) */
        int16_t* pcm = (int16_t*)raw_data;
        for (uint32_t i = 0; i < nb_samples; i++) {
            int phase = (s->sample_count + i) % 100;
            int16_t val = (phase < 50) ? 8000 : -8000;
            pcm[i * 2] = val;
            pcm[i * 2 + 1] = val;
        }
        s->sample_count += nb_samples;

        /* Simulate 44100Hz timing: 1024 samples takes ~23.2ms */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 23219954 };
        nanosleep(&ts, NULL);
    } else {
        snd_pcm_sframes_t frames = snd_pcm_readi(s->handle, raw_data, nb_samples);
        if (frames < 0) {
            if (frames == -EPIPE) {
                snd_pcm_prepare(s->handle);
            }
            memset(raw_data, 0, data_size);
        } else if (frames < (snd_pcm_sframes_t)nb_samples) {
            /* Zero out remaining frames if incomplete read */
            size_t read_bytes = frames * s->channels * sizeof(int16_t);
            memset(raw_data + read_bytes, 0, data_size - read_bytes);
        }
        s->sample_count += nb_samples;
    }

    /* Set nanosecond PTS */
    buf->pts = (s->sample_count - nb_samples) * 1000000000ULL / s->sample_rate;
    buf->duration = nb_samples * 1000000000ULL / s->sample_rate;

    *out = buf;
    return ZST_OK;
}

static zst_element_ops_t g_ops = {
    .name    = "alsasrc",
    .open    = alsa_open,
    .close   = alsa_close,
    .start   = alsa_start,
    .process = alsa_process,
};

zst_element_t*
zst_alsa_source_create(void)
{
    zst_element_t* el;
    alsa_source_t* priv;
    zst_pad_t* src;

    priv = calloc(1, sizeof(*priv));
    el = zst_element_create(&g_ops, priv);
    src = zst_pad_create("src", ZST_PAD_SRC);
    zst_element_add_pad(el, src);
    return el;
}

#ifdef BUILDING_PLUGIN
#include "zst_plugin.h"
#include <string.h>

static zst_element_t*
plugin_create_element(const char* name)
{
    if (strcmp(name, "alsasrc") == 0) {
        return zst_alsa_source_create();
    }
    return NULL;
}

static zst_plugin_t g_plugin = {
    .desc = {
        .name = "alsasource_plugin",
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
