#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * PS/2 keyboard driver -- roadmap Phase 18, the kernel's first INPUT
 * device; extended in the "usable for real work" pass to the FULL
 * keyboard.
 *
 * Interrupt-driven (IRQ1 -> vector 33): hal_keyboard_irq_handler() reads
 * the scancode off port 0x60 and decodes it into a ring buffer;
 * hal_keyboard_poll() (the SYS_KEY_READ backing call) drains it
 * NON-blockingly, returning -1 if nothing has arrived.
 *
 * Scancode Set 1, US QWERTY. Decoded:
 *   - printable keys, with Shift and Caps Lock (Caps affects letters only)
 *   - Ctrl+letter -> control codes 1..26 (Ctrl+A = 1 ...)
 *   - Alt+key -> KEY_ALT | character
 *   - the extended (0xE0-prefixed) block: arrows, Home/End, PgUp/PgDn,
 *     Insert/Delete, keypad Enter and '/'  -> KEY_* codes (>= 0x100)
 *   - F9-F12 -> KEY_F9.. (F1-F8 are the desktop's own window switch keys
 *     and never reach an app, so no app can steal or fake a switch)
 * A key-release scancode only matters for the modifiers.
 *
 * A second key source feeds the same buffer: the serial port (COM1). Its
 * bytes are injected as keystrokes by hal_keyboard_poll_serial(), so text
 * can be PASTED into Vajra, and a whole session can be scripted from a
 * host, over the serial line -- the same focused-window rule applies.
 * ---------------------------------------------------------------- */

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_BUF_SIZE    256
#define COM1_DATA       0x3F8
#define COM1_LSR        0x3FD

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static volatile uint16_t kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;
static int shift_held = 0;
static int ctrl_held = 0;
static int alt_held = 0;
static int caps_on = 0;
static int e0_pending = 0;

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

#define SC_LCTRL  0x1D
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LALT   0x38
#define SC_CAPS   0x3A

static void kbd_push(uint16_t code) {
    hal_console_scroll(0, 1); /* typing returns the view to the live screen */
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) { /* a full buffer drops the keystroke rather than corrupt it */
        kbd_buf[kbd_head] = code;
        kbd_head = next;
    }
}

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void hal_keyboard_irq_handler(void) {
    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == 0xE0) {
        e0_pending = 1;
        return;
    }
    int ext = e0_pending;
    e0_pending = 0;
    int release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    /* modifiers (press and release) */
    if (code == SC_LSHIFT || code == SC_RSHIFT) { shift_held = !release; return; }
    if (code == SC_LCTRL)                       { ctrl_held = !release;  return; } /* left or (ext) right */
    if (code == SC_LALT)                        { alt_held = !release;   return; }
    if (release) {
        return;
    }
    if (code == SC_CAPS && !ext) {
        caps_on = !caps_on;
        return;
    }

    if (ext) {
        switch (code) {
            case 0x48: kbd_push(KEY_UP);     return;
            case 0x50: kbd_push(KEY_DOWN);   return;
            case 0x4B: kbd_push(KEY_LEFT);   return;
            case 0x4D: kbd_push(KEY_RIGHT);  return;
            case 0x47: kbd_push(KEY_HOME);   return;
            case 0x4F: kbd_push(KEY_END);    return;
            case 0x49: if (shift_held) { hal_console_scroll(10, 0); return; } hal_console_scroll(0, 1); kbd_push(KEY_PGUP); return;
            case 0x51: if (shift_held) { hal_console_scroll(-10, 0); return; } hal_console_scroll(0, 1); kbd_push(KEY_PGDN); return;
            case 0x52: kbd_push(KEY_INSERT); return;
            case 0x53: kbd_push(KEY_DELETE); return;
            case 0x1C: kbd_push('\n');       return; /* keypad Enter */
            case 0x35: kbd_push('/');        return; /* keypad / */
            default:   return;
        }
    }

    /* F1-F8: the desktop's window-switch keys -- handled here, never buffered. */
    if (code >= 0x3B && code <= 0x42) {
        hal_console_focus_app(code == 0x42 ? -1 : (int)(code - 0x3B));
        return;
    }
    if (code == 0x43) { kbd_push(KEY_F9);  return; }
    if (code == 0x44) { kbd_push(KEY_F10); return; }
    if (code == 0x57) { kbd_push(KEY_F11); return; }
    if (code == 0x58) { kbd_push(KEY_F12); return; }

    if (code >= sizeof(scancode_lower)) {
        return; /* outside the mapped table -- an unhandled key, not an error */
    }

    char c;
    if (is_letter(scancode_lower[code])) {
        c = (shift_held != caps_on) ? scancode_upper[code] : scancode_lower[code];
    } else {
        c = shift_held ? scancode_upper[code] : scancode_lower[code];
    }
    if (c == 0) {
        return;
    }
    if (ctrl_held && is_letter(c)) {
        kbd_push((uint16_t)((c | 0x20) - 'a' + 1)); /* Ctrl+A = 1 ... Ctrl+Z = 26 */
        return;
    }
    if (alt_held) {
        kbd_push((uint16_t)(KEY_ALT | (uint8_t)c));
        return;
    }
    kbd_push((uint16_t)(uint8_t)c);
}

/* Feeds bytes waiting on the serial line into the keyboard buffer, as if
 * typed. Called from the boot core's timer tick. CR becomes newline and DEL
 * becomes backspace, the usual terminal conventions. At most a handful of
 * bytes per call so a flood can never hold the kernel lock long. */
void hal_keyboard_poll_serial(void) {
    for (int n = 0; n < 32; n++) {
        if (!(inb(COM1_LSR) & 0x01)) {
            return;
        }
        int next = (kbd_head + 1) % KBD_BUF_SIZE;
        if (next == kbd_tail) {
            return; /* buffer full: leave the byte in the UART for the next tick */
        }
        uint8_t b = inb(COM1_DATA);
        if (b == '\r') { b = '\n'; }
        if (b == 0x7F) { b = '\b'; }
        kbd_push((uint16_t)b);
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
    uint16_t c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return (int)c;
}
