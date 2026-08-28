#include "bundle_copy.h"
#include "pac_utils.h"
#include "log.h"
#include <copyfile.h>
#include <errno.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>

bool path_is_bundle(const char *path) {
    if (!path)
        return false;
    const char *dot = strstr(path, ".app");
    while (dot) {
        char c = dot[4];
        if (c == '/' || c == '\0')
            return true;
        dot = strstr(dot + 4, ".app");
    }
    return false;
}

bool get_bundle_executable_path(const char *bundle_path, char *exec_path, size_t exec_path_size) {
    CFURLRef bundle_url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, (const UInt8 *)bundle_path,
        (CFIndex)strlen(bundle_path), true);
    if (!bundle_url)
        return false;

    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, bundle_url);
    CFRelease(bundle_url);
    if (!bundle)
        return false;

    CFURLRef exec_url = CFBundleCopyExecutableURL(bundle);
    CFRelease(bundle);
    if (!exec_url)
        return false;

    bool result = CFURLGetFileSystemRepresentation(
        exec_url, true, (UInt8 *)exec_path, (CFIndex)exec_path_size);
    CFRelease(exec_url);
    return result;
}

bool copy_dir_recursive(const char *src_path, const char *dst_path) {
    copyfile_flags_t flags = COPYFILE_ALL | COPYFILE_RECURSIVE;
#ifdef COPYFILE_CLONE
    flags |= COPYFILE_CLONE;
#endif
    if (copyfile(src_path, dst_path, NULL, flags) != 0) {
#ifdef COPYFILE_CLONE
        flags &= ~COPYFILE_CLONE;
        if (copyfile(src_path, dst_path, NULL, flags) == 0)
            return true;
#endif
        log_error("[copy_dir] copyfile failed: %s -> %s (%s)", src_path,
                  dst_path, strerror(errno));
        return false;
    }
    return true;
}

bool resign_bundle(const char *bundle_path) {
    char exec_path[PATH_MAX];
    if (!get_bundle_executable_path(bundle_path, exec_path, sizeof(exec_path)))
        return false;
    if (!strip_code_signature_file(exec_path))
        return false;
    return sign_file(exec_path, NULL);
}
