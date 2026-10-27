/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file camelot_text.h
 * @brief A key in Camelot notation ("8A"), the way EP147's own key display writes it.
 *
 * EP147's deck key display has a Camelot mode: the key's number from a table indexed by its
 * pitch class minus one (pitch class 0, C, and anything out of the table give 8), then "B" for
 * a major key and "A" for a minor one. The key's pitch class is its relative major's for a
 * minor key, so A minor and C major are both pitch class 0: 8A and 8B. This mirrors that rule.
 */
#ifndef AZMOD_CAMELOT_TEXT_H
#define AZMOD_CAMELOT_TEXT_H

/* out: at least 4 bytes; returns the text's length (2 or 3). */
static inline int ct_camelot(int pitch, int major, char *out) {
    static const unsigned char num[11] = {3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1};
    unsigned i = (unsigned) (pitch - 1) & 0xffu;
    int v = i < 11 ? num[i] : 8, n = 0;
    if (v >= 10)
        out[n++] = (char) ('0' + v / 10);
    out[n++] = (char) ('0' + v % 10);
    out[n++] = major ? 'B' : 'A';
    out[n] = 0;
    return n;
}
#endif
