/*=============================================================================
    zst_bus.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"
#include "zst_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_EVENT_EOS,
    ZST_EVENT_ERROR,
    ZST_EVENT_STATE_CHANGED,
    ZST_EVENT_WARNING,
    ZST_EVENT_SEGMENT
} zst_event_type_t;

struct zst_event {
    zst_event_type_t type;
    zst_element_t* src;

    union {
        struct {
            zst_result_t result;
            char* message;
        } error;

        struct {
            zst_state_t old_state;
            zst_state_t new_state;
        } state_changed;

        struct {
            zst_result_t result;
            char* message;
        } warning;

        zst_segment_t segment;
    } as;
};

typedef void (*zst_bus_handler_t)(
    zst_bus_t* bus,
    zst_event_t* event,
    void* user_data);

zst_bus_t* zst_bus_create(void);

void zst_bus_destroy(
    zst_bus_t* bus);

zst_result_t zst_bus_post(
    zst_bus_t* bus,
    zst_event_t* event);

zst_result_t zst_bus_pop(
    zst_bus_t* bus,
    zst_event_t** event,
    uint32_t timeout_ms);

zst_result_t zst_bus_set_handler(
    zst_bus_t* bus,
    zst_bus_handler_t handler,
    void* user_data);

zst_event_t* zst_event_new_eos(
    zst_element_t* src);

zst_event_t* zst_event_new_error(
    zst_element_t* src,
    zst_result_t result,
    const char* message);

zst_event_t* zst_event_new_state_changed(
    zst_element_t* src,
    zst_state_t old_state,
    zst_state_t new_state);

zst_event_t* zst_event_new_warning(
    zst_element_t* src,
    zst_result_t result,
    const char* message);

zst_event_t* zst_event_new_segment(
    zst_element_t* src,
    const zst_segment_t* segment);

void zst_event_destroy(
    zst_event_t* event);

#ifdef __cplusplus
}
#endif
