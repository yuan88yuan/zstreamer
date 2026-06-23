/*=============================================================================
    zst_net_source.h — Net Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NET_SOURCE_FACTORY "netsrc"

#define ZST_NET_SOURCE_PROP_HOST "host"
#define ZST_NET_SOURCE_PROP_PORT "port"
#define ZST_NET_SOURCE_PROP_PROTOCOL "protocol"
#define ZST_NET_SOURCE_PROP_PATH "path"
#define ZST_NET_SOURCE_PROP_CHUNK_SIZE "chunk-size"
#define ZST_NET_SOURCE_PROP_READ_TIMEOUT "read-timeout"

#define ZST_NET_SOURCE_PROTOCOL_TCP_CLIENT "tcp-client"
#define ZST_NET_SOURCE_PROTOCOL_TCP_SERVER "tcp-server"
#define ZST_NET_SOURCE_PROTOCOL_UNIX_CLIENT "unix-client"
#define ZST_NET_SOURCE_PROTOCOL_UNIX_SERVER "unix-server"
#define ZST_NET_SOURCE_PROTOCOL_UDP "udp"
#define ZST_NET_SOURCE_PROTOCOL_UDP_CLIENT "udp-client"

#define ZST_NET_SOURCE_PAD_SRC "src"

zst_element_t* zst_net_source_create(void);

#ifdef __cplusplus
}
#endif
