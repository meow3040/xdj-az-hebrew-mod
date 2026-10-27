/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTEM_STEM_ECHO_H
#define AZSTEM_STEM_ECHO_H
#include <math.h>
/* Two beats at 40 BPM, stereo 44.1 kHz. Separate from the sampler's DSP. */
#define STEM_ECHO_FRAMES 132301
#define STEM_ECHO_FADE 882
typedef struct {
    float line[STEM_ECHO_FRAMES][2];
    int pos, filled, delay, next_delay, fade;
    float wet, target, envelope, release_feedback;
} stem_echo;
static inline int stem_echo_delay(int bpm_q16, int rate) {
    double bpm = bpm_q16 > 0 ? bpm_q16 / 65536.0 : 120.0;
    if (bpm < 40.0) bpm = 40.0;
    if (bpm > 220.0) bpm = 220.0;
    if (rate == 6) return (int) (44100.0 * 60.0 / bpm / 32.0 + 0.5); /* sampler-only */
    if (rate < 0) rate = 0;
    if (rate > 5) rate = 5;
    return (int) (44100.0 * 60.0 / bpm * (1 << rate) / 16.0 + 0.5);
}
static inline void stem_echo_reset(stem_echo *e) {
    e->pos = e->filled = e->fade = 0;
    e->wet = e->envelope = e->release_feedback = 0.f;
}
static inline void stem_echo_begin(stem_echo *e, int delay, int amount) {
    if (delay < 1) delay = 1;
    if (delay >= STEM_ECHO_FRAMES) delay = STEM_ECHO_FRAMES - 1;
    e->target = amount / 1000.f;
    if (!e->filled) {
        e->delay = e->next_delay = delay;
        e->fade = 0;
    } else if (!e->fade && delay != e->delay) {
        e->next_delay = delay;
        e->fade = STEM_ECHO_FADE;
    }
}
/* On release only, short divisions decay over the same elapsed time as
 * the unchanged one-beat setting. Held sends retain their original feedback and repeat timing;
 * one and two beats also keep the exact original release feedback.
 * Calculate once per block, never powf per audio sample. */
static inline void stem_echo_release_decay(stem_echo *e, int reference, int released) {
    e->release_feedback = 0.f;
    int delay = e->fade ? e->next_delay : e->delay;
    if (released && reference > 0 && delay > 0 && delay < reference) {
        float base = 0.20f + 0.52f * e->wet;
        e->release_feedback = powf(base, delay / (float) reference);
    }
}
static inline float stem_echo_read(const stem_echo *e, int delay, int channel) {
    if (e->filled < delay) return 0.f;
    int read = e->pos - delay;
    if (read < 0) read += STEM_ECHO_FRAMES;
    return e->line[read][channel];
}
/* Wet only: dry deck audio stays at unity. A gentle music-specific duck
 * replaces the sampler's strong transient ducking. */
static inline void stem_echo_step(stem_echo *e, float l, float r, float *wet_l, float *wet_r) {
    e->wet += (e->target - e->wet) * 0.005f;
    float peak = fmaxf(fabsf(l), fabsf(r));
    e->envelope = peak > e->envelope ? peak : e->envelope * 0.9995f;
    float duck = 1.f / (1.f + 0.5f * e->envelope);
    float feedback = e->release_feedback > 0.f ? e->release_feedback : 0.20f + 0.52f * e->wet;
    float in[2] = {l, r}, out[2];
    for (int c = 0; c < 2; c++) {
        float delayed = stem_echo_read(e, e->delay, c);
        if (e->fade) {
            float next = stem_echo_read(e, e->next_delay, c);
            float t = (STEM_ECHO_FADE - e->fade) / (float) STEM_ECHO_FADE;
            delayed += (next - delayed) * t;
        }
        float next = (isfinite(in[c]) ? in[c] : 0.f) + delayed * feedback;
        if (!isfinite(next)) next = 0.f;
        if (next > 4.f) next = 4.f;
        if (next < -4.f) next = -4.f;
        if (fabsf(next) < 1e-12f) next = 0.f;
        e->line[e->pos][c] = next;
        out[c] = delayed * e->wet * 0.60f * duck;
    }
    if (e->fade && !--e->fade) e->delay = e->next_delay;
    if (++e->pos == STEM_ECHO_FRAMES) e->pos = 0;
    if (e->filled < STEM_ECHO_FRAMES) e->filled++;
    *wet_l = out[0];
    *wet_r = out[1];
}
#endif
