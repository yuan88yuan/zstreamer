/*=============================================================================
    audio_mixer.c — Audio mixer element — multiple audio inputs mixed into one
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "zst_element.h"
#include "zst_element_factory.h"
#include "zstreamer/elements/zst_audio_mixer.h"
#include "zst_pad.h"
#include "zst_buffer.h"
#include "zst_buffer_pool.h"
#include "zst_caps.h"
#include "zst_queue.h"
#include "zst_log.h"
#include "zst_clock.h"

#define MAX_INPUTS 64

/* ── Private format codes (mirrors audio_test_src.c) ─────────────────── */
#define ZST_AUDIO_FMT_S16LE 0u
#define ZST_AUDIO_FMT_F32LE 3u

typedef struct {
    char        name[32];
    zst_pad_t*  pad;
    zst_queue_t* queue;
    double      volume;
    double      pan;        /* -1.0 (left) to 1.0 (right) */
    bool        mute;
    bool        eos;
} audio_mixer_input_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;
    bool            running;

    audio_mixer_input_t inputs[MAX_INPUTS];
    uint32_t            num_inputs;
    uint32_t            next_pad_index;

    zst_pad_t*          srcpad;
    zst_buffer_pool_t*  pool;

    uint32_t            sample_rate;
    uint32_t            channels;
    uint32_t            format;     /* 0=S16LE, 3=F32LE */
    uint32_t            latency;    /* target latency in ms (reserved) */

    bool                eos_sent;
} audio_mixer_t;

/* ── Forward declarations ────────────────────────────────────────────── */
static zst_result_t audio_mixer_open(zst_element_t* el);
static zst_result_t audio_mixer_close(zst_element_t* el);
static zst_result_t audio_mixer_start(zst_element_t* el);
static zst_result_t audio_mixer_stop(zst_element_t* el);
static zst_result_t audio_mixer_set_property(zst_element_t* el, const char* name, const char* value);
static zst_result_t audio_mixer_get_property(zst_element_t* el, const char* name, char* out, size_t max);
static zst_caps_t*  audio_mixer_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter);
static void*        audio_mixer_worker(void* arg);

static void audio_mixer_buf_free(zst_buffer_t* buf);

/* ── Ops vtable ──────────────────────────────────────────────────────── */
static const zst_element_ops_t g_audio_mixer_ops = {
    .name          = "audiomixer",
    .open          = audio_mixer_open,
    .close         = audio_mixer_close,
    .start         = audio_mixer_start,
    .stop          = audio_mixer_stop,
    .set_property  = audio_mixer_set_property,
    .get_property  = audio_mixer_get_property,
    .get_caps      = audio_mixer_get_caps,
};

/* ── Public API ──────────────────────────────────────────────────────── */

zst_element_t*
zst_audio_mixer_create(void)
{
    return zst_audio_mixer_create_with_config(NULL);
}

zst_element_t*
zst_audio_mixer_create_with_config(const zst_audio_mixer_config_t* config)
{
    audio_mixer_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);

    zst_element_t* el = zst_element_create(&g_audio_mixer_ops, s);
    if (!el) {
        pthread_cond_destroy(&s->cond);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }

    s->srcpad = zst_pad_create("src", ZST_PAD_SRC);
    if (!s->srcpad || zst_element_add_pad(el, s->srcpad) != ZST_OK) {
        if (s->srcpad) zst_pad_destroy(s->srcpad);
        zst_element_destroy(el);
        return NULL;
    }

    /* Defaults */
    s->sample_rate = 48000;
    s->channels    = 2;
    s->format      = ZST_AUDIO_FMT_F32LE;

    if (config) {
        if (config->latency > 0) s->latency = config->latency;
    }

    return el;
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static zst_result_t
audio_mixer_open(zst_element_t* el)
{
    (void)el;
    /* Nothing to open beyond what's in create */
    return ZST_OK;
}

static zst_result_t
audio_mixer_close(zst_element_t* el)
{
    audio_mixer_t* s = el->priv;
    if (!s) return ZST_OK;

    /* Ensure the worker has stopped */
    pthread_mutex_lock(&s->mutex);
    s->running = false;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    if (s->thread) {
        pthread_join(s->thread, NULL);
        s->thread = 0;
    }

    /* Destroy per-input queues */
    for (uint32_t i = 0; i < s->num_inputs; i++) {
        if (s->inputs[i].queue) {
            zst_queue_destroy(s->inputs[i].queue);
            s->inputs[i].queue = NULL;
        }
    }
    s->num_inputs = 0;

    if (s->pool) {
        zst_buffer_pool_destroy(s->pool);
        s->pool = NULL;
    }

    s->eos_sent = false;

    return ZST_OK;
}

static zst_result_t
audio_mixer_start(zst_element_t* el)
{
    audio_mixer_t* s = el->priv;
    s->running  = true;
    s->eos_sent = false;
    pthread_create(&s->thread, NULL, audio_mixer_worker, el);
    return ZST_OK;
}

static zst_result_t
audio_mixer_stop(zst_element_t* el)
{
    audio_mixer_t* s = el->priv;
    if (!s) return ZST_OK;

    pthread_mutex_lock(&s->mutex);
    s->running = false;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    if (s->thread) {
        pthread_join(s->thread, NULL);
        s->thread = 0;
    }
    return ZST_OK;
}

/* ── Caps ────────────────────────────────────────────────────────────── */

static zst_caps_t*
audio_mixer_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter)
{
    (void)filter;
    audio_mixer_t* s = el->priv;
    zst_caps_t* caps = zst_caps_create();
    if (!caps) return NULL;

    const char* fmt_str = (s->format == ZST_AUDIO_FMT_S16LE) ? "S16LE" : "F32LE";

    if (pad == s->srcpad) {
        zst_caps_append(caps,
            zst_caps_struct_create_audio("audio/x-raw",
                (int)s->channels, (int)s->sample_rate, fmt_str));
    } else {
        /* Sink pads accept both S16LE and F32LE */
        zst_caps_append(caps,
            zst_caps_struct_create_audio("audio/x-raw",
                (int)s->channels, (int)s->sample_rate, "S16LE"));
        zst_caps_append(caps,
            zst_caps_struct_create_audio("audio/x-raw",
                (int)s->channels, (int)s->sample_rate, "F32LE"));
    }
    return caps;
}

/* ── Sink pad push callback ──────────────────────────────────────────── */

static zst_result_t
audio_mixer_sink_push(zst_pad_t* pad, zst_buffer_t* buf)
{
    if (!pad || !buf || !pad->priv) return ZST_ERROR;
    audio_mixer_input_t* in = (audio_mixer_input_t*)pad->priv;
    zst_element_t* el = pad->parent;
    audio_mixer_t* s = el->priv;

    if (buf->flags & ZST_BUFFER_FLAG_EOS) {
        pthread_mutex_lock(&s->mutex);
        in->eos = true;
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        zst_buffer_unref(buf);
        return ZST_OK;
    }

    /* Guard against double-stop (worker may have been stopped while we push) */
    if (!s->running) {
        zst_buffer_unref(buf);
        return ZST_OK;
    }

    zst_buffer_ref(buf);
    zst_result_t res = zst_queue_push(in->queue, buf, UINT32_MAX);
    if (res != ZST_OK) {
        zst_buffer_unref(buf);
    } else {
        pthread_mutex_lock(&s->mutex);
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }
    return res;
}

/* ── Pad property helpers ────────────────────────────────────────────── */

static audio_mixer_input_t*
audio_mixer_find_input_by_name(audio_mixer_t* s, const char* name)
{
    for (uint32_t i = 0; i < s->num_inputs; i++) {
        if (strcmp(s->inputs[i].name, name) == 0)
            return &s->inputs[i];
    }
    return NULL;
}

static zst_result_t
audio_mixer_set_pad_property(audio_mixer_input_t* in, const char* prop, const char* value)
{
    if (strcmp(prop, "volume") == 0) {
        in->volume = strtod(value, NULL);
        if (in->volume < 0.0) in->volume = 0.0;
        return ZST_OK;
    }
    if (strcmp(prop, "pan") == 0) {
        in->pan = strtod(value, NULL);
        if (in->pan < -1.0) in->pan = -1.0;
        if (in->pan > 1.0)  in->pan = 1.0;
        return ZST_OK;
    }
    if (strcmp(prop, "mute") == 0) {
        in->mute = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        return ZST_OK;
    }
    return ZST_ERROR;
}

static zst_result_t
audio_mixer_get_pad_property(audio_mixer_input_t* in, const char* prop, char* out, size_t max)
{
    if (strcmp(prop, "volume") == 0) {
        snprintf(out, max, "%f", in->volume);
        return ZST_OK;
    }
    if (strcmp(prop, "pan") == 0) {
        snprintf(out, max, "%f", in->pan);
        return ZST_OK;
    }
    if (strcmp(prop, "mute") == 0) {
        snprintf(out, max, "%s", in->mute ? "true" : "false");
        return ZST_OK;
    }
    return ZST_ERROR;
}

/* ── Request-pad ─────────────────────────────────────────────────────── */

zst_pad_t*
zst_audio_mixer_request_pad(zst_element_t* el, const char* name)
{
    if (!el || !el->priv) return NULL;
    audio_mixer_t* s = el->priv;

    pthread_mutex_lock(&s->mutex);

    if (s->num_inputs >= MAX_INPUTS) {
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    audio_mixer_input_t* in = &s->inputs[s->num_inputs];
    if (name && strlen(name) > 0) {
        strncpy(in->name, name, sizeof(in->name) - 1);
        in->name[sizeof(in->name) - 1] = '\0';
    } else {
        snprintf(in->name, sizeof(in->name), "sink_%u", s->next_pad_index++);
    }

    in->volume = 1.0;
    in->pan    = 0.0;
    in->mute   = false;
    in->eos    = false;

    zst_queue_config_t qcfg = {
        .mode         = ZST_QUEUE_SYNC,
        .max_buffers  = 10,
        .max_bytes    = 0,
        .max_duration = 0
    };
    in->queue = zst_queue_create(&qcfg);
    if (!in->queue) {
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    zst_pad_t* pad = zst_pad_create(in->name, ZST_PAD_SINK);
    if (!pad) {
        zst_queue_destroy(in->queue);
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    pad->push       = audio_mixer_sink_push;
    pad->priv       = in;
    pad->destroy_priv = NULL;
    in->pad         = pad;

    if (zst_element_add_pad(el, pad) != ZST_OK) {
        zst_pad_destroy(pad);
        zst_queue_destroy(in->queue);
        pthread_mutex_unlock(&s->mutex);
        return NULL;
    }

    s->num_inputs++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);

    return pad;
}

/* ── Property splitter (pad_name::prop syntax) ───────────────────────── */

static int
split_pad_prop(const char* name, char* pad, size_t pad_len, const char** prop_out)
{
    const char* sep = strstr(name, "::");
    if (!sep) return 0;
    size_t n = (size_t)(sep - name);
    if (n == 0 || n >= pad_len) return 0;
    memcpy(pad, name, n);
    pad[n] = '\0';
    *prop_out = sep + 2;
    return 1;
}

/* ── Set / get property ──────────────────────────────────────────────── */

static zst_result_t
audio_mixer_set_property(zst_element_t* el, const char* name, const char* value)
{
    audio_mixer_t* s = el->priv;
    const char* prop = NULL;
    char pad_name[32];

    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        pthread_mutex_lock(&s->mutex);
        audio_mixer_input_t* in = audio_mixer_find_input_by_name(s, pad_name);
        zst_result_t r = in ? audio_mixer_set_pad_property(in, prop, value) : ZST_ERROR;
        pthread_mutex_unlock(&s->mutex);
        return r;
    } else if (strcmp(name, "request-pad") == 0) {
        zst_pad_t* p = zst_audio_mixer_request_pad(el, (strlen(value) > 0) ? value : NULL);
        return p ? ZST_OK : ZST_ERROR;
    } else if (strcmp(name, "latency") == 0) {
        s->latency = (uint32_t)strtoul(value, NULL, 10);
        return ZST_OK;
    }

    return ZST_ERROR;
}

static zst_result_t
audio_mixer_get_property(zst_element_t* el, const char* name, char* out, size_t max)
{
    audio_mixer_t* s = el->priv;
    const char* prop = NULL;
    char pad_name[32];

    if (split_pad_prop(name, pad_name, sizeof(pad_name), &prop)) {
        pthread_mutex_lock(&s->mutex);
        audio_mixer_input_t* in = audio_mixer_find_input_by_name(s, pad_name);
        zst_result_t r = in ? audio_mixer_get_pad_property(in, prop, out, max) : ZST_ERROR;
        pthread_mutex_unlock(&s->mutex);
        return r;
    } else if (strcmp(name, "latency") == 0) {
        snprintf(out, max, "%u", s->latency);
        return ZST_OK;
    }

    return ZST_ERROR;
}

/* ── Buffer payload destructor (for manually allocated frames) ───────── */

static void
audio_mixer_buf_free(zst_buffer_t* buf)
{
    if (buf && buf->payload) {
        free(buf->payload);
        buf->payload = NULL;
    }
}

/* ── Worker thread ───────────────────────────────────────────────────── */

static void*
audio_mixer_worker(void* arg)
{
    zst_element_t* el = (zst_element_t*)arg;
    audio_mixer_t* s = el->priv;

    zst_buffer_t* in_bufs[MAX_INPUTS];

    while (1) {
        /* ── Wait until all active inputs have data or all are EOS ── */
        pthread_mutex_lock(&s->mutex);

        while (s->running) {
            bool all_ready   = true;
            bool any_active  = false;
            bool all_eos     = true;

            for (uint32_t i = 0; i < s->num_inputs; i++) {
                if (s->inputs[i].eos) continue;
                all_eos = false;
                if (zst_queue_size(s->inputs[i].queue) == 0) {
                    all_ready = false;
                } else {
                    any_active = true;
                }
            }

            /* All inputs reached EOS — send EOS downstream and exit */
            if (all_eos && s->num_inputs > 0) {
                s->eos_sent = true;
                pthread_mutex_unlock(&s->mutex);

                zst_buffer_t* eos_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
                if (eos_buf) {
                    eos_buf->flags |= ZST_BUFFER_FLAG_EOS;
                    zst_pad_push(s->srcpad, eos_buf);
                }

                pthread_mutex_lock(&s->mutex);
                /* Break out of the cond-wait loop */
                goto worker_done;
            }

            /* Some inputs have data and none are missing — start mixing */
            if (all_ready && any_active) {
                break;
            }

            pthread_cond_wait(&s->cond, &s->mutex);
        }

        if (!s->running) {
            pthread_mutex_unlock(&s->mutex);
            break;
        }

        /* ── Pop one buffer from each active input ── */
        memset(in_bufs, 0, sizeof(in_bufs));
        uint32_t  samples_to_mix = 0;
        zst_time_t mix_pts       = 0;
        zst_time_t mix_duration  = 0;
        bool       have_pts      = false;

        for (uint32_t i = 0; i < s->num_inputs; i++) {
            if (s->inputs[i].eos) continue;
            zst_buffer_t* buf = NULL;
            if (zst_queue_pop(s->inputs[i].queue, &buf, 0) == ZST_OK && buf) {
                in_bufs[i] = buf;

                zst_audio_frame_t* af = (zst_audio_frame_t*)buf->payload;
                if (af && af->nb_samples > samples_to_mix) {
                    samples_to_mix = af->nb_samples;
                }
                if (!have_pts && buf->pts > 0) {
                    mix_pts      = buf->pts;
                    mix_duration = buf->duration;
                    have_pts     = true;
                }
            }
        }

        /* ── Mix into a double-precision accumulation buffer ── */
        double* fmix = NULL;
        if (samples_to_mix > 0) {
            fmix = calloc(samples_to_mix * s->channels, sizeof(double));
        }

        for (uint32_t i = 0; i < s->num_inputs; i++) {
            if (!in_bufs[i] || s->inputs[i].mute) {
                /* Muted or no buffer: skip */
                continue;
            }

            zst_buffer_t*      buf = in_bufs[i];
            zst_audio_frame_t* af  = (zst_audio_frame_t*)buf->payload;
            if (!af || !af->data) continue;

            double volume = s->inputs[i].volume;
            uint32_t n = af->nb_samples * af->channels;
            if (n > samples_to_mix * s->channels) {
                n = samples_to_mix * s->channels;
            }

            if (af->format == ZST_AUDIO_FMT_S16LE) {
                int16_t* src = (int16_t*)af->data;
                for (uint32_t j = 0; j < n; j++) {
                    double gain = volume;
                    if (s->channels >= 2) {
                        uint32_t c = j % s->channels;
                        if (c == 0) {
                            gain *= (s->inputs[i].pan <= 0.0 ? 1.0 : (1.0 - s->inputs[i].pan));
                        } else if (c == 1) {
                            gain *= (s->inputs[i].pan >= 0.0 ? 1.0 : (1.0 + s->inputs[i].pan));
                        }
                    }
                    fmix[j] += ((double)src[j] / 32768.0) * gain;
                }
            } else if (af->format == ZST_AUDIO_FMT_F32LE) {
                float* src = (float*)af->data;
                for (uint32_t j = 0; j < n; j++) {
                    double gain = volume;
                    if (s->channels >= 2) {
                        uint32_t c = j % s->channels;
                        if (c == 0) {
                            gain *= (s->inputs[i].pan <= 0.0 ? 1.0 : (1.0 - s->inputs[i].pan));
                        } else if (c == 1) {
                            gain *= (s->inputs[i].pan >= 0.0 ? 1.0 : (1.0 + s->inputs[i].pan));
                        }
                    }
                    fmix[j] += (double)src[j] * gain;
                }
            }
        }

        pthread_mutex_unlock(&s->mutex);

        /* ── Build and push the output buffer (mutex is unlocked) ── */
        if (samples_to_mix > 0 && fmix) {
            zst_buffer_t* out_buf = zst_buffer_create_with_pool(s->pool);
            if (!out_buf) {
                /* Fallback: allocate manually */
                out_buf = zst_buffer_create(ZST_BUFFER_AUDIO_FRAME);
                if (out_buf) {
                    zst_audio_frame_t* oaf = calloc(1, sizeof(zst_audio_frame_t));
                    if (!oaf) {
                        zst_buffer_unref(out_buf);
                        out_buf = NULL;
                    } else {
                        uint32_t bpf = (s->format == ZST_AUDIO_FMT_S16LE) ? 2 : 4;
                        size_t data_size = (size_t)samples_to_mix * s->channels * bpf;
                        out_buf->payload = oaf;
                        out_buf->destroy = audio_mixer_buf_free;
                        out_buf->memory.data = calloc(1, data_size);
                        out_buf->memory.size = data_size;
                        oaf->data        = out_buf->memory.data;
                        oaf->sample_rate = s->sample_rate;
                        oaf->channels    = s->channels;
                        oaf->format      = s->format;
                        oaf->nb_samples  = samples_to_mix;
                    }
                }
            } else {
                /* Pool provided the buffer — ensure payload is set up */
                zst_audio_frame_t* oaf = (zst_audio_frame_t*)out_buf->payload;
                if (!oaf) {
                    oaf = calloc(1, sizeof(zst_audio_frame_t));
                    out_buf->payload = oaf;
                    out_buf->destroy = audio_mixer_buf_free;
                }
                oaf->sample_rate = s->sample_rate;
                oaf->channels    = s->channels;
                oaf->format      = s->format;
                oaf->nb_samples  = samples_to_mix;
                oaf->data        = out_buf->memory.data;
            }

            if (out_buf) {
                out_buf->pts      = mix_pts;
                out_buf->duration = mix_duration;

                zst_audio_frame_t* oaf = (zst_audio_frame_t*)out_buf->payload;
                if (oaf && oaf->data) {
                    if (s->format == ZST_AUDIO_FMT_S16LE) {
                        int16_t* dst = (int16_t*)oaf->data;
                        for (uint32_t j = 0; j < samples_to_mix * s->channels; j++) {
                            double val = fmix[j];
                            if (val > 1.0)  val = 1.0;
                            if (val < -1.0) val = -1.0;
                            dst[j] = (int16_t)(val * 32767.0);
                        }
                    } else { /* F32LE */
                        float* dst = (float*)oaf->data;
                        for (uint32_t j = 0; j < samples_to_mix * s->channels; j++) {
                            double val = fmix[j];
                            if (val > 1.0)  val = 1.0;
                            if (val < -1.0) val = -1.0;
                            dst[j] = (float)val;
                        }
                    }
                }

                zst_pad_push(s->srcpad, out_buf);
            }

            free(fmix);
        }

        /* Release input buffers */
        for (uint32_t i = 0; i < s->num_inputs; i++) {
            if (in_bufs[i]) {
                zst_buffer_unref(in_bufs[i]);
            }
        }

        /* If all inputs are now EOS (checked without lock, approximate),
         * continue the loop — the top will send EOS on next iteration. */
        continue;

    worker_done:
        pthread_mutex_unlock(&s->mutex);
        break;
    }

    return NULL;
}
