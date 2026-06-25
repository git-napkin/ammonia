# AGENTS.md

## Project Overview

**Ammonia** is a modern macOS extension/tweak loader that injects dynamic libraries into running processes on jailbroken Macs (requires SIP disabled). It uses a three-stage architecture: a LaunchDaemon executable injects shellcode into `launchd` (pid 1), which loads a Frida-Gum-based hook library that intercepts `posix_spawn`/`posix_spawnp`, and then injects an "opener" library into UI processes to load tweaks from a filesystem directory.

---

## Directory Layout

```
ammonia/
├── .github/
│   ├── FUNDING.yml              # Ko-fi sponsorship config
│   └── workflows/
│       └── build.yml            # GitHub Actions CI: build on push/PR to master/backup
├── ammonia/
│   ├── main.h                   # Stub header (only includes stdio.h)
│   └── main.m                   # LaunchDaemon executable: injects shellcode into pid 1
├── libinfect/
│   ├── envbuf.c                 # Environment buffer manipulation utilities
│   ├── envbuf.h                 # Header for envbuf functions
│   ├── frida-gum.h              # Frida-Gum C header (53,836 lines, 3rd-party)
│   └── libinfect.m              # Frida-Gum hook: intercepts posix_spawn/posix_spawnp
├── opener/
│   ├── opener.h                 # Header (includes ammonia.h, sys headers)
│   └── opener.m                 # Tweak loader: scans /tweaks/, loads .dylib files
├── Build/                       # CMake build output (gitignored)
│   ├── ammonia                  # Compiled executable
│   ├── liblibinfect.dylib       # Compiled injection library
│   ├── libopener.dylib          # Compiled opener library
│   ├── CMakeCache.txt
│   ├── Makefile
│   └── compile_commands.json
├── AGENTS.md                    # This file
├── CMakeLists.txt               # CMake build configuration (C, CXX, OBJC)
├── README.md                    # Usage and tweak development docs
├── ammonia.h                    # Shared header: defines SupportFolderP
├── ammonia.pkg                  # Pre-built installer (gitignored)
├── compile.sh                   # CMake configure + make wrapper
├── fridagum.dylib               # Pre-built Frida-Gum shared library
├── libfrida-gum-x86_64-arm64e-arm64.a  # Multi-arch Frida-Gum static library
├── package.sh                   # Creates macOS .pkg installer
├── setup_frida.sh               # Downloads and builds Frida-Gum dependencies
└── uninstaller.sh               # Removes Ammonia from the system
```

---

## Architecture & Execution Flow

### Stage 1: LaunchDaemon (`ammonia/main.m`)

Compiled into an executable (`ammonia`) that runs as a LaunchDaemon (`com.bedtime.ammonia`).

1. Calls `task_for_pid(mach_task_self(), 1, &task)` to obtain a Mach task port for `launchd` (pid 1).
2. Allocates a stack segment (`mach_vm_allocate`, 16 KB) and a code segment in launchd's address space.
3. Patches inline shellcode with the runtime addresses of `pthread_create_from_mach_thread` and `dlopen`, plus the payload path string.
4. Writes the shellcode into launchd's memory and sets memory protection to `VM_PROT_EXECUTE | VM_PROT_READ`.
5. Creates and runs a remote thread in launchd that calls `pthread_create_from_mach_thread` -> `dlopen("/private/var/ammonia/core/liblibinfect.dylib", 1)`.
6. Polls the thread's return value (`__rax`/`__x[0]`) for the sentinel `0x79616265` ("ebay" in ASCII little-endian), then terminates the thread.

#### Platform-specific details

- **x86_64**: Uses `thread_create_running` directly with `x86_thread_state64_t`. Two shellcode functions: outer calls `pthread_create_from_mach_thread`, inner calls `dlopen`.
- **arm64e** (Apple Silicon): Uses `thread_create`, `thread_convert_thread_state` (dlsym'd from `libsystem_kernel.dylib`), then `thread_set_state` + `thread_resume`, or `thread_terminate` + `thread_create_running` for macOS 14.4+ / 15+. Uses `ptrauth` APIs for pointer authentication. Arm64e shellcode uses `pacibsp`/`retab` and `paciza` for authenticated pointers.

### Stage 2: Injection Hook (`libinfect/libinfect.m`)

Loaded via `dlopen` in launchd. Uses Frida-Gum (static library `libfrida-gum-x86_64-arm64e-arm64.a`).

1. Constructor `Infect(void)` runs on library load.
2. Calls `gum_init_embedded()` and `gum_interceptor_obtain()`.
3. Replaces `posix_spawn` with `SpawnNew` and `posix_spawnp` with `SpawnPNew` using Frida-Gum's interceptor.
4. `SpawnNew` determines which processes to inject:
   - **`loginwindow`**: Always injects `libopener.dylib` via `DYLD_INSERT_LIBRARIES`.
   - **`xpcproxy`**: Injects `liblibinfect.dylib` (spreads the hook to child processes).
   - **UI processes** (darwin role `PRIO_DARWIN_ROLE_UI_FOCAL`, `PRIO_DARWIN_ROLE_UI`, or `PRIO_DARWIN_ROLE_UI_NON_FOCAL`): Injects `libopener.dylib`, unless blacklisted.
   - **Drivers** (path ends with `Driver`): Skipped entirely.
5. Appends to existing `DYLD_INSERT_LIBRARIES` if already set; otherwise creates it.
6. Logs to `/private/var/ammonia/core/infect.log` via `LogToFile`.

#### Blacklist (`ammonia.blacklist`)

- Loaded at startup from `SupportFolderP "ammonia.blacklist"`.
- Substring-based matching against the process path using `path_ends_with`.
- Lines support `#` comments, leading/trailing whitespace trimming.
- Blacklisted processes skip opener injection entirely.

### Stage 3: Tweak Loader (`opener/opener.m`)

Loaded via `DYLD_INSERT_LIBRARIES` into target processes.

1. Constructor `ctor_main(void)` runs on library load.
2. Dynamically loads `fridagum.dylib` (shared lib Frida-Gum) and resolves `gum_init_embedded` and `gum_interceptor_obtain`.
3. Calls `Open(interceptor)`.
4. `Open` scans `/private/var/ammonia/core/tweaks/` for regular files.
5. For each `.dylib` file in the tweaks directory:
   - Rejects path traversal attempts (checks for `..` and `/` in filename).
   - Checks for a `.whitelist` sibling: if present, the tweak loads ONLY if the current process path matches an entry (via exact match or suffix match).
   - Checks for a `.blacklist` sibling: if present, the tweak loads UNLESS the current process path matches an entry.
   - If neither whitelist nor blacklist exists, the tweak is **skipped** (not loaded).
   - Validates file ownership (must be `root` uid 0) and permissions (must not be group/world-writable).
   - Loads the tweak via `dlopen(full_path, RTLD_LAZY | RTLD_GLOBAL)`.
   - Optionally calls `void LoadFunction(void *interceptor)` if the symbol is exported.

### Environment Buffer (`libinfect/envbuf.c` / `envbuf.h`)

Utility library for safe environment array manipulation:

| Function | Description |
|---|---|
| `envbuf_len` | Returns number of entries + 1 (for NULL terminator) |
| `envbuf_mutcopy` | Deep-copies a `char **envp` array with `strdup` |
| `envbuf_free` | Frees all entries and the array |
| `envbuf_find` | Finds index of env var by name (returns -1 if not found) |
| `envbuf_getenv` | Gets value of env var by name |
| `envbuf_setenv` | Sets or adds an env var (reallocates if needed) |
| `envbuf_unsetenv` | Removes an env var (reallocates) |

---

## Configuration & Paths

### `ammonia.h` — Central Path Constant

```c
#define SupportFolderP "/private/var/ammonia/core/"
```

All components reference this path. The deploy structure is:

| Path | Purpose |
|---|---|
| `/private/var/ammonia/core/ammonia` | LaunchDaemon executable |
| `/private/var/ammonia/core/liblibinfect.dylib` | Frida-Gum injection hook |
| `/private/var/ammonia/core/libopener.dylib` | Tweak loader library |
| `/private/var/ammonia/core/fridagum.dylib` | Frida-Gum shared library |
| `/private/var/ammonia/core/tweaks/` | User-provided tweak `.dylib` files |
| `/private/var/ammonia/core/gui/` | Reserved for GUI components |
| `/private/var/ammonia/core/ammonia.blacklist` | Optional process blacklist |
| `/private/var/ammonia/core/infect.log` | Log file (appended to) |
| `/Library/LaunchDaemons/com.bedtime.ammonia.plist` | LaunchDaemon plist |

---

## Build System

### CMake (`CMakeLists.txt`)

- **Minimum version**: 3.15
- **Languages**: C, CXX, OBJC
- **Build type**: Release (default)
- **Architectures**: `x86_64` and `arm64e` (via `CMAKE_OSX_ARCHITECTURES`)
- **ARC**: Objective-C Automatic Reference Counting enabled (`-fobjc-arc`) on all three targets
- **Targets**:
  - `ammonia` — executable, links Cocoa + CoreFoundation
  - `libinfect` — shared library (`.dylib`), links Cocoa + CoreFoundation + `libfrida-gum-x86_64-arm64e-arm64.a`
  - `opener` — shared library (`.dylib`), links Foundation only
- Uses `find_library` for macOS frameworks (Foundation, Cocoa, CoreFoundation)
- Contains commented-out `ammapp` macOS app bundle target

### Build Script (`compile.sh`)

```sh
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=1
cd Build && make -j8
```

### Frida Setup (`setup_frida.sh`)

- Downloads Frida-Gum devkit v17.9.11 for x86_64, arm64e, and arm64 from GitHub releases
- Extracts each, renames per-arch `.a` files
- Creates a fat static library via `lipo -create`
- Builds `fridagum.dylib` shared library using `clang -arch x86_64 -arch arm64e -arch arm64 -lresolv -fpic -shared -Wl,-all_load`
- Copies outputs to the project root

### Packaging (`package.sh`)

1. Generates a `postinstall` script that:
   - Writes `/Library/LaunchDaemons/com.bedtime.ammonia.plist`
   - Locks down permissions (`chown -R root:wheel`, `chmod 755`)
   - Loads the LaunchDaemon
   - Configures `nvram boot-args=-arm64e_preview_abi` (idempotent)
   - Disables library validation: `defaults write ... DisableLibraryValidation -bool true`
2. Stages artifacts (`ammonia`, `liblibinfect.dylib`, `libopener.dylib`, `fridagum.dylib`) + empty `tweaks/` + `gui/` directories
3. Runs `sudo pkgbuild` with install location `/private/var/`

### Uninstall (`uninstaller.sh`)

```sh
sudo rm -rf /private/var/ammonia
sudo launchctl bootout system/com.bedtime.ammonia
sudo rm -f /Library/LaunchDaemons/com.bedtime.ammonia.plist
```

---

## CI/CD (GitHub Actions)

`.github/workflows/build.yml`:
- Trigger: Push to `master`/`backup`, PR to `master`
- Runner: `macos-latest`
- Steps: Checkout → Show Xcode version → `setup_frida.sh` → `compile.sh` → Verify artifacts

---

## Tweak Development (from `README.md`)

Developers compile `.dylib` files targeting `arm64` + `arm64e` (and optionally `x86_64` for Rosetta).

### Entry Points (supported by opener)

1. **Objective-C `+load`**: Run automatically by ObjC runtime
2. **C constructor** (`__attribute__((constructor))`): Run by dyld
3. **`void LoadFunction(void *gum_interceptor)`**: Called explicitly by Ammonia's opener after `dlopen`, providing the Frida-Gum `GumInterceptor` pointer

### Tweak Filtering

Each tweak `.dylib` can have sibling `.whitelist` or `.blacklist` files in the tweaks directory:
- **Whitelist** (`.whitelist`): Only load this tweak if the current process path matches an entry (exact path match or suffix match)
- **Blacklist** (`.blacklist`): Load this tweak unless the process path matches an entry
- If **neither** file exists, the tweak is **not loaded**
- If **both** exist, whitelist takes precedence (blacklist is ignored)

Entries in filter files can be:
- An exact path (contains `/`): compared via `strcmp`
- A binary/suffix name (no `/`): matched via `path_ends_with` (e.g., `Safari` matches any path ending in `/Safari`)

---

## Security Model

| Layer | Mechanism |
|---|---|
| SIP | Must be disabled for `task_for_pid` to work |
| Library Validation | Globally disabled (`DisableLibraryValidation = true`) to allow unsigned dylibs |
| Tweak ownership | Must be root-owned (`st_uid == 0`) |
| Tweak permissions | Must not be group-writable or world-writable |
| Path traversal | Rejected in tweak filenames (`..` and `/` detection) |
| Process blacklist | Global `ammonia.blacklist` prevents opener injection |
| File permissions | All core files set to `755`, owned by `root:wheel` |

---

## Source Code Conventions

- **Language**: Objective-C (.m) for all runtime logic; C (.c) for utility code
- **Memory management**: ARC enabled on all targets (`-fobjc-arc`)
- **Initialization**: Uses `__attribute__((constructor))` for library auto-init
- **Logging**: `libinfect` uses file-based logging (`fopen` append to `infect.log`); `opener` uses `syslog`
- **Path constants**: Single definition in `ammonia.h` (`SupportFolderP`), included by all components
- **No tests or linters**: The project has no test framework, no lint configuration, and no code formatting rules
- **No error recovery on partial setup failures**: Injection proceeds even if blacklist loading fails (it's silent/optional)

---

## Key Technical Details

### Shellcode Injection (`ammonia/main.m`)

- Shellcode is a flat `char[]` with placeholder zeros for function pointers and the payload path string
- Function addresses are patched in at runtime: `dlsym(RTLD_DEFAULT, "pthread_create_from_mach_thread")` and `dlsym(RTLD_DEFAULT, "dlopen")`
- The sentinel value `0x79616265` ("ebay") in the thread's return register confirms the shellcode executed
- **arm64e specifics**:
  - Uses `ptrauth_strip` to strip pointer authentication from dlsym'd addresses
  - Uses `ptrauth_sign_unauthenticated` + `ptrauth_key_asia` for the PC
  - macOS 14.4+ / 15+: requires `thread_terminate` + `thread_create_running` instead of `thread_set_state` + `thread_resume`
- 16 KB stack allocated per thread with sentinel `0xCAFEBABE`

### Frida-Gum Integration

- **Static linking** (`libfrida-gum-x86_64-arm64e-arm64.a`): Used by `liblibinfect.dylib` for hooking `posix_spawn`/`posix_spawnp` in launchd
- **Dynamic loading** (`fridagum.dylib`): Used by `libopener.dylib` (loaded lazily into target processes)
- The fork (dynamic vs static) exists because `liblibinfect.dylib` cannot rely on finding `fridagum.dylib` in launchd's environment, while `libopener.dylib` can load it naturally
- The `gum_interceptor` is obtained once and passed to tweaks via `LoadFunction(void *interceptor)`

### Driver Filtering

`PathDriver(path)` macro checks if a process path ends with the literal word `Driver`. DriverKit/IOKit driver processes are excluded from all injection.

### Debug Logs

- `infect.log`: Written by `libinfect` at `SupportFolderP "infect.log"`. Appends via `vfprintf`. Used primarily to log which processes get opener injection.
- `syslog`: Used by `opener` with `LOG_ERR`/`LOG_INFO` for tweak loading diagnostics.

---

## Important Gotchas

1. **Hardcoded path length**: `payload_path` in `main.m` is hardcoded to fit within the 128-byte space at the end of the shellcode buffer. Changing the path requires adjusting both the code and the shellcode layout.
2. **No `dlerror` in libinfect**: `SpawnOld`/`SpawnPNew` replacement functions and blacklist loading never report `dlerror` if symbols are missing.
3. **envbuf double-pointer semantics**: `envbuf_setenv` and `envbuf_unsetenv` take `char **envpp[]` (pointer to the pointer array) because `realloc` may change the base pointer.
4. **opener only loads from `tweaks/`**: The `gui/` directory is created by the package installer but never referenced in code.
5. **No hot-reload**: Tweak loading happens only once at library init (constructor); no runtime re-scan mechanism.
6. **Whitelist takes priority**: If both `.whitelist` and `.blacklist` exist for a tweak, only the whitelist is consulted.
7. **Blacklist uses suffix matching**: The `ammonia.blacklist` uses `path_ends_with` (suffix match), while the per-tweak blacklist/whitelist uses `path_matches_entry` (exact path or suffix).
8. **LaunchDaemon `KeepAlive` is `false`**: The daemon runs once, injects, and exits. It is not respawned by launchd.
9. **xpcproxy inheritance**: When `xpcproxy` spawns, `liblibinfect.dylib` is injected into it, extending the hook to processes it spawns (e.g., XPC services).
10. **No test coverage**: The project has no automated tests; verification is manual via `infect.log`.
