// 7Kern - CMOS real-time clock and calendar helpers
#include "kernel.h"

int g_tz_offset_min = 0;
bool g_clock_24h = true;
uint8_t g_acpi_century_reg;
static int64_t boot_epoch;
static uint64_t boot_ms;

const char *g_month_names[12] = {"January", "February", "March", "April", "May", "June", "July",
                                 "August", "September", "October", "November", "December"};
const char *g_day_names[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

static uint8_t cmos(uint8_t reg) {
    outb(0x70, 0x80 | reg);  // keep NMI disabled while reading
    io_wait();
    return inb(0x71);
}
static bool updating(void) { return cmos(0x0A) & 0x80; }

typedef struct { uint8_t s, m, h, d, mo, y, c; } raw_t;
static void read_raw(raw_t *r) {
    for (int i = 0; i < 100000 && updating(); i++) {}
    r->s = cmos(0x00); r->m = cmos(0x02); r->h = cmos(0x04);
    r->d = cmos(0x07); r->mo = cmos(0x08); r->y = cmos(0x09);
    r->c = g_acpi_century_reg ? cmos(g_acpi_century_reg) : 0;
}

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t dt_to_time(const datetime_t *dt) {
    return days_from_civil(dt->year, dt->month, dt->day) * 86400 + dt->hour * 3600 + dt->minute * 60 + dt->second;
}

void time_to_dt(int64_t t, datetime_t *dt) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    dt->hour = rem / 3600; dt->minute = (rem % 3600) / 60; dt->second = rem % 60;
    dt->weekday = (int)((days + 4) % 7);
    if (dt->weekday < 0) dt->weekday += 7;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    dt->day = doy - (153 * mp + 2) / 5 + 1;
    dt->month = mp < 10 ? mp + 3 : mp - 9;
    dt->year = y + (dt->month <= 2);
}

void rtc_init(void) {
    raw_t a, b;
    read_raw(&a);
    for (int i = 0; i < 5; i++) {
        read_raw(&b);
        if (!memcmp(&a, &b, sizeof a)) break;
        a = b;
    }
    uint8_t regb = cmos(0x0B);
    bool bcd = !(regb & 0x04), h12 = !(regb & 0x02);
    bool pm = a.h & 0x80;
    a.h &= 0x7F;
#define FROM_BCD(v) (((v) & 0x0F) + ((v) >> 4) * 10)
    if (bcd) {
        a.s = FROM_BCD(a.s); a.m = FROM_BCD(a.m); a.h = FROM_BCD(a.h);
        a.d = FROM_BCD(a.d); a.mo = FROM_BCD(a.mo); a.y = FROM_BCD(a.y); a.c = FROM_BCD(a.c);
    }
    if (h12) { if (pm && a.h != 12) a.h += 12; else if (!pm && a.h == 12) a.h = 0; }
    datetime_t dt = {0};
    dt.year = (a.c >= 19 && a.c <= 30) ? a.c * 100 + a.y : 2000 + a.y;
    dt.month = a.mo ? a.mo : 1;
    dt.day = a.d ? a.d : 1;
    dt.hour = a.h; dt.minute = a.m; dt.second = a.s;
    boot_epoch = dt_to_time(&dt);
    boot_ms = timer_ms();
    klog("rtc: %04d-%02d-%02d %02d:%02d:%02d UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

int64_t time_now(void) { return boot_epoch + (int64_t)((timer_ms() - boot_ms) / 1000); }

void time_local(datetime_t *dt) { time_to_dt(time_now() + g_tz_offset_min * 60, dt); }
