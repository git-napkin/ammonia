#include "../syphon/macho_sea.h"

#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_pass = 0, tests_fail = 0;

#define TEST(name)                                                             \
    do {                                                                       \
        printf("  TEST: %s ... ", name);                                       \
    } while (0)
#define PASS                                                                   \
    do {                                                                       \
        printf("PASS\n");                                                      \
        tests_pass++;                                                          \
    } while (0)
#define FAIL(msg)                                                              \
    do {                                                                       \
        printf("FAIL: %s\n", msg);                                             \
        tests_fail++;                                                          \
    } while (0)
#define ASSERT(cond, msg)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            FAIL(msg);                                                         \
            return;                                                            \
        }                                                                      \
    } while (0)

static char *write_tmp(const void *buf, size_t n) {
    char *path = strdup("/tmp/ammonia_sea_XXXXXX");
    if (!path)
        return NULL;
    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }
    ssize_t w = write(fd, buf, n);
    close(fd);
    if (w != (ssize_t)n) {
        unlink(path);
        free(path);
        return NULL;
    }
    return path;
}

static void test_null_path(void) {
    TEST("null path");
    ASSERT(!macho_is_node_sea_binary(NULL), "null");
    ASSERT(!macho_is_node_sea_binary("/no/such/ammonia_sea_file"), "missing");
    PASS;
}

static void test_detects_sea_section(void) {
    TEST("detects __NODE_SEA_BLOB");
    struct {
        struct mach_header_64 hdr;
        struct segment_command_64 seg;
        struct section_64 sect;
    } blob;
    memset(&blob, 0, sizeof(blob));
    blob.hdr.magic = MH_MAGIC_64;
    blob.hdr.ncmds = 1;
    blob.hdr.sizeofcmds =
        (uint32_t)(sizeof(blob.seg) + sizeof(blob.sect));
    blob.seg.cmd = LC_SEGMENT_64;
    blob.seg.cmdsize = blob.hdr.sizeofcmds;
    blob.seg.nsects = 1;
    memcpy(blob.sect.sectname, "__NODE_SEA_BLOB", 16);

    char *path = write_tmp(&blob, sizeof(blob));
    ASSERT(path != NULL, "tmp");
    int hit = macho_is_node_sea_binary(path);
    unlink(path);
    free(path);
    ASSERT(hit, "sea section");
    PASS;
}

static void test_rejects_nsects_past_cmdsize(void) {
    TEST("rejects nsects past cmdsize");
    struct {
        struct mach_header_64 hdr;
        struct segment_command_64 seg;
    } blob;
    memset(&blob, 0, sizeof(blob));
    blob.hdr.magic = MH_MAGIC_64;
    blob.hdr.ncmds = 1;
    blob.hdr.sizeofcmds = (uint32_t)sizeof(blob.seg);
    blob.seg.cmd = LC_SEGMENT_64;
    blob.seg.cmdsize = (uint32_t)sizeof(blob.seg);
    blob.seg.nsects = 100000;

    char *path = write_tmp(&blob, sizeof(blob));
    ASSERT(path != NULL, "tmp");
    int hit = macho_is_node_sea_binary(path);
    unlink(path);
    free(path);
    ASSERT(!hit, "oversized nsects");
    PASS;
}

static void test_rejects_tiny_cmdsize(void) {
    TEST("rejects cmdsize below load_command");
    struct {
        struct mach_header_64 hdr;
        struct load_command lc;
    } blob;
    memset(&blob, 0, sizeof(blob));
    blob.hdr.magic = MH_MAGIC_64;
    blob.hdr.ncmds = 1;
    blob.hdr.sizeofcmds = (uint32_t)sizeof(blob.lc);
    blob.lc.cmd = LC_SEGMENT_64;
    blob.lc.cmdsize = 4;

    char *path = write_tmp(&blob, sizeof(blob));
    ASSERT(path != NULL, "tmp");
    int hit = macho_is_node_sea_binary(path);
    unlink(path);
    free(path);
    ASSERT(!hit, "tiny cmdsize");
    PASS;
}

int main(void) {
    printf("test_macho_sea\n");
    test_null_path();
    test_detects_sea_section();
    test_rejects_nsects_past_cmdsize();
    test_rejects_tiny_cmdsize();
    printf("  %d passed, %d failed\n", tests_pass, tests_fail);
    return tests_fail ? 1 : 0;
}
