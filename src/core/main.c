/* Minimal freestanding C "kernel" - just proves the toolchain works:
 * compiles freestanding, links against the asm entry stub, boots via
 * the existing proven boot.asm, and can write directly to hardware
 * (VGA text memory) with no OS underneath it. */

void kernel_main(void) {
    volatile unsigned short *vga = (unsigned short *)0xB8000;
    const char *msg = "Hello from C! Vajra C toolchain OK.";

    int i = 0;
    while (msg[i] != '\0') {
        /* low byte = ASCII char, high byte = VGA attribute (0x0A = green on black) */
        vga[i] = (unsigned short)msg[i] | (0x0A << 8);
        i++;
    }

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
