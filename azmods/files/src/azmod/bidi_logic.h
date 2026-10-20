/* SPDX-License-Identifier: GPL-3.0-only */
/* bidi_logic.h: the Unicode Bidirectional Algorithm (UAX #9) for one line of text, for the
 * Hebrew mod (hebrew.c): EP147 lays every string out left to right, so a line holding Hebrew
 * is handed to it already in visual order.
 *
 * The whole algorithm for a single paragraph without explicit embeddings: paragraph level
 * from the first strong character (P2, P3) or given; X9 removal; the weak types (W1-W7);
 * paired brackets (BD16, N0); neutrals (N1, N2); implicit levels (I1, I2); trailing and
 * pre-separator whitespace (L1); reordering (L2); mirroring (L4). Explicit embeddings,
 * overrides and isolates (LRE..PDF, LRI..PDI) are dropped like BN instead of opening a level:
 * track names do not carry them, and a dropped mark only loses its own nudge. L3 (combining
 * marks before their base in RTL runs) is not applied; hebrew.c draws Hebrew marks as nothing.
 * Checked against the Unicode Consortium's BidiCharacterTest.txt (every line without explicit
 * formatting characters or paragraph separators) by tests/unit/bidi_test.c.
 *
 * Pure: no allocation, no globals written, no system calls. The caller passes the scratch.
 * Self-contained so the host tests compile it as is. */
#ifndef AZMOD_BIDI_LOGIC_H
#define AZMOD_BIDI_LOGIC_H
#include <stdint.h>

enum bidi_class {
    BIDI_L,
    BIDI_R,
    BIDI_AL,
    BIDI_EN,
    BIDI_ES,
    BIDI_ET,
    BIDI_AN,
    BIDI_CS,
    BIDI_NSM,
    BIDI_BN,
    BIDI_B,
    BIDI_S,
    BIDI_WS,
    BIDI_ON,
    BIDI_LRE,
    BIDI_LRO,
    BIDI_RLE,
    BIDI_RLO,
    BIDI_PDF,
    BIDI_LRI,
    BIDI_RLI,
    BIDI_FSI,
    BIDI_PDI
};
#include "bidi_tables.h"

#define BIDI_MAX 4096        /* longest line handled; longer ones stay as they are */
#define BIDI_MAX_BRACKETS 63 /* BD16's stack depth */

static inline int bidi_class_of(uint32_t cp) {
    int lo = 0, hi = (int) (sizeof BIDI_RANGES / sizeof BIDI_RANGES[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t first = BIDI_RANGES[mid][0], last = BIDI_RANGES[mid][1] & 0xFFFFFFu;
        if (cp < first)
            hi = mid - 1;
        else if (cp > last)
            lo = mid + 1;
        else
            return (int) (BIDI_RANGES[mid][1] >> 24);
    }
    return BIDI_L;
}
/* index into a sorted {cp, ...} table, or -1 */
static inline int bidi_find(const unsigned (*t)[2], int n, uint32_t cp, int stride) {
    int lo = 0, hi = n - 1;
    const unsigned *base = (const unsigned *) t;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t k = base[mid * stride];
        if (cp < k)
            hi = mid - 1;
        else if (cp > k)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}
static inline uint32_t bidi_mirror(uint32_t cp) {
    int i = bidi_find(BIDI_MIRROR, (int) (sizeof BIDI_MIRROR / sizeof BIDI_MIRROR[0]), cp, 2);
    return i < 0 ? cp : BIDI_MIRROR[i][1];
}
/* the bracket's pair and kind: 1 opening, 0 closing, -1 not a paired bracket */
static inline int bidi_bracket(uint32_t cp, uint32_t *pair) {
    int i = bidi_find((const unsigned (*)[2]) BIDI_BRACKET,
                      (int) (sizeof BIDI_BRACKET / sizeof BIDI_BRACKET[0]), cp, 3);
    if (i < 0)
        return -1;
    *pair = BIDI_BRACKET[i][1];
    return (int) BIDI_BRACKET[i][2];
}
/* BD16 compares brackets up to canonical equivalence: U+2329/U+232A are U+3008/U+3009 */
static inline uint32_t bidi_bracket_canon(uint32_t cp) {
    return cp == 0x2329 ? 0x3008 : cp == 0x232A ? 0x3009
                                                : cp;
}
static inline int bidi_strong_of(int t) { /* N0/N1: EN and AN count as R */
    return t == BIDI_L ? BIDI_L : (t == BIDI_R || t == BIDI_AL || t == BIDI_EN || t == BIDI_AN) ? BIDI_R
                                                                                                : -1;
}
static inline int bidi_is_ni(int t) { /* neutral or isolate */
    return t == BIDI_B || t == BIDI_S || t == BIDI_WS || t == BIDI_ON || t == BIDI_LRI ||
           t == BIDI_RLI || t == BIDI_FSI || t == BIDI_PDI;
}
static inline int bidi_is_removed(int t) { /* X9, and the embeddings this subset drops */
    return t == BIDI_BN || (t >= BIDI_LRE && t <= BIDI_PDI);
}

/* Scratch for one line of n code points, allocated by the caller. */
struct bidi_scratch {
    unsigned char *orig; /* [n] Bidi_Class of each character */
    unsigned char *type; /* [n] its resolved type, over the kept characters (compacted) */
    int *kept;           /* [n] kept index -> input index; later the visual order */
    int *pairs;          /* [2 * (n / 2 + 1)] the bracket pairs (BD16) */
};

/* Resolve the line in[0..n). dir: 0 LTR, 1 RTL, 2 from the first strong character (none: LTR).
 * levels[i] gets character i's embedding level, -1 for a removed one (BN, explicit marks).
 * Returns the paragraph level, -1 when n is out of range. */
static int bidi_levels(const uint32_t *in, int n, int dir, signed char *levels,
                       struct bidi_scratch *ws) {
    if (n < 0 || n > BIDI_MAX)
        return -1;
    unsigned char *orig = ws->orig, *T = ws->type;
    int *kept = ws->kept, m = 0;
    for (int i = 0; i < n; i++) {
        orig[i] = (unsigned char) bidi_class_of(in[i]);
        levels[i] = -1;
        if (!bidi_is_removed(orig[i]))
            kept[m++] = i;
    }
    int par = dir == 1;
    if (dir == 2) { /* P2, P3 */
        par = 0;
        for (int k = 0; k < m; k++) {
            int t = orig[kept[k]];
            if (t == BIDI_L || t == BIDI_R || t == BIDI_AL) {
                par = t != BIDI_L;
                break;
            }
        }
    }
    int e = par ? BIDI_R : BIDI_L; /* embedding direction; sos = eos = e */
    for (int k = 0; k < m; k++)
        T[k] = orig[kept[k]];
    /* W1: NSM takes the previous type (sos at the start, ON after an isolate) */
    for (int k = 0; k < m; k++)
        if (T[k] == BIDI_NSM) {
            int p = k ? T[k - 1] : e;
            T[k] = (unsigned char) ((p >= BIDI_LRI && p <= BIDI_PDI) ? BIDI_ON : p);
        }
    /* W2: EN after AL is AN.  W3: AL is R. */
    for (int k = 0, last = e; k < m; k++) {
        if (T[k] == BIDI_L || T[k] == BIDI_R || T[k] == BIDI_AL)
            last = T[k];
        else if (T[k] == BIDI_EN && last == BIDI_AL)
            T[k] = BIDI_AN;
    }
    for (int k = 0; k < m; k++)
        if (T[k] == BIDI_AL)
            T[k] = BIDI_R;
    /* W4: one ES between ENs is EN; one CS between ENs is EN, between ANs AN */
    for (int k = 1; k + 1 < m; k++) {
        if (T[k] == BIDI_ES && T[k - 1] == BIDI_EN && T[k + 1] == BIDI_EN)
            T[k] = BIDI_EN;
        else if (T[k] == BIDI_CS && T[k - 1] == BIDI_EN && T[k + 1] == BIDI_EN)
            T[k] = BIDI_EN;
        else if (T[k] == BIDI_CS && T[k - 1] == BIDI_AN && T[k + 1] == BIDI_AN)
            T[k] = BIDI_AN;
    }
    /* W5: a run of ET next to an EN is EN */
    for (int k = 0; k < m;) {
        if (T[k] != BIDI_ET) {
            k++;
            continue;
        }
        int j = k;
        while (j < m && T[j] == BIDI_ET)
            j++;
        if ((k > 0 && T[k - 1] == BIDI_EN) || (j < m && T[j] == BIDI_EN))
            for (int x = k; x < j; x++)
                T[x] = BIDI_EN;
        k = j;
    }
    /* W6: what is left of the separators and terminators is ON */
    for (int k = 0; k < m; k++)
        if (T[k] == BIDI_ES || T[k] == BIDI_ET || T[k] == BIDI_CS)
            T[k] = BIDI_ON;
    /* W7: EN after L (or an L sos) is L */
    for (int k = 0, last = e; k < m; k++) {
        if (T[k] == BIDI_L || T[k] == BIDI_R)
            last = T[k];
        else if (T[k] == BIDI_EN && last == BIDI_L)
            T[k] = BIDI_L;
    }
    /* BD16: the bracket pairs, in order of their opening bracket */
    {
        int st_pos[BIDI_MAX_BRACKETS], np = 0, sp = 0, max_pairs = n / 2 + 1;
        uint32_t st_close[BIDI_MAX_BRACKETS];
        int *pairs_open = ws->pairs, *pairs_close = ws->pairs + max_pairs;
        for (int k = 0; k < m; k++) {
            uint32_t pair = 0;
            int kind = T[k] == BIDI_ON ? bidi_bracket(in[kept[k]], &pair) : -1;
            if (kind == 1) {
                if (sp == BIDI_MAX_BRACKETS)
                    break; /* BD16: stop processing for the rest of the line */
                st_close[sp] = bidi_bracket_canon(pair);
                st_pos[sp++] = k;
            } else if (kind == 0) {
                uint32_t c = bidi_bracket_canon(in[kept[k]]);
                for (int s = sp - 1; s >= 0; s--)
                    if (st_close[s] == c) {
                        if (np < max_pairs) {
                            pairs_open[np] = st_pos[s];
                            pairs_close[np++] = k;
                        }
                        sp = s;
                        break;
                    }
            }
        }
        /* sort by the opening bracket (insertion: pairs come out nearly sorted) */
        for (int a = 1; a < np; a++) {
            int o = pairs_open[a], c = pairs_close[a], b = a - 1;
            for (; b >= 0 && pairs_open[b] > o; b--) {
                pairs_open[b + 1] = pairs_open[b];
                pairs_close[b + 1] = pairs_close[b];
            }
            pairs_open[b + 1] = o;
            pairs_close[b + 1] = c;
        }
        /* N0 */
        for (int p = 0; p < np; p++) {
            int o = pairs_open[p], c = pairs_close[p], found_e = 0, found_opp = 0;
            for (int k = o + 1; k < c; k++) {
                int s = bidi_strong_of(T[k]);
                if (s == e)
                    found_e = 1;
                else if (s >= 0)
                    found_opp = 1;
            }
            int set = -1;
            if (found_e)
                set = e;
            else if (found_opp) {
                int ctx = e; /* sos */
                for (int k = o - 1; k >= 0; k--) {
                    int s = bidi_strong_of(T[k]);
                    if (s >= 0) {
                        ctx = s;
                        break;
                    }
                }
                set = ctx != e ? ctx : e;
            }
            if (set < 0)
                continue;
            T[o] = T[c] = (unsigned char) set;
            /* NSMs that followed a bracket (W1 gave them its ON) follow it again */
            for (int k = o + 1; k < m && orig[kept[k]] == BIDI_NSM; k++)
                T[k] = (unsigned char) set;
            for (int k = c + 1; k < m && orig[kept[k]] == BIDI_NSM; k++)
                T[k] = (unsigned char) set;
        }
    }
    /* N1, N2: a run of neutrals takes the direction around it when both sides agree */
    for (int k = 0; k < m;) {
        if (!bidi_is_ni(T[k])) {
            k++;
            continue;
        }
        int j = k;
        while (j < m && bidi_is_ni(T[j]))
            j++;
        int before = k ? bidi_strong_of(T[k - 1]) : e, after = j < m ? bidi_strong_of(T[j]) : e;
        int set = (before >= 0 && before == after) ? before : e;
        for (int x = k; x < j; x++)
            T[x] = (unsigned char) set;
        k = j;
    }
    /* I1, I2 */
    for (int k = 0; k < m; k++) {
        int t = T[k], lv = par;
        if (!(par & 1))
            lv += t == BIDI_R ? 1 : (t == BIDI_AN || t == BIDI_EN) ? 2
                                                                   : 0;
        else
            lv += (t == BIDI_L || t == BIDI_EN || t == BIDI_AN) ? 1 : 0;
        levels[kept[k]] = (signed char) lv;
    }
    /* L1: separators, and whitespace before a separator or the line's end, go back to the
     * paragraph level (original types; removed characters in between are skipped) */
    {
        int trailing = 1;
        for (int i = n - 1; i >= 0; i--) {
            int t = orig[i];
            if (t == BIDI_S || t == BIDI_B) {
                levels[i] = (signed char) par;
                trailing = 1;
            } else if (t == BIDI_WS || (t >= BIDI_LRI && t <= BIDI_PDI)) {
                if (trailing && levels[i] >= 0)
                    levels[i] = (signed char) par;
            } else if (!bidi_is_removed(t))
                trailing = 0;
        }
    }
    return par;
}

/* L2: the visual order of the kept characters, as input indices into order[0..return). */
static int bidi_order(const signed char *levels, int n, int *order) {
    int m = 0, maxl = 0, minodd = 127;
    for (int i = 0; i < n; i++)
        if (levels[i] >= 0) {
            order[m++] = i;
            if (levels[i] > maxl)
                maxl = levels[i];
            if ((levels[i] & 1) && levels[i] < minodd)
                minodd = levels[i];
        }
    for (int lv = maxl; lv >= minodd && lv > 0; lv--)
        for (int k = 0; k < m;) {
            if (levels[order[k]] < lv) {
                k++;
                continue;
            }
            int j = k;
            while (j < m && levels[order[j]] >= lv)
                j++;
            for (int a = k, b = j - 1; a < b; a++, b--) {
                int t = order[a];
                order[a] = order[b];
                order[b] = t;
            }
            k = j;
        }
    return m;
}

/* The line in visual order: out[0..return), mirrored glyphs at odd levels (L4). Dropped:
 * the removed characters, and the invisible marks LRM, RLM and ALM (they have done their
 * work, and a face without them would draw a box). *rtl = the paragraph is right to left.
 * -1 when n is out of range. */
static int bidi_visual(const uint32_t *in, int n, int dir, uint32_t *out, int *rtl,
                       signed char *levels, struct bidi_scratch *ws) {
    int par = bidi_levels(in, n, dir, levels, ws);
    if (par < 0)
        return -1;
    int m = bidi_order(levels, n, ws->kept), v = 0;
    for (int k = 0; k < m; k++) {
        int i = ws->kept[k];
        if (in[i] == 0x200E || in[i] == 0x200F || in[i] == 0x061C)
            continue;
        out[v++] = (levels[i] & 1) ? bidi_mirror(in[i]) : in[i];
    }
    *rtl = par & 1;
    return v;
}
#endif
