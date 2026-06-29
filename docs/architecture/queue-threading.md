@page architecture_queue_threading Queue Threading Model Explainer

# Queue Threading Model Explainer

In zstreamer, pipeline execution is primarily driven by the `zst_scheduler_t`. By default, simple pipelines may run entirely inline within a single thread. However, complex pipelines, especially those with multiple branches (e.g., separating audio and video processing), require concurrent execution to utilize multi-core processors efficiently and prevent slow branches from blocking fast ones.

The `zst_queue_t` and `zst_queue_element_t` are the mechanisms used to introduce threading and decouple pipeline execution.

## Decoupling Pipeline Branches

A queue acts as a buffer between two elements. It introduces a thread boundary: the element pushing data into the queue runs on one thread, and the element pulling data from the queue runs on another.

When you insert a `queue` element into a pipeline (e.g., `source -> queue -> sink`):

1.  **Upstream Thread:** The upstream source pushes buffers into the queue. This push operation is fast and non-blocking as long as the queue isn't full.
2.  **Queue Element:** The `zst_queue_element_t` is a first-class element in zstreamer. When the pipeline starts, the queue spawns its own dedicated worker thread.
3.  **Downstream Thread:** This internal worker thread continuously pops buffers from the queue and pushes them to the downstream sink.

This decoupling means that if the sink temporarily blocks (e.g., waiting for the clock to sync before rendering a frame), the source can continue producing buffers and filling the queue, preventing the entire pipeline from stalling.

## Thread-Safe Bounded Queue (zst_queue_t)

The core of this decoupling is the `zst_queue_t` object. It provides thread-safe operations for enqueuing and dequeuing items (typically `zst_buffer_t` objects).

-   **Bounded Capacity:** The queue has a maximum size (either by number of buffers, total byte size, or total duration). This prevents uncontrolled memory growth if the source produces data faster than the sink consumes it.
-   **Blocking Operations:**
    -   If the queue is **full**, the thread pushing data (the upstream thread) will block until space becomes available.
    -   If the queue is **empty**, the thread pulling data (the queue's worker thread) will block until new data arrives.
-   **Synchronization:** Access to the queue's internal state is protected by mutexes, and blocking operations use condition variables to wait efficiently without busy-looping.

## Atomic Ref-Counting

Data flow across these thread boundaries relies heavily on zstreamer's buffer reference counting (`zst_buffer_ref` and `zst_buffer_unref`).

When a buffer is pushed into a queue, its reference count is incremented atomically. The upstream thread can then safely forget about it. The queue's worker thread later pops the buffer, processes it (pushes it downstream), and unreferences it.

The atomic operations ensure that the memory backing the buffer is only freed (or returned to its pool) when the last thread holding a reference releases it, regardless of concurrent access.

By strategically placing queue elements, application developers can define the threading model of their zstreamer pipelines, balancing concurrency, latency, and resource usage.
