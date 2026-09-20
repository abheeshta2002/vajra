#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * Roadmap Phase 10 (completion): finding out how many cores there are.
 *
 * Until now the kernel simply broadcast INIT-SIPI to "every other
 * core" and hoped for exactly one answer, and treated the core index as
 * "whatever the APIC ID happens to be". Firmware already publishes the
 * real list: the ACPI MADT (Multiple APIC Description Table) has one
 * entry per processor with its APIC ID and an "enabled" flag. This file
 * reads just that -- no AML interpreter, no power management, nothing
 * else in ACPI -- by walking RSDP -> RSDT -> the table whose signature
 * is "APIC".
 *
 * Limits, on purpose: only the 32-bit RSDT is used (tables sit below
 * 4GB on every machine this targets), and a table above 256MB is
 * skipped rather than read, because boot.asm's identity map stops there
 * (reading it would page-fault; a machine with >256MB RAM whose ACPI
 * tables live that high falls back to the two-core default below).
 * ---------------------------------------------------------------- */

#define IDENTITY_MAP_LIMIT 0x10000000ULL /* 256MB -- what boot.asm maps */

static int sig_is(const volatile uint8_t *p, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        if (p[i] != (uint8_t)s[i]) {
            return 0;
        }
    }
    return 1;
}

static int checksum_ok(const volatile uint8_t *p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

/* Scans [start, end) on 16-byte boundaries for a valid RSDP. */
static const volatile uint8_t *scan_for_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t a = start; a + 20 <= end; a += 16) {
        const volatile uint8_t *p = (const volatile uint8_t *)a;
        if (sig_is(p, "RSD PTR ", 8) && checksum_ok(p, 20)) {
            return p;
        }
    }
    return 0;
}

/* Fills ids[] with the APIC ID of every enabled processor the MADT
 * lists (up to `max`), BSP included, and returns how many. Returns 0 if
 * ACPI or the MADT can't be found or read -- the caller then falls back
 * to assuming cores 0 and 1. */
int hal_acpi_find_cpus(uint8_t *ids, int max) {
    uint64_t ebda = (uint64_t)(*(volatile uint16_t *)0x40E) << 4;
    const volatile uint8_t *rsdp = 0;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        rsdp = scan_for_rsdp(ebda, ebda + 1024);
    }
    if (!rsdp) {
        rsdp = scan_for_rsdp(0xE0000, 0x100000);
    }
    if (!rsdp) {
        return 0;
    }

    uint64_t rsdt_addr = *(const volatile uint32_t *)(rsdp + 16);
    if (rsdt_addr == 0 || rsdt_addr + 36 > IDENTITY_MAP_LIMIT) {
        return 0;
    }
    const volatile uint8_t *rsdt = (const volatile uint8_t *)rsdt_addr;
    if (!sig_is(rsdt, "RSDT", 4)) {
        return 0;
    }
    uint32_t rsdt_len = *(const volatile uint32_t *)(rsdt + 4);
    if (rsdt_len < 36 || rsdt_addr + rsdt_len > IDENTITY_MAP_LIMIT) {
        return 0;
    }

    uint32_t entries = (rsdt_len - 36) / 4;
    for (uint32_t e = 0; e < entries; e++) {
        uint64_t t_addr = *(const volatile uint32_t *)(rsdt + 36 + e * 4);
        if (t_addr == 0 || t_addr + 44 > IDENTITY_MAP_LIMIT) {
            continue;
        }
        const volatile uint8_t *t = (const volatile uint8_t *)t_addr;
        if (!sig_is(t, "APIC", 4)) {
            continue;
        }
        uint32_t len = *(const volatile uint32_t *)(t + 4);
        if (len < 44 || t_addr + len > IDENTITY_MAP_LIMIT || !checksum_ok(t, len)) {
            return 0;
        }

        int count = 0;
        uint32_t off = 44; /* 36-byte header + local APIC address + flags */
        while (off + 2 <= len) {
            uint8_t type = t[off];
            uint8_t rlen = t[off + 1];
            if (rlen < 2) {
                break;
            }
            if (type == 0 && rlen >= 8) { /* processor local APIC */
                uint8_t apic_id = t[off + 3];
                uint32_t flags = *(const volatile uint32_t *)(t + off + 4);
                if ((flags & 1) && count < max) { /* enabled */
                    ids[count++] = apic_id;
                }
            }
            off += rlen;
        }
        return count;
    }
    return 0;
}
