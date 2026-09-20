#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * CMOS real-time clock -- roadmap Phase 18. Polled, not interrupt-
 * driven (RTC's own periodic-interrupt mode is unrelated to this --
 * this driver just reads the current wall-clock value on demand, the
 * same "synchronous, called when asked" shape hal_disk_read/write
 * already have). No time-keeping state lives in this kernel at all;
 * every read goes straight to the hardware register set QEMU/real
 * firmware already maintains.
 * ---------------------------------------------------------------- */

#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT  0x71

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX_PORT, reg);
    return inb(CMOS_DATA_PORT);
}

static int rtc_update_in_progress(void) {
    return (cmos_read(0x0A) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

void hal_rtc_read(struct rtc_time *out) {
    /* Register 0x0A bit 7 is set while the RTC is mid-update -- a read
     * caught in that window can return a torn value (e.g. seconds
     * rolled over but minutes hasn't yet). Bounded, not infinite: a
     * stuck RTC would otherwise hang the caller forever, the same
     * "poll, don't trust an unbounded wait" discipline core/net.c's
     * SYS_NET_RECEIVE already applies. */
    for (int spin = 0; spin < 1000000 && rtc_update_in_progress(); spin++) {
    }

    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t yr   = cmos_read(0x09);
    uint8_t regB = cmos_read(0x0B);

    if (!(regB & 0x04)) { /* BCD mode (the common default) -- convert to binary */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = (uint8_t)(bcd_to_bin(hour & 0x7F) | (hour & 0x80));
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
    }
    if (!(regB & 0x02) && (hour & 0x80)) { /* 12-hour mode, PM bit set */
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    }

    out->seconds = sec;
    out->minutes = min;
    out->hours   = (uint8_t)(hour & 0x7F);
    out->day     = day;
    out->month   = mon;
    out->year    = (uint16_t)(2000 + yr); /* CMOS gives a 2-digit year -- 21st century assumed,
                                              the same shortcut nearly every small kernel/BIOS
                                              takes; not meaningful past 2099 */
}

/* Seconds since 2000-01-01 00:00:00 (UTC as the CMOS reports it), for file
 * timestamps. Fits a uint32 until 2136. Civil-date to day-count by the
 * standard era arithmetic (no tables, no leap-year special cases beyond it). */
uint32_t hal_rtc_epoch(void) {
    struct rtc_time t;
    hal_rtc_read(&t);
    int y = (int)t.year;
    int m = (int)t.month;
    int d = (int)t.day;
    y -= (m <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int days_since_1970 = era * 146097 + doe - 719468;
    int days = days_since_1970 - 10957; /* 1970-01-01 .. 2000-01-01 */
    if (days < 0) {
        days = 0;
    }
    return (uint32_t)days * 86400u + (uint32_t)t.hours * 3600u + (uint32_t)t.minutes * 60u + (uint32_t)t.seconds;
}
