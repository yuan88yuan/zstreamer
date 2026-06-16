#ifdef HAS_JETSON
#include "zst_allocator.h"
#include <stdlib.h>
#if __has_include(<nvbuf_utils.h>)
#include <nvbuf_utils.h>
#else
#include "stub_nvbuf_utils.h"
#endif
#include <string.h>

typedef struct {
    int dmabuf_fd;
    size_t size;
    void* mmap_ptr;
} zst_jetson_mem_t;

typedef struct {
    zst_jetson_mem_t* mems;
    size_t mem_capacity;
    size_t mem_count;
    int lock;
} zst_jetson_allocator_t;

static void
jetson_lock(zst_jetson_allocator_t* ctx)
{
    while (__sync_lock_test_and_set(&ctx->lock, 1)) {
        // spin
    }
}

static void
jetson_unlock(zst_jetson_allocator_t* ctx)
{
    __sync_lock_release(&ctx->lock);
}

static int
jetson_add_mem(zst_jetson_allocator_t* ctx, int fd, size_t size, void* ptr)
{
    jetson_lock(ctx);
    if (ctx->mem_count == ctx->mem_capacity) {
        size_t new_cap = ctx->mem_capacity == 0 ? 16 : ctx->mem_capacity * 2;
        zst_jetson_mem_t* new_mems = (zst_jetson_mem_t*)realloc(ctx->mems, new_cap * sizeof(zst_jetson_mem_t));
        if (new_mems) {
            ctx->mems = new_mems;
            ctx->mem_capacity = new_cap;
        } else {
            jetson_unlock(ctx);
            return 0; // Failed to realloc
        }
    }
    ctx->mems[ctx->mem_count].dmabuf_fd = fd;
    ctx->mems[ctx->mem_count].size = size;
    ctx->mems[ctx->mem_count].mmap_ptr = ptr;
    ctx->mem_count++;
    jetson_unlock(ctx);
    return 1;
}

static int
jetson_remove_mem(zst_jetson_allocator_t* ctx, void* ptr, int* out_fd, size_t* out_size)
{
    jetson_lock(ctx);
    for (size_t i = 0; i < ctx->mem_count; i++) {
        if (ctx->mems[i].mmap_ptr == ptr) {
            if (out_fd) *out_fd = ctx->mems[i].dmabuf_fd;
            if (out_size) *out_size = ctx->mems[i].size;

            // Move last element to this position
            if (i < ctx->mem_count - 1) {
                ctx->mems[i] = ctx->mems[ctx->mem_count - 1];
            }
            ctx->mem_count--;
            jetson_unlock(ctx);
            return 1;
        }
    }
    jetson_unlock(ctx);
    return 0;
}

static void*
jetson_alloc(zst_allocator_t* allocator, size_t size)
{
    if (!allocator || !allocator->priv || size == 0) return NULL;
    zst_jetson_allocator_t* ctx = (zst_jetson_allocator_t*)allocator->priv;

    int dmabuf_fd = -1;
    NvBufferCreateParams input_params = {0};

    // To allocate a linear 1D buffer of a specific size with NvBuffer:
    input_params.payloadType = NvBufferPayload_MemHandle;
    input_params.memsize = size;
    input_params.layout = NvBufferLayout_Pitch;
    input_params.colorFormat = NvBufferColorFormat_GRAY8;
    input_params.nvbuf_tag = NvBufferTag_NONE;

    if (NvBufferCreateEx(&dmabuf_fd, &input_params) != 0) {
        return NULL;
    }

    void* pVirtAddr = NULL;
    if (NvBufferMemMap(dmabuf_fd, 0, NvBufferMem_Read_Write, &pVirtAddr) != 0) {
        NvBufferDestroy(dmabuf_fd);
        return NULL;
    }

    // Need to sync for CPU if we are returning a CPU visible pointer
    if (NvBufferMemSyncForCpu(dmabuf_fd, 0, &pVirtAddr) != 0) {
        NvBufferMemUnMap(dmabuf_fd, 0, &pVirtAddr);
        NvBufferDestroy(dmabuf_fd);
        return NULL;
    }

    if (!jetson_add_mem(ctx, dmabuf_fd, size, pVirtAddr)) {
        NvBufferMemUnMap(dmabuf_fd, 0, &pVirtAddr);
        NvBufferDestroy(dmabuf_fd);
        return NULL;
    }
    return pVirtAddr;
}

static void
jetson_free(zst_allocator_t* allocator, void* ptr)
{
    if (!allocator || !allocator->priv || !ptr) return;
    zst_jetson_allocator_t* ctx = (zst_jetson_allocator_t*)allocator->priv;

    int dmabuf_fd;
    size_t size;
    if (jetson_remove_mem(ctx, ptr, &dmabuf_fd, &size)) {
        NvBufferMemUnMap(dmabuf_fd, 0, &ptr);
        NvBufferDestroy(dmabuf_fd);
    }
}

static void
jetson_destroy(zst_allocator_t* allocator)
{
    if (!allocator || !allocator->priv) return;
    zst_jetson_allocator_t* ctx = (zst_jetson_allocator_t*)allocator->priv;
    if (ctx) {
        // Free any remaining memory mappings
        for (size_t i = 0; i < ctx->mem_count; i++) {
            void* pVirtAddr = ctx->mems[i].mmap_ptr;
            NvBufferMemUnMap(ctx->mems[i].dmabuf_fd, 0, &pVirtAddr);
            NvBufferDestroy(ctx->mems[i].dmabuf_fd);
        }
        free(ctx->mems);
        free(ctx);
    }
    allocator->priv = NULL;
}

extern "C" zst_allocator_t*
zst_allocator_jetson_create(void)
{
    // Try to create a dummy buffer to see if NvBuffer API is available/working
    int dmabuf_fd = -1;
    NvBufferCreateParams input_params = {0};
    input_params.payloadType = NvBufferPayload_MemHandle;
    input_params.memsize = 4096;
    input_params.layout = NvBufferLayout_Pitch;
    input_params.colorFormat = NvBufferColorFormat_GRAY8;
    input_params.nvbuf_tag = NvBufferTag_NONE;

    if (NvBufferCreateEx(&dmabuf_fd, &input_params) != 0) {
        return NULL; // Hardware not available or API failed
    }
    NvBufferDestroy(dmabuf_fd);

    zst_allocator_t* alloc = (zst_allocator_t*)calloc(1, sizeof(zst_allocator_t));
    if (!alloc) return NULL;

    zst_jetson_allocator_t* ctx = (zst_jetson_allocator_t*)calloc(1, sizeof(zst_jetson_allocator_t));
    if (!ctx) {
        free(alloc);
        return NULL;
    }

    alloc->refcount = 1;
    alloc->alloc    = jetson_alloc;
    alloc->free     = jetson_free;
    alloc->destroy  = jetson_destroy;
    alloc->priv     = ctx;

    return alloc;
}

extern "C" int
zst_allocator_jetson_get_fd(zst_allocator_t* allocator, void* ptr)
{
    if (!allocator || !allocator->priv || !ptr) return -1;
    if (allocator->alloc != jetson_alloc) return -1;

    zst_jetson_allocator_t* ctx = (zst_jetson_allocator_t*)allocator->priv;
    int fd = -1;

    jetson_lock(ctx);
    for (size_t i = 0; i < ctx->mem_count; i++) {
        if (ctx->mems[i].mmap_ptr == ptr) {
            fd = ctx->mems[i].dmabuf_fd;
            break;
        }
    }
    jetson_unlock(ctx);
    return fd;
}
#endif // HAS_JETSON
