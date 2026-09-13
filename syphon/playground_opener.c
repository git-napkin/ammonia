#include "tweak_utils.h"
#include "options_loader.h"
#include "macho_sea.h"
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <os/lock.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <syslog.h>
#include <unistd.h>

#include "ammonia.h"

#define TWEAKS_DIR SUPPORT_PATH "tweaks/"
#define FRIDAGUM_DYLIB SUPPORT_PATH "fridagum.dylib"

static void *g_interceptor = NULL;
static const char *tweak_base_dir = TWEAKS_DIR;

static bool is_dylib_filename(const char *name) {
    if (!name)
        return false;
    size_t len = strlen(name);
    if (len < 6)
        return false;
    return strcmp(name + len - 6, ".dylib") == 0;
}

typedef struct {
    char *path;
    void *handle;
    struct timespec mtime;
} LoadedModule;

static LoadedModule *loaded_modules = NULL;
static size_t loaded_count = 0;
static os_unfair_lock g_scan_lock = OS_UNFAIR_LOCK_INIT;

static bool timespec_equal(const struct timespec *a,
                           const struct timespec *b) {
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static LoadedModule *find_loaded_module(const char *path) {
    for (size_t i = 0; i < loaded_count; ++i) {
        if (strcmp(loaded_modules[i].path, path) == 0)
            return &loaded_modules[i];
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
    LoadedModule *tmp =
        realloc(loaded_modules, (loaded_count + 1) * sizeof(LoadedModule));
    if (!tmp) {
        syslog(LOG_ERR, "opener: failed to track loaded module %s", path);
        return;
    }
    loaded_modules = tmp;
    char *path_copy = strdup(path);
    if (!path_copy) {
        syslog(LOG_ERR, "opener: failed to store path for %s", path);
        return;
    }
    loaded_modules[loaded_count].path = path_copy;
    loaded_modules[loaded_count].handle = handle;
    loaded_modules[loaded_count].mtime = st->st_mtimespec;
    loaded_count++;
}

static void try_load_tweak(const char *dir, const char *d_name,
                           const char *exe_path) {
    if (!is_dylib_filename(d_name) || !is_safe_filename(d_name)) {
        if (is_dylib_filename(d_name))
            syslog(LOG_ERR, "opener: rejecting path traversal: %s", d_name);
        return;
    }

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, d_name);

    if (!should_load_tweak(dir, d_name, exe_path))
        return;

    if (!check_dylib_options(dir, d_name, exe_path)) {
        syslog(LOG_INFO,
               "opener: skip %s: frameworkDependencies not met for %s",
               d_name, exe_path);
        return;
    }

    if (!is_tweak_enabled(d_name)) {
        LoadedModule *existing = find_loaded_module(full_path);
        if (existing && existing->handle != NULL) {
            dlclose(existing->handle);
            existing->handle = NULL;
            syslog(LOG_INFO, "opener: unloaded disabled tweak %s", d_name);
        }
        return;
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        syslog(LOG_ERR, "opener: cannot stat %s", full_path);
        return;
    }
    if (!is_tweak_stat_safe(&st)) {
        syslog(LOG_ERR, "opener: rejecting %s - unsafe permissions",
               full_path);
        return;
    }

    LoadedModule *existing = find_loaded_module(full_path);
    if (existing && existing->handle != NULL &&
        timespec_equal(&st.st_mtimespec, &existing->mtime))
        return;

    if (existing && existing->handle != NULL) {
        dlclose(existing->handle);
        existing->handle = NULL;
    }

    void *handle = dlopen(full_path, RTLD_LAZY | RTLD_GLOBAL);
    if (handle == NULL) {
        syslog(LOG_ERR, "opener: dlopen(%s): %s", full_path, dlerror());
        return;
    }

    void (*LoadFunc)(void *) = (void (*)(void *))dlsym(handle, "LoadFunction");
    if (LoadFunc != NULL) {
        LoadFunc(g_interceptor);
        syslog(LOG_INFO, "opener: called LoadFunction in %s", d_name);
    }

    record_loaded_module(full_path, handle, &st);
    syslog(LOG_INFO, "opener: loaded %s", d_name);
}

static void scan_tweaks(void) {
    if (fangs_load_options().pauseInjection)
        return;
    os_unfair_lock_lock(&g_scan_lock);
    clear_tweak_enabled_cache();

    char *exe_path = get_exe_path();
    if (!exe_path) {
        syslog(LOG_ERR, "opener: cannot resolve executable path");
        os_unfair_lock_unlock(&g_scan_lock);
        return;
    }

    DIR *dr = opendir(tweak_base_dir);
    if (!dr) {
        if (errno != ENOENT)
            syslog(LOG_ERR, "opener: opendir(%s): %s", tweak_base_dir,
                   strerror(errno));
        free(exe_path);
        os_unfair_lock_unlock(&g_scan_lock);
        return;
    }

    struct dirent *en;
    while ((en = readdir(dr)) != NULL) {
        if (en->d_type != DT_REG && en->d_type != DT_UNKNOWN)
            continue;
        try_load_tweak(tweak_base_dir, en->d_name, exe_path);
    }
    closedir(dr);
    free(exe_path);
    os_unfair_lock_unlock(&g_scan_lock);
}

static void apply_options(void) {
    tweak_base_dir = TWEAKS_DIR;
}

static void on_options_changed(void) {
    syslog(LOG_INFO, "opener: options changed, reloading tweaks");
    apply_options();
    scan_tweaks();
}

static void setup_reload_handler(void) {
    signal(SIGUSR1, SIG_IGN);
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
    if (!source) {
        syslog(LOG_ERR, "opener: failed to create reload signal source");
        return;
    }
    dispatch_source_set_event_handler(source, ^{
      syslog(LOG_INFO, "opener: reloading tweaks");
      apply_options();
      scan_tweaks();
    });
    dispatch_resume(source);
}

/* Node SEA aborts when DYLD_INSERT_LIBRARIES is still set at main(). Infect
 * only strips for launchd-spawned SEA; parent-spawned SEA inherit the insert
 * and load opener. Clear the env here (before gum/tweaks) so those binaries
 * can run. */
static bool opener_bail_if_node_sea(void) {
    char *exe_path = get_exe_path();
    if (!exe_path)
        return false;
    bool sea = macho_is_node_sea_binary(exe_path);
    if (sea) {
        unsetenv("DYLD_INSERT_LIBRARIES");
        syslog(LOG_NOTICE,
               "opener: Node SEA binary '%s', stripped DYLD_INSERT_LIBRARIES",
               exe_path);
    }
    free(exe_path);
    return sea;
}

__attribute__((constructor)) static void opener_init(void) {
    openlog("opener", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    if (ammonia_in_safe_boot()) {
        syslog(LOG_NOTICE, "opener: safe boot, not loading tweaks");
        return;
    }

    if (opener_bail_if_node_sea())
        return;

    void *gum = dlopen(FRIDAGUM_DYLIB, RTLD_NOW | RTLD_GLOBAL);
    if (!gum) {
        syslog(LOG_ERR, "opener: failed to load fridagum.dylib: %s",
               dlerror());
        return;
    }

    void (*gum_init)(void) = (void (*)(void))dlsym(gum, "gum_init_embedded");
    if (!gum_init) {
        syslog(LOG_ERR, "opener: gum_init_embedded not found: %s", dlerror());
        return;
    }
    gum_init();

    void *(*gum_interceptor_obtain)(void) =
        (void *(*)(void))dlsym(gum, "gum_interceptor_obtain");
    if (!gum_interceptor_obtain) {
        syslog(LOG_ERR, "opener: gum_interceptor_obtain not found: %s",
               dlerror());
        return;
    }
    g_interceptor = gum_interceptor_obtain();

    syslog(LOG_INFO, "opener: initializing for pid %d", getpid());

    apply_options();
    if (!fangs_load_options().pauseInjection) {
        scan_tweaks();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!fangs_load_options().pauseInjection)
                scan_tweaks();
        });
    }
    setup_reload_handler();
    fangs_watch_options(on_options_changed);
}
