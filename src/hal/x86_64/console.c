#include "vajra/hal.h"
#include "vajra/spinlock.h"

/* ------------------------------------------------------------------
 * VGA text-mode console -- now a small WINDOWING compositor (roadmap
 * Phase 18, revised after the shell's own colored prompt shipped but
 * turned out invisible: 15 actors sharing one unsplit 80x25 screen
 * meant the scripted demo's own flood of output buried the shell's
 * prompt, even though the escape-code rendering underneath it worked
 * correctly the whole time -- confirmed only by actually looking at
 * what the screen showed, not by the code compiling or the syscalls
 * succeeding.
 *
 * Two fixed panes, not a general dynamic window manager -- still
 * genuinely a "desktop" in the sense that mattered here (more than one
 * thing visibly happening on screen at once), still genuinely NOT a
 * GUI (docs/PHILOSOPHY.md §5 -- text characters and CP437 box-drawing
 * glyphs only, no pixel graphics, no mouse, no resizing):
 *   - CONSOLE_WIN_LOG (top pane): where every actor's SYS_WRITE lands
 *     by default -- the entire pre-Phase-18 scripted demo (capability
 *     checks, the quarantine pipeline, networking) renders here,
 *     UNCHANGED, none of those actors know a window even exists.
 *   - CONSOLE_WIN_SHELL (bottom pane): where ONLY the shell actor's
 *     output lands (core/actor.c's per-actor `window` field, set once
 *     via actor_set_window() in kernel_main) -- a small, stable region
 *     the demo's own flood can never touch.
 * Each pane scrolls independently within its own interior rows; a
 * write to one pane can never disturb the other's content. Assigning
 * an actor to a pane is a KERNEL decision (actor_set_window(), same
 * kernel-only convention as actor_set_spawn_quota()), not something
 * actor code can choose for itself -- consistent with every other
 * kernel-mediated resource in this codebase.
 *
 * The scroll behavior here is deliberately correct from the start --
 * the original assembly kernel (V0.29) shipped with a bug where
 * reaching the bottom of the screen wrapped the cursor back to the
 * top WITHOUT clearing or shifting anything, so old and new text
 * overlapped into garbage. The fix there (shift rows up, blank the
 * last row) is applied here from day one instead of being
 * rediscovered -- now scoped to one window's own interior rows rather
 * than the whole screen.
 *
 * Preemption-safe only against a SINGLE core (see Milestone 12's own
 * note, still accurate: the AP doesn't run actors yet). Window
 * selection (hal_console_set_window(), called by the SYS_WRITE syscall
 * handler right before hal_console_write()) and the write itself both
 * happen inside one interrupt-gate-protected syscall with IF already
 * hardware-cleared on entry, so the two steps can't be torn by a timer
 * tick landing in between on this core.
 *
 * Also mirrors every character to COM1 (port 0x3F8) -- not a debug
 * leftover, a permanent second output, unaffected by windowing (the
 * serial log still reads as one linear transcript, panes and all,
 * exactly as before). See git history for the full original
 * rationale: every milestone's headless QEMU verification and CI both
 * depend on this mirror, discovered the hard way once already
 * (Milestone 14) when it was accidentally skipped.
 * ---------------------------------------------------------------- */

#define VGA_BASE       ((volatile uint16_t *)0xB8000)
#define VGA_COLS       80
#define VGA_ROWS       25
#define VGA_COLOR       0x0A   /* green on black, matching the shell's look */
#define COM1_PORT      0x3F8

static hal_spinlock_t console_lock;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t vga_entry(uint16_t c, uint8_t color) {
    return c | ((uint16_t)color << 8);
}

static inline void vga_put(int row, int col, uint16_t entry) {
    VGA_BASE[row * VGA_COLS + col] = entry;
}

/* ------------------------------------------------------------------
 * Fixed screen layout. Chosen to use the full 25 rows exactly:
 *   row 0        title bar
 *   rows 1-15    CONSOLE_WIN_LOG box (border + 13-row interior)
 *   row 16       spacer
 *   rows 17-24   CONSOLE_WIN_SHELL box (border + 6-row interior)
 * Interior columns are always 1..78 (col 0 and 79 are the side
 * borders) -- 78 columns wide, comfortably inside VGA_COLS. */
struct console_window {
    int x, y, w, h; /* interior region: (x,y) top-left, w x h characters */
    int cx, cy;     /* local cursor, relative to (x,y) */
};

#define WIN_COUNT CONSOLE_WIN_COUNT
static struct console_window windows[WIN_COUNT] = {
    [CONSOLE_WIN_LOG]   = { 1, 2,  78, 13, 0, 0 },
    [CONSOLE_WIN_SHELL] = { 1, 18, 78, 6,  0, 0 },
};
static int current_window = CONSOLE_WIN_LOG;

/* ------------------------------------------------------------------
 * Escape-sequence state -- see console.h-equivalent comment history;
 * unchanged in kind from the single-screen version, just now applying
 * to whichever window is current rather than one global cursor. */
typedef enum { ESC_NONE, ESC_GOT_ESC, ESC_IN_SEQ } esc_state_t;
static esc_state_t esc_state = ESC_NONE;
static int esc_params[4];
static int esc_param_count = 0;
static int esc_cur_param = 0;
static uint8_t cur_color = VGA_COLOR;

/* ANSI SGR color index (0-7: black,red,green,yellow,blue,magenta,
 * cyan,white) -> VGA's own 4-bit palette index (0-7:
 * black,blue,green,cyan,red,magenta,brown,white) -- the two orderings
 * genuinely differ (red/blue and yellow/cyan are swapped), so a plain
 * `code - 30` would produce the wrong color, not just a different
 * palette convention. */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void win_clear_and_home(struct console_window *w) {
    for (int r = 0; r < w->h; r++) {
        for (int c = 0; c < w->w; c++) {
            vga_put(w->y + r, w->x + c, vga_entry(' ', cur_color));
        }
    }
    w->cx = 0;
    w->cy = 0;
}

static void apply_sgr(void) {
    if (esc_param_count == 0) {
        cur_color = VGA_COLOR;
        return;
    }
    for (int i = 0; i < esc_param_count; i++) {
        int n = esc_params[i];
        if (n == 0) {
            cur_color = VGA_COLOR;
        } else if (n >= 30 && n <= 37) {
            cur_color = (uint8_t)((cur_color & 0xF0) | ansi_to_vga[n - 30]);
        } else if (n >= 40 && n <= 47) {
            cur_color = (uint8_t)((cur_color & 0x0F) | (uint8_t)(ansi_to_vga[n - 40] << 4));
        }
    }
}

static void win_scroll(struct console_window *w) {
    for (int r = 1; r < w->h; r++) {
        for (int c = 0; c < w->w; c++) {
            vga_put(w->y + r - 1, w->x + c, VGA_BASE[(w->y + r) * VGA_COLS + (w->x + c)]);
        }
    }
    for (int c = 0; c < w->w; c++) {
        vga_put(w->y + w->h - 1, w->x + c, vga_entry(' ', VGA_COLOR));
    }
    w->cy = w->h - 1;
}

/* Draws one window's border (box-drawing glyphs, CP437 -- plain bytes
 * >0x7F that VGA text mode renders directly, no translation needed)
 * plus an optional title centered in the top border. Called once, at
 * init, straight to VGA_BASE -- not through the windowed-cursor path
 * above, since this is boot-time screen decoration, not actor output. */
static void draw_box(int top, int bottom, int left, int right, const char *title) {
    for (int c = left + 1; c < right; c++) {
        vga_put(top, c, vga_entry((uint16_t)0xC4, VGA_COLOR));    /* ─ */
        vga_put(bottom, c, vga_entry((uint16_t)0xC4, VGA_COLOR)); /* ─ */
    }
    for (int r = top + 1; r < bottom; r++) {
        vga_put(r, left, vga_entry((uint16_t)0xB3, VGA_COLOR));  /* │ */
        vga_put(r, right, vga_entry((uint16_t)0xB3, VGA_COLOR)); /* │ */
    }
    vga_put(top, left, vga_entry((uint16_t)0xDA, VGA_COLOR));     /* ┌ */
    vga_put(top, right, vga_entry((uint16_t)0xBF, VGA_COLOR));    /* ┐ */
    vga_put(bottom, left, vga_entry((uint16_t)0xC0, VGA_COLOR));  /* └ */
    vga_put(bottom, right, vga_entry((uint16_t)0xD9, VGA_COLOR)); /* ┘ */

    int len = 0;
    while (title[len]) { len++; }
    int start = left + 2;
    for (int i = 0; i < len && start + i < right - 1; i++) {
        vga_put(top, start + i, vga_entry((uint16_t)title[i], VGA_COLOR));
    }
}

void hal_console_init(void) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_BASE[i] = vga_entry(' ', VGA_COLOR);
    }

    const char *banner = "VAJRA OS -- type in the Shell pane below";
    int i = 0;
    while (banner[i] && i < VGA_COLS) {
        vga_put(0, i, vga_entry((uint16_t)banner[i], VGA_COLOR));
        i++;
    }

    draw_box(1, 15, 0, 79, " System Log ");
    draw_box(17, 24, 0, 79, " Shell ");

    for (int w = 0; w < WIN_COUNT; w++) {
        windows[w].cx = 0;
        windows[w].cy = 0;
    }
    current_window = CONSOLE_WIN_LOG;
    cur_color = VGA_COLOR;
    esc_state = ESC_NONE;
}

/* Roadmap Phase 18 (revised): selects which window subsequent
 * hal_console_write()/hal_console_putchar() calls target. The SYS_WRITE
 * syscall handler (hal/x86_64/syscall.c) calls this once, right before
 * writing, based on the CALLING ACTOR's own window assignment
 * (core/actor.c's actor_current_window()) -- actor code itself never
 * picks a window; it just writes, the same as it always has. Kernel-
 * context callers (kernel_main's own boot messages, before the
 * scheduler exists) never call this at all, so they land wherever
 * current_window already defaults to (CONSOLE_WIN_LOG) -- the boot
 * trace and the scripted demo end up in the same pane, which is
 * exactly right: both are "system log" output, not interactive shell
 * output. */
void hal_console_set_window(int win) {
    if (win < 0 || win >= WIN_COUNT) {
        return;
    }
    current_window = win;
}

void hal_console_putchar(char c) {
    hal_spin_lock(&console_lock);

    outb(COM1_PORT, (uint8_t)c);

    struct console_window *w = &windows[current_window];

    /* Escape-sequence state machine -- see this file's own comment
     * above. Every branch here returns early (still under the lock,
     * released once at the bottom of each branch) rather than falling
     * through to the ordinary character path below. */
    if (esc_state == ESC_NONE && c == 0x1B) {
        esc_state = ESC_GOT_ESC;
        hal_spin_unlock(&console_lock);
        return;
    }
    if (esc_state == ESC_GOT_ESC) {
        if (c == '[') {
            esc_state = ESC_IN_SEQ;
            esc_param_count = 0;
            esc_cur_param = 0;
        } else {
            esc_state = ESC_NONE; /* not a CSI sequence -- drop the lone ESC silently */
        }
        hal_spin_unlock(&console_lock);
        return;
    }
    if (esc_state == ESC_IN_SEQ) {
        if (c >= '0' && c <= '9') {
            esc_cur_param = esc_cur_param * 10 + (c - '0');
        } else if (c == ';') {
            if (esc_param_count < 4) {
                esc_params[esc_param_count++] = esc_cur_param;
            }
            esc_cur_param = 0;
        } else {
            /* Final byte -- terminates the sequence regardless of
             * whether it's one this parser recognizes. */
            if (esc_param_count < 4) {
                esc_params[esc_param_count++] = esc_cur_param;
            }
            if (c == 'J') {
                win_clear_and_home(w);
            } else if (c == 'H') {
                /* Window-relative, 1-based, same convention ANSI's own
                 * ESC[row;colH uses for the whole screen. */
                int row = (esc_param_count >= 1 && esc_params[0] > 0) ? esc_params[0] - 1 : 0;
                int col = (esc_param_count >= 2 && esc_params[1] > 0) ? esc_params[1] - 1 : 0;
                if (row >= w->h) { row = w->h - 1; }
                if (col >= w->w) { col = w->w - 1; }
                w->cy = row;
                w->cx = col;
            } else if (c == 'm') {
                apply_sgr();
            }
            /* Any other final byte: recognized as "end of sequence",
             * just not one this parser acts on -- ignored, not an
             * error, so an unsupported escape never corrupts plain
             * text that happens to follow it. */
            esc_state = ESC_NONE;
        }
        hal_spin_unlock(&console_lock);
        return;
    }

    if (c == '\n') {
        w->cx = 0;
        w->cy++;
    } else if (c == '\b') {
        /* Cursor-back only, no erase -- matches how every existing
         * demo actor already expects to use it (print "\b \b" to
         * actually erase a character: back, blank, back again). */
        if (w->cx > 0) {
            w->cx--;
        } else if (w->cy > 0) {
            w->cy--;
            w->cx = w->w - 1;
        }
    } else {
        vga_put(w->y + w->cy, w->x + w->cx, vga_entry((uint16_t)(uint8_t)c, cur_color));
        w->cx++;
        if (w->cx >= w->w) {
            w->cx = 0;
            w->cy++;
        }
    }

    if (w->cy >= w->h) {
        win_scroll(w);
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
