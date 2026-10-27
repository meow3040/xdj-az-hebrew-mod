/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file scroll_glide.h
 * @brief A scrolled offset that moves smoothly: it stays under a finger, carries on after a
 * flick and slows to a stop, and glides to a knob step.
 *
 * While a finger is down the offset is the finger's point, the moment each report arrives:
 * easing toward it would leave the list trailing the finger (touch_seq.h has the reports). On
 * the lift the finger's last step becomes a fling that slows exponentially and stops at either
 * end of the list (no overscroll). A knob step is a goal the shown offset closes on frame by
 * frame.
 *
 * The fling is EP147's own list scrolling (juce::Viewport's drag-to-scroll, an AnimatedPosition
 * with ContinuousWithMomentum, as the UTILITY lists use it): the release speed is the last drag
 * step over its time (at least 5 ms), the speed is multiplied by 0.92 every 1/60 s (a time
 * constant of 200 ms) and the fling ends below 60 px/s. Two rules are this file's own: a finger
 * that stopped before lifting does not fling, and a fling is capped at GLIDE_MAX_V.
 *
 * Pure arithmetic on the caller's clock (ms), no system calls and no libm: the caller drives
 * it from one thread or under one lock.
 */
#ifndef AZMOD_SCROLL_GLIDE_H
#define AZMOD_SCROLL_GLIDE_H

#define GLIDE_FOLLOW_MS 24.f /* a knob step and a fling: the offset follows its goal this fast */
#define GLIDE_FLING_MS 200.f /* a fling slows with this time constant (EP147: x0.92 per 1/60 s) */
#define GLIDE_STEP_MIN_MS 5  /* a step's speed is over at least this long (EP147's floor) */
#define GLIDE_WINDOW_MS 120  /* a finger whose last step is older than this has stopped: no fling */
#define GLIDE_STOP_V 0.06f   /* px/ms: a fling this slow has stopped (EP147: 60 px/s) */
#define GLIDE_MAX_V 6.f      /* px/ms: the fastest fling */
#define GLIDE_MAX_DT 100     /* ms: a longer gap between frames is taken as this */

struct glide {
    float pos;  /* the offset shown */
    float goal; /* where it is heading */
    float v;    /* a fling's speed in px/ms, 0 when none */
    float sv;   /* the finger's last step's speed in px/ms */
    long sms;   /* ms of the finger's last step (a report that moved it) */
    long last;  /* ms of the last glide_step */
    int on;     /* still moving */
    int ns;     /* the finger's reports in this drag, 0 = none yet (capped) */
};

static inline float glide_clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi
                                : v;
}

/* Put the offset at y at once, nothing moving (a reset, a tab, a page restored). */
static inline void glide_set(struct glide *g, float y) {
    g->pos = g->goal = y;
    g->v = 0;
    g->on = 0;
    g->ns = 0;
    g->sv = 0;
    g->sms = 0;
}

/* A finger report while dragging: the offset is y now. The finger owns the offset, so a fling
 * stops. */
static inline void glide_drag(struct glide *g, float y, long ms) {
    if (g->ns == 0) { /* the drag's first report: no step yet */
        g->sv = 0;
        g->sms = ms;
    } else if (y != g->goal) { /* a step: its speed, as EP147 measures it */
        long dt = ms - g->sms;
        g->sv = (y - g->goal) / (float) (dt > GLIDE_STEP_MIN_MS ? dt : GLIDE_STEP_MIN_MS);
        g->sms = ms;
    }
    g->last = ms;
    g->pos = g->goal = y;
    g->v = 0;
    g->on = 1;
    if (g->ns < 2)
        g->ns++;
}

/* The lift at y after a drag: the offset is y, then a fling at the speed of the finger's last
 * step when that step was recent and the speed is at least minv px/ms. A finger that stopped
 * before lifting has no recent step, so it does not fling. */
static inline void glide_release(struct glide *g, float y, long ms, float minv) {
    glide_drag(g, y, ms);
    float v = ms - g->sms <= GLIDE_WINDOW_MS ? g->sv : 0;
    g->ns = 0;
    g->sv = 0;
    if (v >= minv || v <= -minv)
        g->v = glide_clampf(v, -GLIDE_MAX_V, GLIDE_MAX_V);
}

/* Glide to y (a knob step): from where it is shown now, no fling. */
static inline void glide_to(struct glide *g, float y, long ms) {
    if (!g->on)
        g->last = ms;
    g->goal = y;
    g->v = 0;
    g->on = 1;
}

/* A finger landing during a fling catches it: the offset stays where it is shown. */
static inline void glide_stop(struct glide *g) {
    g->goal = g->pos;
    g->v = 0;
    g->on = 0;
}

static inline int glide_flinging(const struct glide *g) {
    return g->v != 0;
}

/* Advance to ms within [0, max]. Returns 1 while it is still moving (draw another frame). */
static inline int glide_step(struct glide *g, long ms, float max) {
    if (max < 0)
        max = 0;
    if (!g->on) {
        g->pos = g->goal = glide_clampf(g->goal, 0, max);
        return 0;
    }
    long dt = ms - g->last;
    if (dt < 0)
        dt = 0;
    if (dt > GLIDE_MAX_DT)
        dt = GLIDE_MAX_DT;
    g->last = ms;
    float t = (float) dt;
    if (g->v != 0) {
        g->goal += g->v * t;
        g->v *= GLIDE_FLING_MS / (GLIDE_FLING_MS + t);
        if (g->v < GLIDE_STOP_V && g->v > -GLIDE_STOP_V)
            g->v = 0;
    }
    if (g->goal <= 0 || g->goal >= max) {
        g->goal = glide_clampf(g->goal, 0, max);
        g->v = 0; /* a fling stops at the end of the list */
    }
    g->pos += (g->goal - g->pos) * (t / (GLIDE_FOLLOW_MS + t));
    g->pos = glide_clampf(g->pos, 0, max);
    float left = g->goal - g->pos;
    if (g->v == 0 && left < 0.5f && left > -0.5f) {
        g->pos = g->goal;
        g->on = 0;
    }
    return 1;
}

/* The offset to draw, in whole pixels. */
static inline int glide_px(const struct glide *g) {
    return (int) (g->pos + 0.5f);
}

#endif
