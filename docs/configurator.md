# Configurator

A GUI installed to /Applications/Plugin Playground.app.

Settings toggles write `/opt/pluginplayground/current.options` immediately. The opener and fangs_hook reload that file live.

**Install tweak** copies a chosen `.dylib` into `/opt/pluginplayground/tweaks/` as root:wheel mode 755 and adds it to `enabledTweaks`.

The Edit sheet writes `Name.dylib.options` (`blacklistedApps`, `frameworkDependencies`) and `Name.dylib.whitelist`. An empty whitelist file is deleted on save so the tweak is not locked out of every process. Package copies the dylib plus any sibling `.whitelist`, `.blacklist`, `.options`, and `.png`.

SIP and daemon status are probed off the UI thread after launch. The daemon card shows Loaded when the grant job is bootstrapped. grant itself exits after a successful inject.

The Install button for the daemon copies `/opt/pluginplayground/share/com.pluginplayground.grant.plist` into `/Library/LaunchDaemons/` and bootstraps it.
