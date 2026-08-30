#include "tweak_utils.h"
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

bool path_ends_with(const char *path, const char *name) {
    if (!path || !name) return false;
    size_t path_len = strlen(path);
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > path_len) return false;
    return (strncmp(path + path_len - name_len, name, name_len) == 0) &&
           (path_len == name_len || path[path_len - name_len - 1] == '/');
}

bool path_matches_entry(const char *path, const char *entry) {
    if (!path || !entry || entry[0] == '\0') return false;
    if (strchr(entry, '/') != NULL)
        return (strcmp(path, entry) == 0);
    return path_ends_with(path, entry);
}

bool is_safe_filename(const char *name) {
    if (!name || !*name) return false;
    return strstr(name, "..") == NULL && strchr(name, '/') == NULL;
}

bool check_file_read(FILE *f, void *buf, size_t len) {
    return fread(buf, 1, len, f) == len;
}

uint32_t swap32_if(uint32_t val, bool swap) {
    return swap ? OSSwapBigToHostInt32(val) : val;
}

bool ammonia_bootargs_has_safe_mode(const char *args) {
    if (!args)
        return false;
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (p[0] == '-' && p[1] == 'x' &&
            (p[2] == '\0' || p[2] == ' ' || p[2] == '\t'))
            return true;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return false;
}

bool ammonia_in_safe_boot(void) {
    int sb = 0;
    size_t n = sizeof(sb);
    if (sysctlbyname("kern.safeboot", &sb, &n, NULL, 0) == 0 && sb != 0)
        return true;
    char args[1024];
    n = sizeof(args);
    if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) != 0)
        return false;
    if (n >= sizeof(args))
        args[sizeof(args) - 1] = '\0';
    return ammonia_bootargs_has_safe_mode(args);
}

bool macho_has_framework(const char *base, size_t size, const char *framework) {
    uint32_t magic = *(const uint32_t *)base;
    bool swap = (magic == MH_CIGAM_64 || magic == MH_CIGAM);
    uint32_t ncmds;
    size_t header_size;
    const struct load_command *cmds;

    if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64) {
        if (size < sizeof(struct mach_header_64))
            return false;
        const struct mach_header_64 *mh = (const struct mach_header_64 *)base;
        ncmds = swap32_if(mh->ncmds, swap);
        header_size = sizeof(struct mach_header_64);
        cmds = (const struct load_command *)(base + header_size);
    } else if (magic == MH_MAGIC || magic == MH_CIGAM) {
        if (size < sizeof(struct mach_header))
            return false;
        const struct mach_header *mh = (const struct mach_header *)base;
        ncmds = swap32_if(mh->ncmds, swap);
        header_size = sizeof(struct mach_header);
        cmds = (const struct load_command *)(base + header_size);
    } else {
        return false;
    }

    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "/%s.framework/", framework);

    const char *end = base + size;
    const struct load_command *cursor = cmds;
    for (uint32_t i = 0; i < ncmds; i++) {
        if ((size_t)(end - (const char *)cursor) < sizeof(struct load_command))
            return false;
        uint32_t cmd = swap32_if(cursor->cmd, swap);
        uint32_t cmdsize = swap32_if(cursor->cmdsize, swap);
        if (cmdsize < sizeof(struct load_command) ||
            (size_t)(end - (const char *)cursor) < cmdsize)
            return false;
        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB) {
            const struct dylib_command *dc =
                (const struct dylib_command *)cursor;
            if ((size_t)(end - (const char *)dc) < sizeof(struct dylib_command))
                return false;
            uint32_t name_offset = swap32_if(dc->dylib.name.offset, swap);
            if (name_offset >= cmdsize)
                return false;
            const char *dylib_path = (const char *)cursor + name_offset;
            if (strstr(dylib_path, pattern))
                return true;
        }
        cursor = (const struct load_command *)((const char *)cursor + cmdsize);
    }
    return false;
}

#define MAX_LOAD_COMMANDS (16u * 1024u * 1024u)
#define MAX_FAT_ARCH 16

static bool pread_all(int fd, off_t off, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0)
            return false;
        p += r;
        off += r;
        n -= (size_t)r;
    }
    return true;
}

static bool macho_slice_links_to_framework(int fd, off_t slice_off,
                                           size_t slice_size,
                                           const char *framework) {
    uint32_t magic;
    if (slice_size < sizeof(magic) ||
        !pread_all(fd, slice_off, &magic, sizeof(magic)))
        return false;

    bool swap = (magic == MH_CIGAM_64 || magic == MH_CIGAM);
    uint32_t sizeofcmds = 0;
    size_t header_size = 0;

    if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64) {
        struct mach_header_64 mh;
        if (slice_size < sizeof(mh) ||
            !pread_all(fd, slice_off, &mh, sizeof(mh)))
            return false;
        sizeofcmds = swap32_if(mh.sizeofcmds, swap);
        header_size = sizeof(mh);
    } else if (magic == MH_MAGIC || magic == MH_CIGAM) {
        struct mach_header mh;
        if (slice_size < sizeof(mh) ||
            !pread_all(fd, slice_off, &mh, sizeof(mh)))
            return false;
        sizeofcmds = swap32_if(mh.sizeofcmds, swap);
        header_size = sizeof(mh);
    } else {
        return false;
    }

    if (sizeofcmds == 0 || sizeofcmds > MAX_LOAD_COMMANDS)
        return false;
    if (header_size + (size_t)sizeofcmds > slice_size)
        return false;

    size_t blob = header_size + (size_t)sizeofcmds;
    char *buf = malloc(blob);
    if (!buf)
        return false;
    bool ok = pread_all(fd, slice_off, buf, blob) &&
              macho_has_framework(buf, blob, framework);
    free(buf);
    return ok;
}

static bool fd_links_to_framework(int fd, size_t file_size,
                                  const char *framework) {
    uint32_t magic;
    if (file_size < sizeof(magic) || !pread_all(fd, 0, &magic, sizeof(magic)))
        return false;

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header fh;
        if (file_size < sizeof(fh) || !pread_all(fd, 0, &fh, sizeof(fh)))
            return false;
        uint32_t narch = OSSwapBigToHostInt32(fh.nfat_arch);
        if (narch == 0 || narch > MAX_FAT_ARCH)
            return false;
        size_t arches_size = (size_t)narch * sizeof(struct fat_arch);
        if (file_size < sizeof(fh) + arches_size)
            return false;
        struct fat_arch *archs = malloc(arches_size);
        if (!archs)
            return false;
        if (!pread_all(fd, (off_t)sizeof(fh), archs, arches_size)) {
            free(archs);
            return false;
        }
        bool found = false;
        for (uint32_t i = 0; i < narch && !found; i++) {
            uint32_t offset = OSSwapBigToHostInt32(archs[i].offset);
            uint32_t size = OSSwapBigToHostInt32(archs[i].size);
            if ((size_t)offset >= file_size)
                continue;
            size_t slice = size;
            if ((size_t)offset + slice > file_size)
                slice = file_size - (size_t)offset;
            found = macho_slice_links_to_framework(fd, (off_t)offset, slice,
                                                   framework);
        }
        free(archs);
        return found;
    }

    return macho_slice_links_to_framework(fd, 0, file_size, framework);
}

bool process_has_framework(const char *framework) {
    if (!framework || !*framework)
        return false;
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "/%s.framework/", framework);
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, pattern))
            return true;
    }
    return false;
}

bool exe_links_to_framework(const char *exe_path, const char *framework) {
    if (!exe_path || !framework)
        return false;
    int fd = open(exe_path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return false;
    }
    bool result = fd_links_to_framework(fd, (size_t)st.st_size, framework);
    close(fd);
    return result;
}

static int list_match(const char *path, const char *exe) {
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char *line = NULL;
    size_t cap = 0;
    int matched = 0;
    ssize_t nread;
    while ((nread = getline(&line, &cap, f)) != -1) {
        if (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r'))
            line[--nread] = '\0';
        while (nread > 0 && (line[nread - 1] == ' ' || line[nread - 1] == '\t'))
            line[--nread] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            continue;
        if (path_matches_entry(exe, p)) {
            matched = 1;
            break;
        }
    }
    free(line);
    fclose(f);
    return matched;
}

bool check_list_match(const char *path, const char *exe) {
    return list_match(path, exe) == 1;
}

bool is_tweak_stat_safe(const struct stat *st) {
    return st && st->st_uid == 0 && !(st->st_mode & (S_IWGRP | S_IWOTH));
}

bool is_tweak_safe(const char *full_path) {
    struct stat st;
    if (stat(full_path, &st) != 0)
        return false;
    return is_tweak_stat_safe(&st);
}

bool should_load_tweak(const char *dir, const char *name, const char *exe) {
    if (!is_safe_filename(name))
        return false;

    char wl[PATH_MAX], bl[PATH_MAX];
    snprintf(wl, sizeof(wl), "%s/%s.whitelist", dir, name);
    snprintf(bl, sizeof(bl), "%s/%s.blacklist", dir, name);

    int wl_r = list_match(wl, exe);
    if (wl_r >= 0)
        return wl_r == 1;
    int bl_r = list_match(bl, exe);
    if (bl_r >= 0)
        return bl_r == 0;
    return true;
}

char *get_exe_path(void) {
    uint32_t bufsize = 0;
    _NSGetExecutablePath(NULL, &bufsize);
    if (bufsize == 0)
        return NULL;
    char *path = malloc(bufsize);
    if (!path)
        return NULL;
    if (_NSGetExecutablePath(path, &bufsize) != 0) {
        free(path);
        return NULL;
    }
    char *resolved = realpath(path, NULL);
    if (resolved) {
        free(path);
        return resolved;
    }
    return path;
}

static char **s_enabled_cache = NULL;
static int s_enabled_cache_count = 0;
static bool s_enabled_cache_loaded = false;
static os_unfair_lock s_enabled_cache_lock = OS_UNFAIR_LOCK_INIT;

CFDictionaryRef fangs_read_plist_dictionary(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0 || len > 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    if (!check_file_read(f, buf, (size_t)len)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    CFDataRef cfData = CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault, (const UInt8 *)buf, (CFIndex)len, kCFAllocatorNull);
    if (!cfData) { free(buf); return NULL; }

    CFPropertyListRef plist = CFPropertyListCreateWithData(
        kCFAllocatorDefault, cfData, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(cfData);
    free(buf);

    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
        if (plist) CFRelease(plist);
        return NULL;
    }

    return (CFDictionaryRef)plist;
}

static void load_enabled_cache(void) {
    CFDictionaryRef dict = fangs_read_plist_dictionary(
        "/private/var/ammonia/core/current.options");
    if (!dict) return;

    CFArrayRef arr = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("enabledTweaks"));
    if (arr && CFGetTypeID(arr) == CFArrayGetTypeID()) {
        CFIndex count = CFArrayGetCount(arr);
        for (CFIndex i = 0; i < count; i++) {
            CFStringRef s = (CFStringRef)CFArrayGetValueAtIndex(arr, i);
            if (s && CFGetTypeID(s) == CFStringGetTypeID()) {
                char name[PATH_MAX];
                if (CFStringGetCString(s, name, sizeof(name), kCFStringEncodingUTF8)) {
                    char **tmp = realloc(s_enabled_cache, (size_t)(s_enabled_cache_count + 1) * sizeof(char *));
                    if (tmp) {
                        s_enabled_cache = tmp;
                        s_enabled_cache[s_enabled_cache_count] = strdup(name);
                        if (s_enabled_cache[s_enabled_cache_count])
                            s_enabled_cache_count++;
                    }
                }
            }
        }
    }

    CFRelease(dict);
}

bool is_tweak_enabled(const char *name) {
    if (!name || !*name) return false;
    os_unfair_lock_lock(&s_enabled_cache_lock);
    if (!s_enabled_cache_loaded) {
        load_enabled_cache();
        s_enabled_cache_loaded = true;
    }
    bool found = false;
    for (int i = 0; i < s_enabled_cache_count; i++) {
        if (strcmp(s_enabled_cache[i], name) == 0) {
            found = true;
            break;
        }
    }
    os_unfair_lock_unlock(&s_enabled_cache_lock);
    return found;
}

void clear_tweak_enabled_cache(void) {
    os_unfair_lock_lock(&s_enabled_cache_lock);
    for (int i = 0; i < s_enabled_cache_count; i++)
        free(s_enabled_cache[i]);
    free(s_enabled_cache);
    s_enabled_cache = NULL;
    s_enabled_cache_count = 0;
    s_enabled_cache_loaded = false;
    os_unfair_lock_unlock(&s_enabled_cache_lock);
}

bool check_dylib_options(const char *dir, const char *name, const char *exe) {
    char optpath[PATH_MAX];
    snprintf(optpath, sizeof(optpath), "%s/%s.options", dir, name);

    CFDictionaryRef dict = fangs_read_plist_dictionary(optpath);
    if (!dict) return true;
    bool should_load = true;

    CFArrayRef frameworks = (CFArrayRef)CFDictionaryGetValue(
        dict, CFSTR("frameworkDependencies"));
    if (should_load && frameworks &&
        CFGetTypeID(frameworks) == CFArrayGetTypeID() &&
        CFArrayGetCount(frameworks) > 0) {
        should_load = false;
        CFIndex count = CFArrayGetCount(frameworks);
        for (CFIndex i = 0; i < count; i++) {
            CFStringRef str =
                (CFStringRef)CFArrayGetValueAtIndex(frameworks, i);
            if (str && CFGetTypeID(str) == CFStringGetTypeID()) {
                char fname[256];
                CFStringGetCString(str, fname, sizeof(fname),
                                   kCFStringEncodingUTF8);
                if (exe_links_to_framework(exe, fname) ||
                    process_has_framework(fname)) {
                    should_load = true;
                    break;
                }
            }
        }
    }

    CFArrayRef blacklisted = (CFArrayRef)CFDictionaryGetValue(
        dict, CFSTR("blacklistedApps"));
    if (should_load && blacklisted &&
        CFGetTypeID(blacklisted) == CFArrayGetTypeID() &&
        CFArrayGetCount(blacklisted) > 0) {
        const char *base = strrchr(exe, '/');
        base = base ? base + 1 : exe;
        CFIndex count = CFArrayGetCount(blacklisted);
        for (CFIndex i = 0; i < count; i++) {
            CFStringRef str =
                (CFStringRef)CFArrayGetValueAtIndex(blacklisted, i);
            if (str && CFGetTypeID(str) == CFStringGetTypeID()) {
                char appname[256];
                CFStringGetCString(str, appname, sizeof(appname),
                                   kCFStringEncodingUTF8);
                if (path_matches_entry(exe, appname) ||
                    path_matches_entry(base, appname)) {
                    should_load = false;
                    break;
                }
            }
        }
    }

    CFRelease(dict);
    return should_load;
}
