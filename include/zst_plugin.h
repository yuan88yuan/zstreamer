/*=============================================================================
    zst_plugin.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"
#include "zst_element_factory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {

    const char* name;
    const char* author;
    const char* version;

    zst_result_t (*init)(void);
    void (*deinit)(void);

} zst_plugin_desc_t;

typedef zst_element_t* (*zst_create_element_fn)(
    const char* name);

struct zst_plugin {

    zst_plugin_desc_t desc;

    zst_create_element_fn create_element;

    void* priv;

    int refcount;
};

typedef zst_plugin_t* (*zst_get_plugin_fn)(void);

typedef const zst_element_desc_t* (*zst_get_plugin_elements_fn)(
    uint32_t* nb_elements_out);

#define ZST_PLUGIN_EXPORT \
    __attribute__((visibility("default")))

zst_plugin_t* zst_plugin_load(const char* path);

void zst_plugin_unload(zst_plugin_t* plugin);

zst_plugin_t* zst_plugin_ref(
    zst_plugin_t* plugin);

void zst_plugin_unref(
    zst_plugin_t* plugin);

zst_result_t zst_plugin_registry_init(void);

void zst_plugin_registry_deinit(void);

zst_result_t zst_plugin_registry_scan(
    const char* directory);

zst_result_t zst_plugin_registry_scan_env(void);

ZST_PLUGIN_EXPORT
zst_plugin_t* zst_get_plugin(void);

ZST_PLUGIN_EXPORT
const zst_element_desc_t* zst_get_plugin_elements(
    uint32_t* nb_elements_out);

#ifdef __cplusplus
}
#endif
