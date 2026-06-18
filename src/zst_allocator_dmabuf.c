/*=============================================================================
    zst_allocator_dmabuf.c
=============================================================================*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "zst_allocator.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#if defined(__has_include)
#  if __has_include(<linux/dma-heap.h>)
#    include <linux/dma-heap.h>
#  endif
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif

typedef struct {
    int fd;
    size_t size;
    void* mmap_ptr;
} zst_dmabuf_mem_t;

typedef struct {
    zst_dmabuf_mem_t* mems;
    size_t mem_capacity;
    size_t mem_count;
    int lock;
} zst_dmabuf_allocator_t;

static void
dmabuf_lock(zst_dmabuf_allocator_t* ctx)
{
    while (__sync_lock_test_and_set(&ctx->lock, 1)) {
        // spin
    }
}

static void
dmabuf_unlock(zst_dmabuf_allocator_t* ctx)
{
    __sync_lock_release(&ctx->lock);
}

static int
dmabuf_add_mem(zst_dmabuf_allocator_t* ctx, int fd, size_t size, void* ptr)
{
    dmabuf_lock(ctx);
    if (ctx->mem_count == ctx->mem_capacity) {
        size_t new_cap = ctx->mem_capacity == 0 ? 16 : ctx->mem_capacity * 2;
        zst_dmabuf_mem_t* new_mems = realloc(ctx->mems, new_cap * sizeof(zst_dmabuf_mem_t));
        if (new_mems) {
            ctx->mems = new_mems;
            ctx->mem_capacity = new_cap;
        } else {
            dmabuf_unlock(ctx);
            return 0; // Failed to realloc
        }
    }
    ctx->mems[ctx->mem_count].fd = fd;
    ctx->mems[ctx->mem_count].size = size;
    ctx->mems[ctx->mem_count].mmap_ptr = ptr;
    ctx->mem_count++;
    dmabuf_unlock(ctx);
    return 1;
}

static int
dmabuf_remove_mem(zst_dmabuf_allocator_t* ctx, void* ptr, int* out_fd, size_t* out_size)
{
    dmabuf_lock(ctx);
    for (size_t i = 0; i < ctx->mem_count; i++) {
        if (ctx->mems[i].mmap_ptr == ptr) {
            if (out_fd) *out_fd = ctx->mems[i].fd;
            if (out_size) *out_size = ctx->mems[i].size;

            // Move last element to this position
            if (i < ctx->mem_count - 1) {
                ctx->mems[i] = ctx->mems[ctx->mem_count - 1];
            }
            ctx->mem_count--;
            dmabuf_unlock(ctx);
            return 1;
        }
    }
    dmabuf_unlock(ctx);
    return 0;
}

static int
dmabuf_alloc_heap_fd(size_t size)
{
    const char* env_heap = getenv("ZST_DMABUF_HEAP");
    const char* heaps[] = {
        env_heap,
        "/dev/dma_heap/system",
        "/dev/dma_heap/system-uncached",
        "/dev/dma_heap/linux,cma",
        "/dev/dma_heap/cma",
        "/dev/dma_heap/reserved",
        "/dev/dma_heap/default_cma_region"
    };

    for (size_t i = 0; i < sizeof(heaps) / sizeof(heaps[0]); i++) {
        if (!heaps[i] || heaps[i][0] == '\0') continue;

        int heap_fd = open(heaps[i], O_RDWR | O_CLOEXEC);
        if (heap_fd < 0) continue;

        struct dma_heap_allocation_data data;
        memset(&data, 0, sizeof(data));
        data.len = size;
        data.fd_flags = O_RDWR | O_CLOEXEC;
        data.heap_flags = 0;

        if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) == 0) {
            close(heap_fd);
            return (int)data.fd;
        }

        close(heap_fd);
    }

    return -1;
}

static int
dmabuf_alloc_memfd(size_t size)
{
    int fd = memfd_create("zst_dmabuf", MFD_ALLOW_SEALING);
    if (fd < 0) return -1;

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void*
dmabuf_alloc(zst_allocator_t* allocator, size_t size)
{
    zst_dmabuf_allocator_t* ctx = (zst_dmabuf_allocator_t*)allocator->priv;

    /* Prefer a real dma-buf heap when available so V4L2_MEMORY_DMABUF
     * simulation tests (e.g. vivid) receive importable dma-buf fds.  Fall
     * back to memfd for allocator unit tests on hosts without dma heaps. */
    int fd = dmabuf_alloc_heap_fd(size);
    if (fd < 0) {
        fd = dmabuf_alloc_memfd(size);
    }
    if (fd < 0) return NULL;

    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    if (!dmabuf_add_mem(ctx, fd, size, ptr)) {
        munmap(ptr, size);
        close(fd);
        return NULL;
    }
    return ptr;
}

static void
dmabuf_free(zst_allocator_t* allocator, void* ptr)
{
    zst_dmabuf_allocator_t* ctx = (zst_dmabuf_allocator_t*)allocator->priv;
    int fd;
    size_t size;
    if (dmabuf_remove_mem(ctx, ptr, &fd, &size)) {
        munmap(ptr, size);
        close(fd);
    }
}

static void
dmabuf_destroy(zst_allocator_t* allocator)
{
    zst_dmabuf_allocator_t* ctx = (zst_dmabuf_allocator_t*)allocator->priv;
    if (ctx) {
        // Free any remaining memory mappings
        for (size_t i = 0; i < ctx->mem_count; i++) {
            munmap(ctx->mems[i].mmap_ptr, ctx->mems[i].size);
            close(ctx->mems[i].fd);
        }
        free(ctx->mems);
        free(ctx);
    }
}

zst_allocator_t*
zst_allocator_dmabuf_create(void)
{
    zst_allocator_t* alloc = calloc(1, sizeof(*alloc));
    if (!alloc) return NULL;

    zst_dmabuf_allocator_t* ctx = calloc(1, sizeof(zst_dmabuf_allocator_t));
    if (!ctx) {
        free(alloc);
        return NULL;
    }

    alloc->refcount = 1;
    alloc->alloc    = dmabuf_alloc;
    alloc->free     = dmabuf_free;
    alloc->destroy  = dmabuf_destroy;
    alloc->priv     = ctx;

    return alloc;
}

void*
zst_allocator_dmabuf_import(zst_allocator_t* allocator, int fd, size_t size)
{
    if (!allocator || allocator->alloc != dmabuf_alloc || fd < 0) return NULL;
    zst_dmabuf_allocator_t* ctx = (zst_dmabuf_allocator_t*)allocator->priv;

    // Duplicate the file descriptor so we can manage its lifecycle safely
    int dup_fd = dup(fd);
    if (dup_fd < 0) return NULL;

    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dup_fd, 0);
    if (ptr == MAP_FAILED) {
        close(dup_fd);
        return NULL;
    }

    if (!dmabuf_add_mem(ctx, dup_fd, size, ptr)) {
        munmap(ptr, size);
        close(dup_fd);
        return NULL;
    }
    return ptr;
}

int
zst_allocator_dmabuf_get_fd(zst_allocator_t* allocator, void* ptr)
{
    if (!allocator || allocator->alloc != dmabuf_alloc || !ptr) return -1;
    zst_dmabuf_allocator_t* ctx = (zst_dmabuf_allocator_t*)allocator->priv;

    int fd = -1;
    dmabuf_lock(ctx);
    for (size_t i = 0; i < ctx->mem_count; i++) {
        if (ctx->mems[i].mmap_ptr == ptr) {
            fd = ctx->mems[i].fd;
            break;
        }
    }
    dmabuf_unlock(ctx);
    return fd;
}
