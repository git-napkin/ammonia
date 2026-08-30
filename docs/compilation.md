# Compilation

`install.sh` plus CMake. Targets: `ammonia`, `libinject.dylib`, `libopener.dylib` (arm64e), `configurator` / Ammonia.app (arm64; SwiftUI in `gui/`).

Frida: `setup_frida.sh` (devkit 17.9.11) builds `libfrida-gum-arm64e-arm64.a` and `fridagum.dylib`. Opener `dlopen`s `fridagum.dylib` in the target app. Infect links the **Ammonia** Gum archive (`../legacy/ammonia/libfrida-gum-arm64e-arm64.a`) and header so `gum_interceptor_replace` stays `replacement_data` + `original`. Do not compile infect against the 17.9.11 header `setup_frida.sh` writes to `include/frida-gum.h`.

Tweaks:

```sh
clang -arch arm64e -arch arm64 -bundle -undefined dynamic_lookup -o MyTweak.dylib MyTweak.c
```

`LoadFunction(void *interceptor)` is in `/private/var/ammonia/core/include/playground_tweak.h`.
