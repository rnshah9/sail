/*  This file is part of SAIL (https://github.com/smoked-herring/sail)

    Copyright (c) 2020 Dmitry Baryshev

    The MIT License

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SAIL_WIN32
    #include <windows.h> /* FindFirstFile */
#else
    #include <dirent.h> /* opendir */
    #include <sys/types.h>
#endif

#include "sail-common.h"
#include "sail.h"

/*
 * Private functions.
 */

static const char* sail_codecs_path(void) {

    SAIL_THREAD_LOCAL static bool codecs_path_called = false;
    SAIL_THREAD_LOCAL static const char *env = NULL;

    if (codecs_path_called) {
        return env;
    }

    codecs_path_called = true;

#ifdef SAIL_WIN32
    _dupenv_s((char **)&env, NULL, "SAIL_CODECS_PATH");

    /* Construct "\bin\..\lib\sail\codecs" from "\bin\sail.dll". */
    if (env == NULL) {
        char path[MAX_PATH];
        HMODULE thisModule;

        if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)&sail_codecs_path, &thisModule) == 0) {
            SAIL_LOG_ERROR("GetModuleHandleEx() failed with an error code %d. Falling back to loading codecs from '%s'",
                            GetLastError(), SAIL_CODECS_PATH);
            env = SAIL_CODECS_PATH;
        } else if (GetModuleFileName(thisModule, path, sizeof(path)) == 0) {
            SAIL_LOG_ERROR("GetModuleFileName() failed with an error code %d. Falling back to loading codecs from '%s'",
                            GetLastError(), SAIL_CODECS_PATH);
            env = SAIL_CODECS_PATH;
        } else {
            char *lib_sail_codecs_path;

            /* "\bin\sail.dll" -> "\bin". */
            char *last_sep = strrchr(path, '\\');

            if (last_sep == NULL) {
                SAIL_LOG_ERROR("Failed to find a path separator in '%s'. Falling back to loading codecs from '%s'",
                                path, SAIL_CODECS_PATH);
                env = SAIL_CODECS_PATH;
            } else {
                *last_sep = '\0';

                #ifdef SAIL_VCPKG_PORT
                    /* "\bin" -> "\bin\sail\codecs" */
                    const char *CODECS_RELATIVE_PATH = "\\sail\\codecs";
                #else
                    /* "\bin" -> "\bin\..\lib\sail\codecs" */
                    const char *CODECS_RELATIVE_PATH = "\\..\\lib\\sail\\codecs";
                #endif

                SAIL_TRY_OR_EXECUTE(sail_concat(&lib_sail_codecs_path, 2, path, CODECS_RELATIVE_PATH),
                                    /* on error */ SAIL_LOG_ERROR("Failed to concat strings. Falling back to loading codecs from '%s'",
                                                                    SAIL_CODECS_PATH),
                                    env = SAIL_CODECS_PATH);
                env = lib_sail_codecs_path;
                SAIL_LOG_DEBUG("SAIL_CODECS_PATH environment variable is not set. Loading codecs from '%s'", env);
            }
        }
    } else {
        SAIL_LOG_DEBUG("SAIL_CODECS_PATH environment variable is set. Loading codecs from '%s'", env);
    }
#else
    env = getenv("SAIL_CODECS_PATH");

    if (env == NULL) {
        SAIL_LOG_DEBUG("SAIL_CODECS_PATH environment variable is not set. Loading codecs from '%s'", SAIL_CODECS_PATH);
        env = SAIL_CODECS_PATH;
    } else {
        SAIL_LOG_DEBUG("SAIL_CODECS_PATH environment variable is set. Loading codecs from '%s'", env);
    }
#endif

    return env;
}

static const char* client_codecs_path(void) {

    SAIL_THREAD_LOCAL static bool codecs_path_called = false;
    SAIL_THREAD_LOCAL static const char *env = NULL;

    if (codecs_path_called) {
        return env;
    }

    codecs_path_called = true;

#ifdef SAIL_WIN32
    _dupenv_s((char **)&env, NULL, "SAIL_MY_CODECS_PATH");
#else
    env = getenv("SAIL_MY_CODECS_PATH");
#endif

    if (env == NULL) {
        SAIL_LOG_DEBUG("SAIL_MY_CODECS_PATH environment variable is not set. Not loading codecs from it");
    } else {
        SAIL_LOG_DEBUG("SAIL_MY_CODECS_PATH environment variable is set. Loading codecs from '%s'", env);
    }

    return env;
}

/* Add "sail/codecs/lib" to the DLL/SO search path. */
static sail_status_t update_lib_path(const char *codecs_path) {

#ifdef SAIL_WIN32
    char *full_path_to_lib;
    SAIL_TRY(sail_concat(&full_path_to_lib, 2, codecs_path, "\\lib"));

    if (!sail_is_dir(full_path_to_lib)) {
        SAIL_LOG_DEBUG("Optional DLL directory '%s' doesn't exist, so not loading DLLs from it", full_path_to_lib);
        sail_free(full_path_to_lib);
        return SAIL_OK;
    }

    SAIL_LOG_DEBUG("Append '%s' to the DLL search paths", full_path_to_lib);

    wchar_t *full_path_to_lib_w;
    SAIL_TRY_OR_CLEANUP(sail_to_wchar(full_path_to_lib, &full_path_to_lib_w),
                        sail_free(full_path_to_lib));

    if (!AddDllDirectory(full_path_to_lib_w)) {
        SAIL_LOG_ERROR("Failed to update library search path with '%s'. Error: %d", full_path_to_lib, GetLastError());
        sail_free(full_path_to_lib_w);
        sail_free(full_path_to_lib);
        SAIL_LOG_AND_RETURN(SAIL_ERROR_ENV_UPDATE);
    }

    sail_free(full_path_to_lib_w);
    sail_free(full_path_to_lib);
#else
    char *full_path_to_lib;
    SAIL_TRY(sail_concat(&full_path_to_lib, 2, codecs_path, "/lib"));

    if (!sail_is_dir(full_path_to_lib)) {
        SAIL_LOG_DEBUG("Optional LIB directory '%s' doesn't exist, so not updating LD_LIBRARY_PATH with it", full_path_to_lib);
        sail_free(full_path_to_lib);
        return SAIL_OK;
    }

    char *combined_ld_library_path;
    char *env = getenv("LD_LIBRARY_PATH");

    if (env == NULL) {
        SAIL_TRY_OR_CLEANUP(sail_strdup(full_path_to_lib, &combined_ld_library_path),
                            sail_free(full_path_to_lib));
    } else {
        SAIL_TRY_OR_CLEANUP(sail_concat(&combined_ld_library_path, 3, env, ":", full_path_to_lib),
                            sail_free(full_path_to_lib));
    }

    sail_free(full_path_to_lib);
    SAIL_LOG_DEBUG("Set LD_LIBRARY_PATH to '%s'", combined_ld_library_path);

    if (setenv("LD_LIBRARY_PATH", combined_ld_library_path, true) != 0) {
        SAIL_LOG_ERROR("Failed to update library search path: %s", strerror(errno));
        sail_free(combined_ld_library_path);
        SAIL_LOG_AND_RETURN(SAIL_ERROR_ENV_UPDATE);
    }

    sail_free(combined_ld_library_path);
#endif

    return SAIL_OK;
}

static sail_status_t alloc_context(struct sail_context **context) {

    SAIL_CHECK_CONTEXT_PTR(context);

    void *ptr;
    SAIL_TRY(sail_malloc(sizeof(struct sail_context), &ptr));

    *context = ptr;

    (*context)->initialized      = false;
    (*context)->codec_info_node = NULL;

    return SAIL_OK;
}

static sail_status_t build_full_path(const char *sail_codecs_path, const char *name, char **full_path) {

#ifdef SAIL_WIN32
    SAIL_TRY(sail_concat(full_path, 3, sail_codecs_path, "\\", name));
#else
    SAIL_TRY(sail_concat(full_path, 3, sail_codecs_path, "/", name));
#endif

    return SAIL_OK;
}

static sail_status_t build_codec_from_codec_info(const char *codec_info_full_path,
                                                    struct sail_codec_info_node **codec_info_node) {

    SAIL_CHECK_PATH_PTR(codec_info_full_path);
    SAIL_CHECK_CODEC_INFO_PTR(codec_info_node);

    /* Build "/path/jpeg.so" from "/path/jpeg.codec.info". */
    char *codec_info_part = strstr(codec_info_full_path, ".codec.info");

    if (codec_info_part == NULL) {
        SAIL_LOG_AND_RETURN(SAIL_ERROR_MEMORY_ALLOCATION);
    }

    /* The length of "/path/jpeg". */
    size_t codec_full_path_length = strlen(codec_info_full_path) - strlen(codec_info_part);
    char *codec_full_path;

#ifdef SAIL_WIN32
    static const char * const LIB_SUFFIX = "dll";
#else
    static const char * const LIB_SUFFIX = "so";
#endif

    /* The resulting string will be "/path/jpeg.plu" (on Windows) or "/path/jpeg.pl". */
    SAIL_TRY(sail_strdup_length(codec_info_full_path,
                                codec_full_path_length + strlen(LIB_SUFFIX) + 1, &codec_full_path));

#ifdef SAIL_WIN32
    /* Overwrite the end of the path with "dll". */
    strcpy_s(codec_full_path + codec_full_path_length + 1, strlen(LIB_SUFFIX) + 1, LIB_SUFFIX);
#else
    /* Overwrite the end of the path with "so". */
    strcpy(codec_full_path + codec_full_path_length + 1, LIB_SUFFIX);
#endif

    /* Parse codec info. */
    SAIL_TRY_OR_CLEANUP(alloc_codec_info_node(codec_info_node),
                        sail_free(codec_full_path));

    struct sail_codec_info *codec_info;
    SAIL_TRY_OR_CLEANUP(codec_read_info(codec_info_full_path, &codec_info),
                        destroy_codec_info_node(*codec_info_node),
                        sail_free(codec_full_path));

    /* Save the parsed codec info into the SAIL context. */
    (*codec_info_node)->codec_info = codec_info;
    codec_info->path = codec_full_path;

    return SAIL_OK;
}

static sail_status_t destroy_context(struct sail_context *context) {

    if (context == NULL) {
        return SAIL_OK;
    }

    destroy_codec_info_node_chain(context->codec_info_node);
    sail_free(context);

    return SAIL_OK;
}

/* Initializes the context and loads all the codec info files if the context is not initialized. */
static sail_status_t init_context(struct sail_context *context, int flags) {

    SAIL_CHECK_CONTEXT_PTR(context);

    if (context->initialized) {
        return SAIL_OK;
    }

    context->initialized = true;

    /* Time counter. */
    uint64_t start_time = sail_now();

    SAIL_LOG_INFO("Version %s", SAIL_VERSION_STRING);

    /* Our own codecs. */
    const char *our_codecs_path = sail_codecs_path();
    SAIL_TRY(update_lib_path(our_codecs_path));

    /* Client codecs. */
    const char *their_codecs_path = client_codecs_path();
    if (their_codecs_path != NULL) {
        SAIL_TRY(update_lib_path(their_codecs_path));
    }

    /* Used to load and store codec info objects. */
    struct sail_codec_info_node **last_codec_info_node = &context->codec_info_node;
    struct sail_codec_info_node *codec_info_node;

    const int codec_search_paths_length = 2;
    const char* codec_search_paths[2] = { our_codecs_path, their_codecs_path };

    for (int i = 0; i < codec_search_paths_length; i++) {
        const char *codecs_path = codec_search_paths[i];

        if (codecs_path == NULL) {
            continue;
        }

#ifdef SAIL_WIN32
        const char *plugs_info_mask = "\\*.codec.info";

        size_t codecs_path_with_mask_length = strlen(codecs_path) + strlen(plugs_info_mask) + 1;

        void *ptr;
        SAIL_TRY(sail_malloc(codecs_path_with_mask_length, &ptr));
        char *codecs_path_with_mask = ptr;

        strcpy_s(codecs_path_with_mask, codecs_path_with_mask_length, codecs_path);
        strcat_s(codecs_path_with_mask, codecs_path_with_mask_length, plugs_info_mask);

        WIN32_FIND_DATA data;
        HANDLE hFind = FindFirstFile(codecs_path_with_mask, &data);

        if (hFind == INVALID_HANDLE_VALUE) {
            SAIL_LOG_ERROR("Failed to list files in '%s'. Error: %d", codecs_path, GetLastError());
            sail_free(codecs_path_with_mask);
            SAIL_LOG_AND_RETURN(SAIL_ERROR_LIST_DIR);
        }

        do {
            /* Build a full path. */
            char *full_path;

            /* Ignore errors and try to load as much as possible. */
            SAIL_TRY_OR_EXECUTE(build_full_path(codecs_path, data.cFileName, &full_path),
                                /* on error */ continue);

            SAIL_LOG_DEBUG("Found codec info '%s'", data.cFileName);

            if (build_codec_from_codec_info(full_path, &codec_info_node) == SAIL_OK) {
                *last_codec_info_node = codec_info_node;
                last_codec_info_node = &codec_info_node->next;
            }

            sail_free(full_path);
        } while (FindNextFile(hFind, &data));

        if (GetLastError() != ERROR_NO_MORE_FILES) {
            SAIL_LOG_ERROR("Failed to list files in '%s'. Error: %d. Some codecs may be ignored", codecs_path, GetLastError());
        }

        sail_free(codecs_path_with_mask);
        FindClose(hFind);
#else
        DIR *d = opendir(codecs_path);

        if (d == NULL) {
            SAIL_LOG_ERROR("Failed to list files in '%s': %s", codecs_path, strerror(errno));
            continue;
        }

        struct dirent *dir;

        while ((dir = readdir(d)) != NULL) {
            /* Build a full path. */
            char *full_path;

            /* Ignore errors and try to load as much as possible. */
            SAIL_TRY_OR_EXECUTE(build_full_path(codecs_path, dir->d_name, &full_path),
                                /* on error */ continue);

            /* Handle files only. */
            if (sail_is_file(full_path)) {
                bool is_codec_info = strstr(full_path, ".codec.info") != NULL;

                if (is_codec_info) {
                    SAIL_LOG_DEBUG("Found codec info '%s'", dir->d_name);

                    if (build_codec_from_codec_info(full_path, &codec_info_node) == SAIL_OK) {
                        *last_codec_info_node = codec_info_node;
                        last_codec_info_node = &codec_info_node->next;
                    }
                }
            }

            sail_free(full_path);
        }

        closedir(d);
#endif
    }

    if (flags & SAIL_FLAG_PRELOAD_CODECS) {
        SAIL_LOG_DEBUG("Preloading codecs");

        codec_info_node = context->codec_info_node;

        while (codec_info_node != NULL) {
            const struct sail_codec *codec;

            /* Ignore loading errors on purpose. */
            load_codec_by_codec_info(codec_info_node->codec_info, &codec);

            codec_info_node = codec_info_node->next;
        }
    }

    SAIL_LOG_DEBUG("Enumerated codecs:");

    /* Print the found codec infos. */
    struct sail_codec_info_node *node = context->codec_info_node;
    int counter = 1;

    while (node != NULL) {
        SAIL_LOG_DEBUG("%d. %s [%s] %s", counter++, node->codec_info->name, node->codec_info->description, node->codec_info->version);
        node = node->next;
    }

    SAIL_LOG_DEBUG("Initialized in %lu ms.", (unsigned long)(sail_now() - start_time));

    return SAIL_OK;
}

/*
 * Public functions.
 */

sail_status_t control_tls_context(struct sail_context **context, enum SailContextAction action) {

    SAIL_THREAD_LOCAL static struct sail_context *tls_context = NULL;

    switch (action) {
        case SAIL_CONTEXT_ALLOCATE: {
            SAIL_CHECK_CONTEXT_PTR(context);

            if (tls_context == NULL) {
                SAIL_TRY(alloc_context(&tls_context));
                SAIL_LOG_DEBUG("Allocated a new thread-local context %p", tls_context);
            }

            *context = tls_context;
            break;
        }
        case SAIL_CONTEXT_FETCH: {
            *context = tls_context;
            break;
        }
        case SAIL_CONTEXT_DESTROY: {
            SAIL_LOG_DEBUG("Destroyed the thread-local context %p", tls_context);
            destroy_context(tls_context);
            tls_context = NULL;
            break;
        }
    }

    return SAIL_OK;
}

sail_status_t current_tls_context(struct sail_context **context) {

    SAIL_TRY(current_tls_context_with_flags(context, /* flags */ 0));

    return SAIL_OK;
}

sail_status_t current_tls_context_with_flags(struct sail_context **context, int flags) {

    SAIL_CHECK_CONTEXT_PTR(context);

    SAIL_TRY(control_tls_context(context, SAIL_CONTEXT_ALLOCATE));
    SAIL_TRY(init_context(*context, flags));

    return SAIL_OK;
}
