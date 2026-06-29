/*=============================================================================
    zst_sc6f0_source.h — SC6F0 Source element convenience API
=============================================================================*/
#pragma once

#include "zst_element.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZST_SC6F0_SOURCE_FACTORY "sc6f0src"

#define ZST_SC6F0_SOURCE_PROP_MEDIA_DEVICE     "media-device"
#define ZST_SC6F0_SOURCE_PROP_PLATFORM_ID      "platform-id"
#define ZST_SC6F0_SOURCE_PROP_MOCK_MODE        "mock-mode"
#define ZST_SC6F0_SOURCE_PROP_TRIGGER_SIGNAL   "trigger-signal"
#define ZST_SC6F0_SOURCE_PROP_SUBDEV_PATH      "subdev-path"
#define ZST_SC6F0_SOURCE_PROP_VPSS_CSC_PATH    "vpss-csc-path"
#define ZST_SC6F0_SOURCE_PROP_AUDIO_SYSFS_PATH "audio-sysfs-path"

zst_element_t* zst_sc6f0_source_create(void);

#ifdef __cplusplus
}
#endif
