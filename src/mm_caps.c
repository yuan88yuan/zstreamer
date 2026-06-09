/*=============================================================================
    mm_caps.c — Capabilities structure implementation
=============================================================================*/
#include "mm_caps.h"
#include <stdlib.h>
#include <string.h>

mm_caps_t*
mm_caps_create(void)
{
    mm_caps_t* caps = calloc(1, sizeof(*caps));
    return caps;
}

void
mm_caps_destroy(mm_caps_t* caps)
{
    if (!caps) return;
    mm_caps_struct_t* curr = caps->structs;
    while (curr) {
        mm_caps_struct_t* next = curr->next;
        mm_caps_struct_free(curr);
        curr = next;
    }
    free(caps);
}

mm_caps_t*
mm_caps_copy(const mm_caps_t* caps)
{
    if (!caps) return NULL;
    mm_caps_t* copy = mm_caps_create();
    if (!copy) return NULL;
    
    mm_caps_struct_t* curr = caps->structs;
    mm_caps_struct_t* tail = NULL;
    while (curr) {
        mm_caps_struct_t* s_copy = mm_caps_struct_copy(curr);
        if (!s_copy) {
            mm_caps_destroy(copy);
            return NULL;
        }
        if (!copy->structs) {
            copy->structs = s_copy;
        } else {
            tail->next = s_copy;
        }
        tail = s_copy;
        curr = curr->next;
    }
    return copy;
}

mm_caps_struct_t*
mm_caps_struct_create_video(
    const char* media_type,
    int width,
    int height,
    double framerate,
    const char* pixel_format)
{
    mm_caps_struct_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (media_type) {
        strncpy(s->media_type, media_type, sizeof(s->media_type) - 1);
    }
    s->type = MM_CAPS_VIDEO;
    s->video.width = width;
    s->video.height = height;
    s->video.framerate = framerate;
    if (pixel_format) {
        strncpy(s->video.pixel_format, pixel_format, sizeof(s->video.pixel_format) - 1);
    }
    s->next = NULL;
    return s;
}

mm_caps_struct_t*
mm_caps_struct_create_audio(
    const char* media_type,
    int channels,
    int sample_rate,
    const char* format)
{
    mm_caps_struct_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (media_type) {
        strncpy(s->media_type, media_type, sizeof(s->media_type) - 1);
    }
    s->type = MM_CAPS_AUDIO;
    s->audio.channels = channels;
    s->audio.sample_rate = sample_rate;
    if (format) {
        strncpy(s->audio.format, format, sizeof(s->audio.format) - 1);
    }
    s->next = NULL;
    return s;
}

void
mm_caps_struct_free(mm_caps_struct_t* caps_struct)
{
    free(caps_struct);
}

mm_caps_struct_t*
mm_caps_struct_copy(const mm_caps_struct_t* caps_struct)
{
    if (!caps_struct) return NULL;
    mm_caps_struct_t* copy = malloc(sizeof(*copy));
    if (!copy) return NULL;
    memcpy(copy, caps_struct, sizeof(*copy));
    copy->next = NULL;
    return copy;
}

mm_result_t
mm_caps_append(mm_caps_t* caps, mm_caps_struct_t* caps_struct)
{
    if (!caps || !caps_struct) return MM_ERROR;
    caps_struct->next = NULL;
    if (!caps->structs) {
        caps->structs = caps_struct;
    } else {
        mm_caps_struct_t* curr = caps->structs;
        while (curr->next) {
            curr = curr->next;
        }
        curr->next = caps_struct;
    }
    return MM_OK;
}

static mm_caps_struct_t*
mm_caps_struct_intersect(
    const mm_caps_struct_t* s1,
    const mm_caps_struct_t* s2)
{
    if (strcmp(s1->media_type, s2->media_type) != 0) {
        return NULL;
    }
    if (s1->type != s2->type) {
        return NULL;
    }
    
    if (s1->type == MM_CAPS_VIDEO) {
        int w = 0, h = 0;
        double fr = 0.0;
        char fmt[32] = {0};
        
        // Intersect width
        if (s1->video.width != 0 && s2->video.width != 0) {
            if (s1->video.width != s2->video.width) return NULL;
            w = s1->video.width;
        } else {
            w = s1->video.width != 0 ? s1->video.width : s2->video.width;
        }
        
        // Intersect height
        if (s1->video.height != 0 && s2->video.height != 0) {
            if (s1->video.height != s2->video.height) return NULL;
            h = s1->video.height;
        } else {
            h = s1->video.height != 0 ? s1->video.height : s2->video.height;
        }
        
        // Intersect framerate
        if (s1->video.framerate != 0.0 && s2->video.framerate != 0.0) {
            if (s1->video.framerate != s2->video.framerate) return NULL;
            fr = s1->video.framerate;
        } else {
            fr = s1->video.framerate != 0.0 ? s1->video.framerate : s2->video.framerate;
        }
        
        // Intersect format
        if (s1->video.pixel_format[0] != '\0' && s2->video.pixel_format[0] != '\0') {
            if (strcmp(s1->video.pixel_format, s2->video.pixel_format) != 0) return NULL;
            strcpy(fmt, s1->video.pixel_format);
        } else {
            strcpy(fmt, s1->video.pixel_format[0] != '\0' ? s1->video.pixel_format : s2->video.pixel_format);
        }
        
        return mm_caps_struct_create_video(s1->media_type, w, h, fr, fmt);
    } else if (s1->type == MM_CAPS_AUDIO) {
        int ch = 0, rate = 0;
        char fmt[32] = {0};
        
        // Intersect channels
        if (s1->audio.channels != 0 && s2->audio.channels != 0) {
            if (s1->audio.channels != s2->audio.channels) return NULL;
            ch = s1->audio.channels;
        } else {
            ch = s1->audio.channels != 0 ? s1->audio.channels : s2->audio.channels;
        }
        
        // Intersect sample_rate
        if (s1->audio.sample_rate != 0 && s2->audio.sample_rate != 0) {
            if (s1->audio.sample_rate != s2->audio.sample_rate) return NULL;
            rate = s1->audio.sample_rate;
        } else {
            rate = s1->audio.sample_rate != 0 ? s1->audio.sample_rate : s2->audio.sample_rate;
        }
        
        // Intersect format
        if (s1->audio.format[0] != '\0' && s2->audio.format[0] != '\0') {
            if (strcmp(s1->audio.format, s2->audio.format) != 0) return NULL;
            strcpy(fmt, s1->audio.format);
        } else {
            strcpy(fmt, s1->audio.format[0] != '\0' ? s1->audio.format : s2->audio.format);
        }
        
        return mm_caps_struct_create_audio(s1->media_type, ch, rate, fmt);
    } else {
        mm_caps_struct_t* copy = mm_caps_struct_copy(s1);
        return copy;
    }
}

mm_caps_t*
mm_caps_intersect(const mm_caps_t* caps1, const mm_caps_t* caps2)
{
    if (!caps1 || !caps2) return NULL;
    mm_caps_t* result = mm_caps_create();
    if (!result) return NULL;
    
    for (mm_caps_struct_t* s1 = caps1->structs; s1 != NULL; s1 = s1->next) {
        for (mm_caps_struct_t* s2 = caps2->structs; s2 != NULL; s2 = s2->next) {
            mm_caps_struct_t* res_s = mm_caps_struct_intersect(s1, s2);
            if (res_s) {
                mm_caps_append(result, res_s);
            }
        }
    }
    return result;
}

int
mm_caps_struct_is_fixed(const mm_caps_struct_t* s)
{
    if (!s) return 0;
    if (s->type == MM_CAPS_VIDEO) {
        return s->video.width != 0 &&
               s->video.height != 0 &&
               s->video.framerate != 0.0 &&
               s->video.pixel_format[0] != '\0';
    } else if (s->type == MM_CAPS_AUDIO) {
        return s->audio.channels != 0 &&
               s->audio.sample_rate != 0 &&
               s->audio.format[0] != '\0';
    }
    return 0;
}

int
mm_caps_is_fixed(const mm_caps_t* caps)
{
    if (!caps || !caps->structs) return 0;
    if (caps->structs->next != NULL) return 0;
    return mm_caps_struct_is_fixed(caps->structs);
}

mm_result_t
mm_caps_fixate(mm_caps_t* caps)
{
    if (!caps || !caps->structs) return MM_ERROR;
    
    /* Keep only the first structure */
    mm_caps_struct_t* first = caps->structs;
    mm_caps_struct_t* curr = first->next;
    while (curr) {
        mm_caps_struct_t* next = curr->next;
        mm_caps_struct_free(curr);
        curr = next;
    }
    first->next = NULL;
    
    /* Resolve wildcards in the first structure */
    if (first->type == MM_CAPS_VIDEO) {
        if (first->video.width == 0) first->video.width = 640;
        if (first->video.height == 0) first->video.height = 480;
        if (first->video.framerate == 0.0) first->video.framerate = 30.0;
        if (first->video.pixel_format[0] == '\0') {
            strcpy(first->video.pixel_format, "YUV420P");
        }
    } else if (first->type == MM_CAPS_AUDIO) {
        if (first->audio.channels == 0) first->audio.channels = 2;
        if (first->audio.sample_rate == 0) first->audio.sample_rate = 44100;
        if (first->audio.format[0] == '\0') {
            strcpy(first->audio.format, "S16LE");
        }
    }
    
    return MM_OK;
}
