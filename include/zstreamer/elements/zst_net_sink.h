/*=============================================================================
    zst_net_sink.h — Net Sink convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_NET_SINK_FACTORY "netsink"

#define ZST_NET_SINK_PROP_HOST "host"
#define ZST_NET_SINK_PROP_PORT "port"
#define ZST_NET_SINK_PROP_PROTOCOL "protocol"
#define ZST_NET_SINK_PROP_PATH "path"
#define ZST_NET_SINK_PROP_WRITE_TIMEOUT "write-timeout"
#define ZST_NET_SINK_PROP_TTL "ttl"
#define ZST_NET_SINK_PROP_LOOP "loop"

#define ZST_NET_SINK_PROTOCOL_TCP_CLIENT "tcp-client"
#define ZST_NET_SINK_PROTOCOL_TCP_SERVER "tcp-server"
#define ZST_NET_SINK_PROTOCOL_UDP_CLIENT "udp-client"
#define ZST_NET_SINK_PROTOCOL_UDP_SERVER "udp-server"
#define ZST_NET_SINK_PROTOCOL_UNIX_CLIENT "unix-client"
#define ZST_NET_SINK_PROTOCOL_UNIX_SERVER "unix-server"

#define ZST_NET_SINK_PAD_SINK "sink"

zst_element_t* zst_net_sink_create(void);

#ifdef __cplusplus
}
#endif
