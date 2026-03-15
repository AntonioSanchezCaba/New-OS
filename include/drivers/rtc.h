/*
 * drivers/rtc.h - CMOS Real-Time Clock driver
 *
 * Reads date/time from the MC146818 RTC via CMOS ports 0x70/0x71.
 * Provides wall-clock time with configurable UTC offset (timezone).
 */
#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H

#include <types.h>

/* Date/time structure */
typedef struct {
    uint16_t year;      /* e.g. 2026 */
    uint8_t  month;     /* 1-12 */
    uint8_t  day;       /* 1-31 */
    uint8_t  hour;      /* 0-23 (local time after tz offset) */
    uint8_t  minute;    /* 0-59 */
    uint8_t  second;    /* 0-59 */
    uint8_t  weekday;   /* 0=Sun, 1=Mon, ... 6=Sat */
} rtc_time_t;

/* Initialize the RTC driver (reads initial time) */
void rtc_init(void);

/* Read current time from CMOS RTC (UTC) and apply timezone offset */
void rtc_get_time(rtc_time_t* t);

/* Set timezone offset in minutes from UTC (e.g. -300 for EST, +60 for CET) */
void rtc_set_tz_offset(int16_t offset_minutes);

/* Get current timezone offset in minutes */
int16_t rtc_get_tz_offset(void);

/* Check if timezone has been configured (first-boot detection) */
bool rtc_tz_configured(void);

/* Mark timezone as configured */
void rtc_set_tz_configured(bool configured);

#endif /* DRIVERS_RTC_H */
