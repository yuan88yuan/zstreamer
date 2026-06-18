# Phase 9 — Testing & CI

- [x] Docker Compose for multi-service testing (e.g. v4l2loopback)
- [x] CI pipeline (GitHub Actions): build, unit test, docker build, integration test
- [ ] Caps negotiation fuzzing
- [x] Event bus stress test
- [x] Queue element stress test
- [x] Clock precision test
- [ ] Static analysis: `cppcheck`, `clang-tidy`
- [ ] Valgrind memory leak checks in CI

## V4L2 DMABUF Simulation Testing

To verify the `v4l2src` DMABUF memory mode without hardware, use the `vivid` (Virtual Video Test Driver) kernel module, which natively supports `V4L2_MEMORY_DMABUF`:

1. **Load the `vivid` kernel module on the host machine**:
   ```bash
   sudo modprobe vivid n_devs=1 node_types=0x1
   ```
   This typically creates `/dev/video0`.

2. **Run a test pipeline with `dmabuf` memory-type**:
   In your simulation container or host, set up the `v4l2src` element to request the DMABUF memory type.
   ```c
   zst_element_t* src = zst_element_factory_make("v4l2src");
   zst_element_set_property(src, "device", "/dev/video0");
   zst_element_set_property(src, "memory-type", "dmabuf");

   // Create a simple pipeline: v4l2src -> fakesink
   zst_element_t* sink = zst_element_factory_make("fakesink");
   // ... add to pipeline and link ...
   zst_pipeline_set_state(pipe, ZST_STATE_PLAYING);
   ```

3. **Verification**:
   - The element will create a DMABUF pool (e.g., via `zst_allocator_dmabuf_create()`).
   - Observe the `v4l2src` logging to confirm buffers are mapped using `V4L2_MEMORY_DMABUF`.
   - Add a pad probe on `v4l2src`'s source pad to ensure frame payload generation without segfaults.
