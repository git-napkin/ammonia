# AGENTS.md — Plugin Playground

Runtime tweak system for macOS Apple Silicon. CMake build producing dylib injectors + a Slint configurator GUI.

## Platform requirements

- **macOS Apple Silicon (arm64e)** only.
- **SIP must be partially disabled**: `csrutil enable --without debug`. Required to attach to launchd (PID 1) and set hardware breakpoints. Full off is not needed.
- Xcode Command Line Tools (`xcode-select --install`), CMake 3.16+, git.

## Build

Primary path (produces `PluginPlayground-1.0.0.pkg`):

```sh
sh ./install.sh                 # auto-runs setup_frida.sh if fridagum.dylib is missing
```

CI-style build (no configurator, no packaging):

```sh
cmake -S . -B Build -DCMAKE_BUILD_TYPE=Release -DBUILD_CONFIGURATOR=OFF
cmake --build Build -j8
```

Frida-Gum prerequisite (fetches devkit + builds `fridagum.dylib` + `libfrida-gum-arm64e-arm64.a`):

```sh
sh ./setup_frida.sh
```

`install.sh` calls this automatically if `fridagum.dylib` is absent, but CI runs it explicitly as a separate step.

### CMake options (all cache variables)

| Option | Default | Notes |
|---|---|---|
| `BUILD_ARM64E` | `ON` | arm64e vs arm64. Off → plain arm64 (requires `disablePAC` toggle at runtime). |
| `BUILD_CONFIGURATOR` | `ON` | GUI app. Requires Slint via FetchContent (first build downloads from GitHub). |
| `BUILD_TESTS` | `ON` | CTest unit tests. |
| `CODESIGN_IDENTITY` | `-` | Ad-hoc by default. `grant` is signed with `Master.entitlements`. Hook dylibs are signed without an entitlements file. |
| `CMAKE_INSTALL_PREFIX` | `/opt/pluginplayground` | Staging/install prefix when using `cmake --install`. |

Debug builds (`-DCMAKE_BUILD_TYPE=Debug`) auto-enable AddressSanitizer and clang-tidy (if available).

### Nix (optional, reproducible)

```sh
nix build          # builds the package
nix develop        # dev shell with clang-tools, cargo, rustc
```

The Nix build produces **arm64** (not arm64e). Toggle **Disable arm64e (PAC)** in the Configurator so injection works. Native arm64e requires `sudo nvram boot-args="-arm64e_preview_abi"` + reboot (set from Recovery Mode with SIP adjusted).

## Testing

Two tiers — don't conflate them.

### Unit tests (CTest)

Run from any build directory with `BUILD_TESTS=ON`:

```sh
ctest --test-dir Build --output-on-failure
# or directly:
./Build/test_envbuf
./Build/test_tweak_utils
```

`test_envbuf` covers `syphon/envbuf.c`; `test_tweak_utils` covers `syphon/tweak_utils.c`. No services or install required.

### Capability tests (integration, requires installed system)

```sh
sh ./testing.sh
```

This is **not** a unit test. It:
1. Verifies the install at `/opt/pluginplayground` (binaries, `current.options`, grant LaunchDaemon loaded).
2. Builds `testing/testing.dylib` via `make -C testing/`.
3. Installs the tweak + whitelist + options into `/opt/pluginplayground/tweaks/` (root-owned).
4. Adds `testing.dylib` to `enabledTweaks` in `current.options`.
5. Sends `SIGUSR1` to the first whitelisted process (Terminal, Finder, Dock, Safari) that has `libplayground_opener` loaded.
6. Waits up to 30s for `$HOME/pluginplayground_test_results.txt`.

**Prerequisites**: playground installed (`sudo ./install.sh`), grant daemon running, and a whitelisted app launched (e.g. open Terminal). If no target is found, launch one and re-run. Cleanup (tweak removal, options restore) runs automatically via trap.

## Architecture

Injection chain (all required, no opt-out):

```
grant (LaunchDaemon, boot)
  → task_for_pid(1) + mach_vm shellcode
  → dlopen(libfangs_hook.dylib) inside launchd
    → Frida-Gum posix_spawn interceptor
    → injects DYLD_INSERT_LIBRARIES=libplayground_opener.dylib into UI processes
      → loads enabled .dylib tweaks from /opt/pluginplayground/tweaks/
```

- `syphon/exe.c` — PAC stripping: reads `disablePAC` from `current.options`. For arm64e `.app` bundles only, copies to `/tmp/RuntimeApplications/` with `copyfile(COPYFILE_CLONE)` (APFS copy-on-write, full copy fallback), zeroes arm64e cpusubtype, removes `LC_CODE_SIGNATURE`, ad-hoc re-signs. Skips plain arm64. Copy/depacify/resign failure spawns the original path.
- `syphon/options_loader.c` — live-reloads `current.options` via dispatch VNODE watcher (100ms debounce).
- `syphon/tweak_utils.c` — whitelist/blacklist matching, path-traversal guards, tweak loading.
- `include/playground_tweak.h` — optional `LoadFunction(void *interceptor)` entry for tweak authors.
- `configurator/` — Slint GUI (`configurator.slint` + `controller.cpp`/`dmanager.cpp`/`options.cpp`/`tweaks.cpp`).

## Install layout

```
/opt/pluginplayground/
  bin/grant                      # launchd injector (LaunchDaemon: com.pluginplayground.grant)
  lib/libfangs_hook.dylib        # Frida-Gum posix_spawn hook (in launchd)
  lib/libplayground_opener.dylib # per-process tweak loader
  lib/fridagum.dylib             # Frida-Gum runtime (build prerequisite)
  share/com.pluginplayground.grant.plist
  include/playground_tweak.h     # optional LoadFunction() for tweak authors
  tweaks/*.dylib                 # tweaks + .whitelist/.blacklist/.options sidecars
  current.options                # plist config (created by the pkg, chmod 666)
  ammonia.blacklist              # process blacklist for fangs_hook
/Library/LaunchDaemons/com.pluginplayground.grant.plist
/Applications/Plugin Playground.app   # configurator GUI
```

## Configuration (`current.options` plist)

Managed via `defaults`/`plutil` (no custom CLI):

```sh
defaults write /opt/pluginplayground/current.options disablePAC -bool true
defaults write /opt/pluginplayground/current.options enabledTweaks -array-add "MyTweak.dylib"
defaults read /opt/pluginplayground/current.options
```

First write needs `sudo` only if the pkg postinstall has not created the file yet. The installer creates `current.options` with `chmod 666`. Keys: `disablePAC`, `useLegacyAmmonia`, `pauseInjection` (bools); `enabledTweaks` (array of filenames). Options reload live. No restart needed.

The grant LaunchDaemon is `KeepAlive` only when grant exits nonzero (throttled to 10s). A successful inject does not restart grant in a loop.

## Gotchas

- **arm64e vs arm64**: Nix and CI builds produce arm64. If not running native arm64e, set `disablePAC=true` (or toggle in Configurator). Native arm64e needs the `boot-args` nvram flag.
- **Slint FetchContent**: first `BUILD_CONFIGURATOR=ON` build downloads Slint v1.16.1 from GitHub. No network → set `FETCHCONTENT_SOURCE_DIR_SLINT` or use Nix.
- **Build dirs**: `.build/`, `.build-cargo/`, `Build/` are gitignored. The CMake build symlinks `$BUILD_DIR/cargo` → `.build-cargo` for Rust dependency caching.
- **Tweak compilation**: tweaks are arm64 bundles (`-bundle -undefined dynamic_lookup`). See `testing/Makefile` for the canonical flags. Export `LoadFunction(void *interceptor)` if you need the process Frida interceptor. Include `/opt/pluginplayground/include/playground_tweak.h`.
- **Tweak targeting**: `Name.dylib.whitelist` (one executable per line) is exclusive when the file exists, including empty. An empty whitelist loads nowhere. The Configurator Edit sheet writes that sidecar. `blacklistedApps` in `Name.dylib.options` still applies after whitelist/blacklist files.
- **Legacy Ammonia**: `useLegacyAmmonia=true` loads from `/private/var/ammonia/core/tweaks/`. Disable/remove the Ammonia daemon first to avoid conflicts.
- **Uninstall**: `./uninstall.sh` (re-elevates via sudo). Reboot recommended to clear injected code.

## Repo conventions

- Branches: `master` (main), `backup`. PRs target `master`.
- CI (`.github/workflows/build.yml`): builds on `macos-latest`, runs `setup_frida.sh`, builds with `BUILD_CONFIGURATOR=OFF`, verifies `libfangs_hook.dylib` / `libplayground_opener.dylib` / `grant` exist.
- Docs live in `docs/` (ammonia, compilation, configurator, defaults, fangs, grant, pac_stripping).
