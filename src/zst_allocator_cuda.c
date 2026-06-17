/*=============================================================================
    zst_allocator_cuda.c
=============================================================================*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "zst_allocator.h"
#include <stdlib.h>

#ifdef HAS_CUDA
#include <cuda_runtime_api.h>

static void*
cuda_alloc(zst_allocator_t* allocator, size_t size)
{
    (void)allocator;
    void* ptr = NULL;
    if (cudaMalloc(&ptr, size) != cudaSuccess) {
        return NULL;
    }
    return ptr;
}

static void
cuda_free(zst_allocator_t* allocator, void* ptr)
{
    (void)allocator;
    if (ptr) {
        cudaFree(ptr);
    }
}

static void
cuda_destroy(zst_allocator_t* allocator)
{
    (void)allocator;
}

zst_allocator_t*
zst_allocator_cuda_create(void)
{
    zst_allocator_t* alloc = calloc(1, sizeof(*alloc));
    if (!alloc) return NULL;

    alloc->refcount = 1;
    alloc->alloc    = cuda_alloc;
    alloc->free     = cuda_free;
    alloc->destroy  = cuda_destroy;
    alloc->priv     = NULL;

    return alloc;
}
#else
zst_allocator_t*
zst_allocator_cuda_create(void)
{
    return NULL;
}
#endif
