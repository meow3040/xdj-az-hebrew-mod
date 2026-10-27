/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file leadin_jump.h
 * @brief Extended Lead-In: may a beat jump land before the track's start (spin_back.c)?
 *
 * Positions are samples at the track's sample rate, 0 the track's start. A target at or after
 * the start is EP147's own question (LJ_EP147). A target before it may be jumped to while it is
 * not past the lead-in's floor, cap_ms (negative) at that sample rate: the deck can sit and play
 * from anywhere down to the floor, as a jog can take it there. A target past the floor, an
 * unknown sample rate, or a cap that is not before the start is refused (LJ_REFUSE), as EP147
 * refuses a target before the start: the jump does not happen.
 */
#ifndef AZMOD_LEADIN_JUMP_H
#define AZMOD_LEADIN_JUMP_H

#define LJ_RATE_MIN 8000L
#define LJ_RATE_MAX 384000L
enum {
    LJ_REFUSE = 0,
    LJ_ALLOW = 1,
    LJ_EP147 = 2
};

/* the floor in samples: cap_ms at rate, rounded toward the start (never past the cap) */
static inline long lj_floor(long cap_ms, long rate) {
    long long f = (long long) cap_ms * rate;
    return (long) (f / 1000); /* C division truncates toward zero: toward the start */
}
static inline int lj_decide(long target, long rate, long cap_ms) {
    if (target >= 0)
        return LJ_EP147;
    if (rate < LJ_RATE_MIN || rate > LJ_RATE_MAX || cap_ms >= 0)
        return LJ_REFUSE;
    return target >= lj_floor(cap_ms, rate) ? LJ_ALLOW : LJ_REFUSE;
}
#endif
