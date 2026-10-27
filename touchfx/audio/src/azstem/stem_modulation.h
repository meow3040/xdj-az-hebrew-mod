/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTEM_STEM_MODULATION_H
#define AZSTEM_STEM_MODULATION_H
#include <math.h>
typedef struct {
    double phase;
    float depth;
} stem_gate;
static inline void stem_gate_reset(stem_gate *p) {
    p->phase = 0.0;
    p->depth = 0.f;
}
static inline float stem_mod_amount(int send, int value) {
    if (!send || value <= 0) return 0.f;
    return value >= 1000 ? 1.f : value / 1000.f;
}
static inline void stem_gate_block(stem_gate *p, float **ch, int nch, int frames, int send, int value, int period) {
    float amount = stem_mod_amount(send, value);
    if (period < 256) period = 256;
    double edge = fmin(0.06, 128.0 / period);
    for (int i = 0; i < frames; i++) {
        p->depth += (amount - p->depth) * 0.002f;
        if (!amount && p->depth < 1e-5f) p->depth = 0.f;
        if (amount == 1.f && p->depth > 0.99999f) p->depth = 1.f;
        double open = 0.0;
        if (p->phase < 0.5) {
            open = fmin(p->phase / edge, (0.5 - p->phase) / edge);
            if (open > 1.0) open = 1.0;
            open = open * open * (3.0 - 2.0 * open);
        }
        float gain = 1.f - p->depth * (1.f - (float) open);
        ch[0][i] *= gain;
        if (nch > 1 && ch[1]) ch[1][i] *= gain;
        p->phase += 1.0 / period;
        if (p->phase >= 1.0) p->phase -= 1.0;
    }
}
#endif
