/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file cm_browse.h
 * @brief Community Mods > Browse: which catalog mods show, and in what order.
 *
 * The Browse tab opens on Home, a list of shelves, or shows the whole catalog (All mods) or one
 * category, ordered one of four ways. Every input comes from the
 * fetcher's catalog line (screen_community.c cm_load), so nothing here invents data: a mod with
 * no category is under Other, a mod with no valid updated date sorts after every dated one, and a
 * mod with no download count (a repository added by hand, or no counts fetched yet) after every
 * counted one.
 *
 * Categories: the manifest's optional `category`, one of a fixed list (the fetcher drops any
 * other value). Anything else, or none, is Other.
 *
 * Order: by name (A to Z, numbers by value), by updated date (newest first), installed mods first,
 * or by downloads (most first). In every order a mod that cannot be installed on this deck goes
 * after the ones that can (as the tab always did), except that "installed first" keeps every
 * installed mod at the top. Ties keep the catalog's order, so the result is stable.
 *
 * Home: shelves of at most CMB_SHELF_MAX mods each, in this order: Most downloaded (mods with a
 * count above 0), New & updated (mods with an updated date), then one per category (downloads,
 * then A to Z). A shelf with fewer than CMB_SHELF_MIN mods is not
 * shown. "See all" on a shelf opens All mods (or the shelf's category) in the shelf's order.
 *
 * A download count is the Mod Index's count of downloads of the mod's package over all its
 * versions (the fetcher's catalog field 21): digits only, at most 13 of them.
 *
 * Pure logic: no globals, no allocation; the caller owns every string.
 */
#ifndef AZMOD_CM_BROWSE_H
#define AZMOD_CM_BROWSE_H

#include <stddef.h>

#define CMB_NCAT 5  /* the fixed categories, Other last */
#define CMB_OTHER 4 /* no category, or one not in the list */
#define CMB_ALL (-1)
#define CMB_HOME (-2) /* Browse's Home view: the shelves, not a category */
enum {
    CMB_SORT_NAME = 0,
    CMB_SORT_UPDATED,
    CMB_SORT_INSTALLED,
    CMB_SORT_DOWNLOADS,
    CMB_NSORT
};

/* the manifest keys, in display order (Other has none) */
static const char *const cmb_cat_key[CMB_NCAT - 1] = {"visual", "controls", "audio", "tools"};
static const char *const cmb_cat_label[CMB_NCAT] = {"Visual", "Controls", "Audio", "Tools", "Other"};
static const char *const cmb_sort_label[CMB_NSORT] = {"A-Z", "Updated", "Installed", "Downloads"};

typedef struct {
    const char *name;
    const char *cat;     /* the manifest's category value, or "" */
    const char *updated; /* YYYY-MM-DD, or "" */
    int installed;       /* installed on this USB */
    int usable;          /* can be installed on this deck */
    int counted;         /* it has a download count */
    long long downloads; /* the count, when counted */
} cmb_mod;

static inline int cmb_lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

/* the category index of a manifest value; NULL, "", or a value not in the list: Other */
static inline int cmb_cat_of(const char *v) {
    if (!v)
        return CMB_OTHER;
    for (int k = 0; k < CMB_NCAT - 1; k++)
        if (!__builtin_strcmp(v, cmb_cat_key[k]))
            return k;
    return CMB_OTHER;
}

/* 1 when v is a date: YYYY-MM-DD, month 01 to 12, day 01 to 31 */
static inline int cmb_date_ok(const char *v) {
    if (!v)
        return 0;
    for (int i = 0; i < 10; i++) {
        int dash = i == 4 || i == 7;
        if (dash ? v[i] != '-' : (v[i] < '0' || v[i] > '9'))
            return 0;
    }
    if (v[10])
        return 0;
    int mo = (v[5] - '0') * 10 + (v[6] - '0'), d = (v[8] - '0') * 10 + (v[9] - '0');
    return mo >= 1 && mo <= 12 && d >= 1 && d <= 31;
}

/* a download count from the catalog: 1 and *out when v is 1 to 13 digits, else 0 */
static inline int cmb_dl_parse(const char *v, long long *out) {
    if (!v || !*v)
        return 0;
    long long n = 0;
    int i = 0;
    for (; v[i]; i++) {
        if (i >= 13 || v[i] < '0' || v[i] > '9')
            return 0;
        n = n * 10 + (v[i] - '0');
    }
    *out = n;
    return 1;
}

/* a count with a comma every three digits ("1,840"), in b (room for n, at least 24 bytes) */
static inline void cmb_count_text(long long v, char *b, size_t n) {
    char t[32];
    int k = 0, d = 0;
    if (v < 0)
        v = 0;
    do {
        if (d && d % 3 == 0)
            t[k++] = ',';
        t[k++] = (char) ('0' + v % 10);
        v /= 10;
        d++;
    } while (v && k < 30);
    size_t o = 0;
    while (k > 0 && o + 1 < n)
        b[o++] = t[--k];
    if (n)
        b[o] = 0;
}

/* the days from 1970-01-01 to a date cmb_date_ok accepts (the Gregorian calendar) */
static inline long cmb_days(const char *v) {
    long y = (v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0');
    long m = (v[5] - '0') * 10 + (v[6] - '0'), d = (v[8] - '0') * 10 + (v[9] - '0');
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* A mod page's "updated ..." from its date and the deck's clock (seconds since 1970): "updated
 * today", "updated yesterday", "updated N days ago"; the date itself when the clock is not set
 * (before 2024), the date is ahead of it, or it is over ten years back; "" with no valid date. */
#define CMB_CLOCK_SET 1704067200LL /* 2024-01-01 UTC */
static inline void cmb_updated_text(const char *upd, long long now, char *b, size_t n) {
    static const char p[] = "updated ";
    char t[40];
    size_t k = 0;
    if (!n)
        return;
    b[0] = 0;
    if (!cmb_date_ok(upd))
        return;
    long age = now >= CMB_CLOCK_SET ? (long) (now / 86400) - cmb_days(upd) : -1;
    for (const char *s = p; *s; s++)
        t[k++] = *s;
    if (age < 0 || age > 3650) {
        for (int i = 0; i < 10; i++)
            t[k++] = upd[i];
    } else if (age <= 1) {
        for (const char *s = age ? "yesterday" : "today"; *s; s++)
            t[k++] = *s;
    } else {
        char dg[8];
        int nd = 0;
        for (long a = age; a; a /= 10)
            dg[nd++] = (char) ('0' + a % 10);
        while (nd)
            t[k++] = dg[--nd];
        for (const char *s = " days ago"; *s; s++)
            t[k++] = *s;
    }
    size_t o = 0;
    for (; o < k && o + 1 < n; o++)
        b[o] = t[o];
    b[o] = 0;
}

static inline int cmb_digit(char c) { return c >= '0' && c <= '9'; }

/* the names in A to Z order, ignoring ASCII case, with a run of digits compared by its value
 * ("Mod 2" before "Mod 10"; leading zeros do not count): <0, 0, >0 */
static inline int cmb_namecmp(const char *a, const char *b) {
    a = a ? a : "";
    b = b ? b : "";
    for (;;) {
        if (cmb_digit(*a) && cmb_digit(*b)) {
            while (*a == '0')
                a++;
            while (*b == '0')
                b++;
            size_t la = 0, lb = 0;
            while (cmb_digit(a[la]))
                la++;
            while (cmb_digit(b[lb]))
                lb++;
            if (la != lb)
                return la < lb ? -1 : 1;
            for (size_t i = 0; i < la; i++)
                if (a[i] != b[i])
                    return a[i] - b[i];
            a += la;
            b += lb;
            continue;
        }
        int x = cmb_lower((unsigned char) *a), y = cmb_lower((unsigned char) *b);
        if (x != y || !x)
            return x - y;
        a++;
        b++;
    }
}

/* <0 when a goes before b in `sort` order, >0 after, 0 a tie */
static inline int cmb_cmp(const cmb_mod *a, const cmb_mod *b, int sort) {
    if (sort == CMB_SORT_INSTALLED && !a->installed != !b->installed)
        return a->installed ? -1 : 1;
    if (!a->usable != !b->usable)
        return a->usable ? -1 : 1;
    if (sort == CMB_SORT_DOWNLOADS) {
        if (!a->counted != !b->counted)
            return a->counted ? -1 : 1;
        if (a->counted && a->downloads != b->downloads)
            return a->downloads > b->downloads ? -1 : 1; /* most first */
    }
    if (sort == CMB_SORT_UPDATED) {
        int da = cmb_date_ok(a->updated), db = cmb_date_ok(b->updated);
        if (da != db)
            return da ? -1 : 1;
        if (da) {
            int c = __builtin_strcmp(b->updated, a->updated); /* newest first */
            if (c)
                return c;
        }
    }
    return cmb_namecmp(a->name, b->name);
}

/* 1 when mod m shows under category `cat` (CMB_ALL: every one) */
static inline int cmb_shown(const cmb_mod *m, int cat) { return cat == CMB_ALL || cmb_cat_of(m->cat) == cat; }

/* The mods to show, in order: their indexes into m[0..n) in out[] (room for n); returns how
 * many. An unknown sort is taken as by name. */
static inline int cmb_view(const cmb_mod *m, int n, int cat, int sort, int *out) {
    if (sort < 0 || sort >= CMB_NSORT)
        sort = CMB_SORT_NAME;
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!cmb_shown(&m[i], cat))
            continue;
        int j = k++;
        while (j > 0) { /* insertion: stable, and n is small (the catalog's cap) */
            int c = cmb_cmp(&m[out[j - 1]], &m[i], sort);
            if (c < 0 || (c == 0 && out[j - 1] < i))
                break;
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return k;
}

/* how many mods each category holds; counts[CMB_NCAT] gets every one (All) */
static inline void cmb_counts(const cmb_mod *m, int n, int counts[CMB_NCAT + 1]) {
    for (int c = 0; c <= CMB_NCAT; c++)
        counts[c] = 0;
    for (int i = 0; i < n; i++) {
        counts[cmb_cat_of(m[i].cat)]++;
        counts[CMB_NCAT]++;
    }
}

/* ---- Home: the shelves ---------------------------------------------------------------- */
#define CMB_SHELF_MAX 5  /* the mods one shelf holds: the widest row (See all shows the rest) */
#define CMB_SHELF_MIN 2  /* a shelf with fewer is not shown */
enum {
    CMB_SH_TOP = 0, /* Most downloaded */
    CMB_SH_NEW,     /* New & updated */
    CMB_SH_CAT,     /* then one per category, CMB_SH_CAT + its index */
    CMB_NSHELF = CMB_SH_CAT + CMB_NCAT
};
typedef struct {
    int kind;               /* CMB_SH_* */
    int n;                  /* how many mods */
    int idx[CMB_SHELF_MAX]; /* their indexes into the catalog, in shelf order */
} cmb_shelf;

/* the shelf's title */
static inline const char *cmb_shelf_title(int kind) {
    if (kind == CMB_SH_TOP)
        return "Most downloaded";
    if (kind == CMB_SH_NEW)
        return "New & updated";
    return kind >= CMB_SH_CAT && kind < CMB_NSHELF ? cmb_cat_label[kind - CMB_SH_CAT] : "";
}
/* the category a shelf's "See all" picks (CMB_ALL: All mods), and the order */
static inline int cmb_shelf_cat(int kind) { return kind >= CMB_SH_CAT && kind < CMB_NSHELF ? kind - CMB_SH_CAT : CMB_ALL; }
static inline int cmb_shelf_sort(int kind) { return kind == CMB_SH_NEW ? CMB_SORT_UPDATED : CMB_SORT_DOWNLOADS; }
/* 1 when mod m belongs on the shelf */
static inline int cmb_on_shelf(const cmb_mod *m, int kind) {
    if (kind == CMB_SH_TOP)
        return m->counted && m->downloads > 0;
    if (kind == CMB_SH_NEW)
        return cmb_date_ok(m->updated);
    return kind >= CMB_SH_CAT && kind < CMB_NSHELF && cmb_cat_of(m->cat) == kind - CMB_SH_CAT;
}

/* The shelves Home shows, in order, into out[] (room for CMB_NSHELF); returns how many. tmp needs
 * room for n indexes. */
static inline int cmb_shelves(const cmb_mod *m, int n, int *tmp, cmb_shelf *out) {
    int ns = 0;
    for (int kind = 0; kind < CMB_NSHELF; kind++) {
        cmb_shelf *s = &out[ns];
        s->kind = kind;
        s->n = 0;
        int k = cmb_view(m, n, cmb_shelf_cat(kind), cmb_shelf_sort(kind), tmp);
        for (int i = 0; i < k && s->n < CMB_SHELF_MAX; i++)
            if (cmb_on_shelf(&m[tmp[i]], kind))
                s->idx[s->n++] = tmp[i];
        if (s->n >= CMB_SHELF_MIN)
            ns++;
    }
    return ns;
}

#endif
