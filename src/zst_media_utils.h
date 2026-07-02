#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int zst_find_start_code(const uint8_t* data, int size, int offset, int* code_size) {
    if (code_size) *code_size = 0;
    if (!data || size <= 0) return -1;

    for (int i = offset; i + 3 <= size; i++) {
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            if (code_size) *code_size = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (code_size) *code_size = 3;
            return i;
        }
    }
    return -1;
}

#ifdef __cplusplus
}
#endif
