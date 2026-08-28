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
#include <sys/mman.h>
#include <sys/stat.h>
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

static struct {
    char path[PATH_MAX];
    void *map;
    size_t size;
} s_exe_map;

static void unmap_exe(void) {
    if (s_exe_map.map && s_exe_map.map != MAP_FAILED)
        munmap(s_exe_map.map, s_exe_map.size);
    s_exe_map.map = NULL;
    s_exe_map.size = 0;
    s_exe_map.path[0] = '\0';
}

static bool mapped_links_to_framework(const void *mapped, size_t size,
                                      const char *framework) {
    uint32_t magic = *(const uint32_t *)mapped;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        if (size < sizeof(struct fat_header))
            return false;
        const struct fat_header *fh = (const struct fat_header *)mapped;
        uint32_t narch = OSSwapBigToHostInt32(fh->nfat_arch);
        size_t arches_size = (size_t)narch * sizeof(struct fat_arch);
        if (size < sizeof(struct fat_header) + arches_size)
            return false;
        const struct fat_arch *archs =
            (const struct fat_arch *)((const char *)mapped +
                                      sizeof(struct fat_header));
        for (uint32_t i = 0; i < narch; i++) {
            uint32_t offset = OSSwapBigToHostInt32(archs[i].offset);
            if (offset >= size)
                continue;
            if (macho_has_framework((const char *)mapped + offset,
                                    size - offset, framework))
                return true;
        }
        return false;
    }
    return macho_has_framework((const char *)mapped, size, framework);
}

static os_unfair_lock s_exe_map_lock = OS_UNFAIR_LOCK_INIT;

bool exe_links_to_framework(const char *exe_path, const char *framework) {
    if (!exe_path || !framework)
        return false;

    os_unfair_lock_lock(&s_exe_map_lock);
    if (!s_exe_map.map || strcmp(s_exe_map.path, exe_path) != 0) {
        unmap_exe();
        int fd = open(exe_path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0) {
            os_unfair_lock_unlock(&s_exe_map_lock);
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            close(fd);
            os_unfair_lock_unlock(&s_exe_map_lock);
            return false;
        }
        size_t size = (size_t)st.st_size;
        if (size > 64u * 1024u * 1024u) {
            close(fd);
            os_unfair_lock_unlock(&s_exe_map_lock);
            return false;
        }
        void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) {
            os_unfair_lock_unlock(&s_exe_map_lock);
            return false;
        }
        snprintf(s_exe_map.path, sizeof(s_exe_map.path), "%s", exe_path);
        s_exe_map.map = mapped;
        s_exe_map.size = size;
    }

    bool result = mapped_links_to_framework(s_exe_map.map, s_exe_map.size, framework);
    os_unfair_lock_unlock(&s_exe_map_lock);
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
        "/opt/pluginplayground/current.options");
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
                if (exe_links_to_framework(exe, fname)) {
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
