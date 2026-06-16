#ifndef STUB_NVBUF_UTILS_H
#define STUB_NVBUF_UTILS_H

#include <stdint.h>

typedef enum {
    NvBufferPayload_SurfArray,
    NvBufferPayload_MemHandle
} NvBufferPayloadType;

typedef enum {
    NvBufferLayout_Pitch,
    NvBufferLayout_BlockLinear
} NvBufferLayout;

typedef enum {
    NvBufferColorFormat_GRAY8,
    NvBufferColorFormat_Invalid
} NvBufferColorFormat;

typedef enum {
    NvBufferTag_NONE = 0x0
} NvBufferTag;

typedef enum {
    NvBufferMem_Read,
    NvBufferMem_Write,
    NvBufferMem_Read_Write
} NvBufferMemFlags;

typedef struct {
    int32_t width;
    int32_t height;
    NvBufferPayloadType payloadType;
    int32_t memsize;
    NvBufferLayout layout;
    NvBufferColorFormat colorFormat;
    NvBufferTag nvbuf_tag;
} NvBufferCreateParams;

#ifdef __cplusplus
extern "C" {
#endif

int NvBufferCreateEx(int *dmabuf_fd, NvBufferCreateParams *input_params);
int NvBufferMemMap(int dmabuf_fd, unsigned int plane, NvBufferMemFlags memflag, void **pVirtAddr);
int NvBufferMemUnMap(int dmabuf_fd, unsigned int plane, void **pVirtAddr);
int NvBufferMemSyncForCpu(int dmabuf_fd, unsigned int plane, void **pVirtAddr);
int NvBufferDestroy(int dmabuf_fd);

#ifdef __cplusplus
}
#endif

#endif
