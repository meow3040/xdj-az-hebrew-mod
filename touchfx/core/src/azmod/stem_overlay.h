/* SPDX-License-Identifier: GPL-3.0-only */
/* stem_overlay.h: the deck-screen stem UI's geometry and the azstem.so getters it draws from (stem_overlay.c).
 * Included by menu.h (not on its own: it uses menu.h's types). */
#ifndef AZMOD_STEM_OVERLAY_H
#define AZMOD_STEM_OVERLAY_H
/* per-deck stem strip rect + per-button rect (fb pixels). Defined in stem_overlay.c. */
void azmod_stem_strip_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h);
/* Black plate + "STEMS" that replaces the native "BEAT LOOP" pad-mode title, centered over
 * the deck's pad area. */
void azmod_stem_title_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h);

/* Union of the title plate and the button strip: the overlay window a deck's
 * stem UI needs (azmod.c sizes win/win2 to this). */
void azmod_stem_win_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h);
void azmod_stem_lanes_win_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h);

void azmod_stem_btn_rect(const azmod_fb *fb, int deck, int i, int *x, int *y, int *w, int *h);
/* azstem.so's per-stem waveform getter, dlsym'd by azmod.c at init (both .so are preloaded
 * into EP147: an in-process call). NULL = no azstem; the strip's second row stays blank. */
extern int (*azmod_stem_wave)(int deck, const unsigned char **env, int *n, int *nstem, float *pos);
/* azstem.so's per-deck envelope identity, dlsym'd alongside azstem_wave. Bumped whenever
 * azstem replaces that deck's envelope; the mini-waveform peak cache rebuilds on a change.
 * NULL (an older azstem.so) = callers fall back to the have-edge latch. */
extern unsigned (*azmod_stem_env_gen)(int deck);
extern int (*azmod_stem_env_final)(int deck); /* NULL on an older azstem: no envelope available */
/* Second pad row, slot i: that stem's waveform lane. */
void azmod_stem_lane_rect(const azmod_fb *fb, int deck, int i, int *x, int *y, int *w, int *h);
/* 4-DECK layout: no room for the strip or the lanes, so STEM LANES shows four small stem
 * dots over the deck's source cell in the left column instead (1280x800: deck 1 y 202-240,
 * deck 2 y 324-362, x 0-168). AZMOD_STEMDOTS / AZMOD_STEMDOTS2 ("x,y,w,h" ratios) override. */
void azmod_stem_dots_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h);
/* The deck's main waveform band, where STEM LANES paints its four lanes.
 * AZMOD_WAVEBAND / AZMOD_WAVEBAND2 ("x,y,w,h" ratios) override at runtime. */
void azmod_waveband_rect(const azmod_fb *fb, int deck, int *x, int *y, int *w, int *h);
/* Waveform zoom geometry, learned by azmod.c from the data hook: track points
 * per column, where the playhead sits (in columns) and how many columns the
 * band has. Returns 0 until a full sweep has been seen. */
extern int azmod_wave_zoom(int deck, double *ppc, double *phcol, double *ncol, double *total);

/* Native stems tiles are up: suppress our overlay strips (defined in azmod.c). */
extern volatile int azmod_stem_native;

/* Are the row-2 native mini stem lanes (with their playhead) drawn for this deck
 * right now? The one predicate for them: azmod_composite draws on it and the
 * overlay loop repaints on it, so the drawing and the repaint cannot sit behind
 * different gates. */
bool azmod_native_lanes_drawn(const azmod_state *s, int deck);
#endif
