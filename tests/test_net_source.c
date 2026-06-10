/*=============================================================================
    test_net_source.c — Minimal smoke test for net_source element creation
=============================================================================*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "zst_element.h"
#include "zst_plugin.h"

/* Forward declaration from net_source.c if linkable directly */
extern zst_element_t* zst_net_source_create(void);

int main(void)
{
    zst_element_t* el = zst_net_source_create();
    assert(el != NULL);
    assert(strcmp(el->ops->name, "netsrc") == 0);

    char value[128];
    assert(zst_element_get_property(el, "protocol", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "tcp-client") == 0);

    assert(zst_element_get_property(el, "host", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "127.0.0.1") == 0);

    assert(zst_element_get_property(el, "read-timeout", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "100") == 0);

    assert(zst_element_set_property(el, "protocol", "unix") == ZST_OK);
    assert(zst_element_get_property(el, "protocol", value, sizeof(value)) == ZST_OK);
    assert(strcmp(value, "unix-client") == 0);

    zst_element_destroy(el);
    printf("net_source smoke test passed\n");
    return 0;
}
