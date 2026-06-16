# Allocator + Zero-Copy Guide

zstreamer is designed for high-performance multimedia processing, which requires minimizing memory copying and allocation overhead during the active `PLAYING` state. This is achieved through a robust allocator interface (`zst_allocator_t`) and buffer pools (`zst_buffer_pool_t`).

## `zst_allocator_t` Interface

The `zst_allocator_t` provides an abstraction over different types of memory. It defines a standard interface for allocating, freeing, mapping, and unmapping memory blocks.

zstreamer supports several allocator backends:
-   **CPU (Malloc):** Standard system memory allocated via `malloc`.
-   **DMABUF:** Linux Direct Memory Access Buffers, allowing zero-copy sharing of memory between hardware devices (e.g., camera to encoder).
-   **Vulkan/CUDA/oneAPI:** Hardware-specific allocators for GPU processing.

By abstracting the memory source, elements can work with buffers without knowing their underlying physical location, while still enabling zero-copy pipelines when compatible allocators are used.

## `zst_buffer_pool_t` for Allocation Prevention

Dynamic memory allocation (`malloc`/`free`) is a common source of performance jitter and latency. zstreamer avoids this during pipeline execution by using buffer pools.

A `zst_buffer_pool_t` is an object that manages a fixed number of pre-allocated buffers.

1.  **Topology-Aware Sizing:** Before transitioning to the `PLAYING` state, the pipeline analyzes its topology and the negotiated capabilities to determine the required size and number of buffers for each pool.
2.  **Pre-allocation:** The buffer pool allocates the required memory blocks using its configured `zst_allocator_t` during the `READY` -> `PAUSED`/`PLAYING` transition.
3.  **Recycling:** During playback, when a source element needs a buffer, it requests one from the pool using `zst_buffer_pool_acquire()`. If the pool is empty, the element waits (blocks) until a buffer is returned.
4.  **Zero-Allocation Path:** When a buffer's reference count drops to zero, it is not freed back to the system. Instead, it is automatically returned to its originating pool, ready to be reused. This guarantees zero dynamic allocations in the steady-state `PLAYING` phase.

## DMABUF and Device-Specific Memory

The `zst_allocator_dmabuf_t` is crucial for zero-copy hardware acceleration on Linux.

-   **Creation:** A DMABUF allocator is typically backed by `memfd_create` (for software fallback) or provided directly by a hardware driver (like V4L2 or a DRM driver).
-   **Exporting:** Memory allocated via DMABUF can be exported as a standard file descriptor (`zst_allocator_dmabuf_get_fd()`).
-   **Zero-Copy Sharing:** This file descriptor can be passed to other processes or hardware blocks (e.g., a hardware video encoder). The hardware can directly read from or write to the physical memory backing the DMABUF, bypassing the CPU entirely and achieving true zero-copy data transfer.

By combining `zst_allocator_t` backends with `zst_buffer_pool_t`, zstreamer provides predictable, low-latency performance suitable for demanding real-time streaming applications.
