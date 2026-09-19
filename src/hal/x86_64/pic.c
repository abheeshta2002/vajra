#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * 8259 Programmable Interrupt Controller: remapping + EOI.
 *
 * The PIC's power-on default maps IRQ0-7 to interrupt vectors 0x08-
 * 0x0F -- which collides head-on with CPU exception vectors (0x08 is
 * #DF, the double fault!). Every real x86 kernel remaps it before
 * unmasking anything, moving IRQ0-7 to 0x20-0x27 and IRQ8-15 to
 * 0x28-0x2F, clear of the 0-31 range the CPU itself uses.
 *
 * Everything except IRQ0 (the timer, see timer.c) stays masked: an
 * unmasked IRQ with no registered handler would fire into a
 * not-present IDT gate and raise #GP once per occurrence instead of
 * being silently ignorable, and nothing here handles any other IRQ
 * yet.
 * ---------------------------------------------------------------- */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11 /* edge-triggered, cascade mode, ICW4 will follow */
#define ICW4_8086 0x01 /* 8086/88 mode (not the legacy 8080 mode) */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

void hal_pic_remap(void) {
    outb(PIC1_CMD, ICW1_INIT);
    outb(PIC2_CMD, ICW1_INIT);
    outb(PIC1_DATA, 0x20); /* master: IRQ0-7  -> vectors 0x20-0x27 */
    outb(PIC2_DATA, 0x28); /* slave:  IRQ8-15 -> vectors 0x28-0x2F */
    outb(PIC1_DATA, 0x04); /* tell master a slave sits on its IRQ2 line */
    outb(PIC2_DATA, 0x02); /* tell slave its own cascade identity */
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, 0xF8); /* mask all master IRQs except IRQ0 (timer), IRQ1 (PS/2 keyboard,
                               roadmap Phase 18), and IRQ2 -- the cascade line the slave PIC's own
                               interrupts travel through to reach the CPU at all. Unmasking IRQ12
                               (below) on PIC2 alone does nothing without this: IRQ2 is not a real
                               device, it's wiring between the two chips. */
    outb(PIC2_DATA, 0xFF); /* mask all slave IRQs, INCLUDING IRQ12 (PS/2 mouse) -- see
                               hal_pic_unmask_irq12()'s own comment for why this one is
                               deliberately unmasked later, not here. */
}

/* Unmasks IRQ12 (PS/2 mouse) on the slave PIC -- called by hal_mouse_
 * init(), AFTER its own polling-based 8042/mouse handshake (enable
 * aux port, set defaults, enable data reporting -- all read via direct
 * port I/O, not the IRQ path) has fully finished, not from
 * hal_pic_remap() alongside every other IRQ. An edge-triggered PIC
 * latches a request the instant the device asserts it, mask or no
 * mask -- if IRQ12 were unmasked while hal_mouse_init()'s own
 * synchronous reads were draining the controller's ack bytes, each of
 * those bytes would ALSO arm a pending IRQ12 that fires the moment
 * interrupts are finally enabled (hal_enable_interrupts(), much later
 * in kernel_main), handing hal_mouse_irq_handler() a byte that's
 * either stale or (worse) the start of a real movement packet read
 * one position too early -- a 3-byte alignment corruption that could
 * synthesize a spurious click. Confirmed by an actual one: a fresh
 * boot with no mouse_button ever sent still landed a click on the
 * About app. Keeping IRQ12 masked until the handshake is provably
 * done removes the race instead of trying to filter its symptom. */
void hal_pic_unmask_irq12(void) {
    uint8_t mask = 0xFF;
    __asm__ __volatile__("inb %1, %0" : "=a"(mask) : "Nd"((uint16_t)PIC2_DATA));
    mask &= (uint8_t)~0x10; /* IRQ12 is bit 4 on the slave PIC (IRQ8=bit0..IRQ15=bit7) */
    outb(PIC2_DATA, mask);
}

void hal_pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}
