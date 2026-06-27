## 2024-06-27 - [Audiomixer Allocation Overhead]
**Learning:** Found a performance bottleneck in the `audio_mixer_worker` function where it calls `calloc` and `free` on a temporary `fmix` buffer in its core mixing loop.
**Action:** Reused a dynamic buffer (`fmix`) added to the `audio_mixer_t` context struct which scales only when necessary using `realloc` and clears old values using `memset`. This optimization eliminates memory allocation overhead per audio block and reduces fragmentation.
