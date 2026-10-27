/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file ht_tap.h
 * @brief Hide Track Title: tells a deliberate tap on the title bar from any other touch.
 *
 * A touch toggles a title only as a whole tap: one finger presses and lifts again, less
 * than HT_TAP_SLOP px from the press point and within HT_TAP_MAX_MS, with no other finger
 * on the screen meanwhile. The same rule the Mod Menu applies to its controls.
 *
 * On the hardware EP147's touch decoder sends an X ButtonPress the moment a finger lands,
 * another at each step while it moves, and a ButtonRelease when it lifts (touch_seq.h tells
 * a move from a landing: a move away from the press point voids the tap). Button 1 is the first finger; every further finger arrives as button 12 and
 * up. Acting on the press alone made every contact with a title bar a toggle: a drag or a
 * hand resting on the screen that starts there, or a brush in passing.
 *
 * Pure state machine, no system calls: the caller feeds it every button event in order,
 * from one thread or under one lock.
 */
#ifndef AZMOD_HT_TAP_H
#define AZMOD_HT_TAP_H

#include "touch_seq.h"

#define HT_TAP_SLOP 12    /* px between press and release, at 1280 x 800 */
#define HT_TAP_MAX_MS 400 /* press to release */

struct ht_gest {
    int down;      /* button 1 is pressed */
    int multi;     /* another button was pressed during this touch */
    unsigned held; /* other buttons now pressed, bit (button & 31) */
    int x, y;      /* the press point */
    long t;        /* the press time, ms */
    int moved;     /* the finger moved past the slop during this touch */
    struct touch_seq seq;
};

/* One X button event: press = 1 for ButtonPress, 0 for ButtonRelease. Returns 1 when it
 * completes a tap, with the press point in *tx, *ty; otherwise 0. */
static inline int ht_gest_feed(struct ht_gest *g, int press, unsigned button, int x, int y,
                               long ms, int *tx, int *ty) {
    if (button != 1) {
        unsigned bit = 1u << (button & 31);
        if (press) {
            g->held |= bit;
            if (g->down)
                g->multi = 1;
        } else
            g->held &= ~bit;
        return 0;
    }
    if (touch_seq_feed(&g->seq, press, button, ms) == TOUCH_MOVE) {
        int mx = x - g->x, my = y - g->y;
        if (mx < -HT_TAP_SLOP || mx > HT_TAP_SLOP || my < -HT_TAP_SLOP || my > HT_TAP_SLOP)
            g->moved = 1;
        return 0;
    }
    if (press) {
        g->down = 1;
        g->moved = 0;
        g->multi = g->held != 0; /* another finger is already on the screen */
        g->x = x;
        g->y = y;
        g->t = ms;
        return 0;
    }
    if (!g->down)
        return 0;
    g->down = 0;
    if (g->moved || g->multi || ms < g->t || ms - g->t > HT_TAP_MAX_MS)
        return 0;
    int dx = x - g->x, dy = y - g->y;
    if (dx < -HT_TAP_SLOP || dx > HT_TAP_SLOP || dy < -HT_TAP_SLOP || dy > HT_TAP_SLOP)
        return 0;
    *tx = g->x;
    *ty = g->y;
    return 1;
}

#endif
