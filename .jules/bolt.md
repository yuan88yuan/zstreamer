## 2024-06-27 - [Audiomixer Allocation Overhead]
**Learning:** Found a performance bottleneck in the `audio_mixer_worker` function where it calls `calloc` and `free` on a temporary `fmix` buffer in its core mixing loop.
**Action:** Reused a dynamic buffer (`fmix`) added to the `audio_mixer_t` context struct which scales only when necessary using `realloc` and clears old values using `memset`. This optimization eliminates memory allocation overhead per audio block and reduces fragmentation.
## 2024-06-28 - [RTP Payloader Allocation Overhead]
**Learning:** Found a performance bottleneck in the `rtp_payloader_push_packet` function where it calls `malloc` and `free` for each packet. A single packet payload is allocated every time. We can reduce the number of allocations by combining header and payload using two memcpys.
**Action:** Replaced a large malloc and single memcpy by modifying `rtp_payloader_make_packet` to take a header buffer and payload buffer, allocating one continuous buffer, and doing two memcpys. This eliminated a malloc overhead.
