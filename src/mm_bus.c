/*=============================================================================
    mm_bus.c — Async notification channel / thread-safe event queue
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup, clock_gettime, CLOCK_REALTIME */

#include "mm_bus.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

typedef struct mm_event_node {
    mm_event_t*           event;
    struct mm_event_node* next;
} mm_event_node_t;

struct mm_bus {
    mm_event_node_t* head;
    mm_event_node_t* tail;
    uint32_t         count;

    pthread_mutex_t  lock;
    pthread_cond_t   cond;

    volatile int     flushing;

    pthread_t        thread;
    int              has_thread;
    mm_bus_handler_t handler;
    void*            user_data;
};

static void
timespec_from_ms(struct timespec* ts, uint32_t timeout_ms)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    ts->tv_sec  = now.tv_sec  + timeout_ms / 1000;
    ts->tv_nsec = now.tv_nsec + (timeout_ms % 1000) * 1000000L;

    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static void*
bus_dispatch_loop(void* arg)
{
    mm_bus_t* bus = arg;
    while (1) {
        mm_event_t* event = NULL;
        mm_result_t r = mm_bus_pop(bus, &event, UINT32_MAX);
        if (r == MM_OK && event) {
            pthread_mutex_lock(&bus->lock);
            mm_bus_handler_t handler = bus->handler;
            void* user_data = bus->user_data;
            pthread_mutex_unlock(&bus->lock);

            if (handler) {
                handler(bus, event, user_data);
            }
            mm_event_destroy(event);
        } else {
            pthread_mutex_lock(&bus->lock);
            int exit_loop = bus->flushing;
            pthread_mutex_unlock(&bus->lock);
            if (exit_loop) {
                break;
            }
        }
    }
    return NULL;
}

mm_bus_t*
mm_bus_create(void)
{
    mm_bus_t* bus = calloc(1, sizeof(*bus));
    if (!bus) return NULL;

    pthread_mutex_init(&bus->lock, NULL);
    pthread_cond_init(&bus->cond, NULL);
    bus->flushing = 0;
    bus->has_thread = 0;

    return bus;
}

void
mm_bus_destroy(mm_bus_t* bus)
{
    if (!bus) return;

    /* Stop the background dispatch thread if it exists */
    pthread_mutex_lock(&bus->lock);
    bus->flushing = 1;
    pthread_cond_broadcast(&bus->cond);
    int has_thread = bus->has_thread;
    pthread_t thread = bus->thread;
    pthread_mutex_unlock(&bus->lock);

    if (has_thread) {
        pthread_join(thread, NULL);
    }

    /* Free all queued events */
    pthread_mutex_lock(&bus->lock);
    mm_event_node_t* node = bus->head;
    while (node) {
        mm_event_node_t* next = node->next;
        mm_event_destroy(node->event);
        free(node);
        node = next;
    }
    pthread_mutex_unlock(&bus->lock);

    pthread_mutex_destroy(&bus->lock);
    pthread_cond_destroy(&bus->cond);
    free(bus);
}

mm_result_t
mm_bus_post(mm_bus_t* bus, mm_event_t* event)
{
    if (!bus || !event) return MM_ERROR;

    pthread_mutex_lock(&bus->lock);

    if (bus->flushing) {
        pthread_mutex_unlock(&bus->lock);
        return MM_ERROR;
    }

    mm_event_node_t* node = malloc(sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&bus->lock);
        return MM_ERROR;
    }
    node->event = event;
    node->next = NULL;

    if (bus->tail) {
        bus->tail->next = node;
    } else {
        bus->head = node;
    }
    bus->tail = node;
    bus->count++;

    pthread_cond_signal(&bus->cond);
    pthread_mutex_unlock(&bus->lock);
    return MM_OK;
}

mm_result_t
mm_bus_pop(mm_bus_t* bus, mm_event_t** event, uint32_t timeout_ms)
{
    if (!bus || !event) return MM_ERROR;

    pthread_mutex_lock(&bus->lock);

    int use_timeout = (timeout_ms != UINT32_MAX);

    while (!bus->head && !bus->flushing) {
        if (use_timeout) {
            if (timeout_ms == 0) {
                pthread_mutex_unlock(&bus->lock);
                return MM_TIMEOUT;
            }
            struct timespec ts;
            timespec_from_ms(&ts, timeout_ms);
            int ret = pthread_cond_timedwait(&bus->cond, &bus->lock, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&bus->lock);
                return MM_TIMEOUT;
            }
        } else {
            pthread_cond_wait(&bus->cond, &bus->lock);
        }
    }

    if (bus->flushing) {
        pthread_mutex_unlock(&bus->lock);
        return MM_ERROR;
    }

    /* Dequeue */
    mm_event_node_t* node = bus->head;
    bus->head = node->next;
    if (!bus->head) bus->tail = NULL;
    bus->count--;

    *event = node->event;
    free(node);

    pthread_mutex_unlock(&bus->lock);
    return MM_OK;
}

mm_result_t
mm_bus_set_handler(mm_bus_t* bus, mm_bus_handler_t handler, void* user_data)
{
    if (!bus) return MM_ERROR;

    pthread_mutex_lock(&bus->lock);

    /* Stop existing dispatch thread if it exists */
    if (bus->has_thread) {
        bus->flushing = 1;
        pthread_cond_broadcast(&bus->cond);
        pthread_t old_thread = bus->thread;
        pthread_mutex_unlock(&bus->lock);

        pthread_join(old_thread, NULL);

        pthread_mutex_lock(&bus->lock);
        bus->has_thread = 0;
        bus->flushing = 0;
    }

    bus->handler = handler;
    bus->user_data = user_data;

    if (handler) {
        if (pthread_create(&bus->thread, NULL, bus_dispatch_loop, bus) != 0) {
            pthread_mutex_unlock(&bus->lock);
            return MM_ERROR;
        }
        bus->has_thread = 1;
    }

    pthread_mutex_unlock(&bus->lock);
    return MM_OK;
}

mm_event_t*
mm_event_new_eos(mm_element_t* src)
{
    mm_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = MM_EVENT_EOS;
    ev->src = src;
    return ev;
}

mm_event_t*
mm_event_new_error(mm_element_t* src, mm_result_t result, const char* message)
{
    mm_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = MM_EVENT_ERROR;
    ev->src = src;
    ev->as.error.result = result;
    ev->as.error.message = message ? strdup(message) : NULL;
    return ev;
}

mm_event_t*
mm_event_new_state_changed(mm_element_t* src, mm_state_t old_state, mm_state_t new_state)
{
    mm_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = MM_EVENT_STATE_CHANGED;
    ev->src = src;
    ev->as.state_changed.old_state = old_state;
    ev->as.state_changed.new_state = new_state;
    return ev;
}

mm_event_t*
mm_event_new_warning(mm_element_t* src, mm_result_t result, const char* message)
{
    mm_event_t* ev = calloc(1, sizeof(*ev));
    if (!ev) return NULL;
    ev->type = MM_EVENT_WARNING;
    ev->src = src;
    ev->as.warning.result = result;
    ev->as.warning.message = message ? strdup(message) : NULL;
    return ev;
}

void
mm_event_destroy(mm_event_t* event)
{
    if (!event) return;
    if (event->type == MM_EVENT_ERROR) {
        free(event->as.error.message);
    } else if (event->type == MM_EVENT_WARNING) {
        free(event->as.warning.message);
    }
    free(event);
}
