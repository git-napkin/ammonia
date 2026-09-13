# AGENTS.md — Ammonia

Runtime tweak loader for macOS Apple Silicon. Ammonia’s launchd infect path plus Playground’s opener, `enabledTweaks` GUI, and tweak authoring.

## Platform

- Apple Silicon. Native arm64e (`BUILD_ARM64E=ON`, `boot-args=-arm64e_preview_abi`).
- SIP: `csrutil enable --without debug` is enough for `task_for_pid` on launchd. Full off is not required.
- Installer sets `DisableLibraryValidation`.

## Build

```sh
sh ./install.sh
```

CI-style:

```sh
sh ./setup_frida.sh   # if fridagum.dylib / libfrida-gum-arm64e-arm64.a missing; SHA-256 pins 17.9.11 gum-devkits
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release -DBUILD_CONFIGURATOR=OFF
cmake --build Build -j8
```

CMake: `BUILD_ARM64E` (default ON) applies to `ammonia` / `libinject` / `libopener` only. Ammonia.app stays **arm64** (SwiftUI GUI). Ad-hoc `ammonia` is signed without `Master.entitlements` (AMFI Code=-424). Infect is **not** post-codesigned; linker-signed ad-hoc matches stock Ammonia.

`CMAKE_INSTALL_PREFIX` default: `/private/var/ammonia/core` (flat layout, not `bin/` + `lib/`).

## Injection chain

```
ammonia (LaunchDaemon, one-shot, KeepAlive SuccessfulExit=false)
  → trampoline: pthread_create_from_mach_thread → helper dlopen → pthread_join;
    SENTINEL in x0 only after a non-NULL handle, plus dyld image-list check
    → gum_init_embedded + gum_module_find_global_export_by_name("posix_spawn"|"posix_spawnp")
    → UI children get DYLD_INSERT_LIBRARIES=libopener.dylib
    → xpcproxy gets DYLD_INSERT_LIBRARIES=libinject.dylib (propagate hook)
      → opener: enabledTweaks from current.options, dlopen tweaks/

ammonia --scan
  → same trampoline late-injects libopener into already-running AppKit processes
    (skips Dock, WallpaperAgent, WindowServer, loginwindow, …)
```

Do not put Security, PAC strip, options watchers, or `dlopen(fridagum.dylib)` into PID 1. That is what playground fangs/lite did; it SIGSEGVs launchd on Darwin 27. Infect logs to `infect.log` like stock Ammonia. `ammonia.blacklist` at the support root skips opener insert (Dock, WallpaperAgent, WindowServer, …). `loginwindow` is spawn-injected when infect is already in launchd; the no-arg daemon path also late-loads opener into a running loginwindow because that process often wins the boot race (`--scan` does not target loginwindow). Flag file `ammonia.disable-xpcproxy` at the support root disables the xpcproxy `libinject` insert.

**Safe mode:** `kern.safeboot` or boot-args token `-x`. `ammonia` exits 0 without injecting (KeepAlive will not retry). Infect’s constructor does not hook posix_spawn. Opener does not load gum or tweaks.

## Tweak loading (opener)

`libinfect` is compiled against Ammonia's Gum header/ABI (`gum_interceptor_replace` is interceptor, address, replacement, replacement_data, original). Frida 17.9.11 uses that same prototype, and current Gum `main` still does too — that ABI has not moved (`gum_module_find_global_export_by_name` is also still current). `setup_frida.sh` still writes `include/frida-gum.h` from the 17.9.11 SDK — keep infect on the PID-1-proven archive regardless, because that build is the one that survived launchd. CMake prefers `../legacy/ammonia/libinfect/frida-gum.h` and `../legacy/ammonia/libfrida-gum-arm64e-arm64.a`.

Node SEA: infect skips adding opener on launchd UI spawns. If a SEA binary still starts with `DYLD_INSERT_LIBRARIES` (inherited from a non-launchd parent), opener’s constructor detects `__NODE_SEA_BLOB`, `unsetenv`s the insert, and returns before gum/tweaks — Node aborts when that env is still set at main.

Tweaks live under `/private/var/ammonia/core/tweaks/` so sandboxed apps can `dlopen` them. There is no `/opt/pluginplayground` tree. Opener rejects tweaks that are not root-owned or that are group/world-writable. Optional `LoadFunction(void *interceptor)` runs after `dlopen` when present. SIGUSR1 and a vnode watch on `current.options` reload tweaks in already-injected processes.

`frameworkDependencies` in a tweak `.options` sidecar is an optional AppKit-style gate (SquareCorners omits it). It is satisfied if **either**:

- the host executable’s Mach-O load commands name that framework (header + `sizeofcmds` only — large binaries like Warp are not mmap’d whole), or
- the framework is already mapped in the process (`_dyld_get_image_name`). Electron stubs do not link AppKit; it arrives via Electron Framework after opener’s constructor, so opener rescans on the main queue.

If the sidecar is unreadable, the framework / `blacklistedApps` gates are skipped. `check_dylib_options` still refuses hosts whose path contains a `Frameworks`, `PrivateFrameworks`, `libexec`, `sbin`, or `DriverExtensions` component. Sidecar `.whitelist` / `.blacklist` and `.options` `blacklistedApps` also filter hosts (whitelist, if present even empty, is exclusive). A lone `*` entry in those lists matches every process.

## Clean reinstall (drop Playground)

```sh
sudo sh ./uninstall.sh
# reboot — launchd mappings die with PID 1
sh ./install.sh
sudo installer -pkg Ammonia-1.0.0.pkg -target /
# reboot again — every UI spawn gets opener from infect
cd ../tweaks && sudo make TWEAK=SquareCorners install
```

`uninstall.sh` boots out both daemons, deletes `/opt/pluginplayground`, both apps, `/private/var/ammonia`, and the old grant plist. Do not install the pkg until after that first reboot, or postinstall will inject into the still-dirty launchd.

If launchd panics, delete `/Library/LaunchDaemons/com.ammonia.inject.plist` from Recovery.

## Layout

```
/private/var/ammonia/core/
  ammonia
  libinject.dylib
  libopener.dylib
  fridagum.dylib
  current.options
  ammonia.blacklist
  include/playground_tweak.h
  share/com.ammonia.inject.plist
  share/ammonia.blacklist
  tweaks/
/Library/LaunchDaemons/com.ammonia.inject.plist
/Applications/Ammonia.app
```

```sh
defaults write /private/var/ammonia/core/current.options enabledTweaks -array-add "MyTweak.dylib"
defaults read /private/var/ammonia/core/current.options
```

Keys: `enabledTweaks`, `pauseInjection`, `disablePAC`. `pauseInjection` stops opener from loading tweaks; infect still inserts opener on spawn. `disablePAC` is read/written but unused: infect is the launchd payload and never PAC-strips (the old rewriter source has been removed — see `docs/pac_stripping.md`).

## Tests

`sh ./testing.sh` after install. During the run the testing tweak writes `~/ammonia_test_results.txt`; the script prints it, then the EXIT trap deletes it. CMake: `test_envbuf`, `test_tweak_utils`, `test_macho_sea` (`ctest` in `Build/`).

## Module notes

`syphon/macho_sea.c` is the one non-obvious multi-use module: Node SEA detection feeds infect's skip-inject path, opener's constructor `unsetenv`, and its own tests. The old unused playground code (`exe.c` PAC rewriter, `bundle_copy.c`, `fangs_hook.c`/`fangs_hook_lite.c`, `launchd_probe.c`, `pac_utils.c`, `log.h`) has been removed. The GUI is `gui/` (SwiftUI SPM), bundled as `configurator.app` → `/Applications/Ammonia.app`.
