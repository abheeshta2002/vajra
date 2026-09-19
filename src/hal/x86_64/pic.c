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

    outb(PIC1_DATA, 0xFC); /* mask all master IRQs except IRQ0 (timer) and, roadmap Phase 18,
                               IRQ1 (PS/2 keyboard, hal/x86_64/keyboard.c) -- the kernel's first
                               INPUT device, everything before this was output-only or block
                               storage. */
    outb(PIC2_DATA, 0xFF); /* mask all slave IRQs */
}

void hal_pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}
