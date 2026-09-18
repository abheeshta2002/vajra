#ifndef VAJRA_HAL_H
#define VAJRA_HAL_H

#include <stdint.h>

/* ---- Console ---- */
void hal_console_init(void);
void hal_console_putchar(char c);
void hal_console_write(const char *str);
void hal_console_write_hex64(uint64_t value);
void hal_console_write_dec64(uint64_t value);

/* ---- Interrupts / exceptions ---- */
void hal_interrupts_init(void);
void hal_disable_interrupts(void);
void hal_enable_interrupts(void);

/* ---- Paging / per-actor address spaces ----
 * Builds a complete, independent x86-64 address space for actor slot
 * `slot` (0..MAX_ACTORS-1): the kernel commons (0-1MB and 2MB-256MB)
 * identity-mapped exactly as boot.asm set them up, present in every
 * address space, plus [private_base, private_base+private_size) --
 * which must fall within 1MB-2MB, see paging.c's top comment -- mapped
 * present/writable/user-accessible to this actor ALONE. No other
 * address in that whole 1MB-2MB window is mapped at all here, so
 * nothing else -- including any other actor's own memory -- is even
 * addressable from the returned address space.
 * Returns the physical address to load into CR3, or 0 on failure
 * (bad slot, or private_base/private_size outside 1MB-2MB). */
uint64_t hal_address_space_create(int slot, uint64_t private_base, uint64_t private_size);

/* Zeroes exactly one 4KB page at the given PHYSICAL address,
 * regardless of whether that address happens to be mapped in the
 * CURRENTLY active address space. core/memory.c's alloc_page() needs
 * this rather than a plain pointer write: it can now be called while
 * an actor's own (deliberately restricted) address space is active
 * -- e.g. via actor_spawn_child() during a SYS_SPAWN syscall -- and a
 * freshly allocated page for a BRAND NEW actor is never mapped in the
 * CALLING actor's own view, only in the new actor's, which doesn't
 * exist yet at the moment it needs zeroing. */
void hal_zero_page(void *phys_addr);

/* ---- GDT / TSS ----
 * Builds and loads the kernel's own GDT (replacing the bootloader's,
 * now including ring-3 code/data descriptors) and a TSS with a
 * dedicated IST1 stack, then loads it with ltr. Must run before
 * hal_interrupts_init() registers the double-fault vector with ist=1
 * -- interrupts.c calls this itself as the first step, so nothing
 * else needs to sequence it. */
void hal_gdt_init(void);

/* Points TSS.RSP0 at actor slot `slot`'s own dedicated kernel stack --
 * the stack any interrupt or syscall arriving while that actor is
 * running in ring 3 will land on (the CPU switches to it
 * automatically on any privilege-level-changing entry, same
 * mechanism as a syscall). Must be called before switching to an
 * actor that might run in ring 3, i.e. before every
 * hal_context_switch() call -- core/actor.c's schedule_next() is the
 * only caller. Without a dedicated stack per actor, two actors both
 * suspended mid-syscall at once (an entirely normal state -- see
 * core/actor.c's own comment) would silently corrupt each other's
 * saved kernel-mode state, since every ring3->ring0 transition starts
 * fresh at TSS.RSP0 rather than continuing wherever a previous one
 * left off. */
void hal_set_kernel_stack(int slot);

/* Returns the top (highest address) of actor slot `slot`'s dedicated
 * kernel stack -- used once, at spawn time, to build that actor's
 * initial fake suspended frame there (see hal_context_switch) instead
 * of on its user stack. Returns 0 for an out-of-range slot. */
void *hal_get_kernel_stack_top(int slot);

/* ---- Ring 3 / syscalls ----
 * The ABI a ring-3 actor uses to reach the kernel -- the ONLY way,
 * once ring 3 is active: kernel code lives in supervisor-only memory,
 * unreachable from CPL 3 by direct call or jump. hal_syscall() is
 * declared here as an ordinary portable-looking function, but its
 * x86 implementation (hal/x86_64/syscall_invoke.c) is marked
 * __attribute__((section(".user_text"))) so it can actually be
 * fetched and executed at CPL 3 to begin with -- see link.ld's
 * .user_text section. Any actor-callable helper needs the same
 * attribute; core/main.c's demo actors and their small wrappers are
 * the reference example. */
#define SYS_WRITE   1 /* a1 = const char* (NUL-terminated). The kernel reads it directly at
                         CPL 0, where the page's U/S bit is irrelevant -- no copy needed. */
#define SYS_YIELD   2
#define SYS_EXIT    3
#define SYS_SEND    4 /* a1 = dest actor slot, a2 = message type, a3 = message data.
                         Returns 0 on success, -1 if the caller lacks a CAP_SEND capability
                         for dest, or dest is invalid/dead/mailbox-full. */
#define SYS_RECEIVE 5 /* a1 = struct message* (in the caller's own memory) to fill in.
                         Blocks until a message arrives; always returns 0. */
#define SYS_GRANT     6 /* a1 = dest actor slot, a2 = capability op (e.g. CAP_SEND), a3 = target.
                           Delegates a copy of a capability the CALLER already holds to dest.
                           Returns 0 on success, -1 if the caller doesn't hold {op, target}
                           itself, dest is invalid, or dest's capability table is full. */
#define SYS_SPAWN     7 /* a1 = entry point -- must be a .user_text function already linked
                           into the kernel image; there is no loader yet, so this instantiates
                           a known worker, it doesn't load arbitrary code. Requires the caller
                           to hold CAP_SPAWN and be under its spawn quota. On success, returns
                           the new actor's slot index and auto-grants the caller
                           CAP_SEND+CAP_TERMINATE for it, and the new actor CAP_SEND for the
                           caller. Returns -1 on any failure. */
#define SYS_TERMINATE 8 /* a1 = target actor slot. Requires the caller to hold CAP_TERMINATE
                           for target. Returns 0 on success, -1 if denied or target is
                           invalid/already dead/is the caller itself (use SYS_EXIT instead). */
#define SYS_OBJECT_READ    9  /* a1 = object id, a2 = buf* (caller's own memory), a3 = buf
                                  len. Requires CAP_READ_OBJECT for the object. Returns bytes
                                  read (may be less than requested, per the object's actual
                                  size, exactly like a short read), or -1 if denied/invalid. */
#define SYS_OBJECT_WRITE   10 /* a1 = object id, a2 = buf* (caller's own memory), a3 = byte
                                  count. Requires CAP_WRITE_OBJECT for the object. REPLACES the
                                  object's entire contents (no partial-offset writes yet -- see
                                  core/storage.c) and resets its trust to OBJ_UNTRUSTED: new
                                  content invalidates any prior trust decision. Returns bytes
                                  written, or -1 if denied/invalid/too large. */
#define SYS_OBJECT_PROMOTE 11 /* a1 = object id. Requires CAP_PROMOTE_OBJECT (target 0 -- this
                                  authorizes promoting objects in general, the same blanket
                                  convention as CAP_SPAWN, since there's no per-object grant
                                  scheme yet). Advances the object one trust level (OBJ_UNTRUSTED
                                  -> OBJ_QUARANTINED -> OBJ_ANALYZED -> OBJ_TRUSTED). Returns the
                                  new trust level, or -1 if denied/invalid/already OBJ_TRUSTED or
                                  OBJ_REJECTED. */
#define SYS_OBJECT_REJECT  12 /* a1 = object id. Requires CAP_PROMOTE_OBJECT too -- whatever
                                  authority decides an object may advance is the same authority
                                  that decides it may not; these are the two sides of one trust
                                  verdict, not separate grants. Marks the object OBJ_REJECTED,
                                  permanently (see storage_reject()) -- SYS_OBJECT_READ then
                                  refuses it outright, even for a caller that legitimately holds
                                  CAP_READ_OBJECT. Returns 0 on success, -1 if denied/invalid. */
#define SYS_NET_SEND    13 /* a1 = message type, a2 = message data. Requires CAP_NET (target 0).
                               Broadcasts {type, data} to every device on the local link
                               (core/net.c). Returns 0 on success, -1 if denied or no network
                               device is present. */
#define SYS_NET_RECEIVE 14 /* a1 = struct net_message* (caller's own memory), a2 = max_spins
                               (bounded poll iterations -- this can take a while, never forever).
                               Requires CAP_NET. Returns 1 and fills *a1 if a message arrived,
                               0 if nothing did before max_spins elapsed, -1 if denied or no
                               network device is present. */
#define SYS_NET_SEND_TO 15 /* a1 = message type, a2 = message data, a3 = target MAC packed into
                               the low 48 bits (byte i at bits 8*i, i=0..5 -- the same order
                               net_message's own sender_mac is unpacked in). Requires CAP_NET.
                               Unicasts {type, data} to exactly one device instead of
                               broadcasting -- Phase 12's addressing scheme: a specific remote
                               MACHINE, still not a specific remote ACTOR (Phase 13). Returns 0
                               on success, -1 if denied or no network device is present. */
#define SYS_NET_SEND_RELIABLE 16 /* Same args as SYS_NET_SEND_TO. BLOCKS, retransmitting
                                     internally, until the destination's own SYS_NET_RECEIVE call
                                     genuinely ACKs this exact message, or a bounded number of
                                     attempts is exhausted (core/net.c). Requires CAP_NET.
                                     Returns 0 once delivery is confirmed, -1 if never acked or
                                     denied or no device present -- Phase 12's reliability
                                     primitive: one guaranteed delivery to one device, not a
                                     stream, not ordering, not multiple in flight at once. */

/* Filled by SYS_NET_RECEIVE. sender_actor is the remote actor's own
 * local slot index on ITS device -- kernel-stamped there the same way
 * struct message's own sender field is stamped locally, so it can't be
 * forged by the remote actor, but it is only meaningful paired with
 * sender_mac (two different devices both have a slot 3). This is real
 * remote identity (a specific actor on a specific device), still not a
 * fabric-wide address -- see core/net.c's own comment. */
struct net_message {
    uint64_t type;
    uint64_t data;
    uint64_t sender_actor;
    uint8_t  sender_mac[6];
};

uint64_t hal_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);

/* Called once per actor, from actor_trampoline() (core/actor.c) the
 * first time it ever runs. Drops from ring 0 to ring 3 via iretq,
 * landing at `entry` with RSP = user_stack_top and interrupts
 * enabled. Never returns: the only way back to ring 0 from here is a
 * syscall or an exception/interrupt, both of which land on this
 * actor's own kernel stack (see hal_set_kernel_stack) and resume
 * ring 3 execution later via their own iretq, not via this function
 * returning. */
void hal_enter_user_mode(uint64_t entry, uint64_t user_stack_top);

/* ---- 8259 PIC ----
 * Remaps IRQ0-15 to vectors 0x20-0x2F (clear of the CPU's own 0-31
 * exception range) and masks everything except IRQ0. Call once,
 * after hal_interrupts_init() has the IDT ready. */
void hal_pic_remap(void);

/* Acknowledges an IRQ so the PIC will deliver further interrupts on
 * that line -- must be called once per IRQ received, before (or
 * instead of, if not returning soon) doing further work in the
 * handler. irq is the IRQ number (0-15), not the remapped vector. */
void hal_pic_send_eoi(uint8_t irq);

/* ---- PIT timer ----
 * Configures the 8253/8254 PIT's channel 0 to fire IRQ0 at
 * approximately frequency_hz. Purely a hardware tick source --
 * interrupts.c is what decides what a tick means (preemption). */
void hal_timer_init(uint32_t frequency_hz);

/* ---- Disk (block device) ----
 * A minimal synchronous, polling ATA PIO driver on the primary IDE
 * bus -- the same disk boot.asm and this project's own build script
 * already put the kernel image on (see tools/build-c.ps1), just
 * accessed from long mode instead of BIOS INT 13h, which isn't
 * available once real mode is left behind. Deliberately not
 * interrupt-driven: every read/write blocks the whole system
 * (interrupts already disabled for the syscall it's reached through
 * anyway) until the hardware responds, which is negligible under
 * QEMU's emulated disk but a real limit on real hardware -- revisit
 * with an interrupt/DMA-driven driver before that matters.
 * count is in 512-byte sectors (1-255); lba is the starting sector.
 * Both return 0 on success, -1 on any failure (bad count, or the
 * hardware reporting an error/never becoming ready). No partitioning
 * or filesystem format is assumed here at all -- core/storage.c is
 * the one and only thing that interprets what these sectors mean. */
int hal_disk_read(uint64_t lba, uint32_t count, void *buf);
int hal_disk_write(uint64_t lba, uint32_t count, const void *buf);

/* ---- Memory map ----
 * A single usable RAM region, in the portable format core/memory.c
 * works with. Each architecture's HAL is responsible for translating
 * whatever its own firmware gives it (E820 on x86 BIOS, a device
 * tree on most ARM boards, etc.) into this common shape. */
struct hal_memory_region {
    uint64_t base;
    uint64_t length;
};

/* Fills out[] with up to max_regions usable RAM regions and returns
 * how many were written. Returns 0 if no valid memory map could be
 * obtained at all (core/memory.c falls back to a conservative
 * default range in that case). */
int hal_get_memory_map(struct hal_memory_region *out, int max_regions);

/* ---- Context switching (cooperative) ----
 * Saves the currently executing context's callee-saved registers
 * onto its own stack, stores the resulting stack pointer into
 * *old_rsp, then switches to new_rsp AND new_cr3 (see
 * hal_address_space_create above) and resumes whatever context was
 * previously suspended there (or, for a never-yet-run actor, a
 * hand-built initial frame -- see actor_spawn in core/actor.c). */
void hal_context_switch(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);

/* ---- PCI (Milestone 13 / roadmap Phase 12) ----
 * Minimal config-space access -- see hal/x86_64/pci.c's own comment
 * for exactly what this does and doesn't cover (bus 0, function 0
 * only). Exists so hal_net_init() can find virtio-net-pci wherever the
 * chipset happened to assign it, instead of assuming a fixed port
 * range the way ATA's driver could. */
struct hal_pci_device {
    uint8_t slot;
    uint16_t io_base;  /* BAR0, I/O-space bit already masked off */
    uint8_t has_msix;  /* shifts legacy virtio device-config offset by +4 -- see pci.c */
};

/* Scans for a device matching (vendor_id, device_id); returns 0 and
 * fills *out on success (also enabling I/O space + bus mastering on
 * it), -1 if nothing matched. */
int hal_pci_find_device(uint16_t vendor_id, uint16_t device_id, struct hal_pci_device *out);

/* ---- Network (Milestone 13 / roadmap Phase 12) ----
 * A minimal legacy virtio-net-pci driver -- deliberately raw Ethernet
 * frames in and out, no IP/UDP/TCP stack above it yet (that's later
 * Phase 12 work, once this HAL layer is proven, the same two-step
 * Phase 8 -> Phase 9/10 already took with storage: a raw driver first,
 * capability-gated actor-facing syscalls on top of it later). Not yet
 * capability-gated or exposed to actor code at all -- called directly
 * from kernel_main, the same place hal_disk_read/write were first
 * exercised before core/storage.c existed. */

/* Finds and initializes the virtio-net-pci device. Returns 0 on
 * success, -1 if no such device is present (e.g. QEMU wasn't started
 * with `-device virtio-net-pci`) -- an honest, expected outcome on a
 * run that didn't ask for networking, not treated as fatal. */
int hal_net_init(void);

/* Copies this device's 6-byte MAC address into mac. Only meaningful
 * after a successful hal_net_init(). */
void hal_net_get_mac(uint8_t mac[6]);

/* Sends one raw Ethernet frame (caller-supplied, starting at the
 * destination MAC address -- no virtio_net_hdr, this function adds
 * that itself). Returns 0 on success, -1 if len is too large for the
 * static TX buffer. */
int hal_net_send(const void *frame, uint32_t len);

/* Polls (does not block indefinitely) for one received Ethernet frame,
 * copying up to max_len bytes into buf (the virtio_net_hdr prefix
 * already stripped -- buf starts at the destination MAC address, same
 * shape as hal_net_send()'s input). Returns the frame length on
 * success, 0 if nothing arrived before max_spins polling iterations
 * elapsed, -1 if max_len was too small for the frame that arrived. */
int hal_net_poll_receive(void *buf, uint32_t max_len, uint32_t max_spins);

/* ---- Misc ---- */
void hal_halt_forever(void);

/* ---- SMP (Milestone 12 / roadmap Phase 10) ----
 * Deliberately narrow: brings up exactly one Application Processor,
 * proven genuinely running in parallel with the BSP, but NOT yet
 * folded into the actor/capability scheduler -- see
 * hal/x86_64/smp.c's own top comment for what's real here and what's
 * explicitly still future work. */

/* This core's own local APIC ID -- the closest thing this kernel has
 * to "which CPU am I", used on-demand rather than cached (see
 * hal/x86_64/smp.c). */
uint32_t hal_lapic_id(void);

/* Enables THIS core's own local APIC -- per-core hardware state, must
 * be called on every core that will send or receive IPIs, not just
 * once. */
void hal_lapic_enable(void);

/* Sends the INIT-then-SIPI sequence to every core but the caller,
 * targeting physical address (trampoline_page * 4096) as the AP's
 * starting CS:IP. Doesn't wait for a response -- see
 * hal_smp_boot_ap() (hal/x86_64/smp.c) for that. */
void hal_lapic_send_init_sipi(uint8_t trampoline_page);

/* Adds the one physical page the local APIC's MMIO registers live at
 * (0xFEE00000, far outside boot.asm's own 0-256MB identity map) to the
 * BOOT page tables -- see hal/x86_64/paging.c's own comment for why
 * only those, not every per-actor address space, need it in this
 * milestone. */
void hal_map_lapic_mmio(void);

/* Activates the AP's own TSS descriptor/selector (this core's
 * counterpart to hal_gdt_init(), which only ever runs on the BSP) --
 * see hal/x86_64/gdt.c's own comment for why a second, genuinely
 * distinct TSS is required, not just reusing the BSP's. */
void hal_gdt_load_ap(uint64_t rsp0);

/* Loads this core's own IDTR, pointed at the SAME already-built IDT
 * the BSP uses -- IDTR is per-core state that does not carry over from
 * one core to another, unlike the table's own contents. */
void hal_idt_load_ap(void);

/* Copies the AP trampoline into place, sends the wake-up IPIs, and
 * waits (bounded) for the AP to report itself alive. Returns 1 if it
 * did, 0 if nothing responded within the wait -- the expected, honest
 * outcome under QEMU's default `-smp 1`, not treated as a fatal error. */
int hal_smp_boot_ap(void);

#endif
