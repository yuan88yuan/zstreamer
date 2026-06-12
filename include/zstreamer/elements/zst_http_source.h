/*=============================================================================
    zst_http_source.h — HTTP/HTTPS Source convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_HTTP_SOURCE_FACTORY "httpsrc"

#define ZST_HTTP_SRC_PROP_URL                    "url"
#define ZST_HTTP_SRC_PROP_URI                    "uri"
#define ZST_HTTP_SRC_PROP_USER_AGENT             "user-agent"
#define ZST_HTTP_SRC_PROP_HEADERS                "headers"
#define ZST_HTTP_SRC_PROP_TIMEOUT                "timeout"
#define ZST_HTTP_SRC_PROP_CHUNK_SIZE             "chunk-size"
#define ZST_HTTP_SRC_PROP_RECONNECT              "reconnect"
#define ZST_HTTP_SRC_PROP_RECONNECT_DELAY_MS     "reconnect-delay-ms"
#define ZST_HTTP_SRC_PROP_MAX_RECONNECT_ATTEMPTS "max-reconnect-attempts"
#define ZST_HTTP_SOURCE_PROP_URL "url"
#define ZST_HTTP_SOURCE_PROP_URI "uri"
#define ZST_HTTP_SOURCE_PROP_USER_AGENT "user-agent"
#define ZST_HTTP_SOURCE_PROP_HEADERS "headers"
#define ZST_HTTP_SOURCE_PROP_TIMEOUT "timeout"
#define ZST_HTTP_SOURCE_PROP_CHUNK_SIZE "chunk-size"
#define ZST_HTTP_SOURCE_PROP_RECONNECT "reconnect"
#define ZST_HTTP_SOURCE_PROP_RECONNECT_DELAY "reconnect-delay-ms"
#define ZST_HTTP_SOURCE_PROP_MAX_RECONNECT "max-reconnect-attempts"

zst_element_t* zst_http_source_create(const char* url);

#ifdef __cplusplus
}
#endif
