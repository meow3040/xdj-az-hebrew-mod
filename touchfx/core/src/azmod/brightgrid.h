/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file brightgrid.h
 * @brief Bright Grid's pixel rule: brighten a beat ruler's ticks, never its background.
 *
 * makeGridImage fills each ruler row with a background color, then stamps the ticks. In stock
 * EP147 that background is black, but a theme swaps in its own ground color (Midnight's deep
 * navy, for example), so "not black" does not mean "tick". A pixel equal to the ruler's
 * background is treated like a transparent one: never scaled and never counted as ink. Every
 * other pixel is brightened exactly as Bright Grid always has, v * gain per channel.
 * Pixels are premultiplied ARGB; alpha is raised to at least the brightest channel so
 * color <= alpha still holds.
 *
 * waveform.c (gridmake_hook) is the only user in the core; tools/unit-tests/brightgrid_test.c
 * builds this header natively. Nothing here allocates, blocks or calls the system.
 */
#ifndef AZMOD_BRIGHTGRID_H
#define AZMOD_BRIGHTGRID_H

#include <stdint.h>

static void bg_build_lut(uint8_t lut[256], float gain) {
    for (int i = 0; i < 256; i++) {
        int v = (int) ((float) i * gain + 0.5f);
        lut[i] = (uint8_t) (v > 255 ? 255 : v);
    }
}

/* A pixel the brightening leaves alone: transparent / black, or the ruler's own background
 * (bg, 0 when the ruler is stock). */
static int bg_is_ground(uint32_t px, uint32_t bg) {
    return !(px & 0x00ffffffu) || px == bg;
}

/* Brighten one row in place. graypx (opaque, 0 = none) fills the transparent pixels of a row
 * that carries a tick: the optional strip behind the ticks. */
static void bg_row(uint32_t *row, int w, const uint8_t lut[256], uint32_t bg, uint32_t graypx) {
    int inked = 0;
    for (int x = 0; x < w && !inked; x++)
        inked = !bg_is_ground(row[x], bg);
    for (int x = 0; x < w; x++) {
        uint32_t px = row[x];
        if (bg_is_ground(px, bg)) {
            if (graypx && inked && !(px & 0x00ffffffu))
                row[x] = graypx; /* gray strip behind the tick rows only */
            continue;
        }
        unsigned R = lut[(px >> 16) & 255], G = lut[(px >> 8) & 255], B = lut[px & 255];
        unsigned A = px >> 24, m = R > G ? (R > B ? R : B) : (G > B ? G : B);
        if (A < m)
            A = m;
        row[x] = (A << 24) | (R << 16) | (G << 8) | B;
    }
}

#endif
