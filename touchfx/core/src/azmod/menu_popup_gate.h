/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file menu_popup_gate.h
 * @brief Which MENU panel refuses its first stock button (ALL DELETE) while Playlist Editing
 * is on (menu_popup.c).
 *
 * gui::BrowseMenuWidgetController's menu type: 0 HISTORY list, 1 PLAYLIST list, 2 track row,
 * 3 tag list. On the PLAYLIST list the stock ALL DELETE removes every playlist and folder on
 * the USB, whatever level is browsed, so it is hidden and its click refused there. On the
 * HISTORY list the same button clears only the play history, and it stays stock.
 */
#ifndef AZMOD_MENU_POPUP_GATE_H
#define AZMOD_MENU_POPUP_GATE_H

/* 1 when a press on the panel's first stock button is refused for this menu type. */
static inline int mp_b1_refused(int type) {
    return type == 1;
}

#endif
