#include "../syphon/tweak_utils.h"
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

    unlink(dylib);
    unlink(bl);
    rmdir(dir);
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

    printf("\n%d passed, %d failed\n", tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
