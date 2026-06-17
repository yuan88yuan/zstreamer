**scheduler flow — with explicit queue elements**

Each queue element runs its own worker thread. The scheduler thread handles
source elements (producers). Queue threads handle downstream propagation.

```
scheduler thread (round-robin over source elements):
    for each source element assigned to this thread:
        if sink pads == 0:  (is source)
            call process() to produce buffer
            push downstream to next queue's sink pad

queue_element worker thread (one per queue element):
    while running:
        pop buffer from internal queue (blocking with timeout)
        push buffer downstream via src pad's peer
```

**pipeline 執行模型**

```
v4l2src
    -> queue_el (worker thread)
    -> x264enc (runs in queue's thread via default_sink_pad_push)
    -> queue_el (worker thread)
    -> mp4mux

alsasrc
    -> queue_el (worker thread)
    -> aacenc
    -> queue_el (worker thread)
    -> mp4mux
    -> queue_el (worker thread)
    -> filesink
```

Each `queue_el` boundary is a thread switch. Elements without a queue before
them run in the pushing thread (either the scheduler thread for sources, or
the previous queue element's thread for intermediate elements).