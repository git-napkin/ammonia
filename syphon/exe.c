#include "exe.h"
#include "bundle_copy.h"
#include "pac_utils.h"
#include "log.h"
#include <errno.h>
#include <removefile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *find_bundle_ext(char *path) {
    char *last = NULL;
    char *dot = strstr(path, ".app");
    while (dot) {
        char c = dot[4];
        if (c == '/' || c == '\0')
            last = dot;
        dot = strstr(dot + 4, ".app");
    }
    return last;
}

static bool timespec_newer(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec > b->tv_sec ||
           (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

char *getready_process(const char *path) {
    if (!path_is_bundle(path))
        return NULL;

    char bundle_root[PATH_MAX];
    snprintf(bundle_root, sizeof(bundle_root), "%s", path);
    char *app_ext = find_bundle_ext(bundle_root);
    if (app_ext)
        app_ext[4] = '\0';

    const char *bundle_name = strrchr(bundle_root, '/');
    bundle_name = bundle_name ? bundle_name + 1 : bundle_root;

    char runtime_apps_dir[PATH_MAX];
    char dst_bundle_path[PATH_MAX];
    snprintf(runtime_apps_dir, sizeof(runtime_apps_dir), "/tmp/RuntimeApplications");
    snprintf(dst_bundle_path, sizeof(dst_bundle_path), "%s/%s", runtime_apps_dir,
             bundle_name);

    log_info("[bootstrap] processing bundle: %s", bundle_root);

    struct stat src_st, dst_st;
    bool have_dst = stat(dst_bundle_path, &dst_st) == 0 && S_ISDIR(dst_st.st_mode);
    bool src_newer = have_dst && stat(bundle_root, &src_st) == 0 &&
                     timespec_newer(&src_st.st_mtimespec, &dst_st.st_mtimespec);

    if (have_dst && !src_newer) {
        char bundle_exec_tmp[PATH_MAX];
        log_info("[bootstrap] bundle already exists at: %s", dst_bundle_path);
        if (get_bundle_executable_path(dst_bundle_path, bundle_exec_tmp,
                                       sizeof(bundle_exec_tmp)))
            return strdup(bundle_exec_tmp);
    }

    if (have_dst) {
        log_info("[bootstrap] replacing cached bundle: %s", dst_bundle_path);
        removefile(dst_bundle_path, NULL, REMOVEFILE_RECURSIVE);
    }

    if (mkdir(runtime_apps_dir, 0755) != 0 && errno != EEXIST) {
        log_error("[bootstrap] failed to create RuntimeApplications dir: %s",
                  strerror(errno));
        return NULL;
    }

    log_info("[bootstrap] copying bundle to: %s", dst_bundle_path);
    if (!copy_dir_recursive(bundle_root, dst_bundle_path)) {
        log_error("[bootstrap] failed to copy bundle to: %s", dst_bundle_path);
        return NULL;
    }

    char bundle_exec_tmp[PATH_MAX];
    if (!get_bundle_executable_path(dst_bundle_path, bundle_exec_tmp,
                                    sizeof(bundle_exec_tmp))) {
        log_error("[bootstrap] failed to get executable path for: %s",
                  dst_bundle_path);
        return NULL;
    }

    log_info("[bootstrap] depacifying executable: %s", bundle_exec_tmp);
    if (!depacify_file_in_place(bundle_exec_tmp)) {
        syslog(LOG_ERR, "[bootstrap] failed to depacify bundle executable");
        return strdup(bundle_exec_tmp);
    }

    log_info("[bootstrap] resigning bundle: %s", dst_bundle_path);
    if (!resign_bundle(dst_bundle_path))
        syslog(LOG_ERR, "[bootstrap] failed to resign bundle, continuing anyway");

    syslog(LOG_INFO, "[bootstrap] using depacified bundle");
    return strdup(bundle_exec_tmp);
}
