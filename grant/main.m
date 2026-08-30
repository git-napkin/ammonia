#include "options_loader.h"
#include "tweak_utils.h"

#include <dlfcn.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_status.h>
#include <mach-o/dyld_images.h>
#include <os/lock.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <syslog.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

#include "ammonia.h"

#define OPENER_DYLIB SUPPORT_PATH "libopener.dylib"
#define HOOK_DYLIB SUPPORT_PATH "libinject.dylib"
#define SENTINEL 0x79616265ull

extern uint8_t grant_shellcode[];
extern uint8_t grant_shellcode_end[];
extern uint64_t grant_shellcode_pcfmt;
extern uint64_t grant_shellcode_dlopen;
extern uint64_t grant_shellcode_payload;

static kern_return_t (*_thread_convert_thread_state)(
    thread_act_t thread, int direction, thread_state_flavor_t flavor,
    thread_state_t in_state, mach_msg_type_number_t in_stateCnt,
    thread_state_t out_state, mach_msg_type_number_t *out_stateCnt);

static FangsOptions g_opts;
static os_unfair_lock g_opts_lock = OS_UNFAIR_LOCK_INIT;

static bool os_version_at_least(int major, int minor) {
    char str[32];
    size_t len = sizeof(str);
    if (sysctlbyname("kern.osproductversion", str, &len, NULL, 0) != 0)
        return false;
    int maj = 0, min = 0;
    sscanf(str, "%d.%d", &maj, &min);
    return (maj > major) || (maj == major && min >= minor);
}

static uint64_t stripped_dlsym(const char *name) {
    void *p = dlsym(RTLD_DEFAULT, name);
    if (!p)
        return 0;
    p = ptrauth_strip(p, ptrauth_key_function_pointer);
    return (uint64_t)(uintptr_t)p;
}

static bool remote_read(task_t task, mach_vm_address_t addr, void *buf,
                        size_t n) {
    mach_vm_size_t got = n;
    kern_return_t kr = mach_vm_read_overwrite(
        task, addr, n, (mach_vm_address_t)(uintptr_t)buf, &got);
    return kr == KERN_SUCCESS && got == n;
}

static bool task_has_mapped(task_t task, const char *needle) {
    struct task_dyld_info info;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    if (!needle || task_info(task, TASK_DYLD_INFO, (task_info_t)&info, &count) !=
                       KERN_SUCCESS)
        return false;
    if (info.all_image_info_addr == 0 || info.all_image_info_size < 32)
        return false;

    struct dyld_all_image_infos infos;
    memset(&infos, 0, sizeof(infos));
    size_t want = sizeof(infos);
    if (info.all_image_info_size < want)
        want = (size_t)info.all_image_info_size;
    if (!remote_read(task, info.all_image_info_addr, &infos, want))
        return false;
    if (infos.infoArray == NULL || infos.infoArrayCount == 0)
        return false;

    uint32_t n = infos.infoArrayCount;
    if (n > 512)
        n = 512;
    for (uint32_t i = 0; i < n; i++) {
        struct dyld_image_info img;
        mach_vm_address_t slot =
            (mach_vm_address_t)(uintptr_t)(infos.infoArray + i);
        if (!remote_read(task, slot, &img, sizeof(img)))
            continue;
        if (img.imageFilePath == NULL)
            continue;
        char path[PATH_MAX];
        memset(path, 0, sizeof(path));
        if (!remote_read(task, (mach_vm_address_t)(uintptr_t)img.imageFilePath,
                         path, sizeof(path) - 1))
            continue;
        path[sizeof(path) - 1] = '\0';
        if (strstr(path, needle) != NULL)
            return true;
    }
    return false;
}

static bool should_target_path(const char *path) {
    if (!path || path[0] == '\0')
        return false;
    if (path_ends_with(path, "launchd") || path_ends_with(path, "xpcproxy") ||
        path_ends_with(path, "loginwindow") ||
        path_ends_with(path, "WindowServer") || path_ends_with(path, "amfid") ||
        path_ends_with(path, "syspolicyd") || path_ends_with(path, "grant") ||
        path_ends_with(path, "ammonia") ||
        path_ends_with(path, "Driver") || path_ends_with(path, "Dock") ||
        path_ends_with(path, "WallpaperAgent") ||
        path_ends_with(path, "WallpaperAerialsExtension"))
        return false;
    if (strncmp(path, "/usr/libexec/", 13) == 0 ||
        strncmp(path, "/usr/sbin/", 10) == 0 ||
        strncmp(path, "/sbin/", 6) == 0)
        return false;
    return exe_links_to_framework(path, "AppKit");
}

static int inject_dylib(pid_t pid, const char *dylib_path) {
    if (ammonia_in_safe_boot()) {
        syslog(LOG_NOTICE, "ammonia: safe boot, skip inject pid %d", (int)pid);
        return 0;
    }
    if (pid == getpid() || !dylib_path)
        return 1;

    int result = 1;
    mach_port_t task = 0;
    thread_act_t thread = 0;
    mach_vm_address_t code = 0;
    mach_vm_address_t stack = 0;
    mach_vm_address_t payload_str = 0;
    vm_size_t stack_size = 16 * 1024;
    uint64_t stack_contents = 0x00000000CAFEBABE;
    kern_return_t kr;
    size_t payload_len = strlen(dylib_path) + 1;
    size_t sc_len = (size_t)(grant_shellcode_end - grant_shellcode);
    uint8_t *sc = NULL;
    arm_thread_state64_t thread_state = {0};
    arm_thread_state64_t machine_thread_state = {0};
    thread_state_flavor_t thread_flavor = ARM_THREAD_STATE64;
    mach_msg_type_number_t thread_flavor_count = ARM_THREAD_STATE64_COUNT;
    mach_msg_type_number_t machine_thread_flavor_count =
        ARM_THREAD_STATE64_COUNT;

    if (sc_len < 8) {
        syslog(LOG_ERR, "ammonia: shellcode too short (%zu)", sc_len);
        return 1;
    }

    kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: task_for_pid(%d): %s", (int)pid,
               mach_error_string(kr));
        return 1;
    }

    const char *already = strrchr(dylib_path, '/');
    already = already ? already + 1 : dylib_path;
    if (task_has_mapped(task, already)) {
        syslog(LOG_INFO, "grant: pid %d already has %s", (int)pid, already);
        result = 0;
        goto terminate;
    }

    sc = malloc(sc_len);
    if (!sc)
        goto terminate;
    memcpy(sc, grant_shellcode, sc_len);

    kr = mach_vm_allocate(task, &stack, stack_size, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: pid %d stack alloc: %s", (int)pid,
               mach_error_string(kr));
        goto terminate;
    }

    kr = mach_vm_write(task, stack, (vm_address_t)&stack_contents,
                       sizeof(uint64_t));
    if (kr != KERN_SUCCESS)
        goto terminate;

    kr = vm_protect(task, stack, stack_size, 1, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS)
        goto terminate;

    kr = mach_vm_allocate(task, &code, sc_len, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS)
        goto terminate;

    kr = mach_vm_allocate(task, &payload_str, payload_len, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        payload_str = 0;
        goto terminate;
    }

    kr = mach_vm_write(task, payload_str, (vm_address_t)dylib_path,
                       payload_len);
    if (kr != KERN_SUCCESS)
        goto terminate;

    uint64_t pcfmt_address = stripped_dlsym("pthread_create_from_mach_thread");
    uint64_t dlopen_address = stripped_dlsym("dlopen");
    if (pcfmt_address == 0 || dlopen_address == 0) {
        syslog(LOG_ERR, "grant: missing dlsym(pthread_create_from_mach_thread) "
                        "or dlopen");
        goto terminate;
    }

    size_t off_pcfmt =
        (size_t)((uint8_t *)&grant_shellcode_pcfmt - grant_shellcode);
    size_t off_dlopen =
        (size_t)((uint8_t *)&grant_shellcode_dlopen - grant_shellcode);
    size_t off_payload =
        (size_t)((uint8_t *)&grant_shellcode_payload - grant_shellcode);
    uint64_t payload_address = (uint64_t)payload_str;
    memcpy(sc + off_pcfmt, &pcfmt_address, sizeof(uint64_t));
    memcpy(sc + off_dlopen, &dlopen_address, sizeof(uint64_t));
    memcpy(sc + off_payload, &payload_address, sizeof(uint64_t));

    kr = mach_vm_write(task, code, (vm_address_t)sc, sc_len);
    if (kr != KERN_SUCCESS)
        goto terminate;

    kr = vm_protect(task, code, sc_len, 0, VM_PROT_EXECUTE | VM_PROT_READ);
    if (kr != KERN_SUCCESS)
        goto terminate;

    if (!_thread_convert_thread_state) {
        void *handle = dlopen("/usr/lib/system/libsystem_kernel.dylib",
                              RTLD_GLOBAL | RTLD_LAZY);
        if (handle) {
            *(void **)&_thread_convert_thread_state =
                dlsym(handle, "thread_convert_thread_state");
            dlclose(handle);
        }
    }
    if (!_thread_convert_thread_state) {
        syslog(LOG_ERR, "grant: thread_convert_thread_state not found");
        goto terminate;
    }

    __darwin_arm_thread_state64_set_pc_fptr(
        thread_state,
        ptrauth_sign_unauthenticated((void *)(uintptr_t)code,
                                     ptrauth_key_asia, 0));
    __darwin_arm_thread_state64_set_sp(thread_state, stack + (stack_size / 2));

    kr = thread_create(task, &thread);
    if (kr != KERN_SUCCESS) {
        thread = 0;
        goto terminate;
    }

    kr = _thread_convert_thread_state(
        thread, 2, thread_flavor, (thread_state_t)&thread_state,
        thread_flavor_count, (thread_state_t)&machine_thread_state,
        &machine_thread_flavor_count);
    if (kr != KERN_SUCCESS)
        goto terminate;

    if (os_version_at_least(14, 4)) {
        thread_terminate(thread);
        thread = 0;
        kr = thread_create_running(task, thread_flavor,
                                   (thread_state_t)&machine_thread_state,
                                   machine_thread_flavor_count, &thread);
        if (kr != KERN_SUCCESS) {
            syslog(LOG_ERR, "grant: pid %d thread_create_running: %s",
                   (int)pid, mach_error_string(kr));
            thread = 0;
            goto terminate;
        }
    } else {
        kr = thread_set_state(thread, thread_flavor,
                              (thread_state_t)&machine_thread_state,
                              machine_thread_flavor_count);
        if (kr != KERN_SUCCESS)
            goto terminate;
        kr = thread_resume(thread);
        if (kr != KERN_SUCCESS)
            goto terminate;
    }

    usleep(50000);
    for (int i = 0; i < 100; ++i) {
        kr = thread_get_state(thread, thread_flavor,
                              (thread_state_t)&thread_state,
                              &thread_flavor_count);
        if (kr != KERN_SUCCESS)
            goto terminate;
        if (thread_state.__x[0] == SENTINEL) {
            syslog(LOG_INFO, "grant: injected %s into pid %d", dylib_path,
                   (int)pid);
            result = 0;
            goto terminate;
        }
        usleep(20000);
    }
    syslog(LOG_ERR, "grant: pid %d injection timed out", (int)pid);

terminate:
    free(sc);
    if (thread) {
        thread_terminate(thread);
        usleep(10000);
    }
    if (task) {
        if (stack)
            mach_vm_deallocate(task, stack, stack_size);
        if (code)
            mach_vm_deallocate(task, code, sc_len);
        if (payload_str)
            mach_vm_deallocate(task, payload_str, payload_len);
        mach_port_deallocate(mach_task_self(), task);
    }
    return result;
}

static void reload_opts(void) {
    FangsOptions opts = fangs_load_options();
    os_unfair_lock_lock(&g_opts_lock);
    g_opts = opts;
    os_unfair_lock_unlock(&g_opts_lock);
}

#define TRIED_CAP 4096
static pid_t g_tried_pid[TRIED_CAP];
static uint32_t g_tried_start[TRIED_CAP];
static size_t g_tried_n;

static uint32_t pid_start_sec(pid_t pid) {
    struct proc_bsdinfo info;
    int sz = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    if (sz != (int)sizeof(info))
        return 0;
    return info.pbi_start_tvsec;
}

static void mark_tried(pid_t pid, uint32_t start) {
    size_t i = g_tried_n % TRIED_CAP;
    g_tried_pid[i] = pid;
    g_tried_start[i] = start;
    g_tried_n++;
}

static bool already_tried(pid_t pid, uint32_t start) {
    size_t n = g_tried_n < TRIED_CAP ? g_tried_n : TRIED_CAP;
    for (size_t i = 0; i < n; i++) {
        if (g_tried_pid[i] == pid && g_tried_start[i] == start)
            return true;
    }
    return false;
}

static void scan_and_inject(void) {
    if (ammonia_in_safe_boot())
        return;
    os_unfair_lock_lock(&g_opts_lock);
    bool pause = g_opts.pauseInjection;
    os_unfair_lock_unlock(&g_opts_lock);
    if (pause)
        return;

    int buf_bytes = proc_listallpids(NULL, 0);
    if (buf_bytes <= 0)
        return;
    int cap = buf_bytes + 64;
    pid_t *pids = malloc((size_t)cap * sizeof(pid_t));
    if (!pids)
        return;
    int n = proc_listallpids(pids, cap * (int)sizeof(pid_t));
    if (n <= 0) {
        free(pids);
        return;
    }
    for (int i = 0; i < n; i++) {
        pid_t pid = pids[i];
        if (pid <= 1 || pid == getpid())
            continue;
        uint32_t start = pid_start_sec(pid);
        if (already_tried(pid, start))
            continue;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pid, path, sizeof(path)) <= 0)
            continue;
        if (!should_target_path(path))
            continue;
        mark_tried(pid, start);
        inject_dylib(pid, OPENER_DYLIB);
    }
    free(pids);
}

static void inject_running_loginwindow(void) {
    int cap = 4096;
    pid_t *pids = malloc((size_t)cap * sizeof(pid_t));
    if (!pids)
        return;
    int n = proc_listallpids(pids, cap * (int)sizeof(pid_t));
    if (n <= 0) {
        free(pids);
        return;
    }
    for (int i = 0; i < n; i++) {
        pid_t pid = pids[i];
        if (pid <= 1)
            continue;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pid, path, sizeof(path)) <= 0)
            continue;
        if (!path_ends_with(path, "loginwindow"))
            continue;
        syslog(LOG_NOTICE,
               "ammonia: loginwindow pid %d often misses spawn insert; "
               "loading opener",
               (int)pid);
        inject_dylib(pid, OPENER_DYLIB);
    }
    free(pids);
}

static int run_daemon(void) {
    reload_opts();
    fangs_watch_options(reload_opts);

    dispatch_queue_t q = dispatch_get_global_queue(
        DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_source_t timer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0),
                              250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{ scan_and_inject(); });
    dispatch_resume(timer);
    syslog(LOG_INFO,
           "grant: scanning AppKit processes for opener injection (never pid "
           "1)");
    dispatch_main();
    return 0;
}

int main(int argc, const char *argv[]) {
    openlog("ammonia", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    if (argc >= 2 && strcmp(argv[1], "--print-syms") == 0) {
        printf("pthread_create_from_mach_thread %p stripped 0x%llx\n",
               dlsym(RTLD_DEFAULT, "pthread_create_from_mach_thread"),
               stripped_dlsym("pthread_create_from_mach_thread"));
        printf("dlopen %p stripped 0x%llx\n", dlsym(RTLD_DEFAULT, "dlopen"),
               stripped_dlsym("dlopen"));
        printf("shellcode size %zu (slots pcfmt=%zu dlopen=%zu payload=%zu)\n",
               (size_t)(grant_shellcode_end - grant_shellcode),
               (size_t)((uint8_t *)&grant_shellcode_pcfmt - grant_shellcode),
               (size_t)((uint8_t *)&grant_shellcode_dlopen - grant_shellcode),
               (size_t)((uint8_t *)&grant_shellcode_payload - grant_shellcode));
        return 0;
    }

    if (ammonia_in_safe_boot()) {
        syslog(LOG_NOTICE, "ammonia: safe boot, not injecting");
        closelog();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--pid") == 0) {
        pid_t pid = (pid_t)atoi(argv[2]);
        const char *dylib = OPENER_DYLIB;
        if (argc >= 5 && strcmp(argv[3], "--dylib") == 0)
            dylib = argv[4];
        int rc = inject_dylib(pid, dylib);
        closelog();
        return rc;
    }

    if (argc >= 2 && strcmp(argv[1], "--scan") == 0)
        return run_daemon();

    syslog(LOG_NOTICE, "ammonia: injecting libinject into launchd");
    int rc = inject_dylib(1, HOOK_DYLIB);
    inject_running_loginwindow();
    closelog();
    return rc;
}
