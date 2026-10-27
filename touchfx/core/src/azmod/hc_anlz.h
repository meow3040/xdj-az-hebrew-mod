/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file hc_anlz.h
 * @brief A track's hot cues from its rekordbox analysis file, for the Browse preview marks
 * (preview_hotcues.c).
 *
 * An ANLZ file (ANLZ0000.DAT / .EXT on the stick) is a "PMAI" header followed by tagged
 * sections, all big-endian: a four-character tag, the section's header length and its total
 * length. Hot cues are in two kinds of section, each with a list type at +0x0c (1 = hot cues,
 * 0 = memory cues):
 *   - PCO2 (in .EXT; count u16 at +0x10): PCP2 entries holding every hot cue with its color:
 *     slot u32 at +0x0c (1 = A), time in ms u32 at +0x14, comment length u32 at +0x28, the
 *     comment, then a color code byte and the R, G, B bytes;
 *   - PCOB (in .DAT and .EXT; count u16 at +0x12): PCPT entries, no color: slot u32 at +0x0c,
 *     time in ms u32 at +0x20. A file may split its hot cues over two PCOB lists.
 * PCO2 is used when the file has a hot cue PCO2 list, otherwise every hot cue PCOB list.
 *
 * Pure parsing of untrusted bytes: every length is checked against what is left, nothing
 * reads past n, and a malformed file gives -1 (fuzzed by tools/fuzz hcanlz).
 */
#ifndef AZMOD_HC_ANLZ_H
#define AZMOD_HC_ANLZ_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define HC_MAX 8 /* hot cues A..H */

struct hc_cue {
    uint32_t ms;  /* from the start of the track */
    uint32_t rgb; /* 0xRRGGBB, 0 = no color stored */
    uint8_t slot; /* 1 = A .. 8 = H */
};

static inline uint32_t hc_be32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}
static inline uint32_t hc_be16(const uint8_t *p) {
    return (uint32_t) p[0] << 8 | p[1];
}
/* the list's entry for this slot: replaced, or added in slot order */
static inline void hc_put(struct hc_cue *out, int *n, uint32_t slot, uint32_t ms, uint32_t rgb) {
    if (slot < 1 || slot > HC_MAX)
        return;
    int i = 0;
    while (i < *n && out[i].slot < slot)
        i++;
    if (i < *n && out[i].slot == slot) {
        out[i].ms = ms;
        out[i].rgb = rgb;
        return;
    }
    for (int k = *n; k > i; k--)
        out[k] = out[k - 1];
    out[i].slot = (uint8_t) slot;
    out[i].ms = ms;
    out[i].rgb = rgb;
    (*n)++;
}
/* One cue list section at b[o, o + len): its hot cue entries into out. 0, or -1 malformed. */
static inline int hc_list(const uint8_t *b, size_t o, size_t h, size_t len, int pco2,
                          struct hc_cue *out, int *n) {
    if (h < (pco2 ? 0x12u : 0x14u))
        return -1;
    uint32_t want = pco2 ? hc_be16(b + o + 0x10) : hc_be16(b + o + 0x12);
    size_t e = o + h, end = o + len;
    for (uint32_t k = 0; k < want && k < 64; k++) {
        if (end - e < 12)
            return -1;
        uint32_t en = hc_be32(b + e + 8);
        if (en > end - e || en < (pco2 ? 0x2cu : 0x24u))
            return -1;
        uint32_t slot = hc_be32(b + e + 0x0c);
        if (pco2) {
            uint32_t ms = hc_be32(b + e + 0x14), lc = hc_be32(b + e + 0x28), rgb = 0;
            if (lc <= en - 0x2c && en - 0x2c - lc >= 4) {
                const uint8_t *c = b + e + 0x2c + lc;
                rgb = (uint32_t) c[1] << 16 | (uint32_t) c[2] << 8 | c[3];
            }
            hc_put(out, n, slot, ms, rgb);
        } else {
            hc_put(out, n, slot, hc_be32(b + e + 0x20), 0);
        }
        e += en;
    }
    return 0;
}
/* The hot cues of an ANLZ file (up to HC_MAX, in slot order): their count, or -1 when the
 * bytes are not a well-formed ANLZ file. */
static inline int hc_anlz_hot_cues(const uint8_t *b, size_t n, struct hc_cue out[HC_MAX]) {
    if (!b || n < 12 || b[0] != 'P' || b[1] != 'M' || b[2] != 'A' || b[3] != 'I')
        return -1;
    size_t o = hc_be32(b + 4);
    if (o < 12 || o > n)
        return -1;
    struct hc_cue ob[HC_MAX];
    int nco2 = 0, ncob = 0, have_co2 = 0;
    while (n - o >= 12) {
        size_t h = hc_be32(b + o + 4), len = hc_be32(b + o + 8);
        if (len < 12 || len > n - o || h < 12 || h > len)
            return -1;
        const uint8_t *t = b + o;
        int co2 = t[0] == 'P' && t[1] == 'C' && t[2] == 'O' && t[3] == '2';
        int cob = t[0] == 'P' && t[1] == 'C' && t[2] == 'O' && t[3] == 'B';
        if ((co2 || cob) && h >= 0x10 && hc_be32(t + 0x0c) == 1) {
            if (co2) {
                have_co2 = 1;
                if (hc_list(b, o, h, len, 1, out, &nco2))
                    return -1;
            } else if (hc_list(b, o, h, len, 0, ob, &ncob)) {
                return -1;
            }
        }
        o += len;
    }
    if (have_co2)
        return nco2;
    for (int i = 0; i < ncob; i++)
        out[i] = ob[i];
    return ncob;
}
/* The parts of an ANLZ file of `size` bytes that hc_anlz_hot_cues reads, without reading the
 * rest (the waveforms are most of an .EXT): the PMAI header, then every PCOB and PCO2 section,
 * each read whole, back to back. rd(ctx, buf, n, off) reads n bytes at off, 1 when all of them
 * were read. Parsing the result gives what parsing the whole file gives. Returns a malloc'd
 * buffer of *out_n bytes (at most max), or 0 when the file is malformed, unreadable or larger
 * than max: then the whole file would parse as malformed too, unless it is that large. */
typedef int (*hc_read_fn)(void *ctx, uint8_t *buf, size_t n, uint64_t off);
static inline uint8_t *hc_anlz_pick(hc_read_fn rd, void *ctx, uint64_t size, size_t max,
                                    size_t *out_n) {
    uint8_t t[12];
    if (size < 12 || size > max || !rd(ctx, t, 12, 0) || t[0] != 'P' || t[1] != 'M' || t[2] != 'A' ||
        t[3] != 'I')
        return 0;
    uint64_t o = hc_be32(t + 4);
    if (o < 12 || o > size)
        return 0;
    size_t n = (size_t) o;
    uint8_t *b = (uint8_t *) malloc(n);
    if (!b || !rd(ctx, b, n, 0)) {
        free(b);
        return 0;
    }
    while (size - o >= 12) {
        if (!rd(ctx, t, 12, o))
            break;
        uint64_t h = hc_be32(t + 4), len = hc_be32(t + 8);
        if (len < 12 || len > size - o || h < 12 || h > len)
            break; /* malformed: so is the whole file */
        int cue = t[0] == 'P' && t[1] == 'C' && t[2] == 'O' && (t[3] == '2' || t[3] == 'B');
        if (cue) {
            uint8_t *nb = (uint8_t *) realloc(b, n + (size_t) len);
            if (!nb)
                break;
            b = nb;
            if (!rd(ctx, b + n, (size_t) len, o))
                break;
            n += (size_t) len;
        }
        o += len;
    }
    if (size - o >= 12) {
        free(b);
        return 0;
    }
    *out_n = n;
    return b;
}
/* x of a cue in a preview w px wide that spans len_ms, or -1 when it is not inside it */
static inline int hc_x(uint32_t ms, uint32_t len_ms, int w) {
    if (w <= 0 || !len_ms || ms >= len_ms)
        return -1;
    int x = (int) ((uint64_t) ms * (uint64_t) w / len_ms);
    return x < w ? x : w - 1;
}
#endif
