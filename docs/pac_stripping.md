# PAC stripping (`arm64e` bypass)

## What it does

Apple Silicon Macs natively run system processes using the `arm64e` ABI, which incorporates Pointer Authentication Codes (PAC). PAC cryptographically signs function pointers in memory to prevent exploits.

When Plugin Playground injects custom tweaks (`.dylib` files) into a system process, the injected library must load into the target process's memory space. If the target is running as `arm64e` with PAC enabled, loading unauthenticated binaries can cause kernel panics or PAC violations.

## How it works

PAC stripping is implemented in `syphon/exe.c`:

1. In `fangs_hook` (loaded inside launchd), the `disablePAC` option is read from `/opt/pluginplayground/current.options` on startup.
2. On each `posix_spawn`/`posix_spawnp` interception, if `disablePAC` is true, `getready_process()` is called with the target path.
3. If the target is an `.app` bundle whose main executable is arm64e, `copyfile` copies it to `/tmp/RuntimeApplications/<name>-<hash>/` with `COPYFILE_CLONE` (APFS copy-on-write, with a full copy fallback) so the original is untouched. The cache is reused until the source bundle is newer. Concurrent spawns are serialized. The hash is of the source bundle path so two apps with the same basename do not collide. Non-bundles and plain arm64 executables are spawned as-is.
4. The Mach-O header is scanned. If `cpusubtype` indicates `arm64e` (`0x2`), it is zeroed to `0` (plain `arm64`). FAT binaries are handled recursively for each slice.
5. The `LC_CODE_SIGNATURE` load command is removed (invalidated by the header change).
6. The executable is re-signed with ad-hoc SHA-256 via `SecCodeSignerCreate`.
7. `fangs_hook` spawns the depacified copy instead of the original. If copy, depacify, resign, or path lookup fails, the original path is spawned and a notice is logged when the spawn path contained `.app`.

## Alternative: native `arm64e` support

Compile Plugin Playground as native `arm64e` (with Xcode) and skip PAC stripping. Apple disables third-party `arm64e` execution by default, so you need to enable the preview ABI:

1. Run: `sudo nvram boot-args="-arm64e_preview_abi"`
2. Reboot.

Modifying `boot-args` requires SIP disabled or adjusted from Recovery Mode.
