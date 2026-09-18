#ifndef VAJRA_LOADER_H
#define VAJRA_LOADER_H

#include <stdint.h>

/* ------------------------------------------------------------------
 * Roadmap Phase 16: the program loader. Reads a storage object
 * (core/storage.c) containing a genuinely separately-compiled program
 * (never linked into kernel.bin -- see src/userland/) and instantiates
 * it into a FRESH actor whose entry point is inside that loaded code,
 * not a kernel-linked function pointer the way every actor before this
 * milestone worked.
 *
 * Deliberately not ELF: a small custom header is all this kernel's
 * needs justify right now (magic + where execution starts + how much
 * to copy), the same reasoning boot.asm/link.ld already apply to
 * kernel.bin itself -- a real, documented format with its own loader,
 * just not more machinery than the problem requires yet.
 *
 * PROGRAM_VBASE/PROGRAM_WINDOW_MAX are shared between here,
 * hal/x86_64/paging.c (which maps this range per-actor), and
 * core/actor.c (which computes the real entry address from
 * entry_offset) -- one source of truth for the layout all three agree
 * on. Chosen at 256MB deliberately: exactly where boot.asm's identity
 * map (0-256MB, 128 huge pages) ends, so this range is unmapped in
 * every existing address space today -- adding a per-actor mapping
 * here cannot collide with or shadow anything already relied upon.
 * ---------------------------------------------------------------- */

#define PROGRAM_VBASE       0x10000000ULL /* 256MB */
#define PROGRAM_WINDOW_MAX  0x200000ULL   /* 2MB -- one page table's worth, same pattern as
                                              hal/x86_64/paging.c's existing 1MB stack window;
                                              revisit together if either needs to grow. */

#define PROGRAM_MAGIC 0x524A4156u /* bytes 'V','A','J','R' in that order, read little-endian */

/* Prepended to the raw compiled bytes by tools/build-c.ps1's program
 * build step -- not part of the linked binary itself (the linker has
 * no way to know its own final size to embed it), so the build script
 * writes this header immediately before the linker's own output. */
struct program_header {
    uint32_t magic;
    uint32_t entry_offset; /* from PROGRAM_VBASE */
    uint32_t code_size;    /* bytes following this header, to copy into the program window */
};

/* Reads storage object `object_id`, validates it as a program image,
 * and spawns a fresh actor running it -- the loaded code's own entry
 * point, not a kernel-linked function. Returns the new actor's slot on
 * success, -1 if the object isn't a valid program, doesn't fit in
 * PROGRAM_WINDOW_MAX, or the underlying actor_spawn_program() call
 * fails (out of slots/memory). Does NOT check capabilities itself --
 * see hal/x86_64/syscall.c's own SYS_SPAWN_PROGRAM case, matching
 * every other module in core/ having no idea actors or capabilities
 * exist (storage.c's own top comment). */
int loader_spawn_program(int object_id);

#endif
