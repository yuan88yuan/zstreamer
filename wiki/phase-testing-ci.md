# Phase 9 — Testing & CI

- [x] Docker Compose for multi-service testing (e.g. v4l2loopback)
- [x] CI pipeline (GitHub Actions): build, unit test, docker build, integration test
- [ ] Caps negotiation fuzzing
- [x] Event bus stress test
- [x] Queue element stress test
- [x] Clock precision test
- [x] V4L2 DMABUF simulation testing (vivid + DMA heap runner)
- [ ] Static analysis: `cppcheck`, `clang-tidy`
- [ ] Valgrind memory leak checks in CI

## V4L2 DMABUF Simulation Testing  (✅ implemented)

To verify the `v4l2src` DMABUF memory mode without hardware, use the `vivid` (Virtual Video Test Driver) kernel module, which natively supports `V4L2_MEMORY_DMABUF`. The project includes `tests/test_v4l2_dmabuf_sim.c` and the host/container runner `tests/run_v4l2_dmabuf_simulation.sh`:

1. **Load the `vivid` kernel module on the host machine**:
   ```bash
   sudo modprobe vivid n_devs=1 node_types=0x1
   ```
   This typically creates `/dev/video0`.

2. **Run a test pipeline with `dmabuf` memory-type**:
   ```bash
   docker build -t zstreamer:latest --target dev .
   ./tests/run_v4l2_dmabuf_simulation.sh
   ```
   The runner discovers the vivid `/dev/videoX` node, mounts `/dev/dma_heap` when available, and executes:
   ```bash
   /workspace/build/test_v4l2_dmabuf_sim --device /dev/videoX --frames 10 --duration 5
   ```

   The test constructs `v4l2src memory-type=dmabuf ! fakesink` and adds a source-pad probe that asserts every captured frame is `ZST_MEMORY_DMABUF` with a valid fd.

3. **Verification**:
   - `zst_allocator_dmabuf_create()` now prefers Linux DMA heaps (`/dev/dma_heap/system`, etc.) and falls back to `memfd` only for allocator-only tests.
   - `zst_buffer_create_with_allocator()` marks allocator-backed DMA buffers as `ZST_MEMORY_DMABUF` and exposes the fd through `memory.priv` for V4L2 import paths.
   - The CI workflow invokes the simulation runner after the V4L2 loopback integration test; the runner skips gracefully when host DMA heap/vivid prerequisites are unavailable.
