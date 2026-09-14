bits 64
section .rodata

; Wraps the separately-assembled, standalone AP trampoline
; (ap_trampoline.asm -> build/ap_trampoline.bin, a flat 16-bit blob
; meant to run from physical 0x70000, not from wherever it happens to
; land inside THIS kernel image) as inert data inside the main kernel
; binary, so hal/x86_64/smp.c can copy it to its real runtime home with
; an ordinary loop. incbin's path is resolved via nasm's -I search path
; (tools/build-c.ps1 passes the build directory), not relative to this
; file, since the .bin only exists after a build has already run.
global ap_trampoline_blob
global ap_trampoline_blob_end

ap_trampoline_blob:
    incbin "ap_trampoline.bin"
ap_trampoline_blob_end:
