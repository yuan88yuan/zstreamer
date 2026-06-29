/*=============================================================================
    @file zst_bin.h
    @brief Composite element containers and ghost pads

    zst_bin_t provides a container element that manages child elements.
    Ghost pads expose internal child pads on the bin boundary, allowing
    the bin to be treated as a single element in the pipeline graph.
=============================================================================*/
#pragma once

#include "zst_types.h"
#include "zst_element.h"
#include "zst_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create a composite element.  The returned object is a normal zst_element_t
 * whose children are managed with zst_bin_add()/zst_bin_remove(). */
zst_element_t* zst_bin_create(
    const char* name);

zst_result_t zst_bin_add(
    zst_element_t* bin,
    zst_element_t* child);

zst_result_t zst_bin_remove(
    zst_element_t* bin,
    zst_element_t* child);

uint32_t zst_bin_get_child_count(
    zst_element_t* bin);

zst_element_t* zst_bin_get_child(
    zst_element_t* bin,
    uint32_t index);

/* Create a ghost pad that exposes an internal target pad on the bin boundary.
 * - A SINK ghost pad forwards external pushes into the target sink pad.
 * - A SRC ghost pad supports pull-mode and also installs an internal proxy sink
 *   so pushes from the target src pad are forwarded to the bin's external peer.
 */
zst_pad_t* zst_ghost_pad_create(
    const char* name,
    zst_pad_t* target);

zst_result_t zst_ghost_pad_set_target(
    zst_pad_t* ghost_pad,
    zst_pad_t* target);

zst_pad_t* zst_ghost_pad_get_target(
    zst_pad_t* ghost_pad);

zst_result_t zst_bin_add_ghost_pad(
    zst_element_t* bin,
    zst_pad_t* ghost_pad);

#ifdef __cplusplus
}
#endif
