bits 64
section .rodata

; Wraps the separately-compiled, standalone "hello world" program
; (src/userland/hello.c + runtime.c -> build/hello.bin, header-prefixed
; flat binary meant to run from PROGRAM_VBASE, not from wherever it
; happens to land inside THIS kernel image) as inert data inside the
; main kernel binary -- the exact same technique
; ap_trampoline_blob.asm already uses for the AP trampoline, see its
; own comment. core/main.c copies these bytes into a storage object at
; boot; core/loader.c is what actually parses and loads them later,
; never this blob directly.
global hello_blob
global hello_blob_end

hello_blob:
    incbin "hello.bin"
hello_blob_end:
