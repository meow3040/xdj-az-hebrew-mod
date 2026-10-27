/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZSTEM_DRUM_XY_AUDIO_H
#define AZSTEM_DRUM_XY_AUDIO_H
/* One atomic packet per deck: active + rate + attenuation. A heartbeat lease
 * stops new UI retriggers if the overlay stalls. seen/age/gain are RT-owned. */
typedef struct {
    unsigned word, beat;
    unsigned seen;
    int age, gain;
} drum_xy_audio;
static inline unsigned drum_xy_pack(int active, int rate, int gain) {
    if (rate < 0) rate = 0;
    if (rate > 5) rate = 5;
    if (gain < 0) gain = 0;
    if (gain > 1000) gain = 1000;
    return (active ? 0x80000000u : 0u) | ((unsigned) rate << 16) | (unsigned) gain;
}
static inline void drum_xy_publish(drum_xy_audio *s, int active, int rate, int gain) {
    __atomic_store_n(&s->word, drum_xy_pack(active, rate, gain), __ATOMIC_RELEASE);
    __atomic_add_fetch(&s->beat, 1u, __ATOMIC_RELEASE);
}
/* Called once per audio block, no time queries, allocations or locks. */
static inline unsigned drum_xy_audio_tick(drum_xy_audio *s, int frames, int sr, int armed, int physical) {
    unsigned beat = __atomic_load_n(&s->beat, __ATOMIC_ACQUIRE);
    if (beat != s->seen) {
        s->seen = beat;
        s->age = 0;
    } else if (s->age < sr) {
        s->age += frames;
        if (s->age > sr) s->age = sr;
    }
    unsigned word = __atomic_load_n(&s->word, __ATOMIC_ACQUIRE);
    int live = armed && s->age < sr && (word & 0x80000000u);
    if (live) s->gain = (int) (word & 1023u);
    else if (physical || !armed) s->gain = 1000;
    return live ? word : 0;
}
#endif
