/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MRDJJAG_SAMPLER_TOUCH_AUDIO_H
#define MRDJJAG_SAMPLER_TOUCH_AUDIO_H
#include "stem_echo.h"
#include "stem_filter.h"
#include "stem_modulation.h"
/* Independent of sampler controls. All delay state is private to the deck's
 * render callback; control words and heartbeat are atomic. */
typedef struct {
    unsigned word, beat, seen;
    int grid_valid,grid_wait,roll_pending,roll_send;
    double grid_phase;
    int age, mask, playing, filter_mode, effect_mode, echo_reference;
    unsigned gen;
    long long position;
    stem_echo echo;
    stem_filter filter;
    stem_gate gate;
} sampler_touch_audio;
static inline void sampler_touch_publish_fx(sampler_touch_audio *s, int screen, int send, int rate, int mode, int amount) {
    if (rate < 0) rate = 0;
    if (rate > 6) rate = 5; /* 6 is the sampler-only 1/32 division */
    if (amount < 0) amount = 0;
    if (amount > 1000) amount = 1000;
    unsigned w = (screen ? 0x80000000u : 0u) | (send ? 0x40000000u : 0u) |
                 (mode == 0 ? 0x20000000u : mode == 3 ? 0x10000000u
                                        : mode == 7   ? 0x02000000u
                                        : mode == 5   ? 0x04000000u
                                        : mode == 4   ? 0x08000000u
                                                      : 0u) |
                 ((unsigned) rate << 16) | (unsigned) amount;
    __atomic_store_n(&s->word, w, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s->beat, 1u, __ATOMIC_RELEASE);
}
static inline void sampler_touch_publish(sampler_touch_audio *s, int screen, int send, int rate, int amount) {
    sampler_touch_publish_fx(s, screen, send, rate, 2, amount);
}
static inline void sampler_touch_process(sampler_touch_audio *s, float **ch, int nch, int frames,
                                      int enabled, int playing, unsigned mask, unsigned gen,
                                      int bpm_q16, long long position) {
    if (!ch || !ch[0] || nch < 1 || frames < 1) return;
    unsigned beat = __atomic_load_n(&s->beat, __ATOMIC_ACQUIRE);
    if (beat != s->seen) {
        s->seen = beat;
        s->age = 0;
    } else if (s->age < 44100) {
        s->age += frames;
        if (s->age > 44100) s->age = 44100;
    }
    unsigned w = __atomic_load_n(&s->word, __ATOMIC_ACQUIRE);
    int amount = (int) (w & 1023u), rate = (int) ((w >> 16) & 7u);
    int mode = (w & 0x02000000u) ? 7 : (w & 0x04000000u) ? 5 : (w & 0x08000000u) ? 4
                                   : (w & 0x10000000u)   ? 3
                                   : (w & 0x20000000u)   ? 0
                                                         : 2;
    int filter_mode = mode == 0;
    int panel_live = enabled && s->age < 44100 && (w & 0x80000000u);
    int active = panel_live && (mode == 0 || mode == 4 || mode == 5 || mode == 7 || amount);
    int changed = s->gen != gen;
    if (playing && s->playing && (position < 0 || (s->position >= 0 && (position < s->position || position - s->position > 22050)))) changed = 1;
    if (changed || !active || mode != s->effect_mode) {
        stem_echo_reset(&s->echo);
        stem_filter_reset(&s->filter);
        stem_gate_reset(&s->gate);
    }
    s->filter_mode = filter_mode;
    s->effect_mode = mode;
    s->gen = gen;
    s->mask = (int) mask;
    s->position = position;
    s->playing = playing;
    int period = stem_echo_delay(bpm_q16, rate), send = (w & 0x40000000u) != 0;
    if(s->grid_valid&&panel_live&&playing){s->filter.phase=s->grid_phase;s->gate.phase=s->grid_phase;}
    if(changed||!panel_live||!playing||!send){s->roll_pending=0;s->roll_send=0;}
    if (!active) return;
    if (mode == 0 || mode == 4) {
        if (!playing || !mask) {
            stem_filter_reset(&s->filter);
            stem_gate_reset(&s->gate);
            return;
        }
        if (mode == 0) stem_filter_block(&s->filter, ch, nch, frames, send, amount, period);
        else stem_gate_block(&s->gate, ch, nch, frames, send, amount, period);
        return;
    }
    /* LFO Echo: the existing filter shapes the one dry mix and the echo send.
     * On release it fades back to dry while the previously filtered tail rings. */
    if (mode == 3) {
        if (playing && mask) stem_filter_block(&s->filter, ch, nch, frames, send, amount, period);
        else stem_filter_reset(&s->filter);
    }
    /* Echo Roll feeds the captured rolling output, then stops feeding on
     * release. The same bounded delay continues with zero input for its tail.
     * The native transport has kept running underneath the roll throughout. */
    int sending = playing && mask && (w & 0x40000000u) != 0;
    /* Pause stops sends but keeps the captured delay and tail, like release. */
    int delay = sending || !s->echo.filled ? stem_echo_delay(bpm_q16, rate) : (s->echo.fade ? s->echo.next_delay : s->echo.delay);
    if (sending || !s->echo.filled) s->echo_reference = stem_echo_delay(bpm_q16, 4);
    stem_echo_begin(&s->echo, delay, amount);
    stem_echo_release_decay(&s->echo, s->echo_reference, !sending && (mode == 2 || mode == 3 || mode == 7));
    for (int i = 0; i < frames; i++) {
        int feed = sending;
        float input_l = feed ? ch[0][i] : 0.f;
        float input_r = feed ? ((nch > 1 && ch[1]) ? ch[1][i] : input_l) : 0.f;
        float out_l, out_r;
        stem_echo_step(&s->echo, input_l, input_r, &out_l, &out_r);
        /* Original/stem dry output is already in ch. Add ONLY the wet return,
         * never a second copy of the dry stem or of the native original. */
        ch[0][i] += out_l;
        if (nch > 1 && ch[1]) ch[1][i] += out_r;
    }
}
#endif
