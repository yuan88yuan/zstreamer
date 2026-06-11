/*=============================================================================
    zst_element_factory.h — Element factory metadata and introspection
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_pad.h"
#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ZST_PROPERTY_STRING,
    ZST_PROPERTY_INT,
    ZST_PROPERTY_UINT,
    ZST_PROPERTY_DOUBLE,
    ZST_PROPERTY_BOOL,
    ZST_PROPERTY_ENUM
} zst_property_type_t;

typedef enum {
    ZST_PROPERTY_READABLE = 1u << 0,
    ZST_PROPERTY_WRITABLE = 1u << 1,
    ZST_PROPERTY_RUNTIME  = 1u << 2
} zst_property_flags_t;

typedef struct {
    const char* name;
    zst_property_type_t type;
    uint32_t flags;
    const char* default_value;
    const char* description;
} zst_property_spec_t;

typedef struct {
    const char* name;
    zst_pad_direction_t direction;
    const char* caps;
} zst_pad_template_t;

struct zst_element_desc {
    const char* name;
    const char* long_name;
    const char* category;
    const char* description;
    const char* author;

    const zst_property_spec_t* properties;
    uint32_t nb_properties;

    const zst_pad_template_t* pads;
    uint32_t nb_pads;

    zst_element_t* (*create)(void);
};

zst_element_t* zst_element_factory_make(
    const char* name);

uint32_t zst_element_factory_list(
    const zst_element_desc_t*** elements_out);

void zst_element_factory_list_free(
    const zst_element_desc_t** elements);

const zst_element_desc_t* zst_element_factory_get_desc(
    const char* name);

zst_result_t zst_register_builtin_elements(void);

#ifdef __cplusplus
}
#endif
