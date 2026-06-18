/*=============================================================================
    zst_allocator.c
=============================================================================*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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

#ifndef HAS_JETSON
zst_allocator_t*
zst_allocator_jetson_create(void)
{
    return NULL;
}

int
zst_allocator_jetson_get_fd(zst_allocator_t* allocator, void* ptr)
{
    (void)allocator;
    (void)ptr;
    return -1;
}
#endif

#ifndef HAS_ONEAPI
zst_allocator_t*
zst_allocator_oneapi_create(void)
{
    return NULL;
}
#endif

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

#ifdef HAS_VULKAN
#include <vulkan/vulkan.h>

typedef struct {
    VkDeviceMemory memory;
    VkBuffer buffer;
    void* mapped_ptr;
    size_t size;
} zst_vulkan_mem_t;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t memory_type_index;

    zst_vulkan_mem_t* mems;
    size_t mem_capacity;
    size_t mem_count;
    int lock;
} zst_vulkan_allocator_t;

static void
vulkan_lock(zst_vulkan_allocator_t* ctx)
{
    while (__sync_lock_test_and_set(&ctx->lock, 1)) {
        // spin
    }
}

static void
vulkan_unlock(zst_vulkan_allocator_t* ctx)
{
    __sync_lock_release(&ctx->lock);
}

static int
vulkan_add_mem(zst_vulkan_allocator_t* ctx, VkDeviceMemory memory, VkBuffer buffer, void* ptr, size_t size)
{
    vulkan_lock(ctx);
    if (ctx->mem_count == ctx->mem_capacity) {
        size_t new_cap = ctx->mem_capacity == 0 ? 16 : ctx->mem_capacity * 2;
        zst_vulkan_mem_t* new_mems = realloc(ctx->mems, new_cap * sizeof(zst_vulkan_mem_t));
        if (new_mems) {
            ctx->mems = new_mems;
            ctx->mem_capacity = new_cap;
        } else {
            vulkan_unlock(ctx);
            return 0; // Failed to realloc
        }
    }
    ctx->mems[ctx->mem_count].memory = memory;
    ctx->mems[ctx->mem_count].buffer = buffer;
    ctx->mems[ctx->mem_count].mapped_ptr = ptr;
    ctx->mems[ctx->mem_count].size = size;
    ctx->mem_count++;
    vulkan_unlock(ctx);
    return 1;
}

static int
vulkan_remove_mem(zst_vulkan_allocator_t* ctx, void* ptr, VkDeviceMemory* out_memory, VkBuffer* out_buffer, size_t* out_size)
{
    vulkan_lock(ctx);
    for (size_t i = 0; i < ctx->mem_count; i++) {
        if (ctx->mems[i].mapped_ptr == ptr) {
            if (out_memory) *out_memory = ctx->mems[i].memory;
            if (out_buffer) *out_buffer = ctx->mems[i].buffer;
            if (out_size) *out_size = ctx->mems[i].size;

            // Move last element to this position
            if (i < ctx->mem_count - 1) {
                ctx->mems[i] = ctx->mems[ctx->mem_count - 1];
            }
            ctx->mem_count--;
            vulkan_unlock(ctx);
            return 1;
        }
    }
    vulkan_unlock(ctx);
    return 0;
}

static void*
vulkan_alloc(zst_allocator_t* allocator, size_t size)
{
    zst_vulkan_allocator_t* ctx = (zst_vulkan_allocator_t*)allocator->priv;

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer;
    if (vkCreateBuffer(ctx->device, &bufferInfo, NULL, &buffer) != VK_SUCCESS) {
        return NULL;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(ctx->device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = ctx->memory_type_index;

    VkDeviceMemory memory;
    if (vkAllocateMemory(ctx->device, &allocInfo, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(ctx->device, buffer, NULL);
        return NULL;
    }

    vkBindBufferMemory(ctx->device, buffer, memory, 0);

    void* mapped_ptr;
    if (vkMapMemory(ctx->device, memory, 0, size, 0, &mapped_ptr) != VK_SUCCESS) {
        vkFreeMemory(ctx->device, memory, NULL);
        vkDestroyBuffer(ctx->device, buffer, NULL);
        return NULL;
    }

    if (!vulkan_add_mem(ctx, memory, buffer, mapped_ptr, size)) {
        vkUnmapMemory(ctx->device, memory);
        vkFreeMemory(ctx->device, memory, NULL);
        vkDestroyBuffer(ctx->device, buffer, NULL);
        return NULL;
    }

    return mapped_ptr;
}

static void
vulkan_free(zst_allocator_t* allocator, void* ptr)
{
    zst_vulkan_allocator_t* ctx = (zst_vulkan_allocator_t*)allocator->priv;
    VkDeviceMemory memory;
    VkBuffer buffer;
    size_t size;
    if (vulkan_remove_mem(ctx, ptr, &memory, &buffer, &size)) {
        vkUnmapMemory(ctx->device, memory);
        vkFreeMemory(ctx->device, memory, NULL);
        vkDestroyBuffer(ctx->device, buffer, NULL);
    }
}

static void
vulkan_destroy(zst_allocator_t* allocator)
{
    zst_vulkan_allocator_t* ctx = (zst_vulkan_allocator_t*)allocator->priv;
    if (ctx) {
        // Free any remaining memory mappings
        for (size_t i = 0; i < ctx->mem_count; i++) {
            vkUnmapMemory(ctx->device, ctx->mems[i].memory);
            vkFreeMemory(ctx->device, ctx->mems[i].memory, NULL);
            vkDestroyBuffer(ctx->device, ctx->mems[i].buffer, NULL);
        }
        free(ctx->mems);

        vkDestroyDevice(ctx->device, NULL);
        vkDestroyInstance(ctx->instance, NULL);

        free(ctx);
    }
}

zst_allocator_t*
zst_allocator_vulkan_create(void)
{
    zst_allocator_t* alloc = calloc(1, sizeof(*alloc));
    if (!alloc) return NULL;

    zst_vulkan_allocator_t* ctx = calloc(1, sizeof(zst_vulkan_allocator_t));
    if (!ctx) {
        free(alloc);
        return NULL;
    }

    VkApplicationInfo appInfo = {0};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Zstreamer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo = {0};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&createInfo, NULL, &ctx->instance) != VK_SUCCESS) {
        free(ctx);
        free(alloc);
        return NULL;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, NULL);

    if (deviceCount == 0) {
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        free(alloc);
        return NULL;
    }

    VkPhysicalDevice* devices = malloc(sizeof(VkPhysicalDevice) * deviceCount);
    vkEnumeratePhysicalDevices(ctx->instance, &deviceCount, devices);
    ctx->physical_device = devices[0]; // just pick the first one for simplicity
    free(devices);

    VkDeviceQueueCreateInfo queueCreateInfo = {0};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = 0; // Assume queue family 0 exists
    queueCreateInfo.queueCount = 1;
    float queuePriority = 1.0f;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo deviceCreateInfo = {0};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.queueCreateInfoCount = 1;

    if (vkCreateDevice(ctx->physical_device, &deviceCreateInfo, NULL, &ctx->device) != VK_SUCCESS) {
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        free(alloc);
        return NULL;
    }

    // Find memory type index for host visible & coherent memory
    // To do this properly, we could create a dummy buffer and get its memory requirements,
    // but typically memory type indices for host visible memory don't strictly require a specific buffer's typeFilter if we just want a generic host memory pool.
    // Actually, we'll just query memory properties and find the first one with the flags.
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memProperties);
    ctx->memory_type_index = -1;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memProperties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            ctx->memory_type_index = i;
            break;
        }
    }

    if (ctx->memory_type_index == (uint32_t)-1) {
        vkDestroyDevice(ctx->device, NULL);
        vkDestroyInstance(ctx->instance, NULL);
        free(ctx);
        free(alloc);
        return NULL;
    }

    alloc->refcount = 1;
    alloc->alloc    = vulkan_alloc;
    alloc->free     = vulkan_free;
    alloc->destroy  = vulkan_destroy;
    alloc->priv     = ctx;

    return alloc;
}
#else
zst_allocator_t*
zst_allocator_vulkan_create(void)
{
    return NULL;
}
#endif
