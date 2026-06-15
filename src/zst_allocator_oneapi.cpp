/*=============================================================================
    zst_allocator_oneapi.cpp
=============================================================================*/

#ifdef HAS_ONEAPI
#include <sycl/sycl.hpp>
#include <stdlib.h>
#include <string>

#include "zst_allocator.h"

struct zst_oneapi_allocator_t {
    sycl::queue queue;
};

static void*
oneapi_alloc(zst_allocator_t* allocator, size_t size)
{
    if (!allocator || !allocator->priv || size == 0) return nullptr;
    zst_oneapi_allocator_t* ctx = (zst_oneapi_allocator_t*)allocator->priv;

    try {
        void* ptr = sycl::malloc_device(size, ctx->queue);
        return ptr;
    } catch (...) {
        return nullptr;
    }
}

static void
oneapi_free(zst_allocator_t* allocator, void* ptr)
{
    if (!allocator || !allocator->priv || !ptr) return;
    zst_oneapi_allocator_t* ctx = (zst_oneapi_allocator_t*)allocator->priv;

    try {
        sycl::free(ptr, ctx->queue);
    } catch (...) {
        // Suppress exceptions in free callback
    }
}

static void
oneapi_destroy(zst_allocator_t* allocator)
{
    if (!allocator || !allocator->priv) return;
    zst_oneapi_allocator_t* ctx = (zst_oneapi_allocator_t*)allocator->priv;

    try {
        // sycl::queue destructor automatically cleans up resources
        delete ctx;
    } catch (...) {
        // Suppress exceptions in destructor
    }
    allocator->priv = nullptr;
}

extern "C" zst_allocator_t*
zst_allocator_oneapi_create(void)
{
    try {
        sycl::device dev;
        bool found_intel_gpu = false;

        // Try to find an Intel GPU first
        auto devices = sycl::device::get_devices(sycl::info::device_type::gpu);
        for (const auto& d : devices) {
            std::string vendor = d.get_info<sycl::info::device::vendor>();
            if (vendor.find("Intel") != std::string::npos) {
                dev = d;
                found_intel_gpu = true;
                break;
            }
        }

        // Fallback to default GPU, or any default selector
        if (!found_intel_gpu) {
            try {
                dev = sycl::device(sycl::gpu_selector_v);
            } catch (...) {
                dev = sycl::device(sycl::default_selector_v);
            }
        }

        sycl::queue q(dev, sycl::property_list{sycl::property::queue::in_order()});
        zst_oneapi_allocator_t* ctx = new zst_oneapi_allocator_t{ q };

        zst_allocator_t* alloc = (zst_allocator_t*)calloc(1, sizeof(zst_allocator_t));
        if (!alloc) {
            delete ctx;
            return nullptr;
        }

        alloc->refcount = 1;
        alloc->alloc    = oneapi_alloc;
        alloc->free     = oneapi_free;
        alloc->destroy  = oneapi_destroy;
        alloc->priv     = ctx;

        return alloc;

    } catch (...) {
        return nullptr;
    }
}
#endif // HAS_ONEAPI
