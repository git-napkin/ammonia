//
//  opener.c
//  opener
//
//  Created by whisper on 9/2/23.
//
#include <Foundation/Foundation.h>
#include "opener.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

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

static char *GetExePath(char *buf, uint32_t bufsize) {
    if (_NSGetExecutablePath(buf, &bufsize) != 0) {
        syslog(LOG_ERR, "ammonia: executable path too long");
        return NULL;
    }
    return buf;
}

void Open(void * interceptor) { 
    DIR *dr;
    struct dirent *en;
    dr = opendir(SupportFolderP "tweaks/"); // Open the directory
    if (dr) {
        while ((en = readdir(dr)) != NULL) {
            if (en->d_type != DT_REG && en->d_type != DT_UNKNOWN) continue;

            // Prevent path traversal
            if (strstr(en->d_name, "..") != NULL || strchr(en->d_name, '/') != NULL) {
                syslog(LOG_ERR, "ammonia: rejecting path traversal attempt: %s", en->d_name);
                continue;
            }

            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%stweaks/%s", SupportFolderP, en->d_name);

            // Construct paths for whitelist and blacklist files
            char whitelist_file[PATH_MAX];
            snprintf(whitelist_file, sizeof(whitelist_file), "%stweaks/%s.whitelist", SupportFolderP, en->d_name);

            char blacklist_file[PATH_MAX];
            snprintf(blacklist_file, sizeof(blacklist_file), "%stweaks/%s.blacklist", SupportFolderP, en->d_name);

                char exe_path[PATH_MAX];
                GetExePath(exe_path, sizeof(exe_path));
                bool should_load = false;

                // Priority 1: whitelist
                FILE *whitelist_fp = fopen(whitelist_file, "r");
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
                        syslog(LOG_INFO, "Process %s is not whitelisted for %s.", exe_path, en->d_name);
                        goto cleanup;
                    }
                } else {
                    // Priority 2: blacklist
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
                                syslog(LOG_INFO, "Process %s is blacklisted for %s.", exe_path, en->d_name);
                                break;
                            }
                        }
                        fclose(blacklist_fp);
                        if (!should_load) {
                            goto cleanup;
                        }
                    } else {
                        // Neither whitelist nor blacklist exists — skip
                        goto cleanup;
                    }
                }

                // Security check: verify tweak is owned by root and not world-writable
                struct stat st;
                if (stat(full_path, &st) != 0) {
                    syslog(LOG_ERR, "ammonia: cannot stat %s, skipping", full_path);
                    goto cleanup;
                }
                if (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH))) {
                    syslog(LOG_ERR, "ammonia: rejecting %s — not owned by root or unsafe permissions", full_path);
                    goto cleanup;
                }

                // Load the dylib with RTLD_LAZY to defer symbol resolution until needed.
                // RTLD_NOW would force all symbol resolution at constructor time, which
                // is dangerous because tweak constructors run inside the dyld loader lock
                // (held by the parent dlopen from opener). If a tweak's constructor triggers
                // any lazy framework loading (e.g., [NSApplication sharedApplication] →
                // HIToolbox dlopen), RTLD_NOW's eager binding guarantees a re-entrant dlopen
                // deadlock. RTLD_LAZY defers binding until after dyld init completes.
                void *handle = dlopen(full_path, RTLD_LAZY | RTLD_GLOBAL);
                if (handle == NULL) {
                    syslog(LOG_ERR, "Error loading %s: %s", full_path, dlerror());
                } else {
                    void (*LoadFunction)(void *) = dlsym(handle, "LoadFunction");
                    if (LoadFunction != NULL) {
                        LoadFunction(interceptor);
                    }
                }

            cleanup:
                continue;
            }
        }
        closedir(dr);
    } else {
        syslog(LOG_ERR, "Error opening tweaks directory.");
    }
    closelog();
}

typedef void (*GumInitEmbeddedFunc_t)(void);
typedef void *(*GumInterceptorObtainFunc_t)(void);

void __attribute__((constructor)) ctor_main(void) {
    void *hooking = dlopen("/private/var/ammonia/core/fridagum.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (!hooking) {
        syslog(LOG_ERR, "ammonia: failed to load fridagum.dylib: %s", dlerror());
        return;
    }
    GumInitEmbeddedFunc_t GumInitEmbeddedFunc = dlsym(hooking, "gum_init_embedded");
    GumInterceptorObtainFunc_t GumInterceptorObtainFunc = dlsym(hooking, "gum_interceptor_obtain");
    if (!GumInitEmbeddedFunc || !GumInterceptorObtainFunc) {
        syslog(LOG_ERR, "ammonia: failed to resolve Frida-Gum symbols");
        dlclose(hooking);
        return;
    }
    GumInitEmbeddedFunc();
    Open(GumInterceptorObtainFunc());
}