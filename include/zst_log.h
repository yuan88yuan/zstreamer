/*=============================================================================
    zst_log.h — Lightweight structured logging system

    Provides compile-time-strippable, category-tagged logging macros with
    runtime level filtering, source location capture, and custom output
    handler support.

    Usage:
        #include "zst_log.h"

        ZST_LOG_ERROR("v4l2src", "Device open failed: %s", strerror(errno));
        ZST_LOG_WARN("encoder",  "Fallback to software, ret=%d", ret);
        ZST_LOG_INFO("pipeline", "State change: %d -> %d", old, new);
        ZST_LOG_DEBUG("queue",   "push buf=%p seq=%u", (void*)buf, seq);
        ZST_LOG_TRACE("sched",   "worker %u loop tick", id);

    Compile-time ceiling: #define ZST_LOG_LEVEL ZST_LOG_LEVEL_INFO (default:
    DEBUG in debug builds, WARNING in NDEBUG builds).  Any message at a
    level *above* the ceiling is dead-code eliminated.
=============================================================================*/
#pragma once

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Log levels ──────────────────────────────────────────────────────── */
typedef enum {
    ZST_LOG_LEVEL_NONE    = 0,  /* suppress everything */
    ZST_LOG_LEVEL_ERROR   = 1,
    ZST_LOG_LEVEL_WARNING = 2,
    ZST_LOG_LEVEL_INFO    = 3,
    ZST_LOG_LEVEL_DEBUG   = 4,
    ZST_LOG_LEVEL_TRACE   = 5
} zst_log_level_t;

/* ── Default compile-time ceiling ───────────────────────────────────── */
#ifndef ZST_LOG_LEVEL
#  ifdef NDEBUG
#    define ZST_LOG_LEVEL ZST_LOG_LEVEL_WARNING
#  else
#    define ZST_LOG_LEVEL ZST_LOG_LEVEL_TRACE
#  endif
#endif

/* ── Runtime control ─────────────────────────────────────────────────── */
void            zst_log_set_level(zst_log_level_t level);
zst_log_level_t zst_log_get_level(void);

/* ── Custom output handler ───────────────────────────────────────────── */
typedef void (*zst_log_handler_t)(zst_log_level_t level,
                                   const char* category,
                                   const char* file,
                                   int line,
                                   const char* func,
                                   const char* message);

void zst_log_set_handler(zst_log_handler_t handler);

/* ── Core emit functions (used by macros) ────────────────────────────── */
void zst_log_emit(zst_log_level_t level,
                  const char* category,
                  const char* file,
                  int line,
                  const char* func,
                  const char* fmt, ...);

void zst_log_emitv(zst_log_level_t level,
                   const char* category,
                   const char* file,
                   int line,
                   const char* func,
                   const char* fmt,
                   va_list args);

/* ── Default handler (stderr, optional colour) ───────────────────────── */
void zst_log_default_handler(zst_log_level_t level,
                              const char* category,
                              const char* file,
                              int line,
                              const char* func,
                              const char* message);

/* ── Convenience macros ──────────────────────────────────────────────── */

/* Internal: only emit if lvl ≤ compile-time ZST_LOG_LEVEL */
#define ZST_LOG_CAT(lvl, cat, ...)                             \
    do {                                                       \
        if (ZST_LOG_LEVEL >= (lvl)) {                          \
            zst_log_emit((lvl), (cat), __FILE__, __LINE__,     \
                         __func__, __VA_ARGS__);               \
        }                                                      \
    } while (0)

#define ZST_LOG_ERROR(cat, ...)   ZST_LOG_CAT(ZST_LOG_LEVEL_ERROR,   cat, __VA_ARGS__)
#define ZST_LOG_WARN(cat, ...)    ZST_LOG_CAT(ZST_LOG_LEVEL_WARNING, cat, __VA_ARGS__)
#define ZST_LOG_INFO(cat, ...)    ZST_LOG_CAT(ZST_LOG_LEVEL_INFO,    cat, __VA_ARGS__)
#define ZST_LOG_DEBUG(cat, ...)   ZST_LOG_CAT(ZST_LOG_LEVEL_DEBUG,   cat, __VA_ARGS__)
#define ZST_LOG_TRACE(cat, ...)   ZST_LOG_CAT(ZST_LOG_LEVEL_TRACE,   cat, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
