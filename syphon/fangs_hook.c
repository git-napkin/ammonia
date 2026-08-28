#include "envbuf.h"
#include "exe.h"
#include "options_loader.h"
#include "tweak_utils.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include <os/lock.h>

#define SUPPORT_PATH "/opt/pluginplayground/"
#define OPENER_DYLIB SUPPORT_PATH "lib/libplayground_opener.dylib"
#define HOOK_DYLIB SUPPORT_PATH "lib/libfangs_hook.dylib"
#define BLACKLIST_PATH SUPPORT_PATH "ammonia.blacklist"
#define FLAG_DISABLE_XPCPROXY SUPPORT_PATH "disable-xpcproxy"

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

static char **ammonia_blacklist = NULL;
static size_t ammonia_blacklist_count = 0;
static bool disable_xpcproxy_injection = false;
static FangsOptions g_fangs_opts;
static os_unfair_lock g_fangs_opts_lock = OS_UNFAIR_LOCK_INIT;

#define PRIO_DARWIN_ROLE_UI_FOCAL 0x1
#define PRIO_DARWIN_ROLE_UI 0x2
#define PRIO_DARWIN_ROLE_UI_NON_FOCAL 0x4


static void load_ammonia_blacklist(void) {
    FILE *f = fopen(BLACKLIST_PATH, "r");
    if (!f) {
        if (errno != ENOENT)
            syslog(LOG_ERR, "fangs_hook: failed to open blacklist '%s': %s",
                   BLACKLIST_PATH, strerror(errno));
        return;
    }
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    while ((read = getline(&line, &len, f)) != -1) {
        while (read > 0 && (line[read - 1] == '\n' || line[read - 1] == '\r'))
            line[--read] = '\0';
        char *start = line;
        while (*start && isspace((unsigned char)*start))
            start++;
        if (*start == '#' || *start == '\0')
            continue;
        char *end = start + strlen(start) - 1;
        while (end > start && isspace((unsigned char)*end))
            *end-- = '\0';
        char *entry = strdup(start);
        if (!entry) {
            syslog(LOG_ERR, "fangs_hook: failed to allocate blacklist entry");
            continue;
        }
        char **tmp = realloc(ammonia_blacklist,
                             (ammonia_blacklist_count + 1) * sizeof(char *));
        if (!tmp) {
            syslog(LOG_ERR, "fangs_hook: failed to grow blacklist array");
            free(entry);
            continue;
        }
        ammonia_blacklist = tmp;
        ammonia_blacklist[ammonia_blacklist_count++] = entry;
    }
    if (ferror(f))
        syslog(LOG_ERR, "fangs_hook: error reading blacklist '%s': %s",
               BLACKLIST_PATH, strerror(errno));
    free(line);
    fclose(f);
}

static bool is_path_blacklisted(const char *path) {
    if (!path || ammonia_blacklist_count == 0)
        return false;
    for (size_t i = 0; i < ammonia_blacklist_count; ++i) {
        const char *entry = ammonia_blacklist[i];
        if (!entry || entry[0] == '\0')
            continue;
        if (path_matches_entry(path, entry))
            return true;
    }
    return false;
}

static bool is_path_driver(const char *path) {
    return path_ends_with(path, "Driver");
}

static bool macho64_has_sea_blob(int fd) {
    struct mach_header_64 hdr;
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return false;
    if (hdr.magic != MH_MAGIC_64) return false;
    for (uint32_t i = 0; i < hdr.ncmds; i++) {
        off_t cmd_start = lseek(fd, 0, SEEK_CUR);
        if (cmd_start == (off_t)-1) return false;
        struct load_command lc;
        if (read(fd, &lc, sizeof(lc)) != sizeof(lc)) return false;
        if (lc.cmdsize < sizeof(struct load_command))
            return false;
        if (lc.cmd == LC_SEGMENT_64 && lc.cmdsize >=
            (sizeof(struct segment_command_64))) {
            lseek(fd, cmd_start, SEEK_SET);
            struct segment_command_64 seg;
            if (read(fd, &seg, sizeof(seg)) != sizeof(seg)) return false;
            if (strncmp(seg.segname, "__TEXT", sizeof(seg.segname)) == 0) {
                for (uint32_t j = 0; j < seg.nsects; j++) {
                    struct section_64 sect;
                    if (read(fd, &sect, sizeof(sect)) != sizeof(sect))
                        return false;
                    if (strncmp(sect.sectname, "__NODE_SEA_BLOB",
                                sizeof(sect.sectname)) == 0)
                        return true;
                }
            }
        }
        lseek(fd, cmd_start + lc.cmdsize, SEEK_SET);
    }
    return false;
}

#define SEA_CACHE_SIZE 16
static struct {
    dev_t dev;
    ino_t ino;
    struct timespec mtime;
    bool result;
    bool used;
} sea_cache[SEA_CACHE_SIZE];
static unsigned sea_cache_next;
static os_unfair_lock sea_cache_lock = OS_UNFAIR_LOCK_INIT;

static bool is_node_sea_binary_uncached(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    uint32_t magic;
    if (read(fd, &magic, sizeof(magic)) != sizeof(magic)) {
        close(fd);
        return false;
    }
    bool result = false;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header fh;
        lseek(fd, 0, SEEK_SET);
        if (read(fd, &fh, sizeof(fh)) == sizeof(fh)) {
            uint32_t narch = OSSwapBigToHostInt32(fh.nfat_arch);
            for (uint32_t i = 0; i < narch && !result; i++) {
                struct fat_arch arch;
                if (read(fd, &arch, sizeof(arch)) != sizeof(arch)) break;
                lseek(fd, OSSwapBigToHostInt32(arch.offset), SEEK_SET);
                result = macho64_has_sea_blob(fd);
            }
        }
    } else if (magic == MH_MAGIC_64) {
        lseek(fd, 0, SEEK_SET);
        result = macho64_has_sea_blob(fd);
    }
    close(fd);
    return result;
}

static bool is_node_sea_binary(const char *path) {
    if (!path)
        return false;
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    os_unfair_lock_lock(&sea_cache_lock);
    for (unsigned i = 0; i < SEA_CACHE_SIZE; i++) {
        if (sea_cache[i].used && sea_cache[i].dev == st.st_dev &&
            sea_cache[i].ino == st.st_ino &&
            sea_cache[i].mtime.tv_sec == st.st_mtimespec.tv_sec &&
            sea_cache[i].mtime.tv_nsec == st.st_mtimespec.tv_nsec) {
            bool cached = sea_cache[i].result;
            os_unfair_lock_unlock(&sea_cache_lock);
            return cached;
        }
    }
    os_unfair_lock_unlock(&sea_cache_lock);
    bool result = is_node_sea_binary_uncached(path);
    os_unfair_lock_lock(&sea_cache_lock);
    unsigned slot = sea_cache_next++ % SEA_CACHE_SIZE;
    sea_cache[slot].dev = st.st_dev;
    sea_cache[slot].ino = st.st_ino;
    sea_cache[slot].mtime = st.st_mtimespec;
    sea_cache[slot].result = result;
    sea_cache[slot].used = true;
    os_unfair_lock_unlock(&sea_cache_lock);
    return result;
}

static void reload_options(void) {
    FangsOptions new_opts = fangs_load_options();
    os_unfair_lock_lock(&g_fangs_opts_lock);
    g_fangs_opts = new_opts;
    os_unfair_lock_unlock(&g_fangs_opts_lock);
    syslog(LOG_INFO, "fangs_hook: options reloaded: disablePAC=%d pause=%d",
           new_opts.disablePAC, new_opts.pauseInjection);
}

static int spawn_with_env(int (*spawn_fn)(pid_t *, const char *,
                                          const posix_spawn_file_actions_t *,
                                          const posix_spawnattr_t *,
                                          char *const[], char *const[]),
                          pid_t *pid, const char *path,
                          const posix_spawn_file_actions_t *ac,
                          const posix_spawnattr_t *ab, char *const __argv[],
                          char *const __envp[]) {
    if (spawn_fn == NULL) {
        syslog(LOG_ERR, "fangs_hook: original spawn function is NULL for '%s'",
               path ? path : "(null)");
        return EINVAL;
    }

    char **playground = envbuf_mutcopy((const char **)__envp);
    if (__envp != NULL && playground == NULL) {
        syslog(LOG_ERR, "fangs_hook: failed to copy environment for '%s'",
               path ? path : "(null)");
        return ENOMEM;
    }

    uint64_t darwin_role = 0;
    if (ab != NULL && GetDarwinRoleNp != NULL)
        GetDarwinRoleNp(ab, &darwin_role);

    os_unfair_lock_lock(&g_fangs_opts_lock);
    bool pauseInjection = g_fangs_opts.pauseInjection;
    os_unfair_lock_unlock(&g_fangs_opts_lock);
    if (pauseInjection) {
        syslog(LOG_INFO, "fangs_hook: injection paused, skipping for '%s'",
               path ? path : "(null)");
        goto Spawn;
    }

    if (strcmp(path, "/usr/libexec/xpcproxy") == 0) {
        if (!disable_xpcproxy_injection) {
            syslog(LOG_INFO, "fangs_hook: propagating into xpcproxy");
            playground = envbuf_setenv(playground, "DYLD_INSERT_LIBRARIES",
                                       HOOK_DYLIB);
        }
    } else if (!is_path_driver(path)) {
        if (darwin_role == PRIO_DARWIN_ROLE_UI_FOCAL ||
            darwin_role == PRIO_DARWIN_ROLE_UI ||
            darwin_role == PRIO_DARWIN_ROLE_UI_NON_FOCAL) {

            if (is_path_blacklisted(path)) {
                syslog(LOG_INFO,
                       "fangs_hook: skipping opener for blacklisted '%s'",
                       path);
                goto Spawn;
            }

            if (is_node_sea_binary(path)) {
                syslog(LOG_INFO,
                       "fangs_hook: skipping opener for Node SEA binary '%s'",
                       path);
                goto Spawn;
            }

            syslog(LOG_INFO, "fangs_hook: injecting opener into '%s'", path);

            int idx =
                envbuf_find((const char **)playground, "DYLD_INSERT_LIBRARIES");
            if (idx >= 0) {
                const char *old =
                    playground[idx] + strlen("DYLD_INSERT_LIBRARIES=");
                if (!strstr(old, OPENER_DYLIB)) {
                    char *combined = NULL;
                    if (asprintf(&combined, "%s:%s", old, OPENER_DYLIB) != -1) {
                        playground = envbuf_setenv(playground,
                                                   "DYLD_INSERT_LIBRARIES",
                                                   combined);
                        free(combined);
                    }
                }
            } else {
                playground = envbuf_setenv(playground, "DYLD_INSERT_LIBRARIES",
                                           OPENER_DYLIB);
            }
        }
    }

Spawn:
    {
        const char *spawn_path = path;
        char *pac_path = NULL;
        os_unfair_lock_lock(&g_fangs_opts_lock);
        bool disablePAC = g_fangs_opts.disablePAC;
        os_unfair_lock_unlock(&g_fangs_opts_lock);
        if (disablePAC) {
            pac_path = getready_process(path);
            if (pac_path) {
                spawn_path = pac_path;
                syslog(LOG_INFO, "fangs_hook: PAC stripping '%s' -> '%s'", path, pac_path);
            } else if (path && strstr(path, ".app")) {
                syslog(LOG_NOTICE,
                       "fangs_hook: PAC strip skipped for '%s', spawning original",
                       path);
            }
        }
        int k = spawn_fn(pid, spawn_path, ac, ab, __argv, (char *const *)playground);
        free(pac_path);
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

__attribute__((constructor)) static void fangs_hook_init(void) {
    openlog("fangs_hook", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    reload_options();

    load_ammonia_blacklist();
    disable_xpcproxy_injection = access(FLAG_DISABLE_XPCPROXY, F_OK) == 0;
    if (disable_xpcproxy_injection)
        syslog(LOG_INFO, "fangs_hook: xpcproxy propagation disabled");

    *(void **)&GetDarwinRoleNp =
        dlsym(RTLD_DEFAULT, "posix_spawnattr_get_darwin_role_np");
    if (!GetDarwinRoleNp)
        syslog(LOG_WARNING,
               "fangs_hook: posix_spawnattr_get_darwin_role_np not found");

    void *gum = dlopen(SUPPORT_PATH "lib/fridagum.dylib",
                       RTLD_NOW | RTLD_GLOBAL);
    if (!gum) {
        syslog(LOG_ERR, "fangs_hook: failed to load fridagum.dylib: %s",
               dlerror());
        return;
    }

    void (*gum_init)(void) = (void (*)(void))dlsym(gum, "gum_init_embedded");
    if (!gum_init) {
        syslog(LOG_ERR, "fangs_hook: gum_init_embedded not found: %s",
               dlerror());
        dlclose(gum);
        return;
    }
    gum_init();

    void *(*gum_interceptor_obtain)(void) =
        (void *(*)(void))dlsym(gum, "gum_interceptor_obtain");
    if (!gum_interceptor_obtain) {
        syslog(LOG_ERR, "fangs_hook: gum_interceptor_obtain not found: %s",
               dlerror());
        return;
    }
    void *interceptor = gum_interceptor_obtain();

    typedef int (*GumInterceptorBeginTransaction_t)(void *);
    typedef int (*GumInterceptorReplace_t)(void *, void *, void *, void *,
                                           void *);
    typedef int (*GumInterceptorEndTransaction_t)(void *);

    GumInterceptorBeginTransaction_t gum_interceptor_begin_transaction =
        (GumInterceptorBeginTransaction_t)dlsym(gum, "gum_interceptor_begin_transaction");
    GumInterceptorReplace_t gum_interceptor_replace =
        (GumInterceptorReplace_t)dlsym(gum, "gum_interceptor_replace");
    GumInterceptorEndTransaction_t gum_interceptor_end_transaction =
        (GumInterceptorEndTransaction_t)dlsym(gum, "gum_interceptor_end_transaction");

    if (!gum_interceptor_begin_transaction ||
        !gum_interceptor_replace || !gum_interceptor_end_transaction) {
        syslog(LOG_ERR, "fangs_hook: failed to resolve interceptors: %s",
               dlerror());
        return;
    }

    gum_interceptor_begin_transaction(interceptor);

    void *posix_spawn_addr =
        dlsym(RTLD_DEFAULT, "posix_spawn");
    if (posix_spawn_addr == NULL) {
        syslog(LOG_ERR, "fangs_hook: failed to find posix_spawn");
    } else {
        int ret = gum_interceptor_replace(interceptor, posix_spawn_addr,
                                          (void *)SpawnNew, NULL, (void *)&SpawnOld);
        if (ret != 0 || SpawnOld == NULL)
            syslog(LOG_ERR, "fangs_hook: posix_spawn replace failed (%d)",
                   ret);
        else
            syslog(LOG_INFO, "fangs_hook: posix_spawn hooked");
    }

    void *posix_spawnp_addr =
        dlsym(RTLD_DEFAULT, "posix_spawnp");
    if (posix_spawnp_addr == NULL) {
        syslog(LOG_ERR, "fangs_hook: failed to find posix_spawnp");
    } else {
        int ret = gum_interceptor_replace(interceptor, posix_spawnp_addr,
                                          (void *)SpawnPNew, NULL, (void *)&SpawnPOld);
        if (ret != 0 || SpawnPOld == NULL)
            syslog(LOG_ERR, "fangs_hook: posix_spawnp replace failed (%d)",
                   ret);
        else
            syslog(LOG_INFO, "fangs_hook: posix_spawnp hooked");
    }

    gum_interceptor_end_transaction(interceptor);

    fangs_watch_options(reload_options);
    syslog(LOG_INFO, "fangs_hook: initialized");
}
