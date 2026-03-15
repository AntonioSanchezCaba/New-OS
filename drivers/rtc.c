/*
 * drivers/rtc.c - CMOS Real-Time Clock driver
 *
 * Reads date/time from the MC146818 RTC chip via CMOS ports 0x70/0x71.
 * Supports both UTC and local-time hardware clocks (common on VirtualBox
 * running on Windows where the RTC mirrors the host's local time).
 */
#include <drivers/rtc.h>
#include <kernel.h>
#include <types.h>

/* CMOS I/O ports */
#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71

/* CMOS register indices */
#define RTC_SEC     0x00
#define RTC_MIN     0x02
#define RTC_HOUR    0x04
#define RTC_WDAY    0x06
#define RTC_MDAY    0x07
#define RTC_MON     0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32    /* may not exist on all hardware */
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

/*
 * CMOS NVRAM persistence for timezone settings.
 * We use four otherwise-unused CMOS registers:
 *   0x38: magic byte (0xAE = "AEtherOS timezone set")
 *   0x39: timezone offset high byte  (int16_t big-endian)
 *   0x3A: timezone offset low byte
 *   0x3B: flags — bit 0: RTC is UTC (1) or local time (0)
 */
#define CMOS_TZ_MAGIC   0x38
#define CMOS_TZ_HI      0x39
#define CMOS_TZ_LO      0x3A
#define CMOS_TZ_FLAGS   0x3B
#define TZ_MAGIC_VALUE  0xAE
#define TZ_FLAG_UTC     0x01

/* State */
static int16_t g_tz_offset    = 0;     /* minutes: desired_tz from UTC */
static bool    g_tz_configured = false;
static bool    g_rtc_is_utc   = false; /* true if hardware clock is UTC */

/* Read a single CMOS register */
static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

/* Write a single CMOS register */
static void cmos_write(uint8_t reg, uint8_t val)
{
    outb(CMOS_ADDR, reg);
    io_wait();
    outb(CMOS_DATA, val);
}

/* Save all timezone settings to CMOS NVRAM */
static void cmos_save_all(void)
{
    cmos_write(CMOS_TZ_MAGIC, TZ_MAGIC_VALUE);
    cmos_write(CMOS_TZ_HI, (uint8_t)((uint16_t)g_tz_offset >> 8));
    cmos_write(CMOS_TZ_LO, (uint8_t)((uint16_t)g_tz_offset & 0xFF));
    cmos_write(CMOS_TZ_FLAGS, g_rtc_is_utc ? TZ_FLAG_UTC : 0);
}

/* Load timezone settings from CMOS NVRAM. Returns true if valid. */
static bool cmos_load_all(void)
{
    if (cmos_read(CMOS_TZ_MAGIC) != TZ_MAGIC_VALUE)
        return false;
    uint8_t hi = cmos_read(CMOS_TZ_HI);
    uint8_t lo = cmos_read(CMOS_TZ_LO);
    uint8_t fl = cmos_read(CMOS_TZ_FLAGS);
    g_tz_offset  = (int16_t)((uint16_t)hi << 8 | lo);
    g_rtc_is_utc = (fl & TZ_FLAG_UTC) != 0;
    return true;
}

/* Check if RTC update is in progress */
static bool rtc_updating(void)
{
    return (cmos_read(RTC_STATUS_A) & 0x80) != 0;
}

/* Convert BCD to binary */
static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

/* Days in each month (non-leap) */
static const uint8_t days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static bool is_leap_year(uint16_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static uint8_t month_days(uint16_t year, uint8_t month)
{
    if (month == 2 && is_leap_year(year)) return 29;
    if (month >= 1 && month <= 12) return days_in_month[month - 1];
    return 30;
}

/* Compute day of week (0=Sun) using Tomohiko Sakamoto's algorithm */
static uint8_t calc_weekday(uint16_t y, uint8_t m, uint8_t d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (uint8_t)((y + y/4 - y/100 + y/400 + t[m-1] + d) % 7);
}

void rtc_init(void)
{
    /* Try to restore timezone from CMOS NVRAM */
    if (cmos_load_all()) {
        g_tz_configured = true;
    } else {
        g_tz_offset    = 0;
        g_rtc_is_utc   = false; /* default: assume local time (VirtualBox) */
        g_tz_configured = false;
    }
}

void rtc_get_time(rtc_time_t* t)
{
    uint8_t sec, min, hour, day, mon, year, century;

    /* Wait for any in-progress update to finish */
    while (rtc_updating())
        ;

    /* Read values */
    sec     = cmos_read(RTC_SEC);
    min     = cmos_read(RTC_MIN);
    hour    = cmos_read(RTC_HOUR);
    day     = cmos_read(RTC_MDAY);
    mon     = cmos_read(RTC_MON);
    year    = cmos_read(RTC_YEAR);
    century = cmos_read(RTC_CENTURY);

    /* Read status register B to determine format */
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    bool is_bcd    = !(status_b & 0x04);
    bool is_12hour = !(status_b & 0x02);
    bool pm = false;

    if (is_12hour) {
        pm = (hour & 0x80) != 0;
        hour &= 0x7F;
    }

    /* Convert BCD to binary if needed */
    if (is_bcd) {
        sec     = bcd_to_bin(sec);
        min     = bcd_to_bin(min);
        hour    = bcd_to_bin(hour);
        day     = bcd_to_bin(day);
        mon     = bcd_to_bin(mon);
        year    = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    /* Convert 12-hour to 24-hour */
    if (is_12hour) {
        if (hour == 12) hour = 0;
        if (pm) hour += 12;
    }

    /* Construct full year */
    uint16_t full_year;
    if (century > 0 && century < 100) {
        full_year = (uint16_t)century * 100 + year;
    } else {
        full_year = 2000 + year;
    }

    /*
     * Apply timezone offset.
     *
     * If RTC is UTC:   local_time = RTC + tz_offset
     * If RTC is local: local_time = RTC  (offset = 0 effectively,
     *                  the tz_offset is only stored for display label)
     *
     * When RTC is local time (VirtualBox/Windows), the hardware already
     * shows the correct local time, so we don't apply any arithmetic
     * offset — the stored tz_offset is only used for the UTC label.
     */
    int apply_offset = g_rtc_is_utc ? (int)g_tz_offset : 0;
    int total_min = (int)hour * 60 + (int)min + apply_offset;

    /* Handle day rollover from timezone */
    int day_delta = 0;
    while (total_min < 0) {
        total_min += 1440;
        day_delta--;
    }
    while (total_min >= 1440) {
        total_min -= 1440;
        day_delta++;
    }

    hour = (uint8_t)(total_min / 60);
    min  = (uint8_t)(total_min % 60);

    /* Adjust date if timezone caused day change */
    int iday = (int)day + day_delta;
    int imon = (int)mon;
    int iyear = (int)full_year;

    if (iday < 1) {
        imon--;
        if (imon < 1) { imon = 12; iyear--; }
        iday = (int)month_days((uint16_t)iyear, (uint8_t)imon) + iday;
    } else {
        int md = (int)month_days((uint16_t)iyear, (uint8_t)imon);
        if (iday > md) {
            iday -= md;
            imon++;
            if (imon > 12) { imon = 1; iyear++; }
        }
    }

    t->year   = (uint16_t)iyear;
    t->month  = (uint8_t)imon;
    t->day    = (uint8_t)iday;
    t->hour   = hour;
    t->minute = min;
    t->second = sec;
    t->weekday = calc_weekday(t->year, t->month, t->day);
}

void rtc_set_tz_offset(int16_t offset_minutes)
{
    g_tz_offset = offset_minutes;
    cmos_save_all();
}

int16_t rtc_get_tz_offset(void)
{
    return g_tz_offset;
}

void rtc_set_utc_hwclock(bool is_utc)
{
    g_rtc_is_utc = is_utc;
    cmos_save_all();
}

bool rtc_get_utc_hwclock(void)
{
    return g_rtc_is_utc;
}

bool rtc_tz_configured(void)
{
    return g_tz_configured;
}

void rtc_set_tz_configured(bool configured)
{
    g_tz_configured = configured;
}
