Ammonia is a modern macOS extension/tweak loader, enabling modifications on Mac systems. SIP must be disabled. 

# Quick Install
- Turn off SIP
- Install PKG https://github.com/CoreBedtime/ammonia/releases/download/1.5/ammonia.pkg
- Enable arm64e ABI
  - sudo nvram boot-args=-arm64e_preview_abi
- Reboot.

# From source (Build a pkg)
`sh setup_frida.sh && sh compile.sh && sh package.sh`


# Developing Tweaks for Ammonia

Ammonia simply loads dynamic libraries at runtime. This allows developers to write and inject functionality directly into the runtime environment with minimal setup.

You must compile for both `arm64` & `arm64e` architectures.

## Example (using clang or Makefile):

clang -arch arm64 -arch arm64e -dynamiclib -o YourTweak.dylib YourTweak.m

## Entry Points

Ammonia will automatically invoke your entrypoint function when the tweak is loaded. You can use any of the following styles:

### 1. Objective-C +load

If you’re writing Objective-C code:
```objc
@implementation YourTweak
+ (void)load {
    NSLog(@"[!] YourTweak loaded!");
    // Your initialization logic here
}
@end
```
### 2. C Constructor

For plain C/C++ tweaks, mark a function with `__attribute__((constructor))`:
```c
__attribute__((constructor))
static void init_tweak(void) {
    printf("[!] Tweak constructor called!\n");
    // Initialization code here
}
```
### 3. Ammonia-Specific Entry Function

Ammonia can also call a function named `void LoadFunction(void *gum_interceptor);`. This is called directly after your tweak gets loaded into runtime.

This acts similarly to a main() for your tweak. Make sure this symbol is exported.

Example:
```c
void LoadFunction(void *gum_interceptor) {
    printf("[!] LoadFunction called with gum interceptor %p\n", gum_interceptor);
}
```

## Support Ammonia
[![Ko-fi](https://img.shields.io/badge/Ko--fi-F16061?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/corebedtime)
