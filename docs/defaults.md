# Configuration defaults

The Configurator saves preferences in a standard macOS XML Property List (plist) file. Configure the environment from the terminal or scripts without opening the GUI.

Configuration file: `/opt/pluginplayground/current.options`

## Available keys

### Boolean keys

- `disablePAC`: Disables arm64e PAC signing for spawned processes. Required when compiling without native arm64e ABI.
- `useLegacyAmmonia`: Uses the legacy tweak path (`/private/var/ammonia/core/tweaks/`) instead of `/opt/pluginplayground/tweaks/`.
- `pauseInjection`: Pauses tweak injection for all processes.

### Array keys

- `enabledTweaks`: Array of tweak filenames (e.g. `MyTweak.dylib`) that the user has enabled. Tweaks in the directory not listed here are skipped. Manage from the Configurator GUI or command line.

## Command-line usage

Since the config is a standard plist, modify it with `defaults` or `plutil`.

### Examples using `defaults`

To enable PAC stripping, run:

```bash
defaults write /opt/pluginplayground/current.options disablePAC -bool true
```

To turn on legacy Ammonia tweaks, run:

```bash
defaults write /opt/pluginplayground/current.options useLegacyAmmonia -bool true
```

To pause injection, run:

```bash
defaults write /opt/pluginplayground/current.options pauseInjection -bool true
```

To enable a specific tweak, run:

```bash
defaults write /opt/pluginplayground/current.options enabledTweaks -array "MyTweak.dylib"
```

To enable multiple tweaks, list them all in one array:

```bash
defaults write /opt/pluginplayground/current.options enabledTweaks -array "TweakA.dylib" "TweakB.dylib"
```

To add a tweak to the existing enabled list without overwriting it, use `-array-add`:

```bash
defaults write /opt/pluginplayground/current.options enabledTweaks -array-add "NewTweak.dylib"
```

To read the current configuration, run:

```bash
defaults read /opt/pluginplayground/current.options
```

> [!NOTE]
> The pkg postinstall creates `/opt/pluginplayground/current.options` with `chmod 666` if it is missing. The Configurator keeps that mode after each save. If you create the file yourself, use `sudo defaults write` the first time.
