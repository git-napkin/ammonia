# Grant

A tool that injects `libfangs_hook.dylib` into `launchd` (PID 1). Gets the Frida-Gum `posix_spawn` interceptor running inside launchd before any user processes start.

1. Uses `task_for_pid(mach_task_self(), 1, &task)` to attach to launchd.
2. Allocates stack + shellcode memory in launchd's address space via `mach_vm_allocate`.
3. Patches ARM64 shellcode with runtime-resolved addresses for `pthread_create_from_mach_thread` and `dlopen`.
4. Creates a new thread inside launchd that calls `dlopen("libfangs_hook.dylib")`.
5. Waits for a sentinel return value (`0x79616265`) confirming the dylib loaded.
6. Handles macOS 14.4+ `thread_create_running` vs older `thread_set_state` + `thread_resume` differences.
7. Deallocates the remote stack, code, and path string before exit.

Runs as a LaunchDaemon (`com.pluginplayground.grant`) at boot. `RunAtLoad` is true. `KeepAlive` is only when grant exits nonzero, with a 10 second throttle, so a successful inject does not loop. Requires SIP disabled and the `com.apple.system-task-ports` entitlement. Required, no opt-out. The pkg postinstall bootstraps the job. The Configurator Install button copies the same plist from `/opt/pluginplayground/share/`.
