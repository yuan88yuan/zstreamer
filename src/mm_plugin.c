/*=============================================================================
    mm_plugin.c — dlopen-based dynamic plugin loader
=============================================================================*/

#include "mm_plugin.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define DLOPEN(f)   (void*)LoadLibraryA(f)
#  define DLSYM(h, n) (void*)GetProcAddress((HMODULE)h, n)
#  define DLCLOSE(h)   FreeLibrary((HMODULE)h)
#else
#  include <dlfcn.h>
#  define DLOPEN(f)   dlopen(f, RTLD_LAZY | RTLD_LOCAL)
#  define DLSYM(h, n) dlsym(h, n)
#  define DLCLOSE(h)   dlclose(h)
#endif

/* suppress -Wpedantic warning for dlsym cast to function pointer */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

mm_plugin_t*
mm_plugin_load(const char* path)
{
    if (!path) return NULL;

    void* handle = DLOPEN(path);
    if (!handle) return NULL;

    mm_get_plugin_fn get_plugin = (mm_get_plugin_fn)DLSYM(handle, "mm_get_plugin");
    if (!get_plugin) {
        DLCLOSE(handle);
        return NULL;
    }

    mm_plugin_t* plugin = get_plugin();
    if (!plugin) {
        DLCLOSE(handle);
        return NULL;
    }

    /* Call plugin init if provided */
    if (plugin->desc.init)
        plugin->desc.init();

    plugin->priv = (void*)handle;
    return plugin;
}

#pragma GCC diagnostic pop

void
mm_plugin_unload(mm_plugin_t* plugin)
{
    if (!plugin) return;

    if (plugin->desc.deinit)
        plugin->desc.deinit();

    if (plugin->priv)
        DLCLOSE(plugin->priv);

    free(plugin);
}
