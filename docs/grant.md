# ammonia (injector)

Root helper at `/private/var/ammonia/core/ammonia`. With no args it `dlopen`s `libinject.dylib` into launchd using the Jeremy Legendre trampoline (same bytes as stock Ammonia; C-string size is 181 including NUL). In macOS Safe Mode (`kern.safeboot` or boot-args `-x`) it exits 0 and does nothing.

LaunchDaemon `com.ammonia.inject`: RunAtLoad, KeepAlive `{SuccessfulExit=false}`. A successful inject does not loop. A crash retries after ThrottleInterval. A safe-boot exit 0 does not retry.

`ammonia --scan` late-injects `libopener.dylib` into already-running AppKit processes. It skips Dock, loginwindow, WallpaperAgent, WindowServer, launchd, drivers, `/usr/libexec/*`. After infecting launchd, ammonia also one-shot late-loads opener into a running `loginwindow` (that process is often spawned before infect hooks `posix_spawn`). Later loginwindow spawns still go through infect.

`ammonia --pid N [--dylib PATH]` injects one process.

Ad-hoc signing must not embed `Master.entitlements`. Privilege is SIP `--without debug`.

If launchd panics, delete `/Library/LaunchDaemons/com.ammonia.inject.plist` from Recovery.
