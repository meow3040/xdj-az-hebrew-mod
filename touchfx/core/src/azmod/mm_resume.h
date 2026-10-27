/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file mm_resume.h
 * @brief Reopen the Mod Menu when the SOURCE view comes back.
 *
 * The docked Mod Menu lives on the SOURCE view. Leaving SOURCE (BROWSE, TAG LIST, PLAYLIST,
 * SEARCH, a screen tab, a track load) hides it with the view; returning to SOURCE shows it
 * again as it was: focused if it was focused, the preview if it was the preview. Pressing
 * SOURCE while the menu is up closes it for good, as does picking a real source.
 *
 * The level remembered: 0 nothing, 1 the preview (MOD MENU the selected source, knob on the
 * list), 2 focused (the knob in the menu). A leave only raises it, so a later, weaker sample
 * of the same leave (focus already dropped by the close) cannot lower it. Nothing persists
 * across an EP147 restart: the level starts at 0.
 *
 * The callers in bus_interpose.c and source_list_entry.c keep the level in one int
 * (g_mm_resume), written by the bus thread and the JUCE message thread through the __atomic
 * helpers at the end.
 */
#ifndef AZMOD_MM_RESUME_H
#define AZMOD_MM_RESUME_H
#include <stdint.h>

#define MMR_NONE 0
#define MMR_PREVIEW 1
#define MMR_FOCUSED 2

/* screen-select bits on mixer byte 32 */
#define MMR_BTN_SOURCE 0x80u
#define MMR_BTN_OTHERS 0x78u /* BROWSE 0x40, TAG LIST 0x20, PLAYLIST 0x10, SEARCH 0x08 */

/* What the menu shows right now, as a level. */
static inline int mmr_level(int open, int want, int focused) {
    if (!open || !want)
        return MMR_NONE;
    return focused ? MMR_FOCUSED : MMR_PREVIEW;
}
/* A leave raises the remembered level, never lowers it. */
static inline int mmr_raise(int cur, int lvl) {
    return lvl > cur ? lvl : cur;
}
/* A screen-select press (the rising bits). SOURCE with the menu up closes it for good; SOURCE
 * with the menu hidden (pressed from another view) is the way back and keeps what was
 * remembered; another screen button leaves SOURCE and remembers what is up. */
static inline int mmr_on_press(int cur, uint8_t rise, int open, int want, int focused) {
    if (rise & MMR_BTN_SOURCE)
        return mmr_level(open, want, focused) != MMR_NONE ? MMR_NONE : cur;
    if (rise & MMR_BTN_OTHERS)
        return mmr_raise(cur, mmr_level(open, want, focused));
    return cur;
}
/* The SOURCE view was hidden by any path (touch tabs, EP147 on its own). `want` is the
 * selection mirror at that moment: a real source picked first has already cleared it, so a
 * deliberate pick never arms a reopen. */
static inline int mmr_on_hide(int cur, int open, int want, int focused) {
    return mmr_raise(cur, mmr_level(open, want, focused));
}
/* The shared level, updated by either thread: one compare-and-swap loop each. */
static inline void mmr_press(int *lvl, uint8_t rise, int open, int want, int focused) {
    int cur = __atomic_load_n(lvl, __ATOMIC_RELAXED), nxt;
    do
        nxt = mmr_on_press(cur, rise, open, want, focused);
    while (nxt != cur &&
           !__atomic_compare_exchange_n(lvl, &cur, nxt, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}
static inline void mmr_hide(int *lvl, int open, int want, int focused) {
    int cur = __atomic_load_n(lvl, __ATOMIC_RELAXED), nxt;
    do
        nxt = mmr_on_hide(cur, open, want, focused);
    while (nxt != cur &&
           !__atomic_compare_exchange_n(lvl, &cur, nxt, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}
/* SOURCE shown: the level to bring back, consumed. */
static inline int mmr_take(int *lvl) {
    return __atomic_exchange_n(lvl, MMR_NONE, __ATOMIC_RELAXED);
}
/* A real source picked. */
static inline void mmr_forget(int *lvl) {
    __atomic_store_n(lvl, MMR_NONE, __ATOMIC_RELAXED);
}

/* The page. Hiding SOURCE closes the docked menu (its mode becomes AZ_CLOSED, 0) and an open
 * starts at the last category (azmod_menu_reset), so a resume alone brought back Extras, not
 * Extras > Clock > Time Zone. The rest of the page (the selection, the right pane, the
 * scroll, the Community Mods view) is left as it was by the close. So the overlay keeps the mode
 * each close ends (mmr_page_keep), a resume arms it once the MOD MENU row is selected again
 * (mmr_page_arm, message thread), and the open that follows takes it (mmr_page_take, overlay
 * thread): that mode instead of the reset. Nothing arms it after a real close (SOURCE pressed
 * with the menu up, or a real source picked: the level is MMR_NONE, srcrow_resume never runs)
 * and those disarm one still pending (mmr_page_disarm), so such an open resets as before. */
#define MMR_PAGE_NONE 0 /* AZ_CLOSED: no page, open at the last category */
/* A close: the mode it ends; a close of an already closed menu keeps the one kept. */
static inline int mmr_page_keep(int kept, int mode) {
    return mode != MMR_PAGE_NONE ? mode : kept;
}
static inline void mmr_page_arm(int *armed) {
    __atomic_store_n(armed, 1, __ATOMIC_RELEASE);
}
static inline void mmr_page_disarm(int *armed) {
    __atomic_store_n(armed, 0, __ATOMIC_RELEASE);
}
/* An open: the kept page if a resume armed it (taken once), else MMR_PAGE_NONE. */
static inline int mmr_page_take(int *armed, int kept) {
    return __atomic_exchange_n(armed, 0, __ATOMIC_ACQ_REL) ? kept : MMR_PAGE_NONE;
}
#endif
