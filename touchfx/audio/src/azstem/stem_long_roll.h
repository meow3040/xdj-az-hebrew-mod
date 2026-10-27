/* Generated from the unchanged small-buffer engine for STEMS only. */
/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTEM_STEM_LONG_ROLL_H
#define AZSTEM_STEM_LONG_ROLL_H
#include <math.h>
/* One preallocated stereo history per deck. Freeze it while rolling; native
 * playback continues. No copying of a whole loop or allocation on the RT path. */
#define STEM_LONG_ROLL_FRAMES 264601
typedef struct {
    float line[STEM_LONG_ROLL_FRAMES][2];
    float mix;
    int pos, filled, frozen, length, index, old_length, old_index, transition;
} stem_long_roll;
static inline void stem_long_roll_reset(stem_long_roll *p) {
    p->pos = p->filled = p->frozen = p->length = p->index = p->transition = 0;
    p->old_length = p->old_index = 0;
    p->mix = 0.f;
}
static inline float stem_long_roll_read(const stem_long_roll *p, int length, int index, int channel) {
    int at = p->pos - length + index;
    if (at < 0) at += STEM_LONG_ROLL_FRAMES;
    if (at >= STEM_LONG_ROLL_FRAMES) at -= STEM_LONG_ROLL_FRAMES;
    int edge = length / 16;
    if (edge > 128) edge = 128;
    if (edge < 1) edge = 1;
    int distance = index < length - 1 - index ? index : length - 1 - index;
    float gain = distance >= edge ? 1.f : (float) distance / edge;
    gain = gain * gain * (3.f - 2.f * gain);
    return p->line[at][channel] * gain;
}
static inline void stem_long_roll_block(stem_long_roll *p, float **ch, int nch, int frames, int send, int value, int length) {
    if (length < 256) length = 256;
    if (length >= STEM_LONG_ROLL_FRAMES) length = STEM_LONG_ROLL_FRAMES - 1;
    if (value < 0) value = 0;
    if (value > 1000) value = 1000;
    int refill = p->frozen && length > p->filled;
    if (p->frozen && send && !refill && p->length != length) {
        p->old_length = p->length;
        p->old_index = p->index;
        p->length = length;
        p->index = 0;
        p->transition = 128;
    }
    for (int i = 0; i < frames; i++) {
        float dry[2] = {ch[0][i], nch > 1 && ch[1] ? ch[1][i] : ch[0][i]};
        if (!p->frozen && send && p->filled >= length) {
            p->frozen = 1;
            p->length = length;
            p->index = p->transition = 0;
        }
        if (!p->frozen) {
            for (int c = 0; c < 2; c++) p->line[p->pos][c] = isfinite(dry[c]) ? dry[c] : 0.f;
            if (++p->pos == STEM_LONG_ROLL_FRAMES) p->pos = 0;
            if (p->filled < STEM_LONG_ROLL_FRAMES) p->filled++;
            continue;
        }
        float target = send && !refill ? value / 1000.f : 0.f;
        p->mix += (target - p->mix) * 0.004f;
        if (!target && p->mix < 1e-5f) p->mix = 0.f;
        if (target == 1.f && p->mix > 0.99999f) p->mix = 1.f;
        for (int c = 0; c < 2; c++) {
            float loop = stem_long_roll_read(p, p->length, p->index, c);
            if (p->transition) {
                float old = stem_long_roll_read(p, p->old_length, p->old_index, c);
                loop = old + (loop - old) * ((128 - p->transition) / 128.f);
            }
            if (p->mix > 0.f) {
                float out = dry[c] + p->mix * (loop - dry[c]);
                if (!c) ch[0][i] = out;
                else if (nch > 1 && ch[1]) ch[1][i] = out;
            }
        }
        if (++p->index == p->length) p->index = 0;
        if (p->transition) {
            if (++p->old_index == p->old_length) p->old_index = 0;
            p->transition--;
        }
        if (!p->mix && (!send || refill)) {
            /* History has a gap while frozen. Start fresh instead of replaying
             * stale time after release or when a longer loop needs more input. */
            stem_long_roll_reset(p);
            refill = 0;
        }
    }
}
#endif
