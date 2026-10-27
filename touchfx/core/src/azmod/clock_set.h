/* SPDX-License-Identifier: GPL-3.0-only */
/* clock_set.h: the Clock's Set Time Manually picker, the pure part: the fields a user steps
 * (local date and time in the Clock's zone), their conversion to and from UTC through the zone
 * (clock_tz.h), whether the system time can be shown, and where the picker starts. No I/O, no
 * global state. */
#ifndef CLOCK_SET_H
#define CLOCK_SET_H
#include <stdint.h>
#include "clock_tz.h"

enum {
    CS_YEAR,
    CS_MONTH,
    CS_DAY,
    CS_HOUR,
    CS_MINUTE,
    CS_NFIELD
};
#define CS_YEAR_MIN 2024
#define CS_YEAR_MAX 2099
#define CS_T_MIN 1704067200LL /* 2024-01-01 00:00 UTC: an earlier time is an unset clock */
#define CS_SLACK 5            /* seconds a time may sit before the last manual set and still count */

struct cs_fields {
    int v[CS_NFIELD]; /* year, month 1-12, day 1-31, hour 0-23, minute 0-59 */
};

static inline int cs_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}
static inline int cs_dim(int y, int m) {
    static const signed char D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && cs_leap(y) ? 29 : m >= 1 && m <= 12 ? D[m - 1]
                                                         : 0;
}
static inline void cs_range(const struct cs_fields *f, int k, int *lo, int *hi) {
    static const int LO[CS_NFIELD] = {CS_YEAR_MIN, 1, 1, 0, 0}, HI[CS_NFIELD] = {CS_YEAR_MAX, 12, 31, 23, 59};
    *lo = LO[k];
    *hi = k == CS_DAY ? cs_dim(f->v[CS_YEAR], f->v[CS_MONTH]) : HI[k];
}
/* Every field inside its range, the day inside its month. */
static inline void cs_clamp(struct cs_fields *f) {
    for (int k = 0; k < CS_NFIELD; k++) {
        int lo, hi;
        cs_range(f, k, &lo, &hi);
        f->v[k] = f->v[k] < lo ? lo : f->v[k] > hi ? hi
                                                   : f->v[k];
    }
}
/* One field up or down by d, wrapping inside its own range (no carry into the next field); a
 * month or year change keeps the day inside the new month. */
static inline void cs_step(struct cs_fields *f, int k, int d) {
    if (k < 0 || k >= CS_NFIELD)
        return;
    cs_clamp(f);
    int lo, hi;
    cs_range(f, k, &lo, &hi);
    int n = hi - lo + 1, r = (f->v[k] - lo + d % n + n) % n;
    f->v[k] = lo + r;
    cs_clamp(f);
}

/* Days since 1970-01-01 of a civil date, and back (proleptic Gregorian). */
static inline int64_t cs_days(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static inline void cs_civil(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int) (doy - (153 * mp + 2) / 5 + 1);
    *m = (int) (mp < 10 ? mp + 3 : mp - 9);
    *y = (int) (yoe + era * 400 + (*m <= 2));
}
/* The fields of UTC time t at offset off, clamped to the picker's range. */
static inline void cs_from_utc(int64_t t, int32_t off, struct cs_fields *f) {
    int64_t l = t + off, day = (l >= 0 ? l : l - 86399) / 86400, sec = l - day * 86400;
    cs_civil(day, &f->v[CS_YEAR], &f->v[CS_MONTH], &f->v[CS_DAY]);
    f->v[CS_HOUR] = (int) (sec / 3600);
    f->v[CS_MINUTE] = (int) (sec % 3600 / 60);
    cs_clamp(f);
}
/* The UTC time of local fields f in zone z (z NULL: UTC itself): 0 and *t, or -1 when the
 * zone cannot answer. A local time the zone skips (a spring-forward hour) lands one hour on. */
static inline int cs_to_utc(const struct ctz *z, const struct cs_fields *f, int64_t *t) {
    int64_t l = cs_days(f->v[CS_YEAR], f->v[CS_MONTH], f->v[CS_DAY]) * 86400 + (int64_t) f->v[CS_HOUR] * 3600 + (int64_t) f->v[CS_MINUTE] * 60;
    if (!z) {
        *t = l;
        return 0;
    }
    int32_t o1, o2, o3;
    if (ctz_offset(z, l, &o1) != 0 || ctz_offset(z, l - o1, &o2) != 0 || ctz_offset(z, l - o2, &o3) != 0)
        return -1;
    *t = l - (o3 != o2 ? o3 : o2); /* o3 differs only for a skipped local time */
    return 0;
}

/* Whether the system time can be shown: synchronized (NTP), or not before the last manual set
 * (marker: its time, 0 = none). A clock that fell back below it (a power-off reset the RTC) is
 * not set. */
static inline int cs_valid(int sync, int64_t marker, int64_t now) {
    return sync || (marker >= CS_T_MIN && now >= marker - CS_SLACK);
}
/* Where the picker starts: the time itself when it can be shown, else the newest file time on
 * a stick (the last export), else the start of CS_YEAR_MIN. */
static inline int64_t cs_seed(int valid, int64_t now, int64_t newest) {
    return valid ? now : newest >= CS_T_MIN ? newest
                                            : CS_T_MIN;
}
#endif
