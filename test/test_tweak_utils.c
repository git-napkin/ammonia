#include "../syphon/tweak_utils.h"
#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_pass = 0, tests_fail = 0;

#define TEST(name) do { printf("  TEST: %s ... ", name); } while(0)
#define PASS do { printf("PASS\n"); tests_pass++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_fail++; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

static void test_path_ends_with(void) {
    TEST("path_ends_with");
    ASSERT(path_ends_with("/usr/bin/Foo", "Foo"), "basic");
    ASSERT(!path_ends_with("/usr/bin/Foo", "FooBar"), "partial");
    ASSERT(!path_ends_with("/usr/bin/Foo", "oo"), "suffix match");
    ASSERT(path_ends_with("/System/Library/Frameworks/foo.framework/foo", "foo"), "framework exe");
    ASSERT(!path_ends_with(NULL, "foo"), "null path");
    ASSERT(!path_ends_with("foo", NULL), "null name");
    PASS;
}

static void test_path_matches_entry(void) {
    TEST("path_matches_entry");
    ASSERT(path_matches_entry("/usr/bin/foo", "foo"), "basename match");
    ASSERT(path_matches_entry("/usr/bin/foo", "/usr/bin/foo"), "full path match");
    ASSERT(!path_matches_entry("/usr/bin/foo", "/usr/bin/bar"), "full path mismatch");
    ASSERT(!path_matches_entry("/usr/bin/foo", "bar"), "basename mismatch");
    ASSERT(path_matches_entry("/usr/bin/Foo", "Foo"), "case-sensitive match");
    ASSERT(path_matches_entry("/usr/bin/foo", "*"), "wildcard match");
    ASSERT(path_matches_entry("/any/path", "*"), "wildcard any path");
    ASSERT(!path_matches_entry("/usr/bin/foo", ""), "empty entry");
    ASSERT(!path_matches_entry(NULL, "foo"), "null path");
    PASS;
}

static void test_is_safe_filename(void) {
    TEST("is_safe_filename");
    ASSERT(is_safe_filename("test.dylib"), "normal");
    ASSERT(!is_safe_filename(".."), "parent dir");
    ASSERT(!is_safe_filename("../test.dylib"), "path traversal");
    ASSERT(!is_safe_filename("foo/bar.dylib"), "contains slash");
    ASSERT(!is_safe_filename(""), "empty");
    ASSERT(!is_safe_filename(NULL), "null");
    ASSERT(!is_safe_filename("foo..bar"), "contains dots"); /* .. is in the name */
    PASS;
}

static void test_swap32_if(void) {
    TEST("swap32_if");
    ASSERT(swap32_if(0x12345678, false) == 0x12345678, "no swap");
    ASSERT(swap32_if(0x78563412, true) == 0x12345678, "swap");
    PASS;
}

static void test_is_tweak_safe(void) {
    TEST("is_tweak_safe (no file = false)");
    ASSERT(!is_tweak_safe("/nonexistent/path.dylib"), "no file");
    PASS;
}

static void test_check_list_match_no_file(void) {
    TEST("check_list_match (no file)");
    ASSERT(!check_list_match("/nonexistent.list", "/usr/bin/foo"), "no file = false");
    PASS;
}

static void test_should_load_tweak(void) {
    TEST("should_load_tweak");
    char tmpl[] = "/tmp/pp_tweak_test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    ASSERT(dir != NULL, "mkdtemp");

    char dylib[1024], wl[1024], bl[1024];
    snprintf(dylib, sizeof(dylib), "%s/foo.dylib", dir);
    snprintf(wl, sizeof(wl), "%s/foo.dylib.whitelist", dir);
    snprintf(bl, sizeof(bl), "%s/foo.dylib.blacklist", dir);

    FILE *f = fopen(dylib, "w");
    ASSERT(f != NULL, "create dylib");
    fclose(f);

    ASSERT(should_load_tweak(dir, "foo.dylib",
                             "/Applications/Safari.app/Contents/MacOS/Safari"),
           "no sidecar allows all");
    ASSERT(!should_load_tweak(dir, "../foo.dylib",
                              "/Applications/Safari.app/Contents/MacOS/Safari"),
           "reject traversal name");

    f = fopen(wl, "w");
    ASSERT(f != NULL, "create whitelist");
    fprintf(f, "Safari\n");
    fclose(f);
    ASSERT(should_load_tweak(dir, "foo.dylib",
                             "/Applications/Safari.app/Contents/MacOS/Safari"),
           "whitelist hit");
    ASSERT(!should_load_tweak(dir, "foo.dylib",
                              "/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder"),
           "whitelist miss");

    f = fopen(wl, "w");
    ASSERT(f != NULL, "truncate whitelist");
    fclose(f);
    ASSERT(!should_load_tweak(dir, "foo.dylib",
                              "/Applications/Safari.app/Contents/MacOS/Safari"),
           "empty whitelist matches nothing");

    unlink(wl);
    f = fopen(bl, "w");
    ASSERT(f != NULL, "create blacklist");
    fprintf(f, "Finder\n");
    fclose(f);
    ASSERT(should_load_tweak(dir, "foo.dylib",
                             "/Applications/Safari.app/Contents/MacOS/Safari"),
           "blacklist other");
    ASSERT(!should_load_tweak(dir, "foo.dylib",
                              "/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder"),
           "blacklist hit");

    f = fopen(bl, "w");
    ASSERT(f != NULL, "create wildcard blacklist");
    fprintf(f, "*\n");
    fclose(f);
    ASSERT(!should_load_tweak(dir, "foo.dylib",
                              "/Applications/Safari.app/Contents/MacOS/Safari"),
           "wildcard blacklist denies all");

    unlink(bl);
    f = fopen(wl, "w");
    ASSERT(f != NULL, "create wildcard whitelist");
    fprintf(f, "*\n");
    fclose(f);
    ASSERT(should_load_tweak(dir, "foo.dylib",
                             "/Applications/Safari.app/Contents/MacOS/Safari"),
           "wildcard whitelist allows all");
    ASSERT(should_load_tweak(dir, "foo.dylib",
                             "/System/Library/CoreServices/Finder.app/Contents/MacOS/Finder"),
           "wildcard whitelist allows Finder");

    unlink(dylib);
    unlink(wl);
    rmdir(dir);
    PASS;
}

static void test_check_dylib_options_path_exclusions(void) {
    TEST("check_dylib_options path exclusions");
    char tmpl[] = "/tmp/pp_dylib_opts.XXXXXX";
    char *dir = mkdtemp(tmpl);
    ASSERT(dir != NULL, "mkdtemp");

    ASSERT(check_dylib_options(dir, "foo.dylib",
                               "/Applications/Safari.app/Contents/MacOS/Safari"),
           "normal app allowed without options");
    ASSERT(!check_dylib_options(
               dir, "foo.dylib",
               "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"),
           "Frameworks component excluded");
    ASSERT(!check_dylib_options(
               dir, "foo.dylib",
               "/System/Library/PrivateFrameworks/Foo.framework/Foo"),
           "PrivateFrameworks component excluded");
    ASSERT(!check_dylib_options(dir, "foo.dylib", "/usr/libexec/xpcproxy"),
           "libexec component excluded");
    ASSERT(!check_dylib_options(dir, "foo.dylib", "/usr/sbin/cupsd"),
           "sbin component excluded");
    ASSERT(!check_dylib_options(
               dir, "foo.dylib",
               "/System/Library/DriverExtensions/com.apple.foo.dext/foo"),
           "DriverExtensions component excluded");
    ASSERT(check_dylib_options(dir, "foo.dylib", "/usr/bin/ssh"),
           "bin not excluded");

    rmdir(dir);
    PASS;
}

static int write_min_macho_with_appkit(const char *path, off_t total_size) {
    struct mach_header_64 mh;
    memset(&mh, 0, sizeof(mh));
    mh.magic = MH_MAGIC_64;
    mh.cputype = CPU_TYPE_ARM64;
    mh.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    mh.filetype = MH_EXECUTE;
    mh.ncmds = 1;

    const char *lib =
        "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
    size_t namelen = strlen(lib) + 1;
    uint32_t name_off = (uint32_t)sizeof(struct dylib_command);
    uint32_t cmdsize = (uint32_t)((name_off + namelen + 7u) & ~7u);
    mh.sizeofcmds = cmdsize;

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, &mh, sizeof(mh)) != (ssize_t)sizeof(mh)) {
        close(fd);
        return -1;
    }

    struct dylib_command dc;
    memset(&dc, 0, sizeof(dc));
    dc.cmd = LC_LOAD_DYLIB;
    dc.cmdsize = cmdsize;
    dc.dylib.name.offset = name_off;
    if (write(fd, &dc, sizeof(dc)) != (ssize_t)sizeof(dc)) {
        close(fd);
        return -1;
    }
    if (write(fd, lib, namelen) != (ssize_t)namelen) {
        close(fd);
        return -1;
    }
    size_t pad = (size_t)cmdsize - name_off - namelen;
    char zeros[8] = {0};
    if (pad > 0 && write(fd, zeros, pad) != (ssize_t)pad) {
        close(fd);
        return -1;
    }
    if (ftruncate(fd, total_size) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void test_exe_links_to_framework_large_file(void) {
    TEST("exe_links_to_framework (file > 64MiB)");
    char tmpl[] = "/tmp/pp_macho_fw.XXXXXX";
    int tfd = mkstemp(tmpl);
    ASSERT(tfd >= 0, "mkstemp");
    close(tfd);

    off_t huge = (off_t)80 * 1024 * 1024;
    ASSERT(write_min_macho_with_appkit(tmpl, huge) == 0, "write macho");
    ASSERT(exe_links_to_framework(tmpl, "AppKit"), "AppKit in oversized file");
    ASSERT(!exe_links_to_framework(tmpl, "SpriteKit"), "missing framework");
    unlink(tmpl);
    PASS;
}

static void test_safe_boot_bootargs(void) {
    TEST("ammonia_bootargs_has_safe_mode");
    ASSERT(!ammonia_bootargs_has_safe_mode(NULL), "null");
    ASSERT(!ammonia_bootargs_has_safe_mode(""), "empty");
    ASSERT(ammonia_bootargs_has_safe_mode("-x"), "bare -x");
    ASSERT(ammonia_bootargs_has_safe_mode("-arm64e_preview_abi -x"), "trailing");
    ASSERT(ammonia_bootargs_has_safe_mode("-x -v"), "leading");
    ASSERT(!ammonia_bootargs_has_safe_mode("-xhigh"), "not a prefix of another flag");
    ASSERT(!ammonia_bootargs_has_safe_mode("-arm64e_preview_abi"), "normal args");
    PASS;
}

int main(void) {
    printf("tweak_utils tests:\n");
    test_path_ends_with();
    test_path_matches_entry();
    test_is_safe_filename();
    test_swap32_if();
    test_is_tweak_safe();
    test_check_list_match_no_file();
    test_should_load_tweak();
    test_check_dylib_options_path_exclusions();
    test_exe_links_to_framework_large_file();
    test_safe_boot_bootargs();

    printf("\n%d passed, %d failed\n", tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
