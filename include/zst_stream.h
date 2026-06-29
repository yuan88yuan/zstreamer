/*=============================================================================
    @file zst_stream.h
    @brief Stream metadata and query API

    zst_stream_info_t provides descriptive metadata for media streams
    within a demuxer or multi-stream element.  It supports:
    - Stream identification (id, kind: video/audio/text)
    - Stream status tracking (present, changed, removed)
    - Caps association for format negotiation
    - Language and codec metadata

    Used by adaptive demuxers and elements with dynamic pads.
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t zst_stream_id_t;
typedef uint64_t zst_time_t;

typedef enum {
    ZST_MEDIA_UNKNOWN,
    ZST_MEDIA_VIDEO,
    ZST_MEDIA_AUDIO,
    ZST_MEDIA_TEXT,
    ZST_MEDIA_DATA
} zst_media_kind_t;

typedef enum {
    ZST_STREAM_STATUS_PRESENT,
    ZST_STREAM_STATUS_LOST,
    ZST_STREAM_STATUS_REMOVED,
    ZST_STREAM_STATUS_CHANGED
} zst_stream_status_t;

typedef struct zst_caps zst_caps_t;
typedef struct zst_stream_info zst_stream_info_t;

/* Ownership: zst_stream_info_t snapshots returned by APIs own their name,
 * language, and caps fields. Release them with zst_stream_info_clear(). */
struct zst_stream_info {
    size_t struct_size;

    zst_stream_id_t id;
    uint32_t program_id;
    uint32_t index;

    zst_media_kind_t kind;
    zst_stream_status_t status;

    char* name;
    char* language;

    zst_caps_t* caps;

    zst_time_t first_pts;
    zst_time_t last_seen_pts;

    uint32_t flags;
};

void zst_stream_info_clear(zst_stream_info_t* info);

#ifdef __cplusplus
}
#endif
