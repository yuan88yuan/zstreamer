/*=============================================================================
    zst_allocator.c
=============================================================================*/

#include "zst_allocator.h"
#include <stdlib.h>

zst_allocator_t*
zst_allocator_ref(zst_allocator_t* allocator)
{
    if (!allocator) return NULL;
    __sync_fetch_and_add(&allocator->refcount, 1);
    return allocator;
}

void
zst_allocator_unref(zst_allocator_t* allocator)
{
    if (!allocator) return;
    if (__sync_sub_and_fetch(&allocator->refcount, 1) > 0)
        return;

    if (allocator->destroy)
        allocator->destroy(allocator);

    free(allocator);
}

void*
zst_allocator_alloc(zst_allocator_t* allocator, size_t size)
{
    if (!allocator || !allocator->alloc) return NULL;
    return allocator->alloc(allocator, size);
}

void
zst_allocator_free(zst_allocator_t* allocator, void* ptr)
{
    if (!allocator || !allocator->free || !ptr) return;
    allocator->free(allocator, ptr);
}

static void*
cpu_alloc(zst_allocator_t* allocator, size_t size)
{
    (void)allocator;
    return malloc(size);
}

static void
cpu_free(zst_allocator_t* allocator, void* ptr)
{
    (void)allocator;
    free(ptr);
}

static void
cpu_destroy(zst_allocator_t* allocator)
{
    (void)allocator;
}

zst_allocator_t*
zst_allocator_cpu_create(void)
{
    zst_allocator_t* alloc = calloc(1, sizeof(*alloc));
    if (!alloc) return NULL;

    alloc->refcount = 1;
    alloc->alloc    = cpu_alloc;
    alloc->free     = cpu_free;
    alloc->destroy  = cpu_destroy;
    alloc->priv     = NULL;

    return alloc;
}
