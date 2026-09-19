#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * PS/2 keyboard driver -- roadmap Phase 18, the kernel's first INPUT
 * device. Everything before this milestone was output-only (console)
 * or block storage; hal_interrupts_init()'s own top comment already
 * noted this gap.
 *
 * Interrupt-driven (IRQ1 -> vector 33, see interrupts.c/pic.c), not
 * polled the way ata.c's disk driver is -- a keystroke can arrive at
 * any time relative to whatever the scheduler is doing, unlike a disk
 * read the kernel itself explicitly initiated and is willing to block
 * on. hal_keyboard_irq_handler() (called from exception_handler) reads
 * the scancode off port 0x60 and decodes it into a small ring buffer;
 * hal_keyboard_poll() (the SYS_KEY_READ backing call) drains it
 * NON-blockingly -- returns -1 immediately if nothing has arrived yet,
 * the same "bounded poll, caller yields between attempts" pattern
 * core/net.c's SYS_NET_RECEIVE already established, rather than adding
 * a brand new "block this actor until a key arrives" scheduler
 * primitive for one device.
 *
 * Scancode Set 1 (QEMU's PS/2 default), US QWERTY layout only. Shift
 * is tracked (both physical shift keys); Ctrl/Alt/Caps Lock/function
 * keys/arrows are deliberately not decoded -- unmapped scancodes
 * produce no character at all rather than a wrong one. A release
 * scancode (bit 7 set) is ignored except for the two shift keys, whose
 * releases clear shift state.
 * ---------------------------------------------------------------- */

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_BUF_SIZE    32

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;
static int shift_held = 0;

/* Index = scancode (0x00-0x39 covers everything mapped here).
 * 0 means "no printable character" (unmapped, or a modifier key). */
static const char scancode_lower[0x3A] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',  /* 0x00-0x09 */
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',  /* 0x0A-0x13 */
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   /* 0x14-0x1D (0x1D = LCTRL) */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  /* 0x1E-0x27 */
    '\'','`', 0,  '\\','z', 'x', 'c', 'v', 'b', 'n',   /* 0x28-0x31 (0x2A = LSHIFT) */
    'm', ',', '.', '/', 0,  '*', 0,   ' '               /* 0x32-0x39 (0x36 = RSHIFT) */
};

static const char scancode_upper[0x3A] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?', 0,  '*', 0,   ' '
};

#define SCANCODE_LSHIFT 0x2A
#define SCANCODE_RSHIFT 0x36

void hal_keyboard_irq_handler(void) {
    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == SCANCODE_LSHIFT || sc == SCANCODE_RSHIFT) {
        shift_held = 1;
        return;
    }
    if (sc == (SCANCODE_LSHIFT | 0x80) || sc == (SCANCODE_RSHIFT | 0x80)) {
        shift_held = 0;
        return;
    }
    if (sc & 0x80) {
        return; /* release of an ordinary key -- nothing to do */
    }
    if (sc >= sizeof(scancode_lower)) {
        return; /* outside the mapped table -- an unhandled key, not an error */
    }

    char c = shift_held ? scancode_upper[sc] : scancode_lower[sc];
    if (c == 0) {
        return;
    }

    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) { /* drop the keystroke on a full buffer rather than corrupt it --
                                same bounded-backpressure choice actor mailboxes already made */
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
}

void hal_keyboard_init(void) {
    /* Drain anything already pending in the PS/2 controller's output
     * buffer (e.g. a stray byte from BIOS/firmware handoff) so the
     * first real keystroke isn't preceded by garbage. */
    while (inb(KBD_STATUS_PORT) & 0x01) {
        inb(KBD_DATA_PORT);
    }
}

int hal_keyboard_poll(void) {
    if (kbd_head == kbd_tail) {
        return -1;
    }
    char c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (int)(unsigned char)c;
}
