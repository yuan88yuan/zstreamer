# Event Bus Patterns

The Event Bus (`zst_bus_t`) in zstreamer is a central communication mechanism for asynchronous notifications. It decouples the core pipeline execution threads from the application thread that manages the pipeline, allowing elements to report status, errors, and metadata without blocking data flow.

## Core Concepts

A bus is essentially a thread-safe queue of `zst_event_t` objects. Each pipeline has an associated bus. Elements within the pipeline post events to the bus, and the application periodically polls or waits on the bus to handle these events.

The `zst_event_t` structure contains:
- `type`: An enumeration (`zst_event_type_t`) indicating the event kind (e.g., Error, EOS, State Change).
- `src`: A pointer to the element or object that generated the event.
- `as`: A union containing event-specific data payloads.

## Common Event Patterns

### 1. Handling Errors (`ZST_EVENT_ERROR`)

When an element encounters a fatal error (e.g., failed to open a file, decoding error), it should not simply return an error code or crash. Instead, it posts an error event to the bus.

-   **Posting:** `zst_bus_post_error(bus, element, "Detailed error message");`
-   **Handling:** The application pops the event, logs the error message (`event->as.error.message`), and typically initiates pipeline shutdown or recovery procedures.

### 2. EOS Propagation (`ZST_EVENT_EOS`)

End-Of-Stream (EOS) indicates that a stream has finished and no more data will be produced.

-   **Generation:** A source element (like a file reader) detects the end of the file and posts an EOS event. Alternatively, an element can send an EOS event downstream via `zst_pad_push_event()`. When an EOS event reaches a sink element, the sink typically posts a bus message to notify the application.
-   **Handling:** The application waits for EOS events from all active sinks. Once received, it knows the pipeline has finished processing and can be safely stopped.

### 3. State Changes (`ZST_EVENT_STATE_CHANGED`)

When an element transitions between states (NULL -> READY -> PLAYING), it posts a state change event.

-   **Posting:** The framework automatically handles posting state change events when `zst_element_set_state()` completes successfully. The event payload (`event->as.state_changed`) contains the `old_state` and `new_state`.
-   **Handling:** The application uses these events to track the overall progress of pipeline state changes, especially asynchronous ones.

### 4. Segment Messages (`ZST_EVENT_SEGMENT`)

Segment events communicate timing and boundaries for the media stream. They are crucial for seeking and synchronized playback.

-   **Posting:** When a source element starts a new stream or completes a seek operation, it generates a segment event detailing the `start`, `stop`, `time`, `base`, and `rate` of the new segment.
-   **Propagation:** Segment events travel downstream via pads.
-   **Handling:** Elements use segment information to adjust timestamps, drop buffers outside the valid range (clipping), and handle playback speed (`rate`). Applications can also monitor segment events on the bus to update UI elements like playback progress bars.

## Async Event Handling

The typical pattern for an application is to have a dedicated thread polling the bus, or to integrate the bus file descriptor into a main event loop (like glib's GMainLoop or Qt's event loop).

```c
zst_event_t* event;
// Block until an event arrives or timeout occurs
while (zst_bus_pop(bus, &event, 1000) == ZST_OK) {
    if (!event) continue; // Timeout

    switch (event->type) {
        case ZST_EVENT_ERROR:
            fprintf(stderr, "Error from %s: %s\n", event->src->name, event->as.error.message);
            // Handle error...
            break;
        case ZST_EVENT_EOS:
            printf("Got EOS!\n");
            // Handle EOS...
            break;
        // ... handle other events
    }
    zst_event_destroy(event); // Must free the event!
}
```
