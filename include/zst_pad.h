/*=============================================================================
    zst_pad.h - High-performance peer pads with lock-free probe fast-path
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_buffer.h"
#include "zst_caps.h"
#include "zst_segment.h"
#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_PAD_SRC,
    ZST_PAD_SINK
} zst_pad_direction_t;

typedef zst_result_t (*zst_pad_push_fn)(zst_pad_t* pad, zst_buffer_t* buf);
typedef zst_result_t (*zst_pad_pull_fn)(zst_pad_t* pad, zst_buffer_t** out);

typedef enum {
    ZST_PAD_PROBE_PRE_BUFFER  = 1u << 0,
    ZST_PAD_PROBE_POST_BUFFER = 1u << 1,
    ZST_PAD_PROBE_PRE_EVENT   = 1u << 2,
    ZST_PAD_PROBE_POST_EVENT  = 1u << 3
} zst_pad_probe_type_t;

typedef enum {
    ZST_PAD_PROBE_OK,
    ZST_PAD_PROBE_DROP,
    ZST_PAD_PROBE_BLOCK,
    ZST_PAD_PROBE_REBLOCK
} zst_pad_probe_return_t;

typedef zst_pad_probe_return_t (*zst_pad_probe_fn)(
    zst_pad_t* pad,
    zst_buffer_t* buf,
    zst_pad_probe_type_t type,
    void* user_data);

typedef struct zst_pad_probe {
    uint64_t id;
    uint32_t types;
    zst_pad_probe_fn callback;
    void* user_data;
    struct zst_pad_probe* next;
} zst_pad_probe_t;

/* Optimized downstream spillover behavior on backpressure */
typedef enum {
    ZST_SPILLOVER_BLOCK,    /* Traditional GStreamer blocking behavior */
    ZST_SPILLOVER_DROP_NEW, /* Drop incoming frames on downstream stall */
    ZST_SPILLOVER_DROP_OLD  /* Drop oldest queued frames on downstream stall */
} zst_spillover_policy_t;

struct zst_pad {
    alignas(ZST_CACHE_LINE_SIZE) const char* name;
    zst_pad_direction_t direction;
    zst_element_t* parent;

    zst_caps_t* caps;          /* Negotiated active caps */
    zst_caps_t* template_caps; /* Supported capabilities */

    zst_pad_push_fn push;
    zst_pad_pull_fn pull;

    zst_pad_t* peer;
    void* priv;
    void (*destroy_priv)(zst_pad_t* pad);

    /* Atomic indicator to completely skip probe locks when no probes are active */
    _Atomic uint32_t has_probes;
    zst_pad_probe_t* probes;
    uint64_t next_probe_id;

    pthread_mutex_t probe_lock;
    pthread_cond_t probe_cond;
    _Atomic int blocked;
    int block_callback_fired;
    zst_pad_probe_fn block_callback;
    void* block_user_data;

    /* Segment context */
    int has_segment;
    zst_segment_t segment;

    /* Downstream spillover strategy configuration */
    zst_spillover_policy_t spillover_policy;
};

zst_pad_t* zst_pad_create(const char* name, zst_pad_direction_t direction);
void zst_pad_destroy(zst_pad_t* pad);

zst_result_t zst_pad_link(zst_pad_t* src, zst_pad_t* sink);
void zst_pad_unlink(zst_pad_t* pad);

zst_result_t zst_pad_push(zst_pad_t* pad, zst_buffer_t* buf);
zst_result_t zst_pad_pull(zst_pad_t* pad, zst_buffer_t** out);

void zst_pad_reset_callbacks(zst_pad_t* pad);

zst_result_t zst_pad_set_caps(zst_pad_t* pad, const zst_caps_t* caps);
zst_caps_t* zst_pad_get_caps(zst_pad_t* pad);
zst_result_t zst_pad_set_template_caps(zst_pad_t* pad, const zst_caps_t* caps);
zst_result_t zst_pad_negotiate(zst_pad_t* src, zst_pad_t* sink);

uint64_t zst_pad_add_probe(
    zst_pad_t* pad,
    uint32_t types,
    zst_pad_probe_fn callback,
    void* user_data);

zst_result_t zst_pad_remove_probe(zst_pad_t* pad, uint64_t probe_id);

zst_result_t zst_pad_block(zst_pad_t* pad);
zst_result_t zst_pad_unblock(zst_pad_t* pad);
int zst_pad_is_blocked(zst_pad_t* pad);

zst_result_t zst_pad_set_block_callback(
    zst_pad_t* pad,
    zst_pad_probe_fn callback,
    void* user_data);

zst_result_t zst_pad_set_segment(zst_pad_t* pad, const zst_segment_t* segment);
zst_result_t zst_pad_get_segment(zst_pad_t* pad, zst_segment_t* segment_out);
void zst_pad_clear_segment(zst_pad_t* pad);
zst_result_t zst_pad_push_segment(zst_pad_t* src, const zst_segment_t* segment);

#ifdef __cplusplus
}
#endif