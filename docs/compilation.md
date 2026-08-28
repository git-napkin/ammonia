# Compilation

The build process is managed by install.sh and CMake.

## install.sh

The primary build script. It builds each component and packages them into the final installer.

## CMakeLists.txt

The project build configuration. Specifies targets, fetches dependencies like Slint, and coordinates the compilation of grant, libfangs_hook.dylib, libplayground_opener.dylib, and the configurator.

## Nix (optional)

A declarative build environment using flake.nix. Provides a reproducible alternative to system tools. Run `nix build` or use `nix develop` to build the project. Optional.

It can also be wrapped into a nixpkgs package for `nix-darwin` and `home-manager` integration.

The Nix build produces `arm64` binaries, not `arm64e`. Toggle **Disable arm64e (PAC)** in the Configurator so injection works.

## Tweaks

Compile as an arm64 bundle:

```sh
clang -arch arm64 -bundle -undefined dynamic_lookup -o MyTweak.dylib MyTweak.c
```

See `testing/Makefile` for the flags used by the capability test. Optional `LoadFunction(void *interceptor)` is declared in `/opt/pluginplayground/include/playground_tweak.h`. playground_opener calls it after `dlopen` and passes the process GumInterceptor, or NULL if Frida-Gum did not load.

A sidecar `MyTweak.dylib.whitelist` with one executable name or path per line is an allow list. If that file exists, including when it is empty, only listed processes load the tweak. Configurator package copies those sidecars with the dylib.
