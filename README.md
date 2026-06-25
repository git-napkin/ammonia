# Ammonia

**Ammonia** is a modern macOS extension/tweak loader that injects dynamic libraries into running processes on jailbroken Macs (requires SIP disabled). It uses a three-stage architecture: a LaunchDaemon injects shellcode into `launchd` (pid 1), which loads a Frida-Gum-based hook library that intercepts `posix_spawn`/`posix_spawnp`, and then injects an "opener" library into UI processes to load tweaks from a filesystem directory.

---

## Table of Contents

- [Requirements](#requirements)
- [Quick Install](#quick-install)
- [Building from Source](#building-from-source)
- [Architecture](#architecture)
- [Developing Tweaks](#developing-tweaks-for-ammonia)
- [Tweak Filtering](#tweak-filtering)
- [Global Process Blacklist](#global-process-blacklist)
- [Security Model](#security-model)
- [File Layout](#file-layout)
- [Uninstallation](#uninstallation)
- [Debug Logging](#debug-logging)
- [Support](#support-ammonia)

---



## Requirements

- macOS with **SIP disabled** (required for `task_for_pid`)
- **Library validation disabled** (done automatically by the installer)
- **arm64e ABI enabled** (automatically configured by the installer)

---



## Quick Install

1. **Turn off SIP** (recovery mode: `csrutil disable`)
2. **Download and install the package**:
  ```sh
   curl -LO https://github.com/git-napkin/ammonia/releases/download/1.0/ammonia.pkg
   sudo installer -pkg ammonia.pkg -target /
  ```
3. **Reboot** your Mac.

The installer handles `arm64e_preview_abi`, library validation, and LaunchDaemon registration automatically.

---



## Building from Source

```sh
sh setup_frida.sh    # Downloads and builds Frida-Gum dependencies
sh compile.sh        # CMake configure + make
sh package.sh        # Creates a .pkg installer
```

`compile.sh` runs:

```sh
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=1
cd Build && make -j8
```



### Build Outputs


| Artifact                   | Description                        |
| -------------------------- | ---------------------------------- |
| `Build/ammonia`            | LaunchDaemon executable (Stage 1)  |
| `Build/liblibinfect.dylib` | Frida-Gum injection hook (Stage 2) |
| `Build/libopener.dylib`    | Tweak loader library (Stage 3)     |


---



## Architecture

Ammonia operates in three stages:

### Stage 1: LaunchDaemon (`ammonia/main.m`)

Compiled into an executable that runs as a LaunchDaemon (`com.bedtime.ammonia`). It:

1. Obtains a Mach task port for `launchd` (pid 1) via `task_for_pid`.
2. Allocates stack and code segments in launchd's address space.
3. Patches shellcode with runtime addresses (`pthread_create_from_mach_thread`, `dlopen`) and the payload path.
4. Writes shellcode into launchd's memory with `VM_PROT_EXECUTE` protection.
5. Creates a remote thread that calls `dlopen("/private/var/ammonia/core/liblibinfect.dylib", 1)`.
6. Waits for the sentinel value `0x79616265` ("ebay") in the return register, then terminates.

Supports both arm64e (with pointer authentication) and arm64. On macOS 14.4+ / 15+, uses `thread_terminate` + `thread_create_running`.

### Stage 2: Injection Hook (`libinfect/libinfect.m`)

Loaded via `dlopen` inside launchd. Uses **Frida-Gum** (statically linked) to:

- Intercept `posix_spawn` and `posix_spawnp` via `gum_interceptor_replace`.
- `loginwindow`: Always inject `libopener.dylib`.
- `xpcproxy`: Inject `liblibinfect.dylib` (spreads the hook to child processes), unless disabled via `ammonia.disable-xpcproxy`.
- **UI processes** (darwin role `UI_FOCAL`, `UI`, or `UI_NON_FOCAL`): Inject `libopener.dylib`, subject to the global blacklist.
- **Drivers** (path ending in `Driver`): Skipped entirely.
- Appends to `DYLD_INSERT_LIBRARIES` if already set.



### Stage 3: Tweak Loader (`opener/opener.m`)

Injected into target processes via `DYLD_INSERT_LIBRARIES`. It:

1. Dynamically loads `fridagum.dylib` (shared library).
2. Scans `/private/var/ammonia/core/tweaks/` and `/private/var/ammonia/core/gui/` for loadable modules.
3. Runs security checks (ownership, permissions, path traversal).
4. Evaluates per-tweak whitelist/blacklist filters.
5. Calls `dlopen` and optionally invokes `LoadFunction(void *gum_interceptor)`.
6. Responds to `SIGUSR1` by rescanning both directories and loading new or modified modules.

---



## Developing Tweaks for Ammonia

Ammonia loads dynamic libraries at runtime into UI processes. Compile for both `arm64` and `arm64e`:

```sh
clang -arch arm64 -arch arm64e -dynamiclib -o YourTweak.dylib YourTweak.m
```



### Entry Points

Ammonia supports three entry point styles:

#### 1. Objective-C `+load`

```objc
@implementation YourTweak
+ (void)load {
    NSLog(@"[!] YourTweak loaded!");
    // Your initialization logic here
}
@end
```



#### 2. C Constructor

```c
__attribute__((constructor))
static void init_tweak(void) {
    printf("[!] Tweak constructor called!\n");
}
```



#### 3. Ammonia `LoadFunction` (recommended for Frida-Gum access)

```c
void LoadFunction(void *gum_interceptor) {
    printf("[!] LoadFunction called with gum interceptor %p\n", gum_interceptor);
}
```

This is called directly after `dlopen` and provides the Frida-Gum `GumInterceptor` pointer, allowing you to set up hooks immediately.

### Deploying Tweaks

Place compiled `.dylib` files in `/private/var/ammonia/core/tweaks/`. Files must be:

- **Owned by root** (`uid 0`)
- **Not group-writable or world-writable** (mode must not have `S_IWGRP` or `S_IWOTH`)

---



## Tweak Filtering

Each tweak `.dylib` can have a sibling `.whitelist` or `.blacklist` file in the tweaks directory:


| Filter File  | Behavior                                                             |
| ------------ | -------------------------------------------------------------------- |
| `.whitelist` | Load the tweak **only if** the current process path matches an entry |
| `.blacklist` | Load the tweak **unless** the process path matches an entry          |
| Neither      | **Skip** the tweak (never loaded)                                    |
| Both         | Whitelist takes precedence; a warning is logged if both files exist |




### Entry Formats

Entries in filter files can be:

- **Exact path** (contains `/`): matched via `strcmp` — e.g., `/System/Applications/Safari.app/Contents/MacOS/Safari`
- **Binary/suffix name** (no `/`): matched via `path_ends_with` — e.g., `Safari` matches any path ending in `/Safari`



### Example

```
/private/var/ammonia/core/tweaks/
├── mytweak.dylib
├── mytweak.dylib.whitelist    # only loads in these processes
└── mytweak.dylib.blacklist    # loads everywhere except these
```

---



## Global Process Blacklist

The file `/private/var/ammonia/core/ammonia.blacklist` prevents opener injection into specific processes globally (Stage 2). Lines support `#` comments and whitespace trimming. Matching uses `path_matches_entry` (exact path or suffix name).

This is independent of per-tweak filtering — blacklisted processes never receive `libopener.dylib` at all.

---



## Security Model


| Layer              | Mechanism                                            |
| ------------------ | ---------------------------------------------------- |
| SIP                | Must be disabled for `task_for_pid` to work          |
| Library Validation | Globally disabled to allow unsigned dylibs           |
| Tweak ownership    | Must be root-owned (`st_uid == 0`)                   |
| Tweak permissions  | Must not be group-writable or world-writable         |
| Path traversal     | Rejected in tweak filenames (`..` and `/` detection) |
| Process blacklist  | Global `ammonia.blacklist` prevents opener injection |
| File permissions   | All core files set to `755`, owned by `root:wheel`   |


---



## File Layout


| Path                                               | Purpose                            |
| -------------------------------------------------- | ---------------------------------- |
| `/private/var/ammonia/core/ammonia`                | LaunchDaemon executable            |
| `/private/var/ammonia/core/liblibinfect.dylib`     | Frida-Gum injection hook           |
| `/private/var/ammonia/core/libopener.dylib`        | Tweak loader library               |
| `/private/var/ammonia/core/fridagum.dylib`         | Frida-Gum shared library           |
| `/private/var/ammonia/core/tweaks/`                | User-provided tweak `.dylib` files |
| `/private/var/ammonia/core/gui/`                   | GUI tweak `.dylib` files (same loading rules as `tweaks/`) |
| `/private/var/ammonia/core/ammonia.blacklist`      | Optional process blacklist         |
| `/private/var/ammonia/core/ammonia.disable-xpcproxy` | Optional flag to disable xpcproxy hook propagation |
| `/private/var/ammonia/core/infect.log`             | Stage 2 log file (appended)        |
| `/Library/LaunchDaemons/com.bedtime.ammonia.plist` | LaunchDaemon plist                 |


---



## Uninstallation

```sh
sudo rm -rf /private/var/ammonia
sudo launchctl bootout system/com.bedtime.ammonia
sudo rm -f /Library/LaunchDaemons/com.bedtime.ammonia.plist
```

---



## Debug Logging

- **Stage 2** (`liblibinfect.dylib`): Writes to `/private/var/ammonia/core/infect.log` — logs which processes receive opener injection and blacklist skips.
- **Stage 3** (`libopener.dylib`): Uses `syslog` with `LOG_ERR`/`LOG_INFO` — view with `log stream --predicate 'eventMessage contains "ammonia"'`. Send `kill -USR1 <pid>` to trigger a tweak rescan without restarting the process.

---



## Support the Original Creator

[Ko-fi](https://ko-fi.com/corebedtime)