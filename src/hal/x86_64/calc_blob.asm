bits 64
section .rodata

; Wraps the VajraLang-compiled calculator program (src/userland/calc.vj
; -> tools/vajrac.ps1 -> build/calc_gen.c -> build/calc.bin) as inert
; data inside the main kernel binary -- the exact same incbin technique
; hello_blob.asm already uses, see its own comment. core/main.c copies
; these bytes into a storage object at boot; core/loader.c is what
; actually parses and loads them later, never this blob directly.
global calc_blob
global calc_blob_end

calc_blob:
    incbin "calc.bin"
calc_blob_end:
