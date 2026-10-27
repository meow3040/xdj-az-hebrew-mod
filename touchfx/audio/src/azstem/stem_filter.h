/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTEM_STEM_FILTER_H
#define AZSTEM_STEM_FILTER_H
#include <math.h>
/* Causal stereo LFO low-pass, no lookahead/delay ring or RT allocation.
 * Period is one complete BPM-synced sweep. Coefficients update every 32 frames
 * and interpolate per sample; depth/release crossfade to exact dry. */
typedef struct {
    float low[2][2];
    float coefficient, mix;
    double phase;
    int initialized;
} stem_filter;
static inline void stem_filter_reset(stem_filter *p) {
    for (int c = 0; c < 2; c++) p->low[c][0] = p->low[c][1] = 0.f;
    p->coefficient = p->mix = 0.f;
    p->phase = 0.0;
    p->initialized = 0;
}
static inline void stem_filter_block(stem_filter *p, float **ch, int nch, int frames, int send, int value, int period) {
    if (value < 0) value = 0;
    if (value > 1000) value = 1000;
    if (period < 256) period = 256;
    float amount = value / 1000.f, target = p->coefficient;
    for (int i = 0; i < frames; i++) {
        float dry[2] = {ch[0][i], nch > 1 && ch[1] ? ch[1][i] : ch[0][i]};
        if (!(i & 31)) {
            double sweep = 0.5 - 0.5 * cos(6.283185307179586 * p->phase);
            double frequency = 18000.0 * pow(100.0 / 18000.0, amount * sweep);
            target = (float) (1.0 - exp(-6.283185307179586 * frequency / 44100.0));
        }
        if (!p->initialized) {
            for (int c = 0; c < 2; c++) p->low[c][0] = p->low[c][1] = isfinite(dry[c]) ? dry[c] : 0.f;
            p->coefficient = target;
            p->initialized = 1;
        }
        p->coefficient += (target - p->coefficient) * 0.04f;
        float target_mix = send && value ? 1.f : 0.f;
        p->mix += (target_mix - p->mix) * 0.002f;
        if (!target_mix && p->mix < 1e-5f) p->mix = 0.f;
        for (int c = 0; c < 2; c++) {
            float input = isfinite(dry[c]) ? dry[c] : 0.f;
            p->low[c][0] += p->coefficient * (input - p->low[c][0]);
            p->low[c][1] += p->coefficient * (p->low[c][0] - p->low[c][1]);
            if (p->mix > 0.f) {
                float out = input + p->mix * (p->low[c][1] - input);
                if (!c) ch[0][i] = out;
                else if (nch > 1 && ch[1]) ch[1][i] = out;
            }
        }
        p->phase += 1.0 / period;
        if (p->phase >= 1.0) p->phase -= 1.0;
    }
}
#endif
