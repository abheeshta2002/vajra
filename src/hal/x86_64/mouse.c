#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * PS/2 mouse driver -- docs/DESKTOP_DESIGN.md Stage 1: the smallest
 * slice that proves real mouse input works at all, before anything
 * about the window/compositor model changes. A visible cursor glyph
 * over the EXISTING two-pane console (hal/x86_64/console.c) -- no new
 * windows yet.
 *
 * The PS/2 mouse lives on the 8042 controller's AUXILIARY port, which
 * (unlike the keyboard, already enabled by BIOS/firmware) needs real
 * setup before it reports anything: enable the aux port, unmask its
 * IRQ line in the controller's own command byte, then tell the mouse
 * device itself (via the 0xD4 "next byte goes to the aux device"
 * prefix) to start streaming movement packets. IRQ12 -> vector 44, on
 * the SLAVE PIC -- see pic.c's own updated mask comment for why the
 * master's IRQ2 cascade line also has to be unmasked for a slave IRQ
 * to ever reach the CPU at all.
 *
 * Standard 3-byte packets (byte0: button/sign/overflow flags, byte1:
 * delta X, byte2: delta Y -- Y is sign-inverted versus screen rows,
 * since the mouse's own convention is "up is positive"). Accumulated
 * into a fixed-point cell position (CELL_FRAC sub-cell units per
 * character cell) so movement feels smooth despite 80x25 being coarse,
 * then clamped to the screen and exposed as CURRENT absolute position
 * + button state -- a desktop needs "where is the cursor now," not
 * "how far did it move" (hal_mouse_poll()'s own doc comment).
 * ---------------------------------------------------------------- */

#define MOUSE_DATA_PORT    0x60
#define MOUSE_STATUS_PORT  0x64
#define MOUSE_CMD_PORT     0x64

#define STATUS_OUTPUT_FULL 0x01 /* controller has a byte ready to read on 0x60 */
#define STATUS_INPUT_FULL  0x02 /* controller is still busy with a byte written to 0x60/0x64 */

#define VGA_COLS 80
#define VGA_ROWS 25
#define CELL_FRAC 256 /* sub-cell fixed-point units per character cell */

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Bounded spins, not infinite waits -- same discipline keyboard.c's
 * own init already follows (draining stray bytes there is bounded by
 * the status flag itself going false). A real 8042 controller answers
 * within a handful of microseconds; a bound here just means a broken
 * or absent controller can't hang boot forever. */
static int wait_input_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(MOUSE_STATUS_PORT) & STATUS_INPUT_FULL)) {
            return 1;
        }
    }
    return 0;
}

static int wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(MOUSE_STATUS_PORT) & STATUS_OUTPUT_FULL) {
            return 1;
        }
    }
    return 0;
}

static void mouse_write(uint8_t val) {
    wait_input_clear();
    outb(MOUSE_CMD_PORT, 0xD4); /* "next byte on 0x60 goes to the aux device" */
    wait_input_clear();
    outb(MOUSE_DATA_PORT, val);
}

static uint8_t mouse_read_ack(void) {
    wait_output_full();
    return inb(MOUSE_DATA_PORT);
}

static int32_t pos_x256 = (VGA_COLS / 2) * CELL_FRAC;
static int32_t pos_y256 = (VGA_ROWS / 2) * CELL_FRAC;
static int buttons_state = 0;

static int discard_next_packet = 1; /* see hal_mouse_irq_handler()'s own comment -- the real-
                                        hardware equivalent of this driver's own "buttons=7 out of
                                        nowhere" confirmed via serial trace: the 8042/mouse
                                        handshake (0xF6/0xF4, ack'd via polling reads) can still
                                        leave exactly one stray/misaligned byte for the freshly
                                        unmasked IRQ to pick up as if it were a real packet's first
                                        byte, even with hal_pic_unmask_irq12() deferred until after
                                        every polling read is done -- unmask and the controller's
                                        own internal state aren't perfectly synchronized down to the
                                        byte. Unconditionally discarding the very first COMPLETED
                                        packet after init, rather than trusting it, is the standard
                                        PS/2 driver answer to this class of noise. */
static uint8_t packet[3];
static int packet_idx = 0;
static volatile int packet_ready = 0; /* set by the IRQ handler, cleared by hal_mouse_poll() --
                                          "a new packet arrived since the last poll", the same
                                          one-slot-of-freshness shape keyboard.c's ring buffer
                                          gives SYS_KEY_READ, just collapsed to the latest state
                                          instead of a FIFO of keystrokes (see this file's own
                                          top comment: a desktop wants CURRENT position, not a
                                          queue of every delta). */

void hal_mouse_init(void) {
    outb(MOUSE_CMD_PORT, 0xA8); /* enable the auxiliary (mouse) port */

    /* Read-modify-write the controller's own command byte: set bit 1
     * (enable IRQ12 on aux-port data), leave everything else alone. */
    wait_input_clear();
    outb(MOUSE_CMD_PORT, 0x20); /* "read command byte" */
    uint8_t status = mouse_read_ack();
    status |= 0x02;  /* enable IRQ12 */
    status &= ~0x20; /* make sure the aux clock isn't disabled */

    wait_input_clear();
    outb(MOUSE_CMD_PORT, 0x60); /* "write command byte" */
    wait_input_clear();
    outb(MOUSE_DATA_PORT, status);

    mouse_write(0xF6); /* set defaults */
    mouse_read_ack();

    mouse_write(0xF4); /* enable data reporting -- streaming packets begin after this */
    mouse_read_ack();

    packet_idx = 0;
    packet_ready = 0;
    discard_next_packet = 1;

    /* Only now -- see hal_pic_unmask_irq12()'s own comment for why
     * unmasking any earlier (even just at hal_pic_remap() time,
     * before this handshake's own polling reads run) can corrupt the
     * very first packet's byte alignment. */
    hal_pic_unmask_irq12();
}

void hal_mouse_irq_handler(void) {
    uint8_t b = inb(MOUSE_DATA_PORT);

    /* Byte 0 of a real packet always has bit 3 set -- the standard
     * alignment check every PS/2 mouse driver uses to resynchronize if
     * a byte is ever missed (an IRQ arriving mid-packet after a
     * spurious byte, for instance) instead of silently reading offset
     * garbage as X/Y forever after. */
    if (packet_idx == 0 && !(b & 0x08)) {
        return;
    }

    packet[packet_idx++] = b;
    if (packet_idx < 3) {
        return;
    }
    packet_idx = 0;

    if (discard_next_packet) {
        discard_next_packet = 0;
        return;
    }

    uint8_t flags = packet[0];
    int dx = (int8_t)packet[1];
    int dy = (int8_t)packet[2];
    if (flags & 0x40) { dx = 0; } /* X overflow -- discard rather than trust a garbage delta */
    if (flags & 0x80) { dy = 0; } /* Y overflow */

    pos_x256 += dx * CELL_FRAC;
    pos_y256 -= dy * CELL_FRAC; /* mouse Y is inverted versus screen rows */

    int32_t max_x = (VGA_COLS - 1) * CELL_FRAC;
    int32_t max_y = (VGA_ROWS - 1) * CELL_FRAC;
    if (pos_x256 < 0) { pos_x256 = 0; }
    if (pos_x256 > max_x) { pos_x256 = max_x; }
    if (pos_y256 < 0) { pos_y256 = 0; }
    if (pos_y256 > max_y) { pos_y256 = max_y; }

    buttons_state = flags & 0x07; /* bits 0-2: left, right, middle */
    packet_ready = 1;
}

int hal_mouse_poll(int *col, int *row, int *buttons) {
    if (!packet_ready) {
        return -1;
    }
    packet_ready = 0;
    *col = pos_x256 / CELL_FRAC;
    *row = pos_y256 / CELL_FRAC;
    *buttons = buttons_state;
    return 0;
}
