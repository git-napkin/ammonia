/* Not in the default CMake build. PID 1 payload is libinfect.m. */
#include "ammonia.h"
#include "envbuf.h"
#include "frida-gum.h"
#include "options_loader.h"
#include "tweak_utils.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define OPENER_DYLIB SUPPORT_PATH "libopener.dylib"
#define TWEAKS_DIR SUPPORT_PATH "tweaks"
#define HOOK_FALLBACK SUPPORT_PATH "libinject.dylib"

static char g_hook_path[PATH_MAX];

static int (*SpawnOld)(pid_t *pid, const char *path,
                       const posix_spawn_file_actions_t *ac,
                       const posix_spawnattr_t *ab, char *const __argv[],
                       char *const __envp[]);

static int (*SpawnPOld)(pid_t *restrict pid, const char *restrict path,
                        const posix_spawn_file_actions_t *file_actions,
                        const posix_spawnattr_t *restrict attrp,
                        char *const argv[restrict],
                        char *const envp[restrict]);

static int (*GetDarwinRoleNp)(const posix_spawnattr_t *__restrict attr,
                              uint64_t *__restrict darwin_rolep);

#define PRIO_DARWIN_ROLE_UI_FOCAL 0x1
#define PRIO_DARWIN_ROLE_UI 0x2
#define PRIO_DARWIN_ROLE_UI_NON_FOCAL 0x4

static bool skip_chrome(const char *path) {
    return path_ends_with(path, "Dock") ||
           path_ends_with(path, "ControlCenter") ||
           path_ends_with(path, "NotificationCenter") ||
           path_ends_with(path, "SystemUIServer") ||
           path_ends_with(path, "WallpaperAgent") ||
           path_ends_with(path, "WallpaperAerialsExtension") ||
           path_ends_with(path, "WindowServer") ||
           path_ends_with(path, "loginwindow");
}

static char **append_insert(char **env, const char *dylib) {
    int idx = envbuf_find((const char **)env, "DYLD_INSERT_LIBRARIES");
    if (idx < 0)
        return envbuf_setenv(env, "DYLD_INSERT_LIBRARIES", dylib);
    const char *old = env[idx] + strlen("DYLD_INSERT_LIBRARIES=");
    if (strstr(old, dylib))
        return env;
    char *combined = NULL;
    if (asprintf(&combined, "%s:%s", old, dylib) == -1)
        return env;
    env = envbuf_setenv(env, "DYLD_INSERT_LIBRARIES", combined);
    free(combined);
    return env;
}

static char **append_enabled_tweaks(char **env, const char *spawn_path) {
    clear_tweak_enabled_cache();
    DIR *dr = opendir(TWEAKS_DIR);
    if (!dr)
        return env;
    struct dirent *en;
    while ((en = readdir(dr)) != NULL) {
        const char *name = en->d_name;
        size_t nlen = strlen(name);
        if (nlen < 6 || strcmp(name + nlen - 6, ".dylib") != 0)
            continue;
        if (!is_safe_filename(name) || !is_tweak_enabled(name))
            continue;
        if (!should_load_tweak(TWEAKS_DIR, name, spawn_path))
            continue;
        if (!check_dylib_options(TWEAKS_DIR, name, spawn_path))
            continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", TWEAKS_DIR, name);
        env = append_insert(env, full);
    }
    closedir(dr);
    return env;
}

static int spawn_with_env(int (*spawn_fn)(pid_t *, const char *,
                                          const posix_spawn_file_actions_t *,
                                          const posix_spawnattr_t *,
                                          char *const[], char *const[]),
                          pid_t *pid, const char *path,
                          const posix_spawn_file_actions_t *ac,
                          const posix_spawnattr_t *ab, char *const __argv[],
                          char *const __envp[]) {
    if (spawn_fn == NULL)
        return EINVAL;

    char **playground = envbuf_mutcopy((const char **)__envp);
    if (__envp != NULL && playground == NULL)
        return ENOMEM;

    uint64_t darwin_role = 0;
    if (ab != NULL && GetDarwinRoleNp != NULL)
        GetDarwinRoleNp(ab, &darwin_role);

    FangsOptions opts = fangs_load_options();
    if (opts.pauseInjection)
        goto spawn;

    const char *hook = g_hook_path[0] ? g_hook_path : HOOK_FALLBACK;

    if (strcmp(path, "/usr/libexec/xpcproxy") == 0) {
        playground = append_insert(playground, hook);
    } else if (!path_ends_with(path, "Driver") && !skip_chrome(path)) {
        if (darwin_role == PRIO_DARWIN_ROLE_UI_FOCAL ||
            darwin_role == PRIO_DARWIN_ROLE_UI ||
            darwin_role == PRIO_DARWIN_ROLE_UI_NON_FOCAL) {
            playground = append_insert(playground, OPENER_DYLIB);
            playground = append_enabled_tweaks(playground, path);
        }
    }

spawn:
    {
        int k = spawn_fn(pid, path, ac, ab, __argv, (char *const *)playground);
        envbuf_free(playground);
        return k;
    }
}

static int SpawnNew(pid_t *pid, const char *path,
                    const posix_spawn_file_actions_t *ac,
                    const posix_spawnattr_t *ab, char *const __argv[],
                    char *const __envp[]) {
    return spawn_with_env(SpawnOld, pid, path, ac, ab, __argv, __envp);
}

static int SpawnPNew(pid_t *restrict pid, const char *restrict path,
                     const posix_spawn_file_actions_t *ac,
                     const posix_spawnattr_t *restrict ab,
                     char *const *restrict argv,
                     char *const *restrict envp) {
    return spawn_with_env(SpawnPOld, pid, path, ac, ab, argv, envp);
}

__attribute__((constructor)) static void fangs_hook_lite_init(void) {
    openlog("fangs_hook_lite", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr((const void *)fangs_hook_lite_init, &info) && info.dli_fname)
        strlcpy(g_hook_path, info.dli_fname, sizeof(g_hook_path));

    syslog(LOG_NOTICE, "fangs_hook_lite: loaded in pid %d from %s",
           (int)getpid(), g_hook_path[0] ? g_hook_path : "(unknown)");

    *(void **)&GetDarwinRoleNp =
        dlsym(RTLD_DEFAULT, "posix_spawnattr_get_darwin_role_np");

    gum_init_embedded();
    GumInterceptor *interceptor = gum_interceptor_obtain();
    gum_interceptor_begin_transaction(interceptor);

    gpointer posix_spawn_addr =
        (gpointer)gum_module_find_global_export_by_name("posix_spawn");
    if (posix_spawn_addr != NULL) {
        GumReplaceReturn ret = gum_interceptor_replace(
            interceptor, posix_spawn_addr, (gpointer)SpawnNew, NULL,
            (gpointer *)&SpawnOld);
        syslog(LOG_NOTICE, "fangs_hook_lite: posix_spawn replace %d old=%p",
               (int)ret, (void *)SpawnOld);
    }

    gpointer posix_spawnp_addr =
        (gpointer)gum_module_find_global_export_by_name("posix_spawnp");
    if (posix_spawnp_addr != NULL) {
        GumReplaceReturn ret = gum_interceptor_replace(
            interceptor, posix_spawnp_addr, (gpointer)SpawnPNew, NULL,
            (gpointer *)&SpawnPOld);
        syslog(LOG_NOTICE, "fangs_hook_lite: posix_spawnp replace %d old=%p",
               (int)ret, (void *)SpawnPOld);
    }

    gum_interceptor_end_transaction(interceptor);
    syslog(LOG_NOTICE, "fangs_hook_lite: initialized");
}
