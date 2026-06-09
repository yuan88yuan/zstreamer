/*=============================================================================
    mm_pipeline.h
=============================================================================*/
#pragma once

#include "mm_types.h"
#include "mm_element.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mm_pipeline {

    mm_element_t** elements;

    uint32_t nb_elements;

    mm_state_t state;

    void* priv;
};

mm_pipeline_t* mm_pipeline_create(void);

void mm_pipeline_destroy(
    mm_pipeline_t* pipe);

mm_result_t mm_pipeline_add(
    mm_pipeline_t* pipe,
    mm_element_t* el);

mm_result_t mm_pipeline_remove(
    mm_pipeline_t* pipe,
    mm_element_t* el);

mm_result_t mm_pipeline_set_state(
    mm_pipeline_t* pipe,
    mm_state_t state);

mm_result_t mm_pipeline_start(
    mm_pipeline_t* pipe);

mm_result_t mm_pipeline_stop(
    mm_pipeline_t* pipe);

#ifdef __cplusplus
}
#endif