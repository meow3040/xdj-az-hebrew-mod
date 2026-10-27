/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file pad_leds.c
 * @brief Pad-LED color publishing (stems colors, pad-mode buttons)
 *
 * The overlay thread publishes colors for the erp-fd ioctl interposer, which copies them into
 * the outgoing LED frame: g_pad_led (one 32-byte pad block per deck, stem colors, behind a
 * sequence counter) and g_mode_rgbw / g_mode2_rgbw (one 0x01RRGGBB word per pad-mode button,
 * primary and secondary page), filled from the chosen palette by publish_mode_colors(). Each
 * color is one atomic word and each block is copied whole or not at all (bus_cfg.h), so the
 * bus thread never sends half of an update. LED bytes are linear light, so each
 * palette entry carries a pre-encoded value; the azmod-ledraw flag file switches to the raw
 * sRGB swatch for an A/B comparison.
 *
 * The frame copy is the only path. Inline hooks on the LED serializer (0x2148b78) and on
 * meow::HuiLed getColor (0x21edba0) froze the real-time LED path on hardware, so colors are
 * never injected there.
 *
 * The file also holds the screen-detection constants and counters used by the overlay thread
 * (tab-row geometry, WAVE_SEEN_MS, paint/composite counters, azstem availability paths) and
 * includes day_mode.c.
 *
 * Entry point: publish_mode_colors().
 */
/* ---- pad-LED override (erp LED frame, pad block @ bytes 30-61, 8x4 entries) ----
 * Entry = {brightness, Green, Red, Blue} (GRB); idle 08 0b 04 00, lit 10 ff 00 00.
 * The overlay publishes the 32-byte block (pads 1-4 = stem color full/dim, pads 5-8 =
 * off) from g_state.stem_on; the erp-fd ioctl interposer copies it into the LED frame
 * (past the CRC'd prefix, so no CRC fix) when that deck is Beat Loop + engaged. */
/* Pad-mode buttons live at 0x0e + 4*j (same {R,G,B,0} cells, same +0x38 deck stride),
 * j = bit index of the ERP mode mask, so BEAT LOOP (0x02) = j 1. While STEMS is armed
 * that button is recolored green: full brightness when it is the selected mode, dimmed
 * (same >>3 as the pads) otherwise, like every other unselected button. */
static _Atomic int
    g_bl_btn_green[2]; /* telemetry: BEAT LOOP button stems-green per side, 0 no / 1 dim / 2 full */
static struct seqblk g_pad_led[AZMOD_STEM_DECKS]; /* published by overlay (seqblk_write) */
/* Pad-mode color picker: the DJ's chosen mode-button colors (bus_rgb_pack: 0x01RRGGBB,
 * j=0..3); the erp interposer writes them into the LED frame at 0x0e+4*j (both decks),
 * whether or not stems are armed. 0 leaves that button native. */
static uint32_t g_mode_rgbw[AZMOD_PADMODE_COUNT];  /* __atomic builtins only */
static uint32_t g_mode2_rgbw[AZMOD_PADMODE_COUNT]; /* the same, secondary page (lime frames) */
static volatile unsigned char g_padcell_tlm[8][3]; /* telemetry: side 0 pad cells as sent */
static volatile unsigned char
    g_modecell_tlm[4][3]; /* telemetry: side 0 mode-button cells as written */
/* The LED bytes are linear light, so a palette entry carries both its on-screen sRGB
 * (`swatch`) and that color sRGB->linear encoded (`led`); `led` belongs in the frame
 * (encoding note in pad_palette.h). The azmod-ledraw flag file writes the raw sRGB instead,
 * for an A/B on hardware without a rebuild. */
static int g_led_raw = -1;
static int led_raw_enabled(void) {
    if (g_led_raw < 0)
        g_led_raw = (AZ_PROBE_ON("/home/root/settings/azmod-ledraw"));
    return g_led_raw;
}
static uint32_t pad_led_value(const azmod_pad_color *e) {
    return led_raw_enabled() ? e->swatch : e->led;
}
static void publish_mode_colors(const azmod_state *s) {
    static const azmod_pad_color PAL[AZMOD_PAD_PALETTE_COUNT] = AZMOD_PAD_PALETTE;
    for (int j = 0; j < AZMOD_PADMODE_COUNT; j++) {
        int pi = s->padmode_color[j];
        /* master enable off: publish nothing, so every button shows EP147's own color */
        int on = s->pad_colors_on && pi >= 0 && pi < AZMOD_PAD_PALETTE_COUNT;
        __atomic_store_n(&g_mode_rgbw[j], bus_rgb_pack(on, on ? pad_led_value(&PAL[pi]) : 0),
                         __ATOMIC_RELAXED);
        int p2 = s->padmode_color2[j]; /* the secondary page, same rules */
        int on2 = s->pad_colors_on && p2 >= 0 && p2 < AZMOD_PAD_PALETTE_COUNT;
        __atomic_store_n(&g_mode2_rgbw[j], bus_rgb_pack(on2, on2 ? pad_led_value(&PAL[p2]) : 0),
                         __ATOMIC_RELAXED);
    }
}

static void save_config(const azmod_state *s); /* fwd: load_config self-heals via it */

static long now_ms(void);





/* SOURCE / deck-view detection correlates the top-left title region against reference
 * bitmaps (srcref.h, wavref.h; regenerate them if a firmware moves the title). Their
 * coords are pixels at this reference size and must be scaled by the actual X root
 * size, or the region misses and nothing ever matches. SRCREF_* and WAVREF_* share the
 * geometry; only the ink mask differs. */
#define TITLE_REF_W 1280 /* srcref/wavref coords are calibrated at the AZ's true 1280x800 */
#define TITLE_REF_H 800

/* How many times a screen-tab edge on mixer byte 32 dropped the deck-view latch. The
 * physical buttons are the only route between views, so this should count every
 * switch; 0 after pressing BROWSE means the edge never reaches us. */
static _Atomic long g_viewleave_n = 0;
static _Atomic long g_waveview = 0; /* the WaveformView instance, captured on 1st paint */
static int g_setvis_hooked = 0;
static _Atomic long g_paint_n = 0; /* paints/s, logged so the window below can be tuned */
/* Our overlay's software composite (fb_ground + azmod_composite), per dirty frame: count,
 * total and worst us since the last /tmp/azmod-rates line. */
static _Atomic long g_comp_n, g_comp_us, g_comp_max;
static _Atomic long g_rtick_n = 0; /* LinuxRepaintManager::timerCallback ticks/s */
/* How stale a paint may be before the deck view counts as gone. WaveformView::paint runs
 * at a steady 32/s (31 ms) while the deck view is up and 0/s once it is not, so 65 ms
 * tolerates two missed frames. Lower starts blinking the overlay whenever EP147 drops a
 * frame with the view up. */
#define WAVE_SEEN_MS 65
/* Which deck layout is up: the tab row at the top ("2 DECK | 2/4 DECK | 4 DECK") draws
 * the selected one in EP147's blue accent. Sampled with one small XGetImage when the
 * deck view appears; per-frame grabs are too expensive.
 *   0 = two waveform bands (2 DECK and 2/4 DECK share the same geometry)
 *   1 = four bands (4 DECK); our band rects do not cover it, so the lanes
 *       hide rather than paint over the wrong deck. */
#define TABROW_X 190
#define TABROW_Y 26
#define TABROW_W 180
#define TABROW_H 16
#define TAB4_X 343 /* center of the "4 DECK" label at 1280x800 */


/* Fade-to-clear for the open menu: with no compositor for real alpha, an X-Shape mask is
 * dithered across the fade strip (solid on the left, more holes toward the right) so
 * EP147 shows through. Holes are SHAPE_BLK-sized blocks to keep the region cheap, and the
 * mask pixmap is built once and cached. */
#define SHAPE_BLK 6


/* azstem.so publishes, per deck, whether the loaded track has stems. */
#define AVAIL_PATH "/home/root/settings/azstem.avail"
#define AVAIL2_PATH "/home/root/settings/azstem.avail2"
#define AVAIL3_PATH "/home/root/settings/azstem.avail3"
#define AVAIL4_PATH "/home/root/settings/azstem.avail4"

/* azstem.so publishes each deck's loaded track name; when it changes, that deck's stem
 * toggles reset to all on so a new track does not inherit the previous muted stems. */
#define CURTRACK_PATH "/home/root/settings/azstem.curtrack"
#define CURTRACK2_PATH "/home/root/settings/azstem.curtrack2"
#define CURTRACK3_PATH "/home/root/settings/azstem.curtrack3"
#define CURTRACK4_PATH "/home/root/settings/azstem.curtrack4"





#include "day_mode.c" /* Themes: included early because ov_hook and the constructor call into it */
