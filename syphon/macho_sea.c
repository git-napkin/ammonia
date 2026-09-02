#include "macho_sea.h"

#include <stdint.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>

#define MACHO_SEA_MAX_NCMDS 4096u
#define MACHO_SEA_MAX_SIZEOFCMDS (16u * 1024u * 1024u)
#define MACHO_SEA_MAX_FAT_ARCH 64u

static bool macho64_has_sea_blob(int fd) {
    struct mach_header_64 hdr;
    if (read(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
        return false;
    if (hdr.magic != MH_MAGIC_64)
        return false;
    if (hdr.ncmds > MACHO_SEA_MAX_NCMDS ||
        hdr.sizeofcmds > MACHO_SEA_MAX_SIZEOFCMDS)
        return false;

    off_t cmds_base = lseek(fd, 0, SEEK_CUR);
    if (cmds_base == (off_t)-1)
        return false;

    for (uint32_t i = 0; i < hdr.ncmds; i++) {
        off_t cmd_start = lseek(fd, 0, SEEK_CUR);
        if (cmd_start == (off_t)-1)
            return false;
        if ((uint64_t)(cmd_start - cmds_base) >= hdr.sizeofcmds)
            return false;

        struct load_command lc;
        if (read(fd, &lc, sizeof(lc)) != sizeof(lc))
            return false;
        if (lc.cmdsize < sizeof(struct load_command))
            return false;
        if ((uint64_t)(cmd_start - cmds_base) + lc.cmdsize > hdr.sizeofcmds)
            return false;

        if (lc.cmd == LC_SEGMENT_64) {
            if (lc.cmdsize < sizeof(struct segment_command_64))
                return false;
            if (lseek(fd, cmd_start, SEEK_SET) == (off_t)-1)
                return false;
            struct segment_command_64 seg;
            if (read(fd, &seg, sizeof(seg)) != sizeof(seg))
                return false;

            uint32_t sect_budget =
                (lc.cmdsize - (uint32_t)sizeof(struct segment_command_64)) /
                (uint32_t)sizeof(struct section_64);
            if (seg.nsects > sect_budget)
                return false;

            for (uint32_t j = 0; j < seg.nsects; j++) {
                struct section_64 sect;
                if (read(fd, &sect, sizeof(sect)) != sizeof(sect))
                    return false;
                if (strncmp(sect.sectname, "__NODE_SEA_BLOB",
                            sizeof(sect.sectname)) == 0)
                    return true;
            }
        }

        if (lseek(fd, cmd_start + lc.cmdsize, SEEK_SET) == (off_t)-1)
            return false;
    }

    return false;
}

bool macho_is_node_sea_binary(const char *path) {
    if (!path)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    uint32_t magic;
    if (read(fd, &magic, sizeof(magic)) != sizeof(magic)) {
        close(fd);
        return false;
    }

    bool result = false;

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        struct fat_header fh;
        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
            close(fd);
            return false;
        }
        if (read(fd, &fh, sizeof(fh)) == sizeof(fh)) {
            uint32_t narch = OSSwapBigToHostInt32(fh.nfat_arch);
            if (narch > MACHO_SEA_MAX_FAT_ARCH)
                narch = 0;
            for (uint32_t i = 0; i < narch && !result; i++) {
                struct fat_arch arch;
                if (read(fd, &arch, sizeof(arch)) != sizeof(arch))
                    break;
                if (lseek(fd, OSSwapBigToHostInt32(arch.offset), SEEK_SET) ==
                    (off_t)-1)
                    break;
                result = macho64_has_sea_blob(fd);
            }
        }
    } else if (magic == MH_MAGIC_64) {
        if (lseek(fd, 0, SEEK_SET) != (off_t)-1)
            result = macho64_has_sea_blob(fd);
    }

    close(fd);
    return result;
}
