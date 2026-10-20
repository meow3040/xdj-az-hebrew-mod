/* SPDX-License-Identifier: GPL-3.0-only */
/* The Hebrew mod's line reordering (guest/azmod/bidi_logic.h): track names as the deck shows
 * them (right-to-left titles, mixed Hebrew and Latin, numbers, brackets, trailing spaces), and
 * the cases of the Unicode Consortium's BidiCharacterTest.txt that the subset covers.
 *
 *   bidi_test                          the cases below
 *   bidi_test BidiCharacterTest.txt    also every line of the conformance file without explicit
 *                                      formatting characters or paragraph separators: levels and
 *                                      visual order must match exactly
 *
 * Control (run.sh): L4 mirroring and N0 bracket pairing disabled must each fail a case. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include BIDI_INC

static int utf8_decode(const char *s, uint32_t *out, int max) {
    const unsigned char *u = (const unsigned char *) s;
    int n = 0;
    while (*u && n < max) {
        uint32_t c = *u++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2
                               : c >= 0xC0   ? 1
                                             : 0;
        c &= more == 3 ? 0x07 : more == 2 ? 0x0F
                            : more == 1   ? 0x1F
                                          : 0x7F;
        while (more-- && *u)
            c = (c << 6) | (*u++ & 0x3F);
        out[n++] = c;
    }
    return n;
}
static void utf8_encode(const uint32_t *cp, int n, char *out) {
    unsigned char *p = (unsigned char *) out;
    for (int i = 0; i < n; i++) {
        uint32_t c = cp[i];
        if (c < 0x80)
            *p++ = (unsigned char) c;
        else if (c < 0x800) {
            *p++ = (unsigned char) (0xC0 | c >> 6);
            *p++ = (unsigned char) (0x80 | (c & 63));
        } else if (c < 0x10000) {
            *p++ = (unsigned char) (0xE0 | c >> 12);
            *p++ = (unsigned char) (0x80 | (c >> 6 & 63));
            *p++ = (unsigned char) (0x80 | (c & 63));
        } else {
            *p++ = (unsigned char) (0xF0 | c >> 18);
            *p++ = (unsigned char) (0x80 | (c >> 12 & 63));
            *p++ = (unsigned char) (0x80 | (c >> 6 & 63));
            *p++ = (unsigned char) (0x80 | (c & 63));
        }
    }
    *p = 0;
}

static unsigned char g_orig[BIDI_MAX], g_type[BIDI_MAX];
static int g_kept[BIDI_MAX], g_pairs[BIDI_MAX + 2];
static struct bidi_scratch g_ws = {g_orig, g_type, g_kept, g_pairs};

/* the logical UTF-8 line, shown left to right: 1 when it reads `want` and its direction is rtl */
static int visual_is(const char *logical, int dir, const char *want, int want_rtl) {
    uint32_t in[256], out[256];
    signed char lv[256];
    char got[1024];
    int rtl = -1, n = utf8_decode(logical, in, 256);
    int m = bidi_visual(in, n, dir, out, &rtl, lv, &g_ws);
    if (m < 0)
        return 0;
    utf8_encode(out, m, got);
    if (strcmp(got, want) || rtl != want_rtl) {
        printf("      \"%s\" -> \"%s\" (rtl %d), want \"%s\" (rtl %d)\n", logical, got, rtl, want,
               want_rtl);
        return 0;
    }
    return 1;
}

/* BidiCharacterTest.txt: fields cps; direction (0 LTR, 1 RTL, 2 auto); paragraph level;
 * levels (x = removed); visual order */
static void conformance(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        CHECK("BidiCharacterTest.txt opened", 0);
        return;
    }
    static char line[16384];
    long run = 0, ok = 0, skipped = 0, shown = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char *fld[5], *p = line;
        int nf = 0;
        while (nf < 5) {
            fld[nf++] = p;
            char *s = strchr(p, ';');
            if (!s)
                break;
            *s = 0;
            p = s + 1;
        }
        if (nf < 5)
            continue;
        uint32_t in[BIDI_MAX];
        int n = 0, bad = 0;
        for (char *q = fld[0]; *q && n < BIDI_MAX;) {
            char *e;
            unsigned long v = strtoul(q, &e, 16);
            if (e == q)
                break;
            int t = bidi_class_of((uint32_t) v);
            if ((t >= BIDI_LRE && t <= BIDI_PDI) || t == BIDI_B)
                bad = 1; /* outside the subset */
            in[n++] = (uint32_t) v;
            q = e;
        }
        if (bad) {
            skipped++;
            continue;
        }
        int dir = atoi(fld[1]), want_par = atoi(fld[2]);
        signed char want[BIDI_MAX], got[BIDI_MAX];
        int k = 0;
        for (char *q = fld[3]; *q && k < n; q++) {
            if (*q == 'x')
                want[k++] = -1;
            else if (*q >= '0' && *q <= '9')
                want[k++] = (signed char) strtol(q, &q, 10), q--;
        }
        int order_want[BIDI_MAX], nw = 0;
        for (char *q = fld[4]; *q && nw < n;) {
            char *e;
            long v = strtol(q, &e, 10);
            if (e == q) {
                q++;
                continue;
            }
            order_want[nw++] = (int) v;
            q = e;
        }
        run++;
        int par = bidi_levels(in, n, dir, got, &g_ws);
        int good = par == want_par && k == n;
        for (int i = 0; good && i < n; i++)
            good = got[i] == want[i];
        int order[BIDI_MAX];
        int m = good ? bidi_order(got, n, order) : -1;
        good = good && m == nw;
        for (int i = 0; good && i < m; i++)
            good = order[i] == order_want[i];
        if (good)
            ok++;
        else if (shown++ < 5)
            printf("      mismatch: %s;%s;%s;%s;%s", fld[0], fld[1], fld[2], fld[3], fld[4]);
    }
    fclose(f);
    printf("      conformance: %ld of %ld lines match (%ld outside the subset)\n", ok, run, skipped);
    CHECK("BidiCharacterTest.txt: every line in the subset matches", run > 0 && ok == run);
}

int main(int argc, char **argv) {
    ut_suite = "bidi";
    CHECK("an English title stays as it is",
          visual_is("Hello World (Original Mix)", 2, "Hello World (Original Mix)", 0));
    CHECK("a Hebrew word reads right to left", visual_is("שיר", 2, "ריש", 1));
    CHECK("a Hebrew title with a Latin remix tag: the tag keeps its order, brackets mirror",
          visual_is("שיר (Remix)", 2, "(Remix) ריש", 1));
    CHECK("an English artist before a Hebrew title: the line stays left to right",
          visual_is("Artist - שיר חדש", 2, "Artist - שדח ריש", 0));
    CHECK("a number in a Hebrew title keeps its digits in order",
          visual_is("קיץ 2024", 2, "2024 ץיק", 1));
    CHECK("a decimal and a percent stay whole", visual_is("מהירות 12.5%", 2, "12.5% תוריהמ", 1));
    CHECK("an English word inside a Hebrew title", visual_is("רמיקס של DJ Dan", 2, "DJ Dan לש סקימר", 1));
    CHECK("Hebrew inside an English title: a number after it joins the Hebrew run",
          visual_is("Live at תל אביב 2023", 2, "Live at 2023 ביבא לת", 0));
    CHECK("a trailing space stays at the line's end (L1)", visual_is("שלום ", 2, " םולש", 1));
    CHECK("an RLM before a Latin start makes the line right to left, and is dropped",
          visual_is("\xe2\x80\x8f"
                    "DJ שיר",
                    2, "ריש DJ", 1));
    CHECK("a zero-width joiner is dropped (X9)", visual_is("א\xe2\x80\x8d"
                                                           "ב",
                                                           2, "בא", 1));
    CHECK("brackets around Hebrew in an English line", visual_is("Song (גרסה) Mix", 2,
                                                                 "Song (הסרג) Mix", 0));
    CHECK("brackets around Latin in a Hebrew line stay a pair (N0)",
          visual_is("ש (a) b", 2, "b (a) ש", 1));
    CHECK("geresh and gershayim stay with their word", visual_is("צ׳לו צה״ל", 2, "ל״הצ ול׳צ", 1));
    CHECK("a forced left-to-right line", visual_is("שיר", 0, "ריש", 0));
    CHECK("an over-long line is refused", bidi_levels((const uint32_t *) g_pairs, BIDI_MAX + 1, 2,
                                                      (signed char *) g_type, &g_ws) == -1);
    if (argc > 1)
        conformance(argv[1]);
    return ut_summary();
}
