# Ammonia.app

Native SwiftUI GUI at `/Applications/Ammonia.app` (built from `gui/` via Swift Package Manager).

Toggles write `/private/var/ammonia/core/current.options`. Opener reloads that file in injected processes.

**Install tweak** copies a `.dylib` into `/private/var/ammonia/core/tweaks/` as root:wheel 755 and adds it to `enabledTweaks`.

Edit sheet writes `.options` and `.whitelist`. Empty whitelist is deleted on save. A `*` entry in whitelist / blacklist / `blacklistedApps` matches every process.

SIP and daemon status are probed off the UI thread. Install copies `com.ammonia.inject.plist` and bootstraps it (and removes the old Playground grant job).
