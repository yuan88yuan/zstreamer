/*=============================================================================
    zst_allocator.h
=============================================================================*/
#pragma once

#include "zst_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zst_allocator {
    volatile int refcount;

    void* (*alloc)(zst_allocator_t* allocator, size_t size);
    void  (*free)(zst_allocator_t* allocator, void* ptr);
    void  (*destroy)(zst_allocator_t* allocator);

    void* priv;
};

zst_allocator_t* zst_allocator_cpu_create(void);

zst_allocator_t* zst_allocator_ref(zst_allocator_t* allocator);

void zst_allocator_unref(zst_allocator_t* allocator);

void* zst_allocator_alloc(zst_allocator_t* allocator, size_t size);

void zst_allocator_free(zst_allocator_t* allocator, void* ptr);

#ifdef __cplusplus
}
#endif
