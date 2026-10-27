/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file ht_cover.h
 * @brief Hide Track Title: a press counts for a deck's title bar only when the bar gets it.
 *
 * The tap's point alone cannot tell a press on the title bar from a press on something EP147
 * shows over it. EP147's popups (gui::OkCancelPopupWidget for EJECT and other questions, and
 * the like) are full-screen components above the WAVEFORM view that take every press, so a
 * press outside the popup's box closes it. Such a press lands on the bar's screen position but
 * never reaches the bar, and it must not toggle the title.
 *
 * So the press is asked of JUCE's own hit test (juce::Component::getComponentAt on the
 * top-level component, the same test that routes the mouse) at the moment it arrives, before
 * EP147 handles it. The deck's bar gets the press only when the component found lies inside
 * that deck's PlayerInfo widget (it, or a child of it, such as the title's label).
 *
 * Pure logic: the caller supplies the hit test and the parent walk.
 */
#ifndef AZMOD_HT_COVER_H
#define AZMOD_HT_COVER_H

#include <stdint.h>

#define HT_COVER_DECKS 4
#define HT_COVER_DEPTH 64 /* a deeper parent chain than any real one: treated as outside */

/* 1 when `hit` is `owner` or lies inside it (its parent chain reaches `owner`) */
static inline int ht_hit_in(uintptr_t hit, uintptr_t owner, uintptr_t (*parent)(uintptr_t)) {
    if (!owner)
        return 0;
    for (int i = 0; hit && i < HT_COVER_DEPTH; i++, hit = parent(hit))
        if (hit == owner)
            return 1;
    return 0;
}

/* Bit d set when the component the hit test `at` finds under (x, y) in `root` lies inside
 * owner[d], deck d's PlayerInfo widget. A deck without an owner (0), no root, or no hit: 0. */
static inline unsigned ht_cover_mask(const uintptr_t owner[HT_COVER_DECKS], uintptr_t root, int x,
                                     int y, uintptr_t (*at)(uintptr_t, int, int),
                                     uintptr_t (*parent)(uintptr_t)) {
    if (!root)
        return 0;
    uintptr_t hit = at(root, x, y);
    unsigned m = 0;
    for (int d = 0; d < HT_COVER_DECKS; d++)
        if (ht_hit_in(hit, owner[d], parent))
            m |= 1u << d;
    return m;
}

#endif
