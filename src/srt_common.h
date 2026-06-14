#pragma once
#include <stddef.h>

void srt_global_init(void);
void srt_global_cleanup(void);

void srt_parse_uri(const char* uri, char* host, size_t host_len, int* port,
                          char* mode, size_t mode_len, int* latency,
                          char* passphrase, size_t passphrase_len, int* pbkeylen,
                          char* streamid, size_t streamid_len, int* payload_size);
