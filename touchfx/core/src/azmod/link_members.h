/* SPDX-License-Identifier: GPL-3.0-only */
/* link_members.h: which PRO DJ LINK player numbers are on the network, kept from EP147's own
 * NetworkManager member events (link_members.c), and the Stem Volume rule that depends on them.
 * Pure functions over plain values, so the host tests compile this file as is. */
#ifndef AZMOD_LINK_MEMBERS_H
#define AZMOD_LINK_MEMBERS_H
#include <stdint.h>

#define LM_PLAYER_MAX 63 /* bit n of the set = player n; 0 and above 63 are never recorded */

/* Record player `player` joining (present = 1) or leaving (0) in *set. */
static inline void lm_note(uint64_t *set, uint32_t player, int present) {
    if (player < 1 || player > LM_PLAYER_MAX)
        return;
    if (present)
        __atomic_fetch_or(set, 1ull << player, __ATOMIC_RELEASE);
    else
        __atomic_fetch_and(set, ~(1ull << player), __ATOMIC_RELEASE);
}

/* Stem Volume borrows the CH3 and CH4 EQ knobs for deck 1's and deck 2's stems, so it may only
 * do that while both channels are idle. It stands down when:
 *   - it cannot tell: the deck-track export or the member watch is missing;
 *   - internal deck 3 or 4 has a track (the channel belongs to that deck);
 *   - a PRO DJ LINK player holds number 3 or 4 (its audio and its EQ are on that channel).
 * Returns 1 to stand down (the EQ stays stock), 0 when CH3/CH4 may set stem volumes. */
static inline int stemvol_stand_down(int have_track_fn, int deck3_track, int deck4_track,
                                     int watch_ok, uint64_t players) {
    if (!have_track_fn || !watch_ok)
        return 1;
    if (deck3_track || deck4_track)
        return 1;
    return (players & ((1ull << 3) | (1ull << 4))) != 0;
}
#endif
