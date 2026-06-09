/*=============================================================================
    mm_bus.h
=============================================================================*/
#pragma once

#include "mm_types.h"
#include "mm_element.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MM_EVENT_EOS,
    MM_EVENT_ERROR,
    MM_EVENT_STATE_CHANGED,
    MM_EVENT_WARNING
} mm_event_type_t;

struct mm_event {
    mm_event_type_t type;
    mm_element_t* src;

    union {
        struct {
            mm_result_t result;
            char* message;
        } error;

        struct {
            mm_state_t old_state;
            mm_state_t new_state;
        } state_changed;

        struct {
            mm_result_t result;
            char* message;
        } warning;
    } as;
};

typedef void (*mm_bus_handler_t)(
    mm_bus_t* bus,
    mm_event_t* event,
    void* user_data);

mm_bus_t* mm_bus_create(void);

void mm_bus_destroy(
    mm_bus_t* bus);

mm_result_t mm_bus_post(
    mm_bus_t* bus,
    mm_event_t* event);

mm_result_t mm_bus_pop(
    mm_bus_t* bus,
    mm_event_t** event,
    uint32_t timeout_ms);

mm_result_t mm_bus_set_handler(
    mm_bus_t* bus,
    mm_bus_handler_t handler,
    void* user_data);

mm_event_t* mm_event_new_eos(
    mm_element_t* src);

mm_event_t* mm_event_new_error(
    mm_element_t* src,
    mm_result_t result,
    const char* message);

mm_event_t* mm_event_new_state_changed(
    mm_element_t* src,
    mm_state_t old_state,
    mm_state_t new_state);

mm_event_t* mm_event_new_warning(
    mm_element_t* src,
    mm_result_t result,
    const char* message);

void mm_event_destroy(
    mm_event_t* event);

#ifdef __cplusplus
}
#endif
