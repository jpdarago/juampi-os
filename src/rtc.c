// CMOS/RTC driver (see rtc.h). The real-time clock lives behind the CMOS
// index/data port pair (0x70/0x71): write a register number to 0x70, read/write
// its value at 0x71. Register A's top bit (UIP) flags an update in progress, so
// we read the time fields only when it's clear and read twice to rule out a
// tick landing mid-read. Register B says whether the fields are BCD or binary
// and 12- or 24-hour. This is the one thing that gives juampiOS a real wall
// clock; ktime is only monotonic-since-boot.

#include <rtc.h>
#include <ports.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS 0x04
#define RTC_DAY 0x07
#define RTC_MONTH 0x08
#define RTC_YEAR 0x09
#define RTC_CENTURY 0x32 // ACPI century register (QEMU + most modern boards)
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

#define STATUS_A_UIP 0x80 // update in progress
#define STATUS_B_24H 0x02 // 1 = 24-hour hours field
#define STATUS_B_BIN 0x04 // 1 = binary fields (else BCD)

static uint8_t cmos_read(uint8_t reg)
{
    // Preserve the NMI-disable bit (0x80) as clear by writing the bare index.
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static bool update_in_progress(void)
{
    return (cmos_read(RTC_STATUS_A) & STATUS_A_UIP) != 0;
}

static int from_bcd(uint8_t v)
{
    return (v & 0x0F) + (v >> 4) * 10;
}

// One coherent snapshot of the raw time registers (taken with UIP clear).
struct raw {
    uint8_t sec, min, hour, day, mon, year, cent;
};

static struct raw read_raw(void)
{
    struct raw r;
    r.sec = cmos_read(RTC_SECONDS);
    r.min = cmos_read(RTC_MINUTES);
    r.hour = cmos_read(RTC_HOURS);
    r.day = cmos_read(RTC_DAY);
    r.mon = cmos_read(RTC_MONTH);
    r.year = cmos_read(RTC_YEAR);
    r.cent = cmos_read(RTC_CENTURY);
    return r;
}

static bool raw_eq(struct raw a, struct raw b)
{
    return a.sec == b.sec && a.min == b.min && a.hour == b.hour &&
           a.day == b.day && a.mon == b.mon && a.year == b.year &&
           a.cent == b.cent;
}

bool rtc_read(struct rtc_time* out)
{
    if (out == NULL) {
        return false;
    }

    // Wait for any update to finish, then read; re-read until two consecutive
    // snapshots agree, so a tick can't split the fields. Bounded so a dead RTC
    // can't hang us.
    struct raw a, b;
    int tries = 0;
    do {
        int spins = 0;
        while (update_in_progress() && spins++ < 1000000) {
        }
        a = read_raw();
        while (update_in_progress() && spins++ < 1000000) {
        }
        b = read_raw();
    } while (!raw_eq(a, b) && ++tries < 8);

    uint8_t statusb = cmos_read(RTC_STATUS_B);
    bool binary = (statusb & STATUS_B_BIN) != 0;
    bool h24 = (statusb & STATUS_B_24H) != 0;

    int sec, min, hour, day, mon, year2, cent;
    bool pm = false;
    if (!h24) {
        pm = (b.hour & 0x80) != 0;
        b.hour &= 0x7F;
    }
    if (binary) {
        sec = b.sec;
        min = b.min;
        hour = b.hour;
        day = b.day;
        mon = b.mon;
        year2 = b.year;
        cent = b.cent;
    } else {
        sec = from_bcd(b.sec);
        min = from_bcd(b.min);
        hour = from_bcd(b.hour);
        day = from_bcd(b.day);
        mon = from_bcd(b.mon);
        year2 = from_bcd(b.year);
        cent = from_bcd(b.cent);
    }
    if (!h24) {
        // 12-hour: 12 AM -> 0, 12 PM stays 12, other PM hours += 12.
        if (hour == 12) {
            hour = 0;
        }
        if (pm) {
            hour += 12;
        }
    }

    int year;
    if (cent >= 19 && cent <= 21) {
        year = cent * 100 + year2; // valid century register
    } else {
        year = (year2 < 70 ? 2000 : 1900) + year2; // pivot fallback
    }

    // Sanity-check before trusting it.
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 ||
        sec > 59 || year < 1970 || year > 2200) {
        return false;
    }

    out->year = year;
    out->month = mon;
    out->day = day;
    out->hour = hour;
    out->minute = min;
    out->second = sec;
    return true;
}

// Days since 1970-01-01 for a civil (proleptic Gregorian) date. Howard
// Hinnant's algorithm; valid across the whole range we allow.
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                  // [0, 399]
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + doe - 719468;
}

uint64_t rtc_epoch(void)
{
    struct rtc_time t;
    if (!rtc_read(&t)) {
        return 0;
    }
    int64_t days = days_from_civil(t.year, t.month, t.day);
    int64_t secs = days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    return secs < 0 ? 0 : (uint64_t)secs;
}
