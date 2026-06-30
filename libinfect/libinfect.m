//
//  libinfect.c
//  libinfect
//
//  Created by bedtime on 11/19/23.
//

#include "ammonia.h"
#include "envbuf.h"
#include "frida-gum.h"

#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include <libkern/OSByteOrder.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

void LogToFile(const char *format, ...) {
  // Open the file in append mode
  FILE *file = fopen(SupportFolderP "infect.log", "a");

  if (file == NULL) {
    // Failed to open the file
    perror("Error opening file");
    return;
  }

  // Initialize variable arguments
  va_list args;
  va_start(args, format);

  // Use vfprintf to write to the file
  vfprintf(file, format, args);

  // Clean up variable arguments
  va_end(args);

  // Close the file
  fclose(file);
}

int (*SpawnOld)(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *ac,
                const posix_spawnattr_t *ab, char *const __argv[],
                char *const __envp[]);

int (*SpawnPOld)(pid_t *restrict pid, const char *restrict file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *restrict attrp,
                 char *const argv[restrict], char *const envp[restrict]);

static int (*GetDarwinRoleNp)(const posix_spawnattr_t *__restrict attr,
                              uint64_t *__restrict darwin_rolep);

static bool path_ends_with(const char *path, const char *suffix) {
    if (!path || !suffix) return false;
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (slen == 0 || slen > plen) return false;
    return strncmp(path + plen - slen, suffix, slen) == 0 &&
           (plen == slen || path[plen - slen - 1] == '/');
}

static bool path_matches_entry(const char *path, const char *entry) {
    if (!path || !entry || entry[0] == '\0') return false;
    if (strchr(entry, '/') != NULL) {
        return (strcmp(path, entry) == 0);
    }
    return path_ends_with(path, entry);
}

static bool path_matches_driver(const char *path) {
    return path_ends_with(path, "Driver");
}

#define PathDriver(path) path_matches_driver(path)

#define PRIO_DARWIN_ROLE_UI_FOCAL 0x1     /* On  screen,     focal UI */
#define PRIO_DARWIN_ROLE_UI 0x2           /* On  screen UI,  focal unknown */
#define PRIO_DARWIN_ROLE_UI_NON_FOCAL 0x4 /* On  screen, non-focal UI */

static const char *gum_replace_strerror(GumReplaceReturn ret) {
  switch (ret) {
  case GUM_REPLACE_OK:
    return "ok";
  case GUM_REPLACE_WRONG_SIGNATURE:
    return "wrong signature";
  case GUM_REPLACE_ALREADY_REPLACED:
    return "already replaced";
  case GUM_REPLACE_POLICY_VIOLATION:
    return "policy violation";
  case GUM_REPLACE_WRONG_TYPE:
    return "wrong type";
  default:
    return "unknown error";
  }
}

/* --- ammonia.blacklist storage --- */
static char **ammonia_blacklist = NULL;
static size_t ammonia_blacklist_count = 0;
static bool disable_xpcproxy_injection = false;

static bool flag_file_exists(const char *filename) {
  char pathbuf[PATH_MAX];
  if (snprintf(pathbuf, sizeof(pathbuf), "%s%s", SupportFolderP, filename) >=
      (int)sizeof(pathbuf)) {
    return false;
  }
  return access(pathbuf, F_OK) == 0;
}

static void load_ammonia_blacklist(void) {
  char pathbuf[PATH_MAX];
  if (snprintf(pathbuf, sizeof(pathbuf), "%s%s", SupportFolderP,
               "ammonia.blacklist") >= (int)sizeof(pathbuf)) {
    LogToFile("ammonia: blacklist path overflow\n");
    return;
  }

  FILE *f = fopen(pathbuf, "r");
  if (!f) {
    if (errno != ENOENT) {
      LogToFile("ammonia: failed to open blacklist '%s': %s\n", pathbuf,
                strerror(errno));
    }
    return;
  }

  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  while ((read = getline(&line, &len, f)) != -1) {
    // strip newline(s)
    while (read > 0 && (line[read - 1] == '\n' || line[read - 1] == '\r')) {
      line[--read] = '\0';
    }

    // skip leading whitespace
    char *start = line;
    while (*start && isspace((unsigned char)*start))
      start++;

    // skip comments and empty lines
    if (*start == '#' || *start == '\0')
      continue;

    // trim trailing whitespace
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end))
      *end-- = '\0';

    // duplicate and store
    char *entry = strdup(start);
    if (!entry) {
      LogToFile("ammonia: failed to allocate blacklist entry\n");
      continue;
    }

    char **tmp = realloc(ammonia_blacklist,
                         (ammonia_blacklist_count + 1) * sizeof(char *));
    if (!tmp) {
      LogToFile("ammonia: failed to grow blacklist array\n");
      free(entry);
      continue;
    }
    ammonia_blacklist = tmp;
    ammonia_blacklist[ammonia_blacklist_count++] = entry;
  }

  if (ferror(f)) {
    LogToFile("ammonia: error reading blacklist '%s': %s\n", pathbuf,
              strerror(errno));
  }

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
    if (path_matches_entry(path, entry)) {
      return true;
    }
  }
  return false;
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

        if (lc.cmd == LC_SEGMENT_64) {
            lseek(fd, cmd_start, SEEK_SET);
            struct segment_command_64 seg;
            if (read(fd, &seg, sizeof(seg)) != sizeof(seg)) return false;

            if (strncmp(seg.segname, "__TEXT", sizeof(seg.segname)) == 0) {
                for (uint32_t j = 0; j < seg.nsects; j++) {
                    struct section_64 sect;
                    if (read(fd, &sect, sizeof(sect)) != sizeof(sect)) return false;
                    if (strncmp(sect.sectname, "__NODE_SEA_BLOB", sizeof(sect.sectname)) == 0) {
                        return true;
                    }
                }
            }
        }

        lseek(fd, cmd_start + lc.cmdsize, SEEK_SET);
    }

    return false;
}

static bool is_node_sea_binary(const char *path) {
    if (!path) return false;

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


static int spawn_with_env(int (*spawn_fn)(pid_t *, const char *,
                                          const posix_spawn_file_actions_t *,
                                          const posix_spawnattr_t *,
                                          char *const[], char *const[]),
                          pid_t *pid, const char *path,
                          const posix_spawn_file_actions_t *ac,
                          const posix_spawnattr_t *ab, char *const __argv[],
                          char *const __envp[]) {
  if (spawn_fn == NULL) {
    LogToFile("ammonia: original spawn function is NULL for '%s'\n",
              path ? path : "(null)");
    return EINVAL;
  }

  char **playground = envbuf_mutcopy((const char **)__envp);
  if (__envp != NULL && playground == NULL) {
    LogToFile("ammonia: failed to copy environment for '%s'\n",
              path ? path : "(null)");
    return ENOMEM;
  }
  int k;

  uint64_t darwin_rolep = 0;
  if (ab != NULL && GetDarwinRoleNp != NULL) {
    GetDarwinRoleNp(ab, &darwin_rolep);
  }

  if (strcmp(path, "/System/Library/CoreServices/loginwindow.app/Contents/"
                   "MacOS/loginwindow") == 0) {
    goto InjectOpener;
  }
  if (strcmp(path, "/usr/libexec/xpcproxy") == 0) {
    if (!disable_xpcproxy_injection) {
      playground = envbuf_setenv(playground, "DYLD_INSERT_LIBRARIES",
                                 SupportFolderP "liblibinfect.dylib");
    }
  } else if (!PathDriver(path)) {
    if (darwin_rolep == PRIO_DARWIN_ROLE_UI_FOCAL ||
        darwin_rolep == PRIO_DARWIN_ROLE_UI ||
        darwin_rolep == PRIO_DARWIN_ROLE_UI_NON_FOCAL) {

      if (is_path_blacklisted(path)) {
        LogToFile("ammonia: skipping opener for blacklisted path '%s'\n", path);
        goto Spawn;
      }

      if (is_node_sea_binary(path)) {
        LogToFile("ammonia: skipping opener for Node.js SEA binary '%s'\n",
                  path);
        goto Spawn;
      }

    InjectOpener:
      LogToFile("ammonia: injecting opener into '%s'\n", path);

      char *newlib = SupportFolderP "libopener.dylib";

      int idx =
          envbuf_find((const char **)playground, "DYLD_INSERT_LIBRARIES");
      if (idx >= 0) {
        const char *old = playground[idx] + strlen("DYLD_INSERT_LIBRARIES=");
        char *combined = NULL;
        if (asprintf(&combined, "%s:%s", old, newlib) != -1) {
          playground =
              envbuf_setenv(playground, "DYLD_INSERT_LIBRARIES", combined);
          free(combined);
        } else {
          LogToFile("ammonia: failed to append opener to DYLD_INSERT_LIBRARIES "
                    "for '%s'\n",
                    path);
        }
      } else {
        playground = envbuf_setenv(playground, "DYLD_INSERT_LIBRARIES", newlib);
      }
    }
  }

Spawn:
  if (is_node_sea_binary(path)) {
    LogToFile("ammonia: stripping DYLD_INSERT_LIBRARIES for Node.js SEA "
              "binary '%s'\n",
              path);
    playground = envbuf_unsetenv(playground, "DYLD_INSERT_LIBRARIES");
  }
  k = spawn_fn(pid, path, ac, ab, __argv, (char *const *)playground);
  envbuf_free(playground);
  return k;
}

int SpawnNew(pid_t *pid, const char *path, const posix_spawn_file_actions_t *ac,
             const posix_spawnattr_t *ab, char *const __argv[],
             char *const __envp[]) {
  return spawn_with_env(SpawnOld, pid, path, ac, ab, __argv, __envp);
}

int SpawnPNew(pid_t *restrict pid, const char *restrict path,
              const posix_spawn_file_actions_t *ac,
              const posix_spawnattr_t *restrict ab, char *const *restrict argv,
              char *const *restrict envp) {
  return spawn_with_env(SpawnPOld, pid, path, ac, ab, argv, envp);
}

void __attribute__((constructor)) Infect(void) {
  load_ammonia_blacklist();
  disable_xpcproxy_injection = flag_file_exists("ammonia.disable-xpcproxy");
  if (disable_xpcproxy_injection) {
    LogToFile("ammonia: xpcproxy libinfect propagation disabled\n");
  }

  GetDarwinRoleNp =
      dlsym(RTLD_DEFAULT, "posix_spawnattr_get_darwin_role_np");
  if (!GetDarwinRoleNp) {
    LogToFile("ammonia: failed to resolve posix_spawnattr_get_darwin_role_np: %s\n",
              dlerror());
  }

  gum_init_embedded();
  GumInterceptor *interceptor = gum_interceptor_obtain();
  gum_interceptor_begin_transaction(interceptor);

  gpointer posix_spawn_addr =
      (gpointer)gum_module_find_global_export_by_name("posix_spawn");
  if (posix_spawn_addr == NULL) {
    LogToFile("ammonia: failed to find export 'posix_spawn'\n");
  } else {
    GumReplaceReturn spawn_ret = gum_interceptor_replace(
        interceptor, posix_spawn_addr, (gpointer)SpawnNew, NULL,
        (gpointer *)&SpawnOld);
    if (spawn_ret != GUM_REPLACE_OK) {
      LogToFile("ammonia: failed to replace posix_spawn: %s\n",
                gum_replace_strerror(spawn_ret));
    } else if (SpawnOld == NULL) {
      LogToFile("ammonia: posix_spawn replacement succeeded but SpawnOld is NULL\n");
    }
  }

  gpointer posix_spawnp_addr =
      (gpointer)gum_module_find_global_export_by_name("posix_spawnp");
  if (posix_spawnp_addr == NULL) {
    LogToFile("ammonia: failed to find export 'posix_spawnp'\n");
  } else {
    GumReplaceReturn spawnp_ret = gum_interceptor_replace(
        interceptor, posix_spawnp_addr, (gpointer)SpawnPNew, NULL,
        (gpointer *)&SpawnPOld);
    if (spawnp_ret != GUM_REPLACE_OK) {
      LogToFile("ammonia: failed to replace posix_spawnp: %s\n",
                gum_replace_strerror(spawnp_ret));
    } else if (SpawnPOld == NULL) {
      LogToFile("ammonia: posix_spawnp replacement succeeded but SpawnPOld is NULL\n");
    }
  }

  gum_interceptor_end_transaction(interceptor);
}
