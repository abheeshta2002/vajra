#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * PIT (8253/8254 Programmable Interval Timer) channel 0, used purely
 * as a periodic tick source for preemption -- see interrupts.c's
 * dispatch of vector 32 (IRQ0) to actor_yield(). Nothing here is
 * scheduling policy; this file only knows how to make the hardware
 * fire an interrupt at a given frequency, matching the HAL boundary's
 * translation principle (docs/vajra_philosophy addendum A3).
 * ---------------------------------------------------------------- */

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182u /* the PIT's fixed input clock */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

void hal_timer_init(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_HZ / frequency_hz;

    outb(PIT_COMMAND, 0x36); /* channel 0, lobyte/hibyte access, mode 3 (square wave), binary */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}
