# current.options

File: `/private/var/ammonia/core/current.options`

- `disablePAC` (bool): unused while infect is the launchd payload. Prefer native arm64e.
- `pauseInjection` (bool): opener skips loading tweaks.
- `enabledTweaks` (array of filenames)

```bash
defaults write /private/var/ammonia/core/current.options enabledTweaks -array-add "MyTweak.dylib"
defaults read /private/var/ammonia/core/current.options
```

Postinstall creates the file with mode 666 if missing.
