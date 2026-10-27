/* SPDX-License-Identifier: GPL-3.0-only */
/* screen_themes.c: the Themes picker: the theme rows under the Themes switch and the theme.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
#ifdef AZMOD_HAVE_DAYMODE
/* Themes: the screen lists the themes as radio rows under its switch. They are
 * not mods (azmod.cfg is positional): pseudo row ids C_THEME_ROW0+k, never used as
 * an array index. The pick is g_azmod_theme (saved as azmod.conf's "theme"), which
 * day_mode.c's watcher reads twice a second; picking one also turns the switch on. */
#define C_THEME_ROW0 0x200
_Static_assert(C_THEME_ROW0 > AZI_COUNT, "theme pseudo rows must not collide with item ids");
#define C_THEME_N 5
static const char *const C_THEME_KEY[C_THEME_N] = {"day", "azmods", "midnight", "ember", "forest"};
int azmod_theme_keys(const char *const **keys) { /* the words azmod.conf's "theme" holds */
    *keys = C_THEME_KEY;
    return C_THEME_N;
}
static const char *const C_THEME_NAME[C_THEME_N] = {"Day", "AZ-MODS", "Midnight", "Ember",
                                                    "Forest"};
static const char *const C_THEME_DESC[C_THEME_N] = {
    "Light gray and white, for daylight.",
    "Black and charcoal with AZ-MODS orange.",
    "Deep navy and slate blue with ice-white text.",
    "Near-black reds with a coral accent.",
    "Pine and moss greens with a mint accent.",
};
int g_azmod_theme; /* menu.h: azmod.conf's "theme", as an index; __atomic builtins only */
static bool c_is_theme(int it) {
    return it >= C_THEME_ROW0 && it < C_THEME_ROW0 + C_THEME_N;
}
static int c_theme_get(void) {
    int k = __atomic_load_n(&g_azmod_theme, __ATOMIC_RELAXED);
    return k >= 0 && k < C_THEME_N ? k : 0;
}
static void c_theme_pick(azmod_state *s, int k) {
    if (k < 0 || k >= C_THEME_N)
        return;
    __atomic_store_n(&g_azmod_theme, k, __ATOMIC_RELAXED);
    s->config_dirty = true; /* azmod.c saves it to azmod.conf */
    if (!az_on(AZF_THEMES))
        az_set(AZF_THEMES, true);
    flash_and_activate(s, C_THEME_ROW0 + k);
}
#endif
