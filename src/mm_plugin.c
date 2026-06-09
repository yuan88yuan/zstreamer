/*=============================================================================
    mm_plugin.c — dlopen-based dynamic plugin loader and registry
=============================================================================*/

#define _POSIX_C_SOURCE 200809L  /* strdup */

#include "mm_plugin.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <stdio.h>

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

typedef struct mm_registry_entry {
    mm_plugin_t*              plugin;
    char*                     path;
    struct mm_registry_entry* next;
} mm_registry_entry_t;

static struct {
    mm_registry_entry_t* head;
    pthread_mutex_t      lock;
    int                  initialized;
} g_registry = {
    .head = NULL,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = 0
};

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

    plugin->refcount = 1;

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

mm_plugin_t*
mm_plugin_ref(mm_plugin_t* plugin)
{
    if (!plugin) return NULL;
    __sync_fetch_and_add(&plugin->refcount, 1);
    return plugin;
}

void
mm_plugin_unref(mm_plugin_t* plugin)
{
    if (!plugin) return;
    if (__sync_sub_and_fetch(&plugin->refcount, 1) <= 0) {
        mm_plugin_unload(plugin);
    }
}

mm_result_t
mm_plugin_registry_init(void)
{
    pthread_mutex_lock(&g_registry.lock);
    if (g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.lock);
        return MM_OK;
    }
    g_registry.head = NULL;
    g_registry.initialized = 1;
    pthread_mutex_unlock(&g_registry.lock);
    return MM_OK;
}

void
mm_plugin_registry_deinit(void)
{
    pthread_mutex_lock(&g_registry.lock);
    if (!g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.lock);
        return;
    }
    mm_registry_entry_t* curr = g_registry.head;
    while (curr) {
        mm_registry_entry_t* next = curr->next;
        mm_plugin_unref(curr->plugin);
        free(curr->path);
        free(curr);
        curr = next;
    }
    g_registry.head = NULL;
    g_registry.initialized = 0;
    pthread_mutex_unlock(&g_registry.lock);
}

mm_result_t
mm_plugin_registry_scan(const char* directory)
{
    if (!directory) return MM_ERROR;

    DIR* dir = opendir(directory);
    if (!dir) return MM_ERROR;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".so") == 0) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

            /* Check if already loaded in registry */
            pthread_mutex_lock(&g_registry.lock);
            mm_registry_entry_t* curr = g_registry.head;
            int already_loaded = 0;
            while (curr) {
                if (strcmp(curr->path, path) == 0) {
                    already_loaded = 1;
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&g_registry.lock);

            if (already_loaded) continue;

            mm_plugin_t* plugin = mm_plugin_load(path);
            if (plugin) {
                mm_registry_entry_t* node = malloc(sizeof(*node));
                if (node) {
                    node->plugin = plugin;
                    node->path = strdup(path);
                    
                    pthread_mutex_lock(&g_registry.lock);
                    node->next = g_registry.head;
                    g_registry.head = node;
                    pthread_mutex_unlock(&g_registry.lock);
                } else {
                    mm_plugin_unref(plugin);
                }
            }
        }
    }

    closedir(dir);
    return MM_OK;
}

mm_result_t
mm_plugin_registry_scan_env(void)
{
    const char* env = getenv("ZSTREAMER_PLUGIN_PATH");
    if (!env) return MM_OK;

    char* env_copy = strdup(env);
    if (!env_copy) return MM_ERROR;

    char* token = strtok(env_copy, ":");
    while (token) {
        mm_plugin_registry_scan(token);
        token = strtok(NULL, ":");
    }

    free(env_copy);
    return MM_OK;
}

mm_element_t*
mm_element_factory_make(const char* name)
{
    if (!name) return NULL;

    pthread_mutex_lock(&g_registry.lock);
    mm_registry_entry_t* curr = g_registry.head;
    while (curr) {
        mm_plugin_t* plugin = curr->plugin;
        if (plugin->create_element) {
            mm_element_t* el = plugin->create_element(name);
            if (el) {
                el->plugin = mm_plugin_ref(plugin);
                pthread_mutex_unlock(&g_registry.lock);
                return el;
            }
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_registry.lock);
    return NULL;
}
