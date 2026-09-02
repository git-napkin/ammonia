# ammonia (injector)

Root helper at `/private/var/ammonia/core/ammonia`. With no args it `dlopen`s `libinject.dylib` into launchd using a mach-thread trampoline: `pthread_create_from_mach_thread`, then `pthread_join` of a helper that `dlopen`s the payload. Success is `x0 == 0x79616265` on the mach thread **and** the dylib path showing up in the target’s dyld image list. `pthread_create` / `pthread_join` / `dlopen` failure leaves `x0` unset as that sentinel. In macOS Safe Mode (`kern.safeboot` or boot-args `-x`) it exits 0 and does nothing.

LaunchDaemon `com.ammonia.inject`: RunAtLoad, KeepAlive `{SuccessfulExit=false}`. A successful inject does not loop. A crash retries after ThrottleInterval. A safe-boot exit 0 does not retry.

`ammonia --scan` late-injects `libopener.dylib` into already-running AppKit processes. It skips Dock, loginwindow, WallpaperAgent, WindowServer, launchd, drivers, `/usr/libexec/*`. After infecting launchd, ammonia also one-shot late-loads opener into a running `loginwindow` (that process is often spawned before infect hooks `posix_spawn`). Later loginwindow spawns still go through infect.

`ammonia --pid N [--dylib PATH]` injects one process.

Ad-hoc signing must not embed `Master.entitlements`. Privilege is SIP `--without debug`.

If launchd panics, delete `/Library/LaunchDaemons/com.ammonia.inject.plist` from Recovery.
