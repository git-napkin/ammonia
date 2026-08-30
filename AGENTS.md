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
sh ./setup_frida.sh   # if fridagum.dylib / libfrida-gum-arm64e-arm64.a missing
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release -DBUILD_CONFIGURATOR=OFF
cmake --build Build -j8
```

CMake: `BUILD_ARM64E` (default ON) applies to `ammonia` / `libinject` / `libopener` only. Ammonia.app stays **arm64** (SwiftUI GUI). Ad-hoc `ammonia` is signed without `Master.entitlements` (AMFI Code=-424). Infect is **not** post-codesigned; linker-signed ad-hoc matches stock Ammonia.

`CMAKE_INSTALL_PREFIX` default: `/private/var/ammonia/core` (flat layout, not `bin/` + `lib/`).

## Injection chain

```
ammonia (LaunchDaemon, one-shot, KeepAlive SuccessfulExit=false)
  → trampoline dlopen(libinject.dylib) into launchd
    → gum_init_embedded + gum_module_find_global_export_by_name("posix_spawn")
    → UI children / xpcproxy get DYLD_INSERT_LIBRARIES=libopener.dylib
      → opener: enabledTweaks from current.options, dlopen tweaks/

ammonia --scan
  → late dlopen(libopener) into already-running AppKit processes
    (skips Dock, WallpaperAgent, WindowServer, …)
```

Do not put Security, PAC strip, options watchers, or `dlopen(fridagum.dylib)` into PID 1. That is what playground fangs/lite did; it SIGSEGVs launchd on Darwin 27. Infect logs to `infect.log` like stock Ammonia. `ammonia.blacklist` at the support root skips opener insert (Dock, WallpaperAgent, …). `loginwindow` is spawn-injected when infect is already in launchd; ammonia also late-loads opener into a running loginwindow because that process often wins the boot race. TransparentPictures may load there but must not install global `NSCachedImageRep` / `wantsLayer` hooks — that panicked WindowServer at the boot login screen. The lock-screen photo is `LUI2TrackedImageView` (force an opaque white plate; do not map it to clearColor).

**Safe mode:** `kern.safeboot` or boot-args token `-x`. `ammonia` exits 0 without injecting (KeepAlive will not retry). Infect’s constructor does not hook posix_spawn. Opener does not load gum or tweaks.

## Tweak loading (opener)

`libinfect` is compiled against Ammonia's Gum header/ABI (`gum_interceptor_replace` is interceptor, address, replacement, replacement_data, original). Current Frida 17.9.11 docs list a different last argument. `setup_frida.sh` writes `include/frida-gum.h` for that newer SDK; do not point infect at it. CMake prefers `../legacy/ammonia/libinfect/frida-gum.h` and `../legacy/ammonia/libfrida-gum-arm64e-arm64.a`.

Tweaks live under `/private/var/ammonia/core/tweaks/` so sandboxed apps can `dlopen` them. There is no `/opt/pluginplayground` tree.

`frameworkDependencies` in a tweak `.options` sidecar is an AppKit-style gate (TransparentPictures uses it; SquareCorners does not). It is satisfied if **either**:

- the host executable’s Mach-O load commands name that framework (header + `sizeofcmds` only — large binaries like Warp are not mmap’d whole), or
- the framework is already mapped in the process (`_dyld_get_image_name`). Electron stubs do not link AppKit; it arrives via Electron Framework after opener’s constructor, so opener rescans on the main queue.

If the sidecar is unreadable, the framework gate is skipped (`check_dylib_options` returns true).

## Clean reinstall (drop Playground)

```sh
sudo sh ./uninstall.sh
# reboot — launchd mappings die with PID 1
sh ./install.sh
sudo installer -pkg Ammonia-1.0.0.pkg -target /
# reboot again — every UI spawn gets opener from infect
cd ../tweaks && sudo make TWEAK=SquareCorners install && sudo make TWEAK=TransparentPictures install
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
  include/playground_tweak.h
  share/com.ammonia.inject.plist
  tweaks/
/Library/LaunchDaemons/com.ammonia.inject.plist
/Applications/Ammonia.app
```

```sh
defaults write /private/var/ammonia/core/current.options enabledTweaks -array-add "MyTweak.dylib"
defaults read /private/var/ammonia/core/current.options
```

Keys: `disablePAC`, `pauseInjection`, `enabledTweaks`. `pauseInjection` stops opener from loading tweaks; infect still inserts opener on spawn.

## Tests

`sh ./testing.sh` after install. Results: `~/ammonia_test_results.txt`.

## Leftover source

`syphon/fangs_hook.c`, `fangs_hook_lite.c`, `launchd_probe.c` are not built. PAC strip in `exe.c` is unused while infect is the launchd payload. The GUI is `gui/` (SwiftUI SPM), bundled as `configurator.app` → `/Applications/Ammonia.app`.
