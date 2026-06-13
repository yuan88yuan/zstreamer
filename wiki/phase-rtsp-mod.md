# RTSP Server Media-On-Demand Refactoring Plan

This document outlines the detailed tasks and implementation checklist for refactoring the `rtsp_server` element in `zstreamer` to support dynamic, media-on-demand session mounting.

---

## 1. Objectives

- Allow clients to request arbitrary mount points (e.g. `rtsp://host:8554/dynamic_mount`).
- Provide a callback mechanism (`zst_rtsp_server_mount_cb_t`) that is triggered when a requested session is not pre-registered.
- Allow the application to determine and dynamically construct the appropriate media source (e.g., `net_source`, `v4l2_source`, `mp4_demuxer`, `http_source`, or custom sources) and link it to the server pads within the callback.
- Ensure full thread-safety when dynamically adding/linking elements to the pipeline while the scheduler is running, without compromising scheduler loop performance.
- Support standard RTP-over-RTSP interleaved TCP transport transparently for dynamic sessions.

---

## 2. API Design

### public interface additions (`include/zst_rtsp_server.h`)

```c
typedef zst_result_t (*zst_rtsp_server_mount_cb_t)(
    zst_element_t* server,
    const char* session_name,
    void* user_data
);

zst_result_t zst_rtsp_server_set_mount_callback(
    zst_element_t* server,
    zst_rtsp_server_mount_cb_t callback,
    void* user_data
);
```

---

## 3. Implementation Checklist

### Phase 1: Thread-Safety Integration
- [x] Add a `pthread_rwlock_t elements_lock` field to `struct zst_pipeline` in `include/zst_pipeline.h`.
- [x] Initialize the rwlock in `zst_pipeline_create()` and destroy it in `zst_pipeline_destroy()`.
- [x] Acquire a write lock (`pthread_rwlock_wrlock`) in `zst_pipeline_add()` and `zst_pipeline_remove()` to protect modifications to the elements list.
- [x] Acquire a read lock (`pthread_rwlock_rdlock`) in the worker loops of `src/zst_scheduler.c` before accessing the `pipe->elements` array, unlocking it before processing each element to maintain performance.

### Phase 2: RTSP Server Element Refactoring
- [x] Add `self` pointer, `mount_callback`, and `mount_user_data` to the `rtsp_server_priv_t` struct in `src/rtsp_server.c`.
- [x] Populate `priv->self` in `zst_rtsp_server_create()`.
- [x] Implement the public API wrapper `zst_rtsp_server_set_mount_callback()`.

### Phase 3: URL Extraction & Callback Dispatch
- [x] Implement `extract_mount_clean()` helper to cleanly parse the mount name by stripping track prefixes (like `/trackID=0` or `/trackid=0`) and options.
- [x] Modify `on_describe` to detect if the session does not exist.
- [x] If the session is missing and a callback is present:
  1. Unlock the server mutex `srv->lock`.
  2. Invoke the user mount callback.
  3. Re-acquire `srv->lock` and check if the session was successfully created/linked.
- [x] Ensure `cl->session` is populated with the dynamically created session.

### Phase 4: Verification & Integration Tests
- [x] Add a test case `test_rtsp_server_media_on_demand` to `tests/test_core.c` (or a separate test module) that sets a mount callback, requests a dynamic session, and verifies it is correctly created and linked.
- [x] Verify that all existing unit tests and the newly added tests pass successfully inside Docker.

