<p align="center">
  <img src=".pics/PlainLogo.png" width="128" alt="Ammonia">
</p>

# Ammonia

Runtime tweak loader for macOS Apple Silicon. Spawn-time `DYLD_INSERT_LIBRARIES` via a posix_spawn hook in launchd, plus a GUI for enabling tweaks.

> [!WARNING]
> SIP must allow debugging: `csrutil enable --without debug`. Full off is not required. The installer also turns off library validation and expects `boot-args=-arm64e_preview_abi`.
>
> macOS Safe Mode (`kern.safeboot` / boot-args `-x`) must leave Ammonia idle: the daemon exits 0 without injecting, infect does not hook `posix_spawn`, and opener does not load tweaks. Hold Shift at boot (Apple Silicon: Shift-click Continue in startup options) to get a clean login if a tweak panics.

Build as **arm64e**. The GUI is `/Applications/Ammonia.app`. Core files live under `/private/var/ammonia/core/`.

To throw away Plugin Playground (`/opt/pluginplayground`, old grant job, old app) and start clean: `sudo sh ./uninstall.sh`, reboot, install the pkg, reboot, then `sudo make TWEAK=… install` from `tweaks/`.

![Configurator](.pics/Configurator.png)

## What tweaks do

Tweaks are `.dylib`s inserted at spawn (`libopener` via infect in launchd). They can change drawing, window chrome, and framework behavior without patching the app on disk. SquareCorners loads in Chromium/Electron as well as AppKit apps. TransparentPictures stays AppKit-only; loginwindow gets an opaque white plate behind the lock-screen photo (that process often needs a late opener load).

- **Classic Dock** — pre-Yosemite shelf.
![Classic Dock](.pics/ClassicDock.png)
- **Classic Scrollbars** — arrows and aqua thumb.
<img src=".pics/ClassicScrollbars.png" height="260" alt="Classic Scrollbars">

## Build

- macOS Apple Silicon
- Xcode CLT, CMake 3.16+, git
- Swift 5.9+ (included with Xcode CLT) for the GUI

```sh
sh ./install.sh
```

Produces `Ammonia-1.0.0.pkg`. Uninstall: `./uninstall.sh`.

## Docs

- [Injection (ammonia binary)](docs/grant.md)
- [Infect hook](docs/fangs.md)
- [GUI](docs/configurator.md)
- [defaults](docs/defaults.md)
- [Compilation](docs/compilation.md)
