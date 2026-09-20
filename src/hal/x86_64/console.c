#include "vajra/hal.h"
#include "vajra/spinlock.h"
#include "vajra/memory.h"
#include "vajra/storage.h"

/* ------------------------------------------------------------------
 * VGA text-mode console -- a real desktop compositor now (the user's
 * own words: "a windows or ubuntu like desktop feel... a separate
 * apps finder, desktop, icons, symbols, buttons, everything"), not
 * the two fixed always-visible panes Milestone 18 shipped. Still
 * genuinely text-mode only (docs/PHILOSOPHY.md Sec.5 -- CP437
 * glyphs and 16-color cell attributes, no pixel graphics), the same
 * "Norton Commander, not a GUI toolkit" precedent docs/
 * DESKTOP_DESIGN.md already set.
 *
 * Layout (80x25):
 *   row 0       title bar -- the FOCUSED app's title + a [X] close
 *               button, or "Vajra Desktop" when nothing is focused.
 *   rows 1-23   content: either the desktop (background + icons) or
 *               whichever app window is currently focused, filling
 *               the whole area (apps are always "maximized" -- no
 *               drag/resize/overlap in this pass, see the design
 *               doc's own later stages for that).
 *   row 24      taskbar: an [ Apps ] launcher button, one tab per
 *               app (click to switch focus, current one highlighted),
 *               and a live clock from the CMOS RTC.
 * Clicking [ Apps ] pops a start-menu-style list above the taskbar;
 * picking an item (or clicking its desktop icon, or its taskbar tab)
 * focuses that app. Every app KEEPS RUNNING in the background while
 * unfocused -- its own offscreen buffer (see struct app_window)
 * retains whatever it wrote, so switching back shows it unchanged,
 * exactly like a real desktop's window list, not a single shared
 * scrolling log.
 *
 * The four apps are still exactly hal.h's old CONSOLE_WIN_LOG/SHELL,
 * plus two new ones: CONSOLE_WIN_FILES (a live storage listing,
 * regenerated from core/storage.c every time it's focused -- the
 * same "id is public knowledge, no separate icon state to keep in
 * sync" reasoning docs/DESKTOP_DESIGN.md's own Sec.3 already argued
 * for) and CONSOLE_WIN_ABOUT (static, written once by kernel_main).
 * Which fixed app lives at which slot, and its title/glyph, is still
 * a hardcoded roster here rather than a real registration API actors
 * can extend -- a deliberate simplification for this pass, not a
 * permanent design; a dynamic SYS_WINDOW_CREATE is docs/
 * DESKTOP_DESIGN.md's own later stage.
 *
 * Two-phase init, and this is NOT optional: hal_console_init() also
 * doubles as the kernel panic screen's reset (exception_handler,
 * interrupts.c) and must keep working before memory_init() has ever
 * run (kernel_main calls it that early too, for its own first boot
 * messages) or even if the allocator itself is what's broken. So
 * hal_console_init() allocates NOTHING -- every app_window starts
 * `in_use = 0`, and hal_console_putchar() falls back to a plain,
 * always-available full-screen scrolling raw mode (the pre-Milestone-
 * 18 console's own behavior) whenever the CURRENT window isn't
 * allocated yet. hal_console_alloc_windows() -- called once from
 * kernel_main, strictly after memory_init() -- is what actually gets
 * each window its own offscreen buffer (via alloc_dma_pages(), the
 * SAME "commons" allocator hal/x86_64/virtio_net.c's DMA buffers
 * already use) and switches the screen over to the real desktop.
 * Buffers deliberately do NOT live in .bss: four 80x23-cell buffers
 * would be ~14KB, and this project's own fixed low-memory layout
 * (see start.asm's own changelog -- five separate collisions with a
 * boot-time structure, so far) has no room to spare for that as a
 * static array. Pulling them from the general physical pool instead
 * (127MB detected, Milestone 12) sidesteps that whole bug class.
 *
 * Still mirrors every character to COM1 -- unaffected by any of this,
 * still the one thing every headless verification in this project's
 * history has depended on (see git history / Milestone 14).
 * ---------------------------------------------------------------- */

#define VGA_BASE       ((volatile uint16_t *)0xB8000)
#define VGA_COLS       80
#define VGA_ROWS       25
#define VGA_COLOR       0x0A   /* green on black -- default app text color */
#define COM1_PORT      0x3F8

#define CONTENT_TOP 1
#define CONTENT_H   23   /* screen rows 1..23 */
#define CONTENT_W   80
#define TASKBAR_ROW 24

static hal_spinlock_t console_lock;
static hal_spinlock_t window_lock;  /* held across a whole begin_window()..end_window() run */
static int saved_window_for_run = CONSOLE_WIN_LOG;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t vga_entry(uint16_t c, uint8_t color) {
    return c | ((uint16_t)color << 8);
}

/* A copy of what is currently ON the screen, kept in ordinary RAM, so a
 * redraw only stores the cells that actually changed. Every store to VGA
 * memory is an MMIO access -- cheap on real hardware, but under QEMU each
 * one is a trap into the device model, and a full redraw is 2000 of them.
 * Since every console write happens inside a syscall holding the kernel
 * lock, redrawing the whole screen per write made the lock look ~95% held
 * (measured by the Cores app's lock gauge) and starved the other cores.
 * With the shadow, a redraw after typing a few characters writes a few
 * cells. `vga_shadow_valid` stays 0 until hal_console_init() has cleared
 * the screen and set the shadow to match. */
static uint16_t vga_shadow[VGA_COLS * VGA_ROWS];
static int vga_shadow_valid;

static inline void vga_put(int row, int col, uint16_t entry) {
    int i = row * VGA_COLS + col;
    if (vga_shadow_valid && vga_shadow[i] == entry) {
        return;
    }
    vga_shadow[i] = entry;
    VGA_BASE[i] = entry;
}

/* ------------------------------------------------------------------
 * Per-app offscreen content buffer. NOT VGA_BASE -- actor writes
 * (hal_console_putchar) land here; a separate compositor pass
 * (hal_console_redraw) decides whether/where this is visible. */
enum { ESC_NONE, ESC_GOT_ESC, ESC_IN_SEQ };

struct app_window {
    int in_use;
    uint16_t *buf; /* CONTENT_W * CONTENT_H cells, from alloc_dma_pages() */
    int cx, cy;    /* local write cursor within buf */
    /* ANSI escape parser state, PER WINDOW: an app may send one sequence
     * across several writes (a number formatted in a separate call), and
     * another window's write landing in between used to be swallowed by
     * -- or to corrupt -- that half-parsed sequence (seen as stray
     * "6;1H" text in the Cores app). */
    int esc_state;
    int esc_params[4];
    int esc_param_count;
    int esc_cur_param;
};

#define WIN_COUNT CONSOLE_WIN_COUNT
static struct app_window windows[WIN_COUNT];
static int current_window = CONSOLE_WIN_LOG; /* which window hal_console_putchar() targets --
                                                  set by the SYS_WRITE syscall handler, same as
                                                  Milestone 18's own convention. */

/* The fixed app roster -- see this file's own top comment on why this
 * is hardcoded for now rather than a registration API. Index order
 * matches hal.h's CONSOLE_WIN_* constants exactly. */
static const char *const app_title[WIN_COUNT]     = { "System Log", "Shell", "Files", "About", "Security", "Fabric", "Cores" };
static const char *const app_tab_label[WIN_COUNT] = { "Log",        "Shell", "Files", "About", "Secure", "Fabric", "Cores" };
static const uint8_t app_body_color[WIN_COUNT]    = { 3 /*cyan*/, 2 /*green*/, 6 /*brown*/, 1 /*blue*/, 4 /*red*/, 0 /*black*/, 7 /*grey*/ };

/* ------------------------------------------------------------------
 * Desktop state -- which app (if any) is focused/maximized, whether
 * the start menu is open, and the mouse cursor's last known position.
 * Mutated only from hal_console_mouse_update() (the SYS_MOUSE_READ
 * syscall handler's own call, docs/DESKTOP_DESIGN.md Stage 1's mouse
 * driver) and read by hal_console_redraw(); both run with interrupts
 * already hardware-disabled for the whole syscall (interrupt gate,
 * not trap gate -- see interrupts.c's own note), so no separate lock
 * is needed for these plain ints the way console_lock guards the
 * actual screen/buffer writes below. */
static int focused_window = -1; /* -1 = desktop (background + icons) visible */
static int menu_open = 0;
static int cursor_col = -1, cursor_row = -1; /* -1: mouse hasn't moved yet, draw nothing */
static int prev_buttons = 0;

/* Bootstrap fallback cursor -- see this file's own top comment on the
 * two-phase init. Only used while the CURRENT window isn't allocated
 * yet (or after a panic resets everything back to this mode). */
static int raw_row = 0, raw_col = 0;

/* ------------------------------------------------------------------
 * Escape-sequence state -- unchanged in kind from Milestone 18's own
 * version, just now applying to an app's offscreen buffer instead of
 * VGA_BASE directly. */
static uint8_t cur_color = VGA_COLOR;

static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* ------------------------------------------------------------------
 * Desktop chrome hit-rects -- kept as named constants and reused by
 * BOTH the drawing functions and handle_click() below, so the two
 * can never silently drift apart (a click rectangle that doesn't
 * match what's actually drawn is worse than no hit-testing at all). */
#define APPS_BTN_COL0     0
#define APPS_BTN_COL1     7   /* "[ Apps ]" is 8 cells, cols 0-7 */
#define TAB_COL0(i)       (9 + (i) * 9)
#define TAB_WIDTH         7 /* seven tabs: 9 + 7*9 = 72 would hit the clock at col 70; the 7th ends at col 69 */
#define ICON_PITCH        11  /* seven apps: 1 + 7*11 = 78 columns */
#define ICON_START_COL    1
#define ICON_ROW_TOP       2
#define ICON_ROW_BOTTOM    5
#define MENU_TOP          14
#define MENU_BOTTOM       23
#define MENU_LEFT          0
#define MENU_RIGHT        17
#define MENU_ITEM_ROW(i)  (16 + (i))
#define TITLE_CLOSE_COL0  76
#define TITLE_CLOSE_COL1  78

#define TITLEBAR_ATTR   0x1F /* white on blue -- Windows/Ubuntu-taskbar-blue chrome */
#define TASKBAR_BG      0x10 /* black on blue */
#define TASKBAR_HILITE  0x3F /* white on cyan -- the focused app's own taskbar tab */
#define DESKTOP_BG      0x50 /* black on magenta -- the desktop's own "wallpaper" color */
#define ICON_LABEL_ATTR 0x5F /* white on magenta -- readable on the desktop background */
#define MENU_ATTR       0x1F
#define CURSOR_GLYPH     0x1A
#define CURSOR_COLOR     0x1E /* yellow on blue -- stands out against every chrome color above */

static void hal_console_redraw(void);

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) { n++; }
    return n;
}

static void write_str(int row, int col, const char *s, uint8_t color) {
    for (int i = 0; s[i] && col + i < VGA_COLS; i++) {
        vga_put(row, col + i, vga_entry((uint16_t)(uint8_t)s[i], color));
    }
}

static void fill_rect_row(int row, int col0, int width, uint8_t color) {
    for (int i = 0; i < width && col0 + i < VGA_COLS; i++) {
        vga_put(row, col0 + i, vga_entry(' ', color));
    }
}

/* Draws one bordered box (CP437 box-drawing glyphs) with an optional
 * centered title -- used only for the start-menu popup now (the old
 * two fixed panes this once drew for are gone; apps are chrome-free,
 * maximized content under the title bar instead). */
static void draw_box(int top, int bottom, int left, int right, const char *title, uint8_t color) {
    for (int c = left + 1; c < right; c++) {
        vga_put(top, c, vga_entry((uint16_t)0xC4, color));
        vga_put(bottom, c, vga_entry((uint16_t)0xC4, color));
    }
    for (int r = top + 1; r < bottom; r++) {
        vga_put(r, left, vga_entry((uint16_t)0xB3, color));
        vga_put(r, right, vga_entry((uint16_t)0xB3, color));
    }
    vga_put(top, left, vga_entry((uint16_t)0xDA, color));
    vga_put(top, right, vga_entry((uint16_t)0xBF, color));
    vga_put(bottom, left, vga_entry((uint16_t)0xC0, color));
    vga_put(bottom, right, vga_entry((uint16_t)0xD9, color));

    int len = str_len(title);
    int start = left + 2;
    for (int i = 0; i < len && start + i < right - 1; i++) {
        vga_put(top, start + i, vga_entry((uint16_t)(uint8_t)title[i], color));
    }
}

static void draw_icon(int slot, int footprint_left) {
    int body_col = footprint_left + (ICON_PITCH - 3) / 2; /* 3-cell tile, centered in the footprint */
    uint8_t bg = app_body_color[slot];
    vga_put(3, body_col,     vga_entry(' ', (uint8_t)(bg << 4)));
    vga_put(3, body_col + 1, vga_entry((uint16_t)(uint8_t)app_tab_label[slot][0], (uint8_t)((bg << 4) | 0x0F)));
    vga_put(3, body_col + 2, vga_entry(' ', (uint8_t)(bg << 4)));

    const char *title = app_title[slot];
    int len = str_len(title);
    int label_col = footprint_left + (ICON_PITCH - len) / 2;
    if (label_col < footprint_left) { label_col = footprint_left; }
    write_str(4, label_col, title, ICON_LABEL_ATTR);
}

static void draw_desktop(void) {
    for (int r = 0; r < CONTENT_H; r++) {
        for (int c = 0; c < CONTENT_W; c++) {
            vga_put(CONTENT_TOP + r, c, vga_entry(' ', DESKTOP_BG));
        }
    }
    for (int i = 0; i < WIN_COUNT; i++) {
        draw_icon(i, ICON_START_COL + i * ICON_PITCH);
    }
}

static void draw_menu(void) {
    if (!menu_open) {
        return;
    }
    for (int r = MENU_TOP + 1; r < MENU_BOTTOM; r++) {
        for (int c = MENU_LEFT + 1; c < MENU_RIGHT; c++) {
            vga_put(r, c, vga_entry(' ', MENU_ATTR));
        }
    }
    draw_box(MENU_TOP, MENU_BOTTOM, MENU_LEFT, MENU_RIGHT, " Apps ", MENU_ATTR);
    for (int i = 0; i < WIN_COUNT; i++) {
        write_str(MENU_ITEM_ROW(i), MENU_LEFT + 2, app_title[i], MENU_ATTR);
    }
}

static void blit_content(void) {
    if (focused_window >= 0 && windows[focused_window].in_use) {
        uint16_t *buf = windows[focused_window].buf;
        for (int r = 0; r < CONTENT_H; r++) {
            for (int c = 0; c < CONTENT_W; c++) {
                vga_put(CONTENT_TOP + r, c, buf[r * CONTENT_W + c]);
            }
        }
        /* The text cursor: the cell the next character will land in, drawn
         * as an inverted block so a person editing text can SEE where they
         * are. Only on the screen copy -- the window's own buffer is
         * untouched, so it never leaks into the content. */
        int cy = windows[focused_window].cy;
        int cx = windows[focused_window].cx;
        if (cy >= 0 && cy < CONTENT_H && cx >= 0 && cx < CONTENT_W) {
            uint16_t e = buf[cy * CONTENT_W + cx];
            uint8_t attr = (uint8_t)(e >> 8);
            uint8_t inv = (uint8_t)((((attr & 0x0F) & 0x07) << 4) | (attr >> 4));
            vga_put(CONTENT_TOP + cy, cx, (uint16_t)((inv << 8) | (e & 0xFF)));
        }
    } else {
        draw_desktop();
    }
    draw_menu();
}

static void draw_titlebar(void) {
    fill_rect_row(0, 0, VGA_COLS, TITLEBAR_ATTR);
    if (focused_window >= 0) {
        write_str(0, 1, app_title[focused_window], TITLEBAR_ATTR);
        write_str(0, TITLE_CLOSE_COL0, "[X]", TITLEBAR_ATTR);
    } else {
        write_str(0, 1, "Vajra Desktop", TITLEBAR_ATTR);
    }
}

static void put2(int row, int col, int val, uint8_t color) {
    if (val < 0) { val = 0; }
    vga_put(row, col,     vga_entry((uint16_t)(uint8_t)('0' + (val / 10) % 10), color));
    vga_put(row, col + 1, vga_entry((uint16_t)(uint8_t)('0' + val % 10), color));
}

static void draw_taskbar(void) {
    fill_rect_row(TASKBAR_ROW, 0, VGA_COLS, TASKBAR_BG);
    write_str(TASKBAR_ROW, APPS_BTN_COL0, "[ Apps ]", menu_open ? TASKBAR_HILITE : TASKBAR_BG);

    for (int i = 0; i < WIN_COUNT; i++) {
        uint8_t color = (focused_window == i) ? TASKBAR_HILITE : TASKBAR_BG;
        int col0 = TAB_COL0(i);
        fill_rect_row(TASKBAR_ROW, col0, TAB_WIDTH, color);
        write_str(TASKBAR_ROW, col0 + 1, app_tab_label[i], color);
    }

    struct rtc_time t;
    hal_rtc_read(&t);
    put2(TASKBAR_ROW, 70, t.hours, TASKBAR_BG);
    vga_put(TASKBAR_ROW, 72, vga_entry(':', TASKBAR_BG));
    put2(TASKBAR_ROW, 73, t.minutes, TASKBAR_BG);
    vga_put(TASKBAR_ROW, 75, vga_entry(':', TASKBAR_BG));
    put2(TASKBAR_ROW, 76, t.seconds, TASKBAR_BG);
}

static void draw_cursor(void) {
    if (cursor_col < 0) {
        return;
    }
    vga_put(cursor_row, cursor_col, vga_entry((uint16_t)CURSOR_GLYPH, CURSOR_COLOR));
}

static void hal_console_redraw(void) {
    hal_spin_lock(&console_lock);
    draw_titlebar();
    blit_content();
    draw_taskbar();
    draw_cursor();
    hal_spin_unlock(&console_lock);
}

/* Regenerates the Files app's content straight from core/storage.c --
 * "the namespace IS the source of truth" (docs/DESKTOP_DESIGN.md's
 * own Sec.3), so there's no separate icon/listing state to keep in
 * sync, just a fresh read every time this app is focused. Reuses the
 * SAME public hal_console_write() path every actor's SYS_WRITE goes
 * through (this runs entirely in kernel context, so that's safe --
 * same "HAL calling straight into core/storage.c" precedent hal/
 * x86_64/syscall.c's own SYS_OBJECT_* handlers already established). */
static const char *trust_name(int trust) {
    if (trust == OBJ_UNTRUSTED) { return "UNTRUSTED"; }
    if (trust == OBJ_QUARANTINED) { return "QUARANTINED"; }
    if (trust == OBJ_ANALYZED) { return "ANALYZED"; }
    if (trust == OBJ_TRUSTED) { return "TRUSTED"; }
    if (trust == OBJ_REJECTED) { return "REJECTED"; }
    return "?";
}

static void regenerate_files_window(void) {
    struct app_window *w = &windows[CONSOLE_WIN_FILES];
    if (!w->in_use) {
        return;
    }
    for (int i = 0; i < CONTENT_W * CONTENT_H; i++) {
        w->buf[i] = vga_entry(' ', VGA_COLOR);
    }
    w->cx = 0;
    w->cy = 0;

    hal_console_begin_window(CONSOLE_WIN_FILES);
    hal_console_write("Objects in storage:\n\n");
    char name[16];
    int id;
    int trust;
    uint32_t size;
    int i = 0;
    while (storage_get_by_index(i, name, &id, &trust, &size)) {
        hal_console_write("  ");
        hal_console_write(name);
        hal_console_write("  (");
        hal_console_write_dec64((uint64_t)size);
        hal_console_write(" bytes, ");
        hal_console_write(trust_name(trust));
        hal_console_write(")\n");
        i++;
    }
    if (i == 0) {
        hal_console_write("  (nothing yet)\n");
    }
    hal_console_end_window();
}

static void set_focus(int win) {
    focused_window = win;
    while (hal_keyboard_poll() >= 0) {
        /* discard keys typed while another app (or the desktop) had focus */
    }
    if (win == CONSOLE_WIN_FILES) {
        regenerate_files_window();
    }
}

/* Left-button-down edge -> hit-test against whatever's currently on
 * screen. Deliberately single-click (not double-click): a real
 * millisecond clock isn't cheaply available here (the CMOS RTC is
 * 1-second granularity), and single-click-to-activate is itself a
 * legitimate desktop convention (most docks/taskbars/start menus
 * already use it) rather than a compromise -- see docs/
 * DESKTOP_DESIGN.md's own note on this being an open question this
 * pass settles pragmatically. */
static void handle_click(int col, int row) {
    if (row == TASKBAR_ROW && col >= APPS_BTN_COL0 && col <= APPS_BTN_COL1) {
        menu_open = !menu_open;
        return;
    }
    if (row == TASKBAR_ROW) {
        for (int i = 0; i < WIN_COUNT; i++) {
            int col0 = TAB_COL0(i);
            if (col >= col0 && col < col0 + TAB_WIDTH) {
                set_focus(i);
                menu_open = 0;
                return;
            }
        }
    }
    if (row == 0 && focused_window >= 0 && col >= TITLE_CLOSE_COL0 && col <= TITLE_CLOSE_COL1) {
        focused_window = -1;
        menu_open = 0;
        return;
    }
    if (menu_open) {
        if (row >= MENU_ITEM_ROW(0) && row <= MENU_ITEM_ROW(WIN_COUNT - 1) &&
            col > MENU_LEFT && col < MENU_RIGHT) {
            int idx = row - MENU_ITEM_ROW(0);
            set_focus(idx);
        }
        menu_open = 0;
        return;
    }
    if (focused_window == -1 && row >= ICON_ROW_TOP && row <= ICON_ROW_BOTTOM) {
        for (int i = 0; i < WIN_COUNT; i++) {
            int left = ICON_START_COL + i * ICON_PITCH;
            if (col >= left && col < left + ICON_PITCH) {
                set_focus(i);
                return;
            }
        }
    }
}

/* SYS_MOUSE_READ's own handler calls this on every successful poll --
 * moves the cursor, handles a fresh click, then recomposites the
 * whole screen. See this file's own top comment on why no extra
 * locking is needed around the plain state mutations here. */
/* Guards against a spurious "click" being the very first thing this
 * driver ever observes -- confirmed by an actual serial trace, not
 * assumed: the first packet after enabling PS/2 mouse reporting can
 * carry a garbage buttons byte (seen consistently as 0x07, every
 * button "held") that doesn't correspond to any real input, likely a
 * byte-alignment artifact from the 8042 handshake that survives even
 * discarding the first COMPLETED packet in mouse.c. A real mouse is
 * always at rest (buttons==0) before a user ever clicks it, so simply
 * refusing to treat a button-down as a CLICK EDGE until at least one
 * clean (buttons==0) reading has been observed first closes this
 * regardless of the exact noise source -- it costs nothing for real
 * input, which always starts idle anyway. */
static int mouse_seen_clean = 0;

void hal_console_mouse_update(int col, int row, int buttons) {
    cursor_col = col;
    cursor_row = row;
    if (buttons == 0) {
        mouse_seen_clean = 1;
    }
    if (mouse_seen_clean && (buttons & 1) && !(prev_buttons & 1)) {
        handle_click(col, row);
    }
    prev_buttons = buttons;
    hal_console_redraw();
}

int hal_console_is_focused(int win) {
    return win == focused_window;
}

/* Phase 1 of init -- see this file's own top comment. Allocates
 * nothing, always succeeds, safe to call before memory_init() (early
 * boot) or mid-panic (exception_handler) regardless of what state the
 * allocator or the desktop itself is in. */
/* Disables the VGA CRTC's own hardware text cursor (a separate
 * blinking underscore/block the BIOS leaves on at whatever position
 * it last used, entirely independent of the software cursor glyph
 * this file draws) -- standard two-port CRTC trick, bit 5 of the
 * cursor-start register. Without this, a stray blinking mark shows up
 * wherever the BIOS left it, unrelated to and easily mistaken for the
 * real (mouse-driven) cursor. */
static void disable_hw_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

void hal_console_init(void) {
    disable_hw_cursor();
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_BASE[i] = vga_entry(' ', VGA_COLOR);
        vga_shadow[i] = vga_entry(' ', VGA_COLOR);
    }
    vga_shadow_valid = 1;
    for (int i = 0; i < WIN_COUNT; i++) {
        windows[i].in_use = 0;
        windows[i].cx = 0;
        windows[i].cy = 0;
        windows[i].esc_state = ESC_NONE;
        windows[i].esc_param_count = 0;
        windows[i].esc_cur_param = 0;
    }
    current_window = CONSOLE_WIN_LOG;
    cur_color = VGA_COLOR;
    focused_window = -1;
    menu_open = 0;
    cursor_col = -1;
    cursor_row = -1;
    prev_buttons = 0;
    mouse_seen_clean = 0;
    raw_row = 0;
    raw_col = 0;
}

/* Phase 2 -- kernel_main calls this once, strictly after memory_init().
 * Gives every app its own offscreen buffer and switches the screen
 * over to the real desktop for the first time. */
void hal_console_alloc_windows(void) {
    for (int i = 0; i < WIN_COUNT; i++) {
        windows[i].buf = (uint16_t *)alloc_dma_pages(1);
        windows[i].in_use = (windows[i].buf != 0);
        windows[i].cx = 0;
        windows[i].cy = 0;
        if (windows[i].in_use) {
            for (int cell = 0; cell < CONTENT_W * CONTENT_H; cell++) {
                windows[i].buf[cell] = vga_entry(' ', VGA_COLOR);
            }
        }
    }
    hal_console_redraw();
}

void hal_console_set_window(int win) {
    if (win < 0 || win >= WIN_COUNT) {
        return;
    }
    current_window = win;
}

/* Redraw is DEFERRED to the end of a whole write, not done per character.
 * It used to redraw all 2000 screen cells (each one a store to VGA memory,
 * an MMIO access) after EVERY character written to the focused window --
 * a 2KB frame from the Cores app was ~2000 full redraws, and since every
 * console write happens inside a syscall holding the kernel lock, that one
 * habit kept the lock held 99% of the time and starved every other core
 * (measured by the Cores app's own lock gauge: cores waiting 74% of their
 * time at four cores; at eight, the machine crawled). Now a write just
 * marks the screen dirty; hal_console_write() and hal_console_end_window()
 * flush once, and the boot core's timer tick flushes anything left. */
static volatile int redraw_pending;

static void finish_write(int win) {
    hal_spin_unlock(&console_lock);
    if (win == focused_window) {
        redraw_pending = 1;
    }
}

void hal_console_flush(void) {
    if (redraw_pending) {
        redraw_pending = 0;
        hal_console_redraw();
    }
}

/* Keyboard window switching (F1-F7, F8 = desktop). Called from the
 * keyboard interrupt, so it only changes state and asks for a redraw --
 * the redraw itself happens at the next flush. */
void hal_console_focus_app(int win) {
    if (win < -1 || win >= WIN_COUNT) {
        return;
    }
    set_focus(win);
    menu_open = 0;
    redraw_pending = 1;
}

void hal_console_putchar(char c) {
    hal_spin_lock(&console_lock);
    outb(COM1_PORT, (uint8_t)c);

    int win = current_window;

    if (!windows[win].in_use) {
        /* Bootstrap fallback -- see this file's own top comment.
         * Plain full-screen scrolling text, straight to VGA_BASE. */
        if (c == '\n') {
            raw_col = 0;
            raw_row++;
        } else if (c == '\b') {
            if (raw_col > 0) { raw_col--; }
        } else {
            vga_put(raw_row, raw_col, vga_entry((uint16_t)(uint8_t)c, VGA_COLOR));
            raw_col++;
            if (raw_col >= VGA_COLS) {
                raw_col = 0;
                raw_row++;
            }
        }
        if (raw_row >= VGA_ROWS) {
            for (int r = 1; r < VGA_ROWS; r++) {
                for (int cc = 0; cc < VGA_COLS; cc++) {
                    vga_put(r - 1, cc, VGA_BASE[r * VGA_COLS + cc]);
                }
            }
            for (int cc = 0; cc < VGA_COLS; cc++) {
                vga_put(VGA_ROWS - 1, cc, vga_entry(' ', VGA_COLOR));
            }
            raw_row = VGA_ROWS - 1;
        }
        hal_spin_unlock(&console_lock);
        return;
    }

    struct app_window *w = &windows[win];

    if (w->esc_state == ESC_NONE && c == 0x1B) {
        w->esc_state = ESC_GOT_ESC;
        finish_write(win);
        return;
    }
    if (w->esc_state == ESC_GOT_ESC) {
        if (c == '[') {
            w->esc_state = ESC_IN_SEQ;
            w->esc_param_count = 0;
            w->esc_cur_param = 0;
        } else {
            w->esc_state = ESC_NONE;
        }
        finish_write(win);
        return;
    }
    if (w->esc_state == ESC_IN_SEQ) {
        if (c >= '0' && c <= '9') {
            w->esc_cur_param = w->esc_cur_param * 10 + (c - '0');
        } else if (c == ';') {
            if (w->esc_param_count < 4) {
                w->esc_params[w->esc_param_count++] = w->esc_cur_param;
            }
            w->esc_cur_param = 0;
        } else {
            if (w->esc_param_count < 4) {
                w->esc_params[w->esc_param_count++] = w->esc_cur_param;
            }
            if (c == 'J') {
                for (int i = 0; i < CONTENT_W * CONTENT_H; i++) {
                    w->buf[i] = vga_entry(' ', cur_color);
                }
                w->cx = 0;
                w->cy = 0;
            } else if (c == 'H') {
                int row = (w->esc_param_count >= 1 && w->esc_params[0] > 0) ? w->esc_params[0] - 1 : 0;
                int col = (w->esc_param_count >= 2 && w->esc_params[1] > 0) ? w->esc_params[1] - 1 : 0;
                if (row >= CONTENT_H) { row = CONTENT_H - 1; }
                if (col >= CONTENT_W) { col = CONTENT_W - 1; }
                w->cy = row;
                w->cx = col;
            } else if (c == 'm') {
                if (w->esc_param_count == 0) {
                    cur_color = VGA_COLOR;
                } else {
                    for (int i = 0; i < w->esc_param_count; i++) {
                        int n = w->esc_params[i];
                        if (n == 0) {
                            cur_color = VGA_COLOR;
                        } else if (n >= 30 && n <= 37) {
                            cur_color = (uint8_t)((cur_color & 0xF0) | ansi_to_vga[n - 30]);
                        } else if (n >= 40 && n <= 47) {
                            cur_color = (uint8_t)((cur_color & 0x0F) | (uint8_t)(ansi_to_vga[n - 40] << 4));
                        }
                    }
                }
            }
            w->esc_state = ESC_NONE;
        }
        finish_write(win);
        return;
    }

    if (c == '\n') {
        w->cx = 0;
        w->cy++;
    } else if (c == '\b') {
        if (w->cx > 0) {
            w->cx--;
        } else if (w->cy > 0) {
            w->cy--;
            w->cx = CONTENT_W - 1;
        }
    } else {
        w->buf[w->cy * CONTENT_W + w->cx] = vga_entry((uint16_t)(uint8_t)c, cur_color);
        w->cx++;
        if (w->cx >= CONTENT_W) {
            w->cx = 0;
            w->cy++;
        }
    }

    if (w->cy >= CONTENT_H) {
        for (int r = 1; r < CONTENT_H; r++) {
            for (int cc = 0; cc < CONTENT_W; cc++) {
                w->buf[(r - 1) * CONTENT_W + cc] = w->buf[r * CONTENT_W + cc];
            }
        }
        for (int cc = 0; cc < CONTENT_W; cc++) {
            w->buf[(CONTENT_H - 1) * CONTENT_W + cc] = vga_entry(' ', VGA_COLOR);
        }
        w->cy = CONTENT_H - 1;
    }

    finish_write(win);
}

/* Serializes a multi-call write to ONE window. current_window is a
 * single global (per-char locking below only protects individual
 * characters), so without this two writers -- an actor's SYS_WRITE on
 * one core, the AP's status line on the other -- could switch it under
 * each other or interleave mid-line: seen as "a[tAtPa cck..." in the
 * Security app once more windows existed to land in. Not reentrant:
 * never nest, and never yield between begin and end. */
void hal_console_begin_window(int win) {
    hal_spin_lock(&window_lock);
    saved_window_for_run = current_window;
    if (win >= 0 && win < WIN_COUNT) {
        current_window = win;
    }
}

void hal_console_end_window(void) {
    hal_console_flush();
    current_window = saved_window_for_run;
    hal_spin_unlock(&window_lock);
}

void hal_console_write(const char *str) {
    while (*str) {
        hal_console_putchar(*str);
        str++;
    }
    hal_console_flush();
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
