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

    int refcount;
};

typedef mm_plugin_t* (*mm_get_plugin_fn)(void);

#define MM_PLUGIN_EXPORT \
    __attribute__((visibility("default")))

mm_plugin_t* mm_plugin_load(const char* path);

void mm_plugin_unload(mm_plugin_t* plugin);

mm_plugin_t* mm_plugin_ref(
    mm_plugin_t* plugin);

void mm_plugin_unref(
    mm_plugin_t* plugin);

mm_result_t mm_plugin_registry_init(void);

void mm_plugin_registry_deinit(void);

mm_result_t mm_plugin_registry_scan(
    const char* directory);

mm_result_t mm_plugin_registry_scan_env(void);

mm_element_t* mm_element_factory_make(
    const char* name);

MM_PLUGIN_EXPORT
mm_plugin_t* mm_get_plugin(void);

#ifdef __cplusplus
}
#endif