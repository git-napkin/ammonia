# Layout

This tree is the Ammonia product (infect + opener + GUI). Upstream [CoreBedtime/ammonia](https://github.com/CoreBedtime/ammonia) is the spawn-hook design this copies.

Install prefix: `/private/var/ammonia/core/`. Tweaks: `tweaks/` plus optional `gui/`. Sidecar `.whitelist` / `.blacklist` still apply after `enabledTweaks`. `.options` `frameworkDependencies` (optional; TransparentPictures uses AppKit, SquareCorners does not) matches the host Mach-O or already-mapped frameworks; opener rescans on the main queue so Electron can satisfy AppKit when a tweak asks for it. `loginwindow` is spawn-injected; TransparentPictures keeps the white avatar plate there and only disables picture-view vibrancy. Safe mode (`kern.safeboot` / `-x`) leaves Ammonia idle.
