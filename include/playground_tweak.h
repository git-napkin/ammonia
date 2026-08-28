#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional. playground_opener looks this up with dlsym after dlopen.
 * interceptor is the process GumInterceptor from gum_interceptor_obtain(),
 * or NULL if Frida-Gum failed to load. Tweaks may also run work from a
 * constructor; LoadFunction is for hooks that need the interceptor.
 */
void LoadFunction(void *interceptor);

#ifdef __cplusplus
}
#endif
