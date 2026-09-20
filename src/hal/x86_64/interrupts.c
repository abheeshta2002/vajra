#include "vajra/hal.h"
#include "vajra/actor.h"

/* ------------------------------------------------------------------
 * IDT setup + centralized exception handling.
 *
 * This is the C equivalent of what the assembly kernel arrived at in
 * V0.32: every registered vector funnels into one handler instead of
 * copy-pasted per-vector stubs, and a genuine kernel-mode fault gets
 * a real diagnostic screen instead of silently freezing.
 * ---------------------------------------------------------------- */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 256 entries -- the architectural maximum, and comfortably covers
 * both the CPU's own 0-31 exception range, the 0x20-0x2F (32-47)
 * range hal_pic_remap() moves IRQ0-15 into, and vector 0x80 (128),
 * the syscall gate. Only a handful of vectors are actually registered
 * below; the rest stay not-present like any other unregistered
 * vector, which is fine since hal_pic_remap() also keeps every IRQ
 * but the timer masked. */
#define IDT_ENTRIES 256
#define TIMER_VECTOR    32
#define KEYBOARD_VECTOR 33 /* IRQ1, roadmap Phase 18 -- see hal/x86_64/keyboard.c */
#define MOUSE_VECTOR    44 /* IRQ12, docs/DESKTOP_DESIGN.md Stage 1 -- see hal/x86_64/mouse.c */
#define SYSCALL_VECTOR 0x80
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

/* Defined in isr_stubs.asm */
extern void isr_stub_0(void);
extern void isr_stub_6(void);
extern void isr_stub_8(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_32(void);
extern void isr_stub_33(void);
extern void isr_stub_44(void);
extern void isr_stub_128(void);

/* Defined in keyboard.c -- reads the scancode off port 0x60 and pushes
 * the decoded character into its own ring buffer (hal_keyboard_poll()
 * drains it). Purely internal HAL-to-HAL linkage, like the isr_stub_*
 * externs above -- no reason for hal.h's public surface to know this
 * exists, same as hal_pic_send_eoi() callers don't need to know IRQ0's
 * own handling lives here rather than in timer.c. */
extern void hal_keyboard_irq_handler(void);

/* Defined in mouse.c -- same internal-linkage reasoning as
 * hal_keyboard_irq_handler() above. */
extern void hal_mouse_irq_handler(void);

static void idt_set_gate(int vector, void (*handler)(void), uint8_t ist, uint8_t dpl) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[vector].selector    = 0x18; /* 64-bit kernel code segment (see hal_gdt_init's GDT --
                                        entry 0x08 there is the transient 32-bit protected-
                                        mode segment, NOT the long-mode one; 0x18 is the
                                        actual 64-bit code segment used from long_mode: onward) */
    idt[vector].ist         = ist;  /* 0 = use whatever stack was already active; 1-7 = force
                                        the matching TSS.ISTn stack (see hal_gdt_init) */
    idt[vector].type_attr   = (uint8_t)(0x80 | ((dpl & 0x3) << 5) | 0x0E);
                                     /* present, DPL, 64-bit interrupt gate. DPL is the highest
                                        CPL allowed to reach this gate directly (e.g. via `int`) --
                                        0 for everything except the syscall gate, which ring-3
                                        actor code must be able to invoke on purpose (DPL=3). A
                                        genuine hardware fault (#GP, #PF, ...) can still reach a
                                        DPL=0 gate from ring 3 regardless -- DPL only gates
                                        deliberate `int N`, never CPU-generated exceptions. */
    idt[vector].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].reserved    = 0;
}

void hal_interrupts_init(void) {
    /* Must run before any gate below can use ist=1 -- it's what
     * builds the TSS and its dedicated double-fault stack. */
    hal_gdt_init();

    /* idt[] is `static`, so every entry starts zeroed -- type_attr's
     * present bit is already 0. Leave unregistered vectors that way
     * instead of pointing them at a null handler: a present gate with
     * offset 0 would send an unexpected exception straight into
     * address 0x0 and whatever garbage lives there, rather than
     * cleanly faulting. A not-present gate makes the CPU raise #GP
     * (vector 13, which *is* handled) on the original faulting
     * instruction instead. */

    idt_set_gate(0,  isr_stub_0,  0, 0);
    idt_set_gate(6,  isr_stub_6,  0, 0);
    idt_set_gate(8,  isr_stub_8,  1, 0); /* #DF: always use the dedicated IST1 stack -- see
                                            hal_gdt_init's comment for why this matters */
    idt_set_gate(13, isr_stub_13, 0, 0);
    idt_set_gate(14, isr_stub_14, 0, 0);
    idt_set_gate(TIMER_VECTOR, isr_stub_32, 0, 0);
    idt_set_gate(KEYBOARD_VECTOR, isr_stub_33, 0, 0);
    idt_set_gate(MOUSE_VECTOR, isr_stub_44, 0, 0);
    idt_set_gate(SYSCALL_VECTOR, isr_stub_128, 0, 3); /* DPL=3: ring-3 actor code must be able
                                                          to `int 0x80` on purpose -- this is
                                                          THE syscall boundary; see hal.h */

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt[0];

    __asm__ __volatile__("lidt %0" : : "m"(idtp));
}

/* The AP-side counterpart to hal_interrupts_init() (Milestone
 * 12/Phase 10) -- the IDT's CONTENTS are ordinary kernel commons,
 * already built once by the BSP and identically visible to every core
 * (hal/x86_64/paging.c's own comment: 0-1MB is present in every
 * address space). IDTR itself, unlike the table it points at, is
 * per-core state that resets to an invalid default independently on
 * every core -- an AP that never issues its own `lidt` would triple-
 * fault on its first exception instead of reaching this kernel's own
 * diagnostic panic screen below. */
void hal_idt_load_ap(void) {
    __asm__ __volatile__("lidt %0" : : "m"(idtp));
}

void hal_disable_interrupts(void) {
    __asm__ __volatile__("cli");
}

void hal_enable_interrupts(void) {
    __asm__ __volatile__("sti");
}

void hal_halt_forever(void) {
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

/* Called from isr_common in isr_stubs.asm for every registered vector. */
void exception_handler(uint64_t vector, uint64_t error_code, uint64_t rip, uint64_t cs) {
    if (vector == TIMER_VECTOR) {
        /* A hardware tick, not a fault -- acknowledge it so the PIC
         * will deliver the next one, then hand off to the portable
         * scheduler exactly as if the interrupted actor had called
         * actor_yield() itself. actor_yield()/schedule_next() disable
         * interrupts for their own critical section and re-enable
         * them once this exact point is resumed (see actor.c), so
         * nothing further is needed here either way. EOI is sent
         * first, before the potential switch, so it happens promptly
         * regardless of how long that takes to eventually return here. */
        hal_pic_send_eoi(0);
        actor_yield();
        return;
    }

    if (vector == KEYBOARD_VECTOR) {
        /* Not a fault, not a scheduling event -- just data becoming
         * available. EOI first (same ordering as the timer case
         * above), then read the scancode and return, resuming whatever
         * was interrupted directly through isr_common's own iretq. No
         * actor_yield() here: unlike a timer tick, an incoming
         * keystroke has no reason to force a context switch. */
        hal_pic_send_eoi(1);
        hal_keyboard_irq_handler();
        return;
    }

    if (vector == MOUSE_VECTOR) {
        /* Same shape as the keyboard case above -- data becoming
         * available, not a fault or a scheduling event. IRQ12 is on
         * the slave PIC, so hal_pic_send_eoi() sends EOI to BOTH
         * controllers (irq >= 8 case, pic.c). */
        hal_pic_send_eoi(12);
        hal_mouse_irq_handler();
        return;
    }

    /* Phase 23: a fault that originated in ring 3 (CS's low 2 bits =
     * the CPL the CPU was executing at when the exception fired, per
     * the Intel SDM's own description of the CS value it pushes) is
     * the actor's bug, not the kernel's -- PHILOSOPHY.md's own
     * isolation invariant means one actor's mistake must not be able
     * to take the rest of the machine down. Terminate just that actor
     * and keep going, the exact same path actor_exit()/SYS_EXIT
     * already uses to end an actor cleanly (state -> DEAD, reap,
     * schedule_next() picks the next runnable actor and never returns
     * here). A CPL0 origin (cs & 3 == 0) is a real kernel bug -- still
     * an unconditional panic, unchanged from before this phase. */
    if ((cs & 3) == 3 && actor_current_slot() >= 0) {
        actor_note_fault();
        hal_console_begin_window(CONSOLE_WIN_LOG); /* not whichever app window last wrote */
        hal_console_write("\n[actor ");
        hal_console_write_hex64((uint64_t)actor_current_slot());
        hal_console_write(" terminated -- fault vector ");
        hal_console_write_hex64(vector);
        hal_console_write(", RIP ");
        hal_console_write_hex64(rip);
        hal_console_write("]\n");
        hal_console_end_window();
        actor_exit(); /* never returns: schedule_next() switches away */
    }

    hal_disable_interrupts();
    hal_console_init();

    hal_console_write("KERNEL PANIC - Unhandled Exception\n\n");
    hal_console_write("Vector:     ");
    hal_console_write_hex64(vector);
    hal_console_write("\nError Code: ");
    hal_console_write_hex64(error_code);
    hal_console_write("\nRIP:        ");
    hal_console_write_hex64(rip);
    hal_console_write("\n");

    if (vector == 14) {
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        hal_console_write("\nCR2 (fault address): ");
        hal_console_write_hex64(cr2);
        hal_console_write("\n");
    }

    hal_halt_forever();
}
