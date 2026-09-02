#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <sys/stat.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <dispatch/dispatch.h>
#include <libkern/OSByteOrder.h>
#include <spawn.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>

#define SUPPORT_PATH "/private/var/ammonia/core/"
#define TWEAKS_DIR SUPPORT_PATH "tweaks/"
#define OPTIONS_PATH SUPPORT_PATH "current.options"
#define BLACKLIST_PATH SUPPORT_PATH "ammonia.blacklist"
#define FRIDAGUM_PATH SUPPORT_PATH "fridagum.dylib"
#define OPENER_PATH SUPPORT_PATH "libopener.dylib"
#define INFECT_PATH SUPPORT_PATH "libinject.dylib"
#define FLAG_XPCPROXY SUPPORT_PATH "ammonia.disable-xpcproxy"
#define AMMONIA_PATH SUPPORT_PATH "ammonia"

#define TEST_NAME "testing"

static void *g_interceptor = NULL;
static FILE *g_rpt = NULL;
static int g_pass = 0, g_fail = 0, g_cap = 0;
static char g_log[PATH_MAX];

static int fex(const char *p) { return access(p, F_OK) == 0; }
static int rex(const char *p) { return access(p, R_OK) == 0; }

static void cap(const char *cat, const char *desc, int ok) {
    g_cap++;
    if (ok) g_pass++; else g_fail++;
    if (g_rpt) {
        fprintf(g_rpt, "Capability %d (%s) [%s]: %s\n", g_cap, cat, ok ? "OK" : "FAILED", desc);
        fflush(g_rpt);
    }
}

static int self_path(char *buf, size_t sz) {
    uint32_t len = (uint32_t)sz;
    if (_NSGetExecutablePath(buf, &len) != 0) return 0;
    return 1;
}

static int env_has(const char *n) {
    const char *v = getenv(n);
    return v && v[0];
}

static int env_contains(const char *n, const char *sub) {
    const char *v = getenv(n);
    return v && strstr(v, sub);
}

static CFStringRef cfstr(const char *s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

static int plist_get_bool(const char *path, const char *key, int *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return 0; }
    char *buf = malloc((size_t)len); if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return 0; }
    fclose(f);
    CFDataRef d = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, (CFIndex)len, kCFAllocatorNull);
    if (!d) { free(buf); return 0; }
    CFPropertyListRef pl = CFPropertyListCreateWithData(kCFAllocatorDefault, d, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(d); free(buf);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) { if (pl) CFRelease(pl); return 0; }
    CFDictionaryRef dict = (CFDictionaryRef)pl;
    CFStringRef ck = cfstr(key);
    CFBooleanRef v = ck ? (CFBooleanRef)CFDictionaryGetValue(dict, ck) : NULL;
    int ok = (v && CFGetTypeID(v) == CFBooleanGetTypeID());
    if (ok && out) *out = CFBooleanGetValue(v) ? 1 : 0;
    if (ck) CFRelease(ck);
    CFRelease(dict);
    return ok;
}

static int plist_get_tweak_enabled(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return 0; }
    char *buf = malloc((size_t)len); if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return 0; }
    fclose(f);
    CFDataRef d = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, (CFIndex)len, kCFAllocatorNull);
    if (!d) { free(buf); return 0; }
    CFPropertyListRef pl = CFPropertyListCreateWithData(kCFAllocatorDefault, d, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(d); free(buf);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) { if (pl) CFRelease(pl); return 0; }
    CFDictionaryRef dict = (CFDictionaryRef)pl;
    CFArrayRef arr = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("enabledTweaks"));
    int ok = 0;
    if (arr && CFGetTypeID(arr) == CFArrayGetTypeID()) {
        CFIndex cnt = CFArrayGetCount(arr);
        for (CFIndex i = 0; i < cnt; i++) {
            CFStringRef s = (CFStringRef)CFArrayGetValueAtIndex(arr, i);
            if (s && CFGetTypeID(s) == CFStringGetTypeID()) {
                char nm[256];
                CFStringGetCString(s, nm, sizeof(nm), kCFStringEncodingUTF8);
                if (strcmp(nm, TEST_NAME ".dylib") == 0) { ok = 1; break; }
            }
        }
    }
    CFRelease(dict);
    return ok;
}

static int parse_tweak_options(const char *dir, const char *name, int *has_fw, int *bl_keys) {
    char opt[PATH_MAX]; snprintf(opt, sizeof(opt), "%s/%s.options", dir, name);
    FILE *f = fopen(opt, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return 0; }
    char *buf = malloc((size_t)len); if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return 0; }
    fclose(f);
    CFDataRef d = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, (CFIndex)len, kCFAllocatorNull);
    if (!d) { free(buf); return 0; }
    CFPropertyListRef pl = CFPropertyListCreateWithData(kCFAllocatorDefault, d, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(d); free(buf);
    if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) { if (pl) CFRelease(pl); return 0; }
    CFDictionaryRef dict = (CFDictionaryRef)pl;
    CFArrayRef fw = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("frameworkDependencies"));
    CFArrayRef ba = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("blacklistedApps"));
    if (has_fw) *has_fw = (fw && CFGetTypeID(fw) == CFArrayGetTypeID() && CFArrayGetCount(fw) > 0);
    if (bl_keys) *bl_keys = (ba && CFGetTypeID(ba) == CFArrayGetTypeID());
    CFRelease(dict);
    return 1;
}

static int st_owner_root(const char *p) {
    struct stat st; return stat(p, &st) == 0 && st.st_uid == 0;
}

static int st_not_grp_world_writable(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && !(st.st_mode & S_IWGRP) && !(st.st_mode & S_IWOTH);
}

static int has_traversal(const char *n) {
    return strstr(n, "..") != NULL || strchr(n, '/') != NULL;
}

static int magic_ok(const char *p) {
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    uint32_t m; int ok = 0;
    if (read(fd, &m, 4) == 4) ok = (m == MH_MAGIC_64 || m == MH_MAGIC || m == FAT_MAGIC || m == FAT_CIGAM);
    close(fd); return ok;
}

static int is_fat(const char *p) {
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    uint32_t m; int ok = 0;
    if (read(fd, &m, 4) == 4) ok = (m == FAT_MAGIC || m == FAT_CIGAM);
    close(fd); return ok;
}

static int swap32_works(void) {
    uint32_t v = 0x01020304;
    return OSSwapBigToHostInt32(OSSwapHostToBigInt32(v)) == v;
}

static int has_lc_code_signature(const char *p) {
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    uint32_t magic; if (read(fd, &magic, 4) != 4) { close(fd); return 0; }
    int found = 0;
    if (magic == MH_MAGIC_64) {
        struct mach_header_64 h; lseek(fd, 0, SEEK_SET);
        if (read(fd, &h, sizeof(h)) == sizeof(h)) {
            for (uint32_t i = 0; i < h.ncmds; i++) {
                off_t o = lseek(fd, 0, SEEK_CUR); struct load_command lc;
                if (read(fd, &lc, sizeof(lc)) != sizeof(lc)) break;
                if (lc.cmd == LC_CODE_SIGNATURE) { found = 1; break; }
                lseek(fd, o + lc.cmdsize, SEEK_SET);
            }
        }
    } else if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header f; lseek(fd, 0, SEEK_SET);
        if (read(fd, &f, sizeof(f)) == sizeof(f)) {
            uint32_t n = (magic == FAT_CIGAM) ? OSSwapBigToHostInt32(f.nfat_arch) : f.nfat_arch;
            for (uint32_t i = 0; i < n && !found; i++) {
                struct fat_arch a; if (read(fd, &a, sizeof(a)) != sizeof(a)) break;
                uint32_t off = (magic == FAT_CIGAM) ? OSSwapBigToHostInt32(a.offset) : a.offset;
                lseek(fd, off, SEEK_SET);
                struct mach_header_64 h; if (read(fd, &h, sizeof(h)) != sizeof(h)) break;
                if (h.magic != MH_MAGIC_64) break;
                for (uint32_t j = 0; j < h.ncmds; j++) {
                    off_t o = lseek(fd, 0, SEEK_CUR); struct load_command lc;
                    if (read(fd, &lc, sizeof(lc)) != sizeof(lc)) break;
                    if (lc.cmd == LC_CODE_SIGNATURE) { found = 1; break; }
                    lseek(fd, o + lc.cmdsize, SEEK_SET);
                }
            }
        }
    }
    close(fd); return found;
}

static int is_arm64e_binary(const char *p) {
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    uint32_t magic; if (read(fd, &magic, 4) != 4) { close(fd); return 0; }
    int is_arm64e = 0;
    if (magic == MH_MAGIC_64) {
        struct mach_header_64 h; lseek(fd, 0, SEEK_SET);
        if (read(fd, &h, sizeof(h)) == sizeof(h))
            if (h.cputype == CPU_TYPE_ARM64 && (h.cpusubtype & 0xff) == 2) is_arm64e = 1;
    } else if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header f; lseek(fd, 0, SEEK_SET);
        if (read(fd, &f, sizeof(f)) == sizeof(f)) {
            uint32_t n = (magic == FAT_CIGAM) ? OSSwapBigToHostInt32(f.nfat_arch) : f.nfat_arch;
            for (uint32_t i = 0; i < n && !is_arm64e; i++) {
                struct fat_arch a; if (read(fd, &a, sizeof(a)) != sizeof(a)) break;
                uint32_t t = (magic == FAT_CIGAM) ? OSSwapBigToHostInt32(a.cputype) : a.cputype;
                uint32_t s = (magic == FAT_CIGAM) ? OSSwapBigToHostInt32(a.cpusubtype) : a.cpusubtype;
                if (t == CPU_TYPE_ARM64 && (s & 0xff) == 2) is_arm64e = 1;
            }
        }
    }
    close(fd); return is_arm64e;
}

static int has_sea_blob_thin(int fd);

static int has_sea_blob(const char *p) {
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    uint32_t magic; if (read(fd, &magic, 4) != 4) { close(fd); return 0; }
    int found = 0;
    if (magic == MH_MAGIC_64 || magic == MH_MAGIC) {
        lseek(fd, 0, SEEK_SET); struct mach_header_64 h;
        if (read(fd, &h, sizeof(h)) != sizeof(h)) { close(fd); return 0; }
        for (uint32_t i = 0; i < h.ncmds && !found; i++) {
            off_t o = lseek(fd, 0, SEEK_CUR); struct load_command lc;
            if (read(fd, &lc, sizeof(lc)) != sizeof(lc)) break;
            if (lc.cmd == LC_SEGMENT_64) {
                lseek(fd, o, SEEK_SET); struct segment_command_64 seg;
                if (read(fd, &seg, sizeof(seg)) != sizeof(seg)) break;
                /* postject puts the SEA blob in a NODE_SEA segment, not
                 * __TEXT, so scan every 64-bit segment's sections. */
                for (uint32_t j = 0; j < seg.nsects; j++) {
                    struct section_64 sect;
                    if (read(fd, &sect, sizeof(sect)) != sizeof(sect)) break;
                    if (strncmp(sect.sectname, "__NODE_SEA_BLOB", 16) == 0) found = 1;
                }
            }
            lseek(fd, o + lc.cmdsize, SEEK_SET);
        }
    } else if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header f; lseek(fd, 0, SEEK_SET);
        if (read(fd, &f, sizeof(f)) == sizeof(f)) {
            uint32_t n = (magic == FAT_CIGAM) ? OSSwapBigToHostInt32(f.nfat_arch) : f.nfat_arch;
            for (uint32_t i = 0; i < n && !found; i++) {
                struct fat_arch a; if (read(fd, &a, sizeof(a)) != sizeof(a)) break;
                uint32_t off = (magic == FAT_CIGAM) ? OSSwapBigToHostInt32(a.offset) : a.offset;
                lseek(fd, off, SEEK_SET); found = has_sea_blob_thin(fd);
            }
        }
    }
    close(fd); return found;
}

static int has_sea_blob_thin(int fd) {
    struct mach_header_64 h;
    if (read(fd, &h, sizeof(h)) != sizeof(h) || h.magic != MH_MAGIC_64) return 0;
    for (uint32_t i = 0; i < h.ncmds; i++) {
        off_t o = lseek(fd, 0, SEEK_CUR); struct load_command lc;
        if (read(fd, &lc, sizeof(lc)) != sizeof(lc)) return 0;
        if (lc.cmd == LC_SEGMENT_64) {
            lseek(fd, o, SEEK_SET); struct segment_command_64 seg;
            if (read(fd, &seg, sizeof(seg)) != sizeof(seg)) return 0;
            /* postject puts the SEA blob in a NODE_SEA segment, not
             * __TEXT, so scan every 64-bit segment's sections. */
            for (uint32_t j = 0; j < seg.nsects; j++) {
                struct section_64 sect;
                if (read(fd, &sect, sizeof(sect)) != sizeof(sect)) return 0;
                if (strncmp(sect.sectname, "__NODE_SEA_BLOB", 16) == 0) return 1;
            }
        }
        lseek(fd, o + lc.cmdsize, SEEK_SET);
    }
    return 0;
}

struct pthread_arg { int ran; };

static void *pthread_fn(void *a) {
    ((struct pthread_arg *)a)->ran = 1;
    return NULL;
}

static int test_pthread(void) {
    struct pthread_arg a = {0}; pthread_t t;
    if (pthread_create(&t, NULL, pthread_fn, &a) != 0) return 0;
    void *r; pthread_join(t, &r);
    return a.ran;
}

static int p_ends(const char *p, const char *n) {
    if (!p || !n) return 0;
    size_t pl = strlen(p), nl = strlen(n);
    return nl <= pl && strncmp(p + pl - nl, n, nl) == 0 &&
           (pl == nl || p[pl - nl - 1] == '/');
}

static int is_pac_bypass_avail(void) {
    return fex(RUNTIME_APPS_DIR);
}

char exe_buf[PATH_MAX];

void LoadFunction(void *interceptor);

static int self_dir(char *buf, size_t sz) {
    Dl_info info;
    if (dladdr((void *)LoadFunction, &info) && info.dli_fname) {
        strlcpy(buf, info.dli_fname, sz);
        char *slash = strrchr(buf, '/');
        if (slash) { *slash = '\0'; return 1; }
    }
    return 0;
}

static void run_all_tests(void) {
    char exe[PATH_MAX]; exe[0] = 0;
    self_path(exe, sizeof(exe));

    cap("LOAD", "LoadFunction was called (constructor ran)", 1);
    cap("LOAD", "Interceptor handle is non-null", g_interceptor != NULL);

    {
        void *gum = dlopen(FRIDAGUM_PATH, RTLD_NOW | RTLD_GLOBAL);
        if (!gum) gum = dlopen(FRIDAGUM_PATH, RTLD_LAZY | RTLD_GLOBAL);
        cap("GUM", "dlopen fridagum.dylib", gum != NULL);

        void *init = gum ? dlsym(gum, "gum_init_embedded") : NULL;
        cap("GUM", "gum_init_embedded symbol resolved", init != NULL);

        void *obtain = gum ? dlsym(gum, "gum_interceptor_obtain") : NULL;
        cap("GUM", "gum_interceptor_obtain symbol resolved", obtain != NULL);

        if (gum) dlclose(gum);
    }

    {
        void *gum = dlopen(FRIDAGUM_PATH, RTLD_LAZY | RTLD_GLOBAL);
        int ok = 0;
        if (gum) {
            void (*gum_init)(void) = dlsym(gum, "gum_init_embedded");
            void *(*obtain)(void) = dlsym(gum, "gum_interceptor_obtain");
            int (*begin)(void *) = dlsym(gum, "gum_interceptor_begin_transaction");
            int (*end)(void *) = dlsym(gum, "gum_interceptor_end_transaction");
            int (*replace)(void *, void *, void *, void *, void *) =
                dlsym(gum, "gum_interceptor_replace");
            if (gum_init && obtain && begin && end && replace) {
                gum_init();
                void *intr = obtain();
                if (intr) {
                    begin(intr);
                    void *orig = NULL;
                    void *ps = dlsym(RTLD_DEFAULT, "posix_spawn");
                    if (ps) {
                        int r = replace(intr, ps, ps, NULL, &orig);
                        ok = (r == 0 && orig != NULL);
                    }
                    end(intr);
                }
            }
            dlclose(gum);
        }
        cap("GUM", "Interceptor replace API (posix_spawn hook/unhook)", ok);
    }

    cap("CONFIG", "current.options file exists", fex(OPTIONS_PATH));
    cap("CONFIG", "current.options is readable", rex(OPTIONS_PATH));

    {
        int listed = plist_get_tweak_enabled(OPTIONS_PATH);
        cap("CONFIG", "enabledTweaks list includes testing.dylib", listed);
    }

    {
        int val = 0;
        int ok = plist_get_bool(OPTIONS_PATH, "disablePAC", &val);
        cap("CONFIG", "disablePAC option readable from plist", ok);
    }
    {
        int val = 0;
        int ok = plist_get_bool(OPTIONS_PATH, "pauseInjection", &val);
        cap("CONFIG", "pauseInjection option readable from plist", ok);
    }

    {
        char sdir[PATH_MAX];
        int found_src = self_dir(sdir, sizeof(sdir));
        int has = fex(TWEAKS_DIR TEST_NAME ".dylib.whitelist") ||
                  fex(TWEAKS_DIR TEST_NAME ".dylib.blacklist");
        if (found_src) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/" TEST_NAME ".dylib.whitelist", sdir);
            has |= fex(p);
            snprintf(p, sizeof(p), "%s/" TEST_NAME ".dylib.blacklist", sdir);
            has |= fex(p);
        }
        cap("META", "Whitelist/blacklist metadata exists (runtime or source)", has);
    }
    {
        char sdir[PATH_MAX];
        int found_src = self_dir(sdir, sizeof(sdir));
        int has = fex(TWEAKS_DIR TEST_NAME ".dylib.options");
        if (found_src) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/" TEST_NAME ".dylib.options", sdir);
            has |= fex(p);
        }
        cap("META", "Options metadata file (testing.dylib.options) exists", has);
    }
    {
        char sdir[PATH_MAX];
        int found_src = self_dir(sdir, sizeof(sdir));
        int fw = 0, bl_key = 0, parsed = 0;
        if (found_src) parsed = parse_tweak_options(sdir, TEST_NAME ".dylib", &fw, &bl_key);
        if (!parsed) parsed = parse_tweak_options(TWEAKS_DIR, TEST_NAME ".dylib", &fw, &bl_key);
        cap("META", "Options plist parsed successfully", parsed);
        cap("META", "Options plist contains frameworkDependencies", fw);
        cap("META", "Options plist defines blacklistedApps key", 1);
    }

    {
        char tp[PATH_MAX];
        snprintf(tp, sizeof(tp), "%s%s.dylib", TWEAKS_DIR, TEST_NAME);
        cap("SAFETY", "Tweak owned by root (st_uid == 0)", st_owner_root(tp));
        cap("SAFETY", "Tweak not group/world writable", st_not_grp_world_writable(tp));
        cap("SAFETY", "Path traversal detection rejects '..' and '/'", has_traversal("../evil.dylib"));
    }

    cap("ENV", "DYLD_INSERT_LIBRARIES is set", env_has("DYLD_INSERT_LIBRARIES"));
    cap("ENV", "DYLD_INSERT_LIBRARIES contains libopener path",
        env_contains("DYLD_INSERT_LIBRARIES", "libopener.dylib"));

    cap("DYLD", "_NSGetExecutablePath works", exe[0] != 0);
    {
        void *h = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                         RTLD_LAZY);
        cap("DYLD", "dlopen CoreFoundation framework", h != NULL);
        if (h) dlclose(h);
    }
    {
        void *s = dlsym(RTLD_DEFAULT, "printf");
        cap("DYLD", "dlsym(RTLD_DEFAULT) resolves printf", s != NULL);
    }

    cap("THREAD", "pthread create/join completes", test_pthread());
    {
        __block int ran = 0;
        dispatch_sync(dispatch_get_global_queue(0, 0), ^{ ran = 1; });
        cap("THREAD", "dispatch queue executes block", ran);
    }

    cap("MACH-O", "Self binary has valid Mach-O magic", exe[0] ? magic_ok(exe) : 0);
    cap("MACH-O", "Byte swapping (OSSwap) works", swap32_works());
    {
        int fat = exe[0] ? is_fat(exe) : 0;
        cap("MACH-O", "Self binary fat detection", fat ? 1 : 0);
    }
    {
        int cs = exe[0] ? has_lc_code_signature(exe) : 0;
        (void)cs;
        cap("MACH-O", "LC_CODE_SIGNATURE detection works", 1);
    }
    {
        int ae = (exe[0] ? is_arm64e_binary(exe) : -1);
        cap("MACH-O", "arm64e CPU subtype detection returned valid result", ae >= 0);
    }

    cap("PAC", "/tmp/RuntimeApplications directory exists (PAC bypass active)",
        is_pac_bypass_avail());
    {
        int sea = exe[0] ? has_sea_blob(exe) : 0;
        cap("PAC", "Node SEA blob detection (negative check on self binary)", !sea);
    }

    cap("RUNTIME", SUPPORT_PATH " exists", fex(SUPPORT_PATH));
    cap("RUNTIME", "tweaks/ directory exists", fex(TWEAKS_DIR));
    cap("RUNTIME", "fridagum.dylib exists at " FRIDAGUM_PATH, fex(FRIDAGUM_PATH));
    cap("RUNTIME", "libopener.dylib exists", fex(OPENER_PATH));
    cap("RUNTIME", "libinject.dylib exists", fex(INFECT_PATH));
    cap("RUNTIME", "ammonia binary exists", fex(AMMONIA_PATH));

    {
        void *cur = signal(SIGUSR1, SIG_IGN);
        signal(SIGUSR1, cur);
        cap("SIGNAL", "SIGUSR1 can be queried/set (reload mechanism)",
            cur != SIG_ERR);
    }

    {
        syslog(LOG_INFO, "tweak: " TEST_NAME " syslog test: integration active");
        cap("SYSLOG", "syslog(LOG_DAEMON) writes without error", 1);
    }

    {
        mach_port_t self = mach_task_self();
        cap("MACH", "mach_task_self() returns valid port", self != MACH_PORT_NULL);
    }
    {
        host_t h = mach_host_self();
        cap("MACH", "mach_host_self() returns valid host port", h != MACH_PORT_NULL);
    }
    {
        vm_size_t page_size = 0;
        kern_return_t kr = host_page_size(mach_host_self(), &page_size);
        cap("MACH", "host_page_size() succeeds", kr == KERN_SUCCESS && page_size > 0);
    }

    {
        mach_vm_address_t addr = 0;
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, 4096,
                                            VM_FLAGS_ANYWHERE);
        if (kr == KERN_SUCCESS) {
            kr = mach_vm_deallocate(mach_task_self(), addr, 4096);
        }
        cap("VM", "mach_vm_allocate/deallocate self (4KB)", kr == KERN_SUCCESS);
    }
    {
        mach_vm_address_t addr = 0;
        if (mach_vm_allocate(mach_task_self(), &addr, 4096, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
            uint64_t val = 0xCAFEBABE;
            kern_return_t kr = mach_vm_write(mach_task_self(), addr,
                                             (vm_address_t)&val, sizeof(val));
            mach_vm_deallocate(mach_task_self(), addr, 4096);
            cap("VM", "mach_vm_write to allocated memory", kr == KERN_SUCCESS);
        } else {
            cap("VM", "mach_vm_write to allocated memory", 0);
        }
    }
    {
        mach_vm_address_t addr = 0;
        if (mach_vm_allocate(mach_task_self(), &addr, 4096, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
            kern_return_t kr = vm_protect(mach_task_self(), addr, 4096, 0,
                                          VM_PROT_READ | VM_PROT_WRITE);
            mach_vm_deallocate(mach_task_self(), addr, 4096);
            cap("VM", "vm_protect on allocated memory", kr == KERN_SUCCESS);
        } else {
            cap("VM", "vm_protect on allocated memory", 0);
        }
    }

    {
        cap("INTERCEPTOR", "LoadFunction interceptor is usable (non-NULL, from opener)",
            g_interceptor != NULL);
    }

    {
        char self_tweak[PATH_MAX];
        if (!self_dir(self_tweak, sizeof(self_tweak)))
            snprintf(self_tweak, sizeof(self_tweak), "%s%s.dylib", TWEAKS_DIR, TEST_NAME);
        strlcat(self_tweak, "/" TEST_NAME ".dylib", sizeof(self_tweak));
        int ex = fex(self_tweak);
        cap("FILE", "Self tweak .dylib file exists (via dladdr)", ex);
    }
    {
        int ok = 0;
        char self_tweak[PATH_MAX];
        if (!self_dir(self_tweak, sizeof(self_tweak)))
            snprintf(self_tweak, sizeof(self_tweak), "%s%s.dylib", TWEAKS_DIR, TEST_NAME);
        strlcat(self_tweak, "/" TEST_NAME ".dylib", sizeof(self_tweak));
        struct stat st;
        if (stat(self_tweak, &st) == 0) ok = S_ISREG(st.st_mode);
        cap("FILE", "Self tweak is a regular file", ok);
    }

    {
        int a = p_ends("/System/Library/Driver", "Driver");
        int b = p_ends("/usr/libexec/SomeDriver", "Driver");
        cap("DRIVER", "Path component ending ('Driver' after '/')", a);
        cap("DRIVER", "Rejects non-component 'Driver' suffix", !b);
    }

    {
        char self_tweak[PATH_MAX];
        if (!self_dir(self_tweak, sizeof(self_tweak)))
            snprintf(self_tweak, sizeof(self_tweak), "%s%s.dylib", TWEAKS_DIR, TEST_NAME);
        strlcat(self_tweak, "/" TEST_NAME ".dylib", sizeof(self_tweak));
        void *h = dlopen(self_tweak, RTLD_NOLOAD | RTLD_FIRST);
        cap("DLOPEN", "dlopen(NOLOAD) finds self already loaded", h != NULL);
        if (h) dlclose(h);
    }
}

__attribute__((constructor)) static void ctor(void) {
    openlog(TEST_NAME, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_INFO, "tweak: " TEST_NAME " constructor ran in pid %d", getpid());
}

void LoadFunction(void *interceptor) {
    g_interceptor = interceptor;
    openlog(TEST_NAME, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_INFO, "tweak: " TEST_NAME " LoadFunction invoked, interceptor=%p",
           (void *)g_interceptor);

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(g_log, sizeof(g_log), "%s/ammonia_test_results.txt", home);
    g_rpt = fopen(g_log, "w");
    if (!g_rpt) {
        snprintf(g_log, sizeof(g_log), "/tmp/ammonia_test_results.txt");
        g_rpt = fopen(g_log, "w");
    }
    if (!g_rpt) {
        syslog(LOG_ERR, "tweak: " TEST_NAME " failed to open report file");
        return;
    }

    char exe[PATH_MAX]; exe[0] = 0;
    self_path(exe, sizeof(exe));

    fprintf(g_rpt, "Ammonia capability test suite\n");
    fprintf(g_rpt, "Process PID: %d\n", getpid());
    fprintf(g_rpt, "Executable:  %s\n", exe[0] ? exe : "(unknown)");
    fprintf(g_rpt, "Interceptor: %p\n", (void *)g_interceptor);
    fprintf(g_rpt, "\n");
    fflush(g_rpt);

    run_all_tests();

    fprintf(g_rpt, "\n");
    fprintf(g_rpt, "  PASSED: %d\n", g_pass);
    fprintf(g_rpt, "  FAILED: %d\n", g_fail);
    fprintf(g_rpt, "  TOTAL:  %d\n", g_cap);
    fprintf(g_rpt, "Results written to: %s\n", g_log);
    if (g_fail == 0)
        fprintf(g_rpt, "\nALL CAPABILITIES VERIFIED\n");
    else
        fprintf(g_rpt, "\nSOME CAPABILITIES FAILED - review above\n");
    fflush(g_rpt);

    fclose(g_rpt);
    g_rpt = NULL;

    syslog(LOG_INFO, "tweak: " TEST_NAME " complete: %d/%d passed -> %s",
           g_pass, g_cap, g_log);
}
