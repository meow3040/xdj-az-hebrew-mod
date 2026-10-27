/* SPDX-License-Identifier: GPL-3.0-only */
/* menu_theme.h: Themes for azmod's own drawing: azm_theme / azm_theme_key / azm_ink.
 * Included by menu.h (not on its own: it uses menu.h's types). */
#ifndef AZMOD_MENU_THEME_H
#define AZMOD_MENU_THEME_H
/* Themes (mod 37) for azmod's own drawing: every pixel primitive in menu.c and text.c
 * passes its color through azm_theme(), the active theme's map (day_mode.c dm_map).
 * Without AZMOD_HAVE_DAYMODE these are identity functions. */
#ifdef AZMOD_HAVE_DAYMODE
int azmod_dm_on(void);
uint32_t azmod_dm_map(uint32_t c); /* day_mode.c dm_map: the active theme's map, identity when off */
static inline uint32_t azm_theme(uint32_t c) {
    if (!azmod_dm_on())
        return c;
    return azmod_dm_map(c);
}
int azmod_dm_key(void); /* 0 = Themes off, else 1 + the active theme: for caches of themed pixels */
static inline unsigned azm_theme_key(void) {
    return (unsigned) azmod_dm_key();
}
/* Day Mode text ink: a colored text color gets the darker shade of its hue that
 * reads at >= 4.5:1 on white and on the light panel gray (day_mode.c dm_ink, the
 * same rule as EP147's glyph path). Text only: text.c applies it to TTF glyphs. */
uint32_t azmod_dm_ink(uint32_t c);
static inline uint32_t azm_ink(uint32_t c) {
    return azmod_dm_ink(c);
}
#else
static inline uint32_t azm_theme(uint32_t c) {
    return c;
}
static inline unsigned azm_theme_key(void) {
    return 0;
}
static inline uint32_t azm_ink(uint32_t c) {
    return c;
}
#endif
#endif
