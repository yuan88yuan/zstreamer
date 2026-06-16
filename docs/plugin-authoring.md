# Plugin Authoring Guide

This guide explains how to author custom plugins in `zstreamer`, focusing on defining element operations (`zst_element_ops_t`), registering properties, setting up capabilities (caps), and dynamic plugin loading via `zst_plugin_registry_t`.

## 1. Overview of an Element

In `zstreamer`, an element is the fundamental building block of a media pipeline. Elements process data (buffers) passing through pads. A plugin is a loadable module that can contain one or more element factories.

An element consists of:
- Internal state/context (usually a `priv` struct).
- `zst_element_ops_t` containing function pointers for lifecycle events, processing, property management, etc.
- One or more `zst_pad_t` for input (sink pads) or output (src pads).

## 2. Implementing zst_element_ops_t

The `zst_element_ops_t` structure provides callbacks that the `zstreamer` framework calls during pipeline execution.

```c
#include "zst_element.h"

typedef struct {
    // Custom internal state
    int some_property;
} my_element_t;

// State transition: READY -> PAUSED
static zst_result_t my_element_start(zst_element_t* el) {
    my_element_t* my = (my_element_t*)el->priv;
    // Initialize resources before processing begins
    return ZST_SUCCESS;
}

// State transition: PAUSED -> READY
static zst_result_t my_element_stop(zst_element_t* el) {
    // Cleanup resources
    return ZST_SUCCESS;
}

// Buffer processing
static zst_result_t my_element_process(zst_element_t* el, zst_buffer_t* in, zst_buffer_t** out) {
    // Modify 'in' or create a new buffer and assign it to '*out'.
    // If outputting a single buffer, assign to *out.
    // If outputting multiple buffers (e.g., decoder), push them manually via src pads
    // and set *out = NULL.
    // NOTE: NEVER unref 'in' inside process; the framework handles it.

    *out = in;
    zst_buffer_ref(*out); // Assuming we are passing it along
    return ZST_SUCCESS;
}

static const zst_element_ops_t my_element_ops = {
    .name = "my_element",
    .start = my_element_start,
    .stop = my_element_stop,
    .process = my_element_process,
    // Add get_caps, set_property, get_property as needed
};
```

## 3. Property Registration

Properties allow users to configure your element. Implement the `set_property` and `get_property` functions in your `ops`.

```c
static zst_result_t my_element_set_property(zst_element_t* el, const char* name, const char* value) {
    my_element_t* my = (my_element_t*)el->priv;
    if (strcmp(name, "my-prop") == 0) {
        my->some_property = atoi(value);
        return ZST_SUCCESS;
    }
    return ZST_ERR_NOT_FOUND;
}

static zst_result_t my_element_get_property(zst_element_t* el, const char* name, char* value_out, size_t max_len) {
    my_element_t* my = (my_element_t*)el->priv;
    if (strcmp(name, "my-prop") == 0) {
        snprintf(value_out, max_len, "%d", my->some_property);
        return ZST_SUCCESS;
    }
    return ZST_ERR_NOT_FOUND;
}
```

Add them to the `my_element_ops`:
```c
    .set_property = my_element_set_property,
    .get_property = my_element_get_property,
```

## 4. Capabilities Setup

Caps (capabilities) define what formats an element can consume or produce.
Implement `get_caps` in your `ops` to return supported capabilities for a specific pad.

```c
#include "zst_caps.h"

static zst_caps_t* my_element_get_caps(zst_element_t* el, zst_pad_t* pad, const zst_caps_t* filter) {
    zst_caps_t* caps = zst_caps_create();

    // Example: Accept ANY video/x-raw
    zst_caps_struct_t* s = zst_caps_struct_create_video(
        "video/x-raw",
        0, 0, 0.0, "" // 0 or "" means wildcard (ANY)
    );
    zst_caps_append(caps, s);

    if (filter) {
        zst_caps_t* intersected = zst_caps_intersect(caps, filter);
        zst_caps_destroy(caps);
        return intersected;
    }

    return caps;
}
```

Add it to the `my_element_ops`:
```c
    .get_caps = my_element_get_caps,
```

## 5. Dynamic Loading via zst_plugin_registry_t

To make your element discoverable and loadable at runtime, you need to expose it via the plugin API.
Your dynamic library (`.so`) must export specific functions using `ZST_PLUGIN_EXPORT`.

```c
#include "zst_plugin.h"
#include <stdlib.h>
#include <string.h>

// Factory function for creating the element
static zst_element_t* create_my_element(const char* name) {
    if (strcmp(name, "my_element") == 0) {
        my_element_t* priv = calloc(1, sizeof(my_element_t));
        zst_element_t* el = zst_element_create(&my_element_ops, priv);

        // Setup pads
        zst_pad_t* sink_pad = zst_pad_create("sink", ZST_PAD_SINK);
        zst_pad_t* src_pad = zst_pad_create("src", ZST_PAD_SRC);
        zst_element_add_pad(el, sink_pad);
        zst_element_add_pad(el, src_pad);

        return el;
    }
    return NULL;
}

// Describe the elements provided by this plugin
static const zst_element_desc_t my_elements[] = {
    { "my_element", "A demonstration element" }
};

// Export the plugin elements
ZST_PLUGIN_EXPORT const zst_element_desc_t* zst_get_plugin_elements(uint32_t* nb_elements_out) {
    *nb_elements_out = sizeof(my_elements) / sizeof(my_elements[0]);
    return my_elements;
}

// Export the plugin description
static zst_plugin_t my_plugin = {
    .desc = {
        .name = "my_plugin",
        .author = "Author Name",
        .version = "1.0.0",
        .init = NULL,
        .deinit = NULL
    },
    .create_element = create_my_element,
    .refcount = 1
};

ZST_PLUGIN_EXPORT zst_plugin_t* zst_get_plugin(void) {
    return &my_plugin;
}
```

When building your plugin, compile it as a shared library (`libmy_plugin.so`).
You can load it into your application using:
```c
zst_plugin_registry_init();
zst_plugin_registry_scan("path/to/plugins/dir");

zst_element_t* el = zst_element_factory_make("my_element");
```
