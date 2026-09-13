# Infect (`libinject.dylib`)

Stock Ammonia hook, built from `syphon/libinfect.m`. Injected into launchd. Statically links Frida-Gum. Does not `dlopen` `fridagum.dylib` into PID 1.

Constructor: `gum_init_embedded`, `gum_module_find_global_export_by_name("posix_spawn"|"posix_spawnp")`, `gum_interceptor_replace`. Same contract as [Frida Gum interceptor replace](https://github.com/frida/frida-gum).

On spawn: UI darwin-role processes get `DYLD_INSERT_LIBRARIES=libopener.dylib`. `xpcproxy` gets `libinject.dylib`. `loginwindow` is always inserted (account pictures). If loginwindow already started before infect hooked `posix_spawn`, `ammonia` late-loads opener into that process once. Drivers, Node SEA (`syphon/macho_sea.c`, hardened load-command bounds), and `ammonia.blacklist` are skipped (Dock, WallpaperAgent, WindowServer). Infect does not strip `DYLD_INSERT_LIBRARIES` for SEA — parent-spawned SEA still inherit the insert; `libopener` detects `__NODE_SEA_BLOB`, `unsetenv`s it in its constructor, and skips gum/tweaks so Node main can run. Safe boot (`kern.safeboot` or boot-args `-x`) leaves posix_spawn unhooked. Logs to `/private/var/ammonia/core/infect.log`.

Not linked: Security.framework, tweak_utils, options watcher, syslog in the constructor. Linker-signed ad-hoc (no `codesign -s -` post-build). Playground copies that post-signed Frida into launchd panicked Darwin 27.
