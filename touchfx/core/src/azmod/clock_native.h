/* SPDX-License-Identifier: GPL-3.0-only */
/* clock_native.h: when the Clock takes the place of EP147's recording time readout, as pure
 * functions (clock_native.c feeds them; the unit tests run them as they are).
 *
 * The readout is EP147's own label in the status bar ("0:00:00"). EP147 shows the recording time
 * there and lights the recording icon beside it (image index 1) while it records. The Clock hides
 * that label and draws the time of day in its place only while EP147 is not recording (icon 0):
 * the moment the icon lights, EP147's label shows again. A label or icon that cannot be read and
 * checked leaves EP147's label as it is and draws nothing. */
#ifndef CLOCK_NATIVE_H
#define CLOCK_NATIVE_H
#include <stdint.h>

/* 1: hide EP147's label and draw the Clock there; 0: EP147's label, as stock */
static inline int cn_take(int clock_on, int icon, int label_ok) {
    return clock_on && label_ok && icon == 0;
}

/* A screen rect packed for one atomic word: x, y, w, h as 16 bits each; 0 = none. Only a rect
 * on the screen (0 <= x, y; 0 < w, h; all below 4096) packs. */
static inline uint64_t cn_rect_pack(int x, int y, int w, int h) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x >= 4096 || y >= 4096 || w >= 4096 || h >= 4096)
        return 0;
    return (uint64_t) x | (uint64_t) y << 16 | (uint64_t) w << 32 | (uint64_t) h << 48;
}
static inline int cn_rect_unpack(uint64_t r, int *x, int *y, int *w, int *h) {
    if (!r)
        return 0;
    *x = (int) (r & 0xffff);
    *y = (int) (r >> 16 & 0xffff);
    *w = (int) (r >> 32 & 0xffff);
    *h = (int) (r >> 48 & 0xffff);
    return *w > 0 && *h > 0;
}
#endif
