//
//  opener.c
//  opener
//
//  Created by whisper on 9/2/23.
//
#import <Foundation/Foundation.h>
#include "opener.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

typedef struct {
    char *path;
    void *handle;
    struct timespec mtime;
} LoadedModule;

static LoadedModule *loaded_modules = NULL;
static size_t loaded_count = 0;
static void *g_interceptor = NULL;
static dispatch_source_t g_reload_source = NULL;

static bool path_ends_with(const char *path, const char *name) {
    if (!path || !name) return false;
    size_t path_len = strlen(path);
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > path_len) return false;
    return (strncmp(path + path_len - name_len, name, name_len) == 0) &&
           (path_len == name_len || path[path_len - name_len - 1] == '/');
}

static bool path_matches_entry(const char *path, const char *entry) {
    if (!path || !entry || entry[0] == '\0') return false;
    if (strchr(entry, '/') != NULL) {
        return (strcmp(path, entry) == 0);
    }
    return path_ends_with(path, entry);
}

static bool timespec_equal(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static char *GetExePath(char *buf, uint32_t bufsize) {
    if (_NSGetExecutablePath(buf, &bufsize) != 0) {
        syslog(LOG_ERR, "ammonia: executable path too long");
        return NULL;
    }
    return buf;
}

static LoadedModule *find_loaded_module(const char *path) {
    for (size_t i = 0; i < loaded_count; ++i) {
        if (strcmp(loaded_modules[i].path, path) == 0) {
            return &loaded_modules[i];
        }
    }
    return NULL;
}

static void record_loaded_module(const char *path, void *handle,
                                 const struct stat *st) {
    LoadedModule *existing = find_loaded_module(path);
    if (existing) {
        existing->handle = handle;
        existing->mtime = st->st_mtimespec;
        return;
    }

    LoadedModule *tmp = realloc(loaded_modules,
                                (loaded_count + 1) * sizeof(LoadedModule));
    if (!tmp) {
        syslog(LOG_ERR, "ammonia: failed to track loaded module %s", path);
        return;
    }
    loaded_modules = tmp;

    char *path_copy = strdup(path);
    if (!path_copy) {
        syslog(LOG_ERR, "ammonia: failed to store path for loaded module %s", path);
        return;
    }

    loaded_modules[loaded_count].path = path_copy;
    loaded_modules[loaded_count].handle = handle;
    loaded_modules[loaded_count].mtime = st->st_mtimespec;
    loaded_count++;
}

static void try_load_file(const char *subdir, const char *d_name, void *interceptor) {
    if (strstr(d_name, "..") != NULL || strchr(d_name, '/') != NULL) {
        syslog(LOG_ERR, "ammonia: rejecting path traversal attempt: %s", d_name);
        return;
    }

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s%s/%s", SupportFolderP, subdir, d_name);

    char whitelist_file[PATH_MAX];
    snprintf(whitelist_file, sizeof(whitelist_file), "%s%s/%s.whitelist",
             SupportFolderP, subdir, d_name);

    char blacklist_file[PATH_MAX];
    snprintf(blacklist_file, sizeof(blacklist_file), "%s%s/%s.blacklist",
             SupportFolderP, subdir, d_name);

    char exe_path[PATH_MAX];
    GetExePath(exe_path, sizeof(exe_path));
    bool should_load = false;

    struct stat whitelist_st, blacklist_st;
    bool has_whitelist = stat(whitelist_file, &whitelist_st) == 0;
    bool has_blacklist = stat(blacklist_file, &blacklist_st) == 0;
    if (has_whitelist && has_blacklist) {
        syslog(LOG_WARNING,
               "ammonia: both whitelist and blacklist exist for %s; using whitelist only",
               d_name);
    }

    FILE *whitelist_fp = has_whitelist ? fopen(whitelist_file, "r") : NULL;
    if (whitelist_fp) {
        char process_name[256];
        while (fgets(process_name, sizeof(process_name), whitelist_fp) != NULL) {
            size_t len = strlen(process_name);
            while (len > 0 && (process_name[len - 1] == '\n' || process_name[len - 1] == '\r')) {
                process_name[--len] = '\0';
            }

            if (path_matches_entry(exe_path, process_name)) {
                should_load = true;
                break;
            }
        }
        fclose(whitelist_fp);

        if (!should_load) {
            syslog(LOG_INFO, "Process %s is not whitelisted for %s.", exe_path, d_name);
            return;
        }
    } else {
        FILE *blacklist_fp = fopen(blacklist_file, "r");
        if (blacklist_fp) {
            should_load = true;
            char process_name[256];
            while (fgets(process_name, sizeof(process_name), blacklist_fp) != NULL) {
                size_t len = strlen(process_name);
                while (len > 0 && (process_name[len - 1] == '\n' || process_name[len - 1] == '\r')) {
                    process_name[--len] = '\0';
                }

                if (path_matches_entry(exe_path, process_name)) {
                    should_load = false;
                    syslog(LOG_INFO, "Process %s is blacklisted for %s.", exe_path, d_name);
                    break;
                }
            }
            fclose(blacklist_fp);
            if (!should_load) {
                return;
            }
        } else {
            return;
        }
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        syslog(LOG_ERR, "ammonia: cannot stat %s, skipping", full_path);
        return;
    }
    if (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        syslog(LOG_ERR, "ammonia: rejecting %s - not owned by root or unsafe permissions", full_path);
        return;
    }

    LoadedModule *existing = find_loaded_module(full_path);
    if (existing && timespec_equal(&st.st_mtimespec, &existing->mtime)) {
        return;
    }
    if (existing && existing->handle != NULL) {
        dlclose(existing->handle);
        existing->handle = NULL;
    }

    void *handle = dlopen(full_path, RTLD_LAZY | RTLD_GLOBAL);
    if (handle == NULL) {
        syslog(LOG_ERR, "Error loading %s: %s", full_path, dlerror());
        return;
    }

    void (*LoadFunction)(void *) = dlsym(handle, "LoadFunction");
    if (LoadFunction != NULL) {
        LoadFunction(interceptor);
    }

    record_loaded_module(full_path, handle, &st);
}

static void scan_directory(const char *subdir, void *interceptor) {
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s%s/", SupportFolderP, subdir);

    DIR *dr = opendir(dir_path);
    if (!dr) {
        if (errno != ENOENT) {
            syslog(LOG_ERR, "ammonia: error opening %s directory: %s", subdir,
                   strerror(errno));
        }
        return;
    }

    struct dirent *en;
    while ((en = readdir(dr)) != NULL) {
        if (en->d_type != DT_REG && en->d_type != DT_UNKNOWN) continue;
        try_load_file(subdir, en->d_name, interceptor);
    }
    closedir(dr);
}

void Open(void *interceptor) {
    scan_directory("tweaks", interceptor);
    scan_directory("gui", interceptor);
}

static void setup_reload_handler(void *interceptor) {
    g_interceptor = interceptor;
    signal(SIGUSR1, SIG_IGN);

    g_reload_source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
    if (!g_reload_source) {
        syslog(LOG_ERR, "ammonia: failed to create reload signal source");
        return;
    }

    dispatch_source_set_event_handler(g_reload_source, ^{
        syslog(LOG_INFO, "ammonia: reloading modules from tweaks/ and gui/");
        Open(g_interceptor);
    });
    dispatch_resume(g_reload_source);
}

typedef void (*GumInitEmbeddedFunc_t)(void);
typedef void *(*GumInterceptorObtainFunc_t)(void);

void __attribute__((constructor)) ctor_main(void) {
    void *hooking = dlopen(SupportFolderP "fridagum.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!hooking) {
        syslog(LOG_ERR, "ammonia: failed to load fridagum.dylib: %s", dlerror());
        return;
    }
    GumInitEmbeddedFunc_t GumInitEmbeddedFunc = dlsym(hooking, "gum_init_embedded");
    GumInterceptorObtainFunc_t GumInterceptorObtainFunc = dlsym(hooking, "gum_interceptor_obtain");
    if (!GumInitEmbeddedFunc || !GumInterceptorObtainFunc) {
        syslog(LOG_ERR, "ammonia: failed to resolve Frida-Gum symbols: %s", dlerror());
        dlclose(hooking);
        return;
    }
    GumInitEmbeddedFunc();
    void *interceptor = GumInterceptorObtainFunc();
    Open(interceptor);
    setup_reload_handler(interceptor);
}
