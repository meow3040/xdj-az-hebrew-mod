/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file wave_att.h
 * @brief The stem waveform's height rule: which share of a track position's stems is audible.
 *
 * The native waveform's height at a track position is scaled by the share of the four stems'
 * peak that is still audible there. A stem counts with its envelope peak times its Stem Volume
 * gain while its pad toggle is on, and not at all while it is off. With every stem on at unity
 * the result is exactly unity, so a deck nobody touched renders stock.
 *
 * The result is capped at unity (WATT_UNITY). Every consumer packs the scaled height back into
 * a few bits next to other fields (5 bits beside the color in the RGB provider, for example), so
 * a factor above 256 would carry into those fields. A boosted stem therefore raises its share
 * of the mix, but the waveform never grows taller than EP147 drew it.
 *
 * Gains are quantized to 1/256 of unity before they reach the curve or the rebuild key. The
 * tallest height EP147 stores is a byte (the 3-band provider), so a step of 1/256 moves it by at
 * most one unit; coarser steps showed as visible jumps on a slow knob turn.
 *
 * Two cadences. The main waveform re-renders every frame while a track plays, so a new curve
 * costs only its rescale (about 0.04 ms per deck per rebuild on a fast arm64 core for a
 * 6-minute track) and is rebuilt at up to ~33 Hz while a knob turns. The overview bars and
 * the deck strips are re-rendered by EP147 itself, one setOverview per tile, so a gain-only
 * change refreshes them at most every WATT_OV_MIN_MS.
 *
 * waveform.c (build_atten) and mod_menu_overlay.c (ov_rebuild_attenuation) are the users;
 * tools/unit-tests/waveatt_test.c builds this header natively. Nothing here allocates, blocks
 * or calls the system.
 */
#ifndef AZMOD_WAVE_ATT_H
#define AZMOD_WAVE_ATT_H

#define WATT_NSTEM 4
#define WATT_UNITY 256  /* attenuation fixed point: 256 = the height EP147 drew */
#define WATT_GQ_ONE 256 /* gain quantum: 256 = unity */
#define WATT_GQ_MAX 512 /* the knob's top, +6 dB */
/* A Stem Volume change alone rebuilds a deck's curve at most this often: about EP147's stock
 * 30 Hz paint. The gains are read on every overlay tick (10 ms while the WAVEFORM screen
 * shows), so a sweep's final value lands on the first tick after the interval. A pad toggle or
 * a track change is never delayed. */
#define WATT_GAIN_MIN_MS 30
/* ...and refreshes the deck's overview tiles at most this often (EP147 re-renders each one). */
#define WATT_OV_MIN_MS 100
/* While a load decodes, azstem's frame count grows on every overlay tick and the envelope fills
 * in behind it. The curve follows that at most this often, and its overview tiles refresh on the
 * gain cadence (WATT_OV_MIN_MS) rather than at once: rebuilding and re-rendering every tile on
 * every tick kept EP147's message thread busy for the whole decode. A pad toggle, a track change
 * or a new envelope is never delayed. */
#define WATT_PROG_MIN_MS 500

/* A stem gain x1000 (azstem_stem_gain) -> 0..WATT_GQ_MAX, rounded. */
static inline int watt_gain_q(int gk) {
    if (gk <= 0)
        return 0;
    long q = ((long) gk * WATT_GQ_ONE + 500) / 1000;
    return q > WATT_GQ_MAX ? WATT_GQ_MAX : (int) q;
}

/* One position's factor, 0..WATT_UNITY. v = each stem's envelope peak there, on = the pad
 * toggles (bit k = stem k), gq = quantized gains. No energy at all keeps the height. */
static inline unsigned short watt_value(const unsigned v[WATT_NSTEM], unsigned on,
                                        const int gq[WATT_NSTEM]) {
    unsigned long all = 0, num = 0;
    for (int k = 0; k < WATT_NSTEM; k++) {
        all += v[k];
        if (on & (1u << k))
            num += (unsigned long) v[k] * (unsigned) gq[k];
    }
    if (!all)
        return WATT_UNITY;
    /* num / WATT_GQ_ONE is the audible peak sum, so the factor is
     * num * WATT_UNITY / WATT_GQ_ONE / all, rounded. At unity gains this is
     * (on_sum * 256 + all / 2) / all, the toggle-only rule. */
    unsigned long a = (num * (WATT_UNITY / WATT_GQ_ONE) + all / 2) / all;
    return (unsigned short) (a > WATT_UNITY ? WATT_UNITY : a);
}

/* The stem that colors this position: the loudest audible one, or -1 when none is louder than
 * near-silence (8 of 255 at unity), which keeps EP147's own color. */
static inline int watt_color_stem(const unsigned v[WATT_NSTEM], unsigned on,
                                  const int gq[WATT_NSTEM]) {
    unsigned long best = 0;
    int bk = -1;
    for (int k = 0; k < WATT_NSTEM; k++) {
        if (!(on & (1u << k)))
            continue;
        unsigned long w = (unsigned long) v[k] * (unsigned) gq[k];
        if (w > best) {
            best = w;
            bk = k;
        }
    }
    return best > 8ul * WATT_GQ_ONE ? bk : -1;
}

/* 1 = the curve is unity everywhere (every stem on at unity gain): scaling is a no-op. */
static inline int watt_identity(unsigned on, const int gq[WATT_NSTEM]) {
    for (int k = 0; k < WATT_NSTEM; k++)
        if (!(on & (1u << k)) || gq[k] != WATT_GQ_ONE)
            return 0;
    return 1;
}

/* The quantized gains as one comparable key, 16 bits per stem. */
static inline unsigned long long watt_gain_key(const int gq[WATT_NSTEM]) {
    unsigned long long g = 0;
    for (int k = 0; k < WATT_NSTEM; k++)
        g |= ((unsigned long long) gq[k] & 0xffffu) << (16 * k);
    return g;
}

/* Should the overlay tick rebuild a deck's curve now?  key = the track and pad toggle
 * signature, gkey = watt_gain_key, *_prev = what the last rebuild used, have = a curve exists,
 * now_ms / last_ms = monotonic times of this tick and of the last rebuild. A key change
 * rebuilds at once, as does a missing curve (azstem publishes its frames just before the
 * envelope, so the first try after a load can land in between). A gain-only change waits out
 * WATT_GAIN_MIN_MS; the caller keeps the old gkey until it rebuilds, so the change is retried
 * and never lost. */
static inline int watt_should_build(unsigned key, unsigned long long gkey, unsigned key_prev,
                                    unsigned long long gkey_prev, int have, long now_ms,
                                    long last_ms) {
    if (key != key_prev || (key && !have))
        return 1;
    if (gkey != gkey_prev)
        return now_ms - last_ms >= WATT_GAIN_MIN_MS;
    return 0;
}

/* Should the overlay tick rebuild a deck's curve for the decode's progress alone?  fr / fr_prev =
 * azstem's frame count now and at the last rebuild. */
static inline int watt_progress_due(long fr, long fr_prev, long now_ms, long last_ms) {
    return fr != fr_prev && now_ms - last_ms >= WATT_PROG_MIN_MS;
}

/* Should this tick refresh the deck's overview tiles?  built = the curve was rebuilt on this
 * tick, keyed = for a track or pad toggle change. A keyed rebuild refreshes at once; a gain-only
 * one leaves the refresh pending, and a pending refresh runs once WATT_OV_MIN_MS has passed
 * since the last one, so the final value of a sweep always reaches the tiles. */
static inline int watt_ov_step(int built, int keyed, int *pend, long *last_ms, long now_ms) {
    if (built && keyed) {
        *pend = 0;
        *last_ms = now_ms;
        return 1;
    }
    if (built)
        *pend = 1;
    if (*pend && now_ms - *last_ms >= WATT_OV_MIN_MS) {
        *pend = 0;
        *last_ms = now_ms;
        return 1;
    }
    return 0;
}

#endif
