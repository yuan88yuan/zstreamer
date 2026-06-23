/*=============================================================================
    zst_stream.c - Stream metadata helpers
=============================================================================*/

#include "zst_stream.h"
#include "zst_caps.h"
#include <stdlib.h>
#include <string.h>

void
zst_stream_info_clear(zst_stream_info_t* info)
{
    if (!info) return;
    free(info->name);
    free(info->language);
    if (info->caps) {
        zst_caps_destroy(info->caps);
    }
    memset(info, 0, sizeof(*info));
}
