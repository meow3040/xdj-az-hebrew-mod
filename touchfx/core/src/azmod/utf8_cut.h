/* SPDX-License-Identifier: GPL-3.0-only */
/* utf8_cut.h: copying text into a fixed buffer without cutting a character in half.
 *
 * A byte-wise cut can end inside a multi-byte UTF-8 sequence (a 95-byte CJK playlist name in a
 * 96-byte buffer keeps two bytes of its last character); written to a stick, that is ill-formed
 * UTF-8 the library then cannot decode. utf8_cut() drops such an incomplete sequence from the
 * end of a string; it changes nothing on a string that ends with a whole character. Libc only. */
#ifndef AZ_UTF8_CUT_H
#define AZ_UTF8_CUT_H
#include <stdio.h>
#include <string.h>

/* drop an incomplete UTF-8 sequence from the end of s (in place) */
static inline void utf8_cut(char *s) {
    size_t n = strlen(s), j = n;
    while (j > 0 && n - j < 4 && ((unsigned char) s[j - 1] & 0xc0) == 0x80)
        j--; /* back over continuation bytes to the sequence's first byte */
    if (!j)
        return;
    unsigned char c = (unsigned char) s[j - 1];
    size_t want = c >= 0xf0 ? 4 : c >= 0xe0 ? 3
                              : c >= 0xc0   ? 2
                                            : 1;
    if (c >= 0xc0 && n - (j - 1) < want)
        s[j - 1] = 0;
}
/* snprintf(dst, cap, "%s", src), cut at a character boundary when src does not fit */
static inline void utf8_copy(char *dst, size_t cap, const char *src) {
    if (!cap)
        return;
    if ((size_t) snprintf(dst, cap, "%s", src ? src : "") >= cap)
        utf8_cut(dst);
}
#endif
