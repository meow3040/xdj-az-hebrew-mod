/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file touch_seq.h
 * @brief Tells a finger landing from a finger moving in the deck's X button stream.
 *
 * On the hardware EP147's touch decoder sends an X ButtonPress (button 1) when a finger lands,
 * and AGAIN at each step while it moves (every few pixels, tens of ms apart), then one
 * ButtonRelease when it lifts. A press that arrives while the finger is still down is a move
 * to that point, never a new touch: read as a new touch, every drag ends with a "press" a pixel
 * from its release, which is a tap wherever the finger lifts.
 *
 * Releases are not always delivered, so a press more than TOUCH_SEQ_GAP_MS after the touch's
 * last event starts a new touch (a moving finger reports far more often than that). Under the
 * emulator the X server sends one press per touch and MotionNotify for moves; the same rule
 * reads that stream unchanged. Buttons other than 1 (further fingers, wheel steps) are not part
 * of the sequence.
 *
 * Pure state machine, no system calls: the caller feeds it every button event in order, from
 * one thread or under one lock.
 */
#ifndef AZMOD_TOUCH_SEQ_H
#define AZMOD_TOUCH_SEQ_H

#define TOUCH_SEQ_GAP_MS 250

struct touch_seq {
    int down;  /* button 1 is pressed */
    long last; /* the touch's last press, ms */
};

enum {
    TOUCH_OTHER = 0,
    TOUCH_DOWN,
    TOUCH_MOVE,
    TOUCH_UP
};

/* One X button event: press = 1 for ButtonPress, 0 for ButtonRelease. Returns what it is. */
static inline int touch_seq_feed(struct touch_seq *t, int press, unsigned button, long ms) {
    if (button != 1)
        return TOUCH_OTHER;
    if (!press) {
        t->down = 0;
        return TOUCH_UP;
    }
    int move = t->down && ms >= t->last && ms - t->last <= TOUCH_SEQ_GAP_MS;
    t->down = 1;
    t->last = ms;
    return move ? TOUCH_MOVE : TOUCH_DOWN;
}

#endif
