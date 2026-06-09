/*=============================================================================
    zst_log.c — Lightweight logging implementation

    Level colours (when stderr is a TTY):
      ERROR   → red
      WARNING → yellow
      INFO    → green
      DEBUG   → blue
      TRACE   → dim
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup, isatty */

#include "zst_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* ── Internal state ───────────────────────────────────────────────────── */
static struct {
    zst_log_level_t   level;          /* runtime filter */
    zst_log_handler_t handler;        /* handler (default if NULL) */
    pthread_mutex_t   lock;
} g_log = {
    .level   = ZST_LOG_LEVEL_TRACE,
    .handler = zst_log_default_handler,
    .lock    = PTHREAD_MUTEX_INITIALIZER,
};

/* ── Level label table ────────────────────────────────────────────────── */
static const char*
level_label(zst_log_level_t lvl)
{
    switch (lvl) {
    case ZST_LOG_LEVEL_ERROR:   return "ERROR";
    case ZST_LOG_LEVEL_WARNING: return "WARN";
    case ZST_LOG_LEVEL_INFO:    return "INFO";
    case ZST_LOG_LEVEL_DEBUG:   return "DEBUG";
    case ZST_LOG_LEVEL_TRACE:   return "TRACE";
    default:                    return "?";
    }
}

/* ── ANSI colour codes (when stderr is a TTY) ────────────────────────── */
#define C_RESET   "\033[0m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_DIM     "\033[2m"

static const char*
level_colour(zst_log_level_t lvl)
{
    switch (lvl) {
    case ZST_LOG_LEVEL_ERROR:   return C_RED;
    case ZST_LOG_LEVEL_WARNING: return C_YELLOW;
    case ZST_LOG_LEVEL_INFO:    return C_GREEN;
    case ZST_LOG_LEVEL_DEBUG:   return C_BLUE;
    case ZST_LOG_LEVEL_TRACE:   return C_DIM;
    default:                    return "";
    }
}

/* ── Runtime control ─────────────────────────────────────────────────── */
void
zst_log_set_level(zst_log_level_t level)
{
    g_log.level = level;
}

zst_log_level_t
zst_log_get_level(void)
{
    return g_log.level;
}

/* ── Custom handler ───────────────────────────────────────────────────── */
void
zst_log_set_handler(zst_log_handler_t handler)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.handler = handler ? handler : zst_log_default_handler;
    pthread_mutex_unlock(&g_log.lock);
}

/* ── Default handler (stderr with timestamp + optional colour) ────────── */
void
zst_log_default_handler(zst_log_level_t level,
                         const char* category,
                         const char* file,
                         int line,
                         const char* func,
                         const char* message)
{
    int use_colour = isatty(STDERR_FILENO);

    /* Build timestamp: HH:MM:SS.mmm */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    localtime_r(&ts.tv_sec, &t);

    char timebuf[32];
    int ms = (int)(ts.tv_nsec / 1000000L);
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d.%03d",
             t.tm_hour, t.tm_min, t.tm_sec, ms);

    const char* label = level_label(level);
    const char* col   = use_colour ? level_colour(level) : "";
    const char* reset = use_colour ? C_RESET : "";

    /* Truncated filename (strip dir prefix) */
    const char* base = strrchr(file, '/');
    base = base ? base + 1 : file;

    fprintf(stderr, "%s %s%-5s%s [%s] %s:%d %s() - %s\n",
            timebuf,
            col, label, reset,
            category ? category : "",
            base, line,
            func,
            message ? message : "(null)");
}

/* ── Core emit: variadic → vsnprintf → handler ───────────────────────── */
void
zst_log_emit(zst_log_level_t level,
             const char* category,
             const char* file,
             int line,
             const char* func,
             const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    zst_log_emitv(level, category, file, line, func, fmt, args);
    va_end(args);
}

void
zst_log_emitv(zst_log_level_t level,
              const char* category,
              const char* file,
              int line,
              const char* func,
              const char* fmt,
              va_list args)
{
    /* Runtime filter */
    if (level > g_log.level)
        return;

    /* Format message */
    char buf[2048];
    int len = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", args);
    if (len < 0)
        return;
    if ((size_t)len >= sizeof(buf))
        len = (int)(sizeof(buf) - 1);
    buf[len] = '\0';

    /* Dispatch (handler is always non-NULL, defaults set at init) */
    pthread_mutex_lock(&g_log.lock);
    zst_log_handler_t handler = g_log.handler;
    pthread_mutex_unlock(&g_log.lock);

    handler(level, category, file, line, func, buf);
}
