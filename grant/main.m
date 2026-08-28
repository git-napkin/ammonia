#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_status.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <syslog.h>
#include <unistd.h>

#define SUPPORT_PATH "/opt/pluginplayground/"
#define HOOK_DYLIB SUPPORT_PATH "lib/libfangs_hook.dylib"

#define SHELLCODE_PCFMT_OFFSET 88
#define SHELLCODE_DLOPEN_OFFSET 164
#define SHELLCODE_PAYLOAD_PTR_OFFSET 172
#define SHELLCODE_SIZE 180

static kern_return_t (*_thread_convert_thread_state)(
    thread_act_t thread, int direction, thread_state_flavor_t flavor,
    thread_state_t in_state, mach_msg_type_number_t in_stateCnt,
    thread_state_t out_state, mach_msg_type_number_t *out_stateCnt);

static unsigned char shell_code[] = {
    0xFF, 0xC3, 0x00, 0xD1, 0xFD, 0x7B, 0x02, 0xA9, 0xFD, 0x83, 0x00, 0x91,
    0xA0, 0xC3, 0x1F, 0xB8, 0xE1, 0x0B, 0x00, 0xF9, 0xE0, 0x23, 0x00, 0x91,
    0x08, 0x00, 0x80, 0xD2, 0xE8, 0x07, 0x00, 0xF9, 0xE1, 0x03, 0x08, 0xAA,
    0xE2, 0x01, 0x00, 0x10, 0xE2, 0x23, 0xC1, 0xDA, 0xE3, 0x03, 0x08, 0xAA,
    0x49, 0x01, 0x00, 0x10, 0x29, 0x01, 0x40, 0xF9, 0x20, 0x01, 0x3F, 0xD6,
    0xA0, 0x4C, 0x8C, 0xD2, 0x20, 0x2C, 0xAF, 0xF2, 0x09, 0x00, 0x00, 0x10,
    0x20, 0x01, 0x1F, 0xD6, 0xFD, 0x7B, 0x42, 0xA9, 0xFF, 0xC3, 0x00, 0x91,
    0xC0, 0x03, 0x5F, 0xD6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x7F, 0x23, 0x03, 0xD5, 0xFF, 0xC3, 0x00, 0xD1, 0xFD, 0x7B, 0x02, 0xA9,
    0xFD, 0x83, 0x00, 0x91, 0xA0, 0xC3, 0x1F, 0xB8, 0xE1, 0x0B, 0x00, 0xF9,
    0x21, 0x00, 0x80, 0xD2, 0x89, 0x01, 0x00, 0x10, 0x20, 0x01, 0x40, 0xF9,
    0x09, 0x01, 0x00, 0x10, 0x29, 0x01, 0x40, 0xF9, 0x20, 0x01, 0x3F, 0xD6,
    0x09, 0x00, 0x80, 0x52, 0xE0, 0x03, 0x09, 0xAA, 0xFD, 0x7B, 0x42, 0xA9,
    0xFF, 0xC3, 0x00, 0x91, 0xFF, 0x0F, 0x5F, 0xD6, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static bool os_version_at_least(int major, int minor) {
    char str[32];
    size_t len = sizeof(str);
    if (sysctlbyname("kern.osproductversion", str, &len, NULL, 0) != 0)
        return false;
    int maj = 0, min = 0;
    sscanf(str, "%d.%d", &maj, &min);
    return (maj > major) || (maj == major && min >= minor);
}

int main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;

    openlog("grant", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    int result = 0;
    mach_port_t task = 0;
    thread_act_t thread = 0;
    mach_vm_address_t code = 0;
    mach_vm_address_t stack = 0;
    mach_vm_address_t payload_str = 0;
    vm_size_t stack_size = 16 * 1024;
    uint64_t stack_contents = 0x00000000CAFEBABE;
    pid_t pid = 1;
    kern_return_t kr;
    size_t payload_len = 0;
    arm_thread_state64_t thread_state = {0};
    arm_thread_state64_t machine_thread_state = {0};
    thread_state_flavor_t thread_flavor = ARM_THREAD_STATE64;
    mach_msg_type_number_t thread_flavor_count = ARM_THREAD_STATE64_COUNT;
    mach_msg_type_number_t machine_thread_flavor_count = ARM_THREAD_STATE64_COUNT;

    char payload_path[PATH_MAX];
    if (snprintf(payload_path, sizeof(payload_path), "%s", HOOK_DYLIB) >=
        (int)sizeof(payload_path)) {
        syslog(LOG_ERR, "grant: payload path too long");
        return 1;
    }

    if (sizeof(shell_code) != SHELLCODE_SIZE) {
        syslog(LOG_ERR, "grant: shellcode layout mismatch (got %zu, expected %d)",
               sizeof(shell_code), SHELLCODE_SIZE);
        return 1;
    }

    kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: task_for_pid(1): %s", mach_error_string(kr));
        return 1;
    }
    syslog(LOG_INFO, "grant: attached to launchd");

    kr = mach_vm_allocate(task, &stack, stack_size, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: stack alloc: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    kr = mach_vm_write(task, stack, (vm_address_t)&stack_contents,
                       sizeof(uint64_t));
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: stack write: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    kr = vm_protect(task, stack, stack_size, 1, VM_PROT_READ | VM_PROT_WRITE);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: stack protect: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    kr = mach_vm_allocate(task, &code, sizeof(shell_code), VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: code alloc: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    payload_len = strlen(payload_path) + 1;
    kr = mach_vm_allocate(task, &payload_str, payload_len, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: payload str alloc: %s", mach_error_string(kr));
        payload_str = 0;
        result = 1;
        goto terminate;
    }

    kr = mach_vm_write(task, payload_str, (vm_address_t)payload_path,
                       payload_len);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: payload str write: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    uint64_t pcfmt_address =
        (uint64_t)dlsym(RTLD_DEFAULT, "pthread_create_from_mach_thread");
    uint64_t dlopen_address =
        (uint64_t)dlsym(RTLD_DEFAULT, "dlopen");
    if (pcfmt_address == 0 || dlopen_address == 0) {
        syslog(LOG_ERR,
               "grant: could not resolve pthread_create_from_mach_thread "
               "or dlopen");
        result = 1;
        goto terminate;
    }

    uint64_t payload_address = (uint64_t)payload_str;

    memcpy(shell_code + SHELLCODE_PCFMT_OFFSET, &pcfmt_address,
           sizeof(uint64_t));
    memcpy(shell_code + SHELLCODE_DLOPEN_OFFSET, &dlopen_address,
           sizeof(uint64_t));
    memcpy(shell_code + SHELLCODE_PAYLOAD_PTR_OFFSET, &payload_address,
           sizeof(uint64_t));

    kr = mach_vm_write(task, code, (vm_address_t)shell_code,
                       sizeof(shell_code));
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: shellcode write: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    kr = vm_protect(task, code, sizeof(shell_code), 0,
                    VM_PROT_EXECUTE | VM_PROT_READ);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: code protect rx: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    void *handle =
        dlopen("/usr/lib/system/libsystem_kernel.dylib", RTLD_GLOBAL | RTLD_LAZY);
    if (handle) {
        *(void **)&_thread_convert_thread_state = dlsym(handle, "thread_convert_thread_state");
        dlclose(handle);
    }

    if (!_thread_convert_thread_state) {
        syslog(LOG_ERR, "grant: thread_convert_thread_state not found");
        result = 1;
        goto terminate;
    }

    __darwin_arm_thread_state64_set_pc_fptr(
        thread_state,
        ptrauth_sign_unauthenticated((void *)code, ptrauth_key_asia, 0));
    __darwin_arm_thread_state64_set_sp(thread_state,
                                       stack + (stack_size / 2));

    kr = thread_create(task, &thread);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: thread_create: %s", mach_error_string(kr));
        thread = 0;
        result = 1;
        goto terminate;
    }

    kr = _thread_convert_thread_state(
        thread, 2, thread_flavor, (thread_state_t)&thread_state,
        thread_flavor_count, (thread_state_t)&machine_thread_state,
        &machine_thread_flavor_count);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "grant: thread_convert: %s", mach_error_string(kr));
        result = 1;
        goto terminate;
    }

    if (os_version_at_least(14, 4)) {
        thread_terminate(thread);
        thread = 0;
        kr = thread_create_running(task, thread_flavor,
                                   (thread_state_t)&machine_thread_state,
                                   machine_thread_flavor_count, &thread);
        if (kr != KERN_SUCCESS) {
            syslog(LOG_ERR, "grant: thread_create_running: %s",
                   mach_error_string(kr));
            thread = 0;
            result = 1;
            goto terminate;
        }
    } else {
        kr = thread_set_state(thread, thread_flavor,
                              (thread_state_t)&machine_thread_state,
                              machine_thread_flavor_count);
        if (kr != KERN_SUCCESS) {
            syslog(LOG_ERR, "grant: thread_set_state: %s",
                   mach_error_string(kr));
            result = 1;
            goto terminate;
        }
        kr = thread_resume(thread);
        if (kr != KERN_SUCCESS) {
            syslog(LOG_ERR, "grant: thread_resume: %s",
                   mach_error_string(kr));
            result = 1;
            goto terminate;
        }
    }

    usleep(50000);

    for (int i = 0; i < 100; ++i) {
        kr = thread_get_state(thread, thread_flavor,
                              (thread_state_t)&thread_state,
                              &thread_flavor_count);
        if (kr != KERN_SUCCESS) {
            result = 1;
            goto terminate;
        }
        if (thread_state.__x[0] == 0x79616265) {
            result = 0;
            syslog(LOG_INFO, "grant: injection successful");
            goto terminate;
        }
        usleep(20000);
    }

    syslog(LOG_ERR, "grant: injection timed out");
    result = 1;

terminate:
    if (thread) {
        thread_terminate(thread);
        usleep(10000);
    }
    if (task) {
        if (stack)
            mach_vm_deallocate(task, stack, stack_size);
        if (code)
            mach_vm_deallocate(task, code, sizeof(shell_code));
        if (payload_str)
            mach_vm_deallocate(task, payload_str, payload_len);
    }
    return result;
}
