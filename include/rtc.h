#ifndef __RTC_H
#define __RTC_H

#include <stdint.h>
#include <stdbool.h>

// CMOS/RTC wall clock (the battery-backed real-time clock behind I/O ports
// 0x70/0x71). Unlike ktime (monotonic since boot), this is the actual date and
// time the firmware keeps — used for timestamps and TLS certificate validity.
// The RTC is read as UTC (as good firmware, and QEMU, keep it).

struct rtc_time {
    int year;   // full year, e.g. 2026
    int month;  // 1-12
    int day;    // 1-31
    int hour;   // 0-23
    int minute; // 0-59
    int second; // 0-59
};

// Read the current wall-clock time. Returns false if the RTC gave no plausible
// value (then *out is left unspecified).
bool rtc_read(struct rtc_time* out);

// Seconds since the Unix epoch (1970-01-01T00:00:00 UTC), or 0 if unavailable.
uint64_t rtc_epoch(void);

#endif
