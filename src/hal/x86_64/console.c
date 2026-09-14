#include "vajra/hal.h"
#include "vajra/spinlock.h"

/* ------------------------------------------------------------------
 * VGA text-mode console.
 *
 * The scroll behavior here is deliberately correct from the start --
 * the original assembly kernel (V0.29) shipped with a bug where
 * reaching the bottom of the screen wrapped the cursor back to the
 * top WITHOUT clearing or shifting anything, so old and new text
 * overlapped into garbage. The fix there (shift rows up, blank the
 * last row) is applied here from day one instead of being
 * rediscovered.
 *
 * Preemption-safe only against a SINGLE core until Milestone 12: every
 * interrupt/syscall gate is an x86 "interrupt gate" (hal/x86_64/
 * interrupts.c's idt_set_gate), which hardware-clears IF on entry, so
 * on one core a console write already could not be interrupted by
 * another console write -- there was only ever one instruction pointer
 * in the whole machine. That guarantee says nothing about a SECOND
 * physical core, which Milestone 12 (roadmap Phase 10) introduces for
 * the first time: hal/x86_64/smp.c's AP writes to this exact console
 * from genuinely, simultaneously running code, and cursor_x/cursor_y
 * below is ordinary unsynchronized shared mutable state -- a classic
 * lost-update race, not just cosmetically interleaved characters.
 * Confirmed by deliberately booting without the lock below before
 * adding it: the AP's own status line and the BSP's actor trace came
 * back with characters from both interleaved mid-string (e.g.
 * "[[IAP core ntruder] attempt..."), not merely two adjacent, intact
 * lines -- proof it's a real per-character race on shared state, not
 * an artifact of log capture. hal_console_putchar() is deliberately
 * where the lock lives, not hal_console_write() (which only some
 * callers go through -- hal_console_write_hex64()/_dec64() each loop
 * calling hal_console_putchar() directly): the shared resource that
 * actually needs protecting is cursor_x/cursor_y and the VGA buffer,
 * not any one caller's idea of "one whole message". A multi-character
 * message from two cores can still interleave at the character level
 * as a result -- a cosmetic version of the exact same trade-off
 * message-passing mailboxes already made once before (bounded,
 * rejected-not-corrupted) -- but the cursor state itself can never
 * again be torn. This finally closes Milestone 4's own long-open
 * follow-up ("console output has no locking"), which stayed harmless
 * right up until a second physical core made it a genuine race.
 * ---------------------------------------------------------------- */

#define VGA_BASE       ((volatile uint16_t *)0xB8000)
#define VGA_COLS       80
#define VGA_ROWS       25
#define VGA_COLOR       0x0A   /* green on black, matching the shell's look */

static int cursor_x = 0;
static int cursor_y = 0;
static hal_spinlock_t console_lock;

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void vga_scroll(void) {
    /* Shift rows 1..24 up into rows 0..23, blank the last row. */
    for (int row = 1; row < VGA_ROWS; row++) {
        for (int col = 0; col < VGA_COLS; col++) {
            VGA_BASE[(row - 1) * VGA_COLS + col] = VGA_BASE[row * VGA_COLS + col];
        }
    }
    for (int col = 0; col < VGA_COLS; col++) {
        VGA_BASE[(VGA_ROWS - 1) * VGA_COLS + col] = vga_entry(' ', VGA_COLOR);
    }
    cursor_y = VGA_ROWS - 1;
}

void hal_console_init(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_BASE[i] = vga_entry(' ', VGA_COLOR);
    }
    cursor_x = 0;
    cursor_y = 0;
}

void hal_console_putchar(char c) {
    hal_spin_lock(&console_lock);

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else {
        VGA_BASE[cursor_y * VGA_COLS + cursor_x] = vga_entry(c, VGA_COLOR);
        cursor_x++;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= VGA_ROWS) {
        vga_scroll();
    }

    hal_spin_unlock(&console_lock);
}

void hal_console_write(const char *str) {
    while (*str) {
        hal_console_putchar(*str);
        str++;
    }
}

void hal_console_write_hex64(uint64_t value) {
    hal_console_write("0x");
    char buf[16];
    for (int i = 15; i >= 0; i--) {
        int nibble = (int)(value & 0xF);
        buf[i] = (nibble < 10) ? (char)('0' + nibble) : (char)('A' + (nibble - 10));
        value >>= 4;
    }
    for (int i = 0; i < 16; i++) {
        hal_console_putchar(buf[i]);
    }
}

void hal_console_write_dec64(uint64_t value) {
    if (value == 0) {
        hal_console_putchar('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (value > 0) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (i > 0) {
        hal_console_putchar(buf[--i]);
    }
}
