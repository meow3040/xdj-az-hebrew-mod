/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file bl_flip.h
 * @brief Whether a BEAT LOOP press inside BEAT LOOP is azmod's page flip or EP147's own.
 *
 * With stems on BEAT LOOP's first page (the swap: "Stems on BEAT LOOP 2" off), EP147 stays
 * in mode 3 and azmod keeps the page itself: the second BEAT LOOP press is swallowed and flips
 * azmod's page instead (bus_interpose.c erp_beat_loop_page). That only makes sense on a deck
 * whose stems page is actually up. On a deck without stems, BEAT LOOP's first page is EP147's
 * own, so the press must reach EP147 and move it to its second page, exactly as stock.
 *
 * Pure: no globals, so the unit test drives every input.
 */
#ifndef AZMOD_BL_FLIP_H
#define AZMOD_BL_FLIP_H

/* swap: stems take BEAT LOOP's first page (mod 21 off).
 * stems_on: the STEMS master is on.
 * deck_stems: this logical deck has stems engaged (g_erp_active: engine on and a stems track).
 * fresh_mode: pad_mode_fresh() for this side and deck (> 0 = EP147's mode + 1, published after
 * the last mode edge; 0 or -1 = not known).
 * 1 = swallow the press and flip azmod's page; 0 = pass it to EP147. */
static inline int bl_flip_swallow(int swap, int stems_on, int deck_stems, int fresh_mode) {
    return swap && stems_on && deck_stems && fresh_mode == 3 + 1;
}

#endif
