#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * Minimal ATA PIO driver, primary bus, master drive, 28-bit LBA.
 * Synchronous/polling -- see hal.h's own comment on why that's an
 * honest, deliberate limit rather than an oversight.
 *
 * This is the same disk boot.asm reads the kernel image from via
 * BIOS INT 13h; the two never run at the same time (INT 13h is real-
 * mode-only and long since left behind by the time any of this runs),
 * so there's no conflict, just two different ways of reaching the
 * same physical disk at two different points in boot.
 * ---------------------------------------------------------------- */

#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA_LOW    0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HIGH   0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7

#define ATA_CMD_READ  0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_FLUSH 0xE7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_TIMEOUT_SPINS 1000000

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ __volatile__("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* Reading the status port 4 times after selecting a drive is a
 * standard ~400ns settle delay (the read's result is discarded --
 * only the act of reading the I/O port takes the time). */
static void ata_delay(void) {
    for (int i = 0; i < 4; i++) {
        inb(ATA_STATUS);
    }
}

static int ata_wait_not_busy(void) {
    for (int spins = 0; spins < ATA_TIMEOUT_SPINS; spins++) {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int spins = 0; spins < ATA_TIMEOUT_SPINS; spins++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) {
            return -1;
        }
        if (status & ATA_SR_DRQ) {
            return 0;
        }
    }
    return -1;
}

static int ata_setup(uint64_t lba, uint32_t count, uint8_t command) {
    if (count == 0 || count > 255) {
        return -1;
    }
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F))); /* master, LBA mode */
    ata_delay();
    outb(ATA_SECCOUNT, (uint8_t)count);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, command);

    return 0;
}

int hal_disk_read(uint64_t lba, uint32_t count, void *buf) {
    if (ata_setup(lba, count, ATA_CMD_READ) != 0) {
        return -1;
    }

    uint16_t *p = (uint16_t *)buf;
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            p[i] = inw(ATA_DATA);
        }
        p += 256;
    }

    return 0;
}

int hal_disk_write(uint64_t lba, uint32_t count, const void *buf) {
    if (ata_setup(lba, count, ATA_CMD_WRITE) != 0) {
        return -1;
    }

    const uint16_t *p = (const uint16_t *)buf;
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) {
            return -1;
        }
        for (int i = 0; i < 256; i++) {
            outw(ATA_DATA, p[i]);
        }
        p += 256;
    }

    /* Make sure the write actually lands before reporting success --
     * FLUSH CACHE only makes sense once the drive is done with the
     * write itself (not busy), and the flush's own completion needs
     * the same wait. */
    if (ata_wait_not_busy() != 0) {
        return -1;
    }
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    if (ata_wait_not_busy() != 0) {
        return -1;
    }

    return 0;
}
