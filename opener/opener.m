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

static char *GetExePath(char *buf, uint32_t bufsize) {
    _NSGetExecutablePath(buf, &bufsize);
    return buf;
}

void Open(void * interceptor) { 
    DIR *dr;
    struct dirent *en;
    dr = opendir(SupportFolderP "tweaks/"); // Open the directory
    if (dr) {
        while ((en = readdir(dr)) != NULL) {
            if (en->d_type == DT_REG) {
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
                        if (len > 0 && process_name[len - 1] == '\n') {
                            process_name[len - 1] = '\0';
                        }

                        if (strstr(exe_path, process_name) != NULL) {
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
                            if (len > 0 && process_name[len - 1] == '\n') {
                                process_name[len - 1] = '\0';
                            }

                            if (strstr(exe_path, process_name) != NULL) {
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

                // Load the dylib
                void *handle = dlopen(full_path, RTLD_NOW | RTLD_GLOBAL);
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