#include "stub_nvbuf_utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

extern "C" {
int NvBufferCreateEx(int *dmabuf_fd, NvBufferCreateParams *input_params) {
    if (!dmabuf_fd || !input_params) return -1;
    int fd = memfd_create("stub_nvbuf", MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, input_params->memsize) < 0) {
        close(fd);
        return -1;
    }
    *dmabuf_fd = fd;
    return 0;
}

int NvBufferMemMap(int dmabuf_fd, unsigned int plane, NvBufferMemFlags memflag, void **pVirtAddr) {
    if (!pVirtAddr) return -1;
    off_t size = lseek(dmabuf_fd, 0, SEEK_END);
    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
    if (ptr == MAP_FAILED) return -1;
    *pVirtAddr = ptr;
    return 0;
}

int NvBufferMemUnMap(int dmabuf_fd, unsigned int plane, void **pVirtAddr) {
    if (!pVirtAddr || !*pVirtAddr) return -1;
    off_t size = lseek(dmabuf_fd, 0, SEEK_END);
    munmap(*pVirtAddr, size);
    *pVirtAddr = NULL;
    return 0;
}

int NvBufferMemSyncForCpu(int dmabuf_fd, unsigned int plane, void **pVirtAddr) {
    return 0;
}

int NvBufferDestroy(int dmabuf_fd) {
    close(dmabuf_fd);
    return 0;
}
}
