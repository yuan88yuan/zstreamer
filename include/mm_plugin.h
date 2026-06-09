/*=============================================================================
    mm_plugin.h
=============================================================================*/
#pragma once

#include "mm_types.h"
#include "mm_element.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {

    const char* name;
    const char* author;
    const char* version;

    mm_result_t (*init)(void);
    void (*deinit)(void);

} mm_plugin_desc_t;

typedef mm_element_t* (*mm_create_element_fn)(
    const char* name);

struct mm_plugin {

    mm_plugin_desc_t desc;

    mm_create_element_fn create_element;

    void* priv;
};

typedef mm_plugin_t* (*mm_get_plugin_fn)(void);

#define MM_PLUGIN_EXPORT \
    __attribute__((visibility("default")))

MM_PLUGIN_EXPORT
mm_plugin_t* mm_get_plugin(void);

#ifdef __cplusplus
}
#endif