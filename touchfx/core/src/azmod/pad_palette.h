/* SPDX-License-Identifier: GPL-3.0-only */
/* pad_palette.h: the pad-mode button colors: the palette (index-stable: azmod.cfg stores the indices), the picker grid, the button labels.
 * Included by menu.h (not on its own: it uses menu.h's types). */
#ifndef AZMOD_PAD_PALETTE_H
#define AZMOD_PAD_PALETTE_H
/* ---- Pad-mode button color picker (AZ_COLORS) ----------------------------
 * The 4 physical pad-mode buttons per deck (j=0..3, the bit index of the ERP mode mask).
 * The picker assigns each button a color; pad_leds.c publishes them and the erp ioctl
 * interposer (bus_interpose.c) writes them into the ERP LED frame (frame[0x0e + 4*j],
 * +0x38 deck stride).
 *
 * Encoding: the three LED bytes are linear light, not sRGB. Stock EP147 does linear
 * arithmetic on them (a button's unselected state is its hue scaled by a flat ~4.7% in the
 * byte domain: HOT CUE 98ff9a -> 070c07, BEAT LOOP d16300 -> 0a0400), and decoding the stock
 * triples through the sRGB EOTF gives the colors the buttons visibly are (d16300 -> #EAA700
 * orange, 98ff9a -> #CBFFCC pale mint). An sRGB value sent as-is lifts the non-dominant
 * channels ~10x and washes the color out (#FF2020 emits as #FF6363). So each palette entry
 * carries both: `swatch`, what the picker draws (sRGB), and `led`, the same color encoded
 * sRGB->linear, which goes in the frame. The 2-bit LedLuminance codes at 0x09..0x0c (LUT
 * 0x2e47820 is the identity {0,1,2}) stay constant across a pad-mode sweep and do not gate
 * the colors.
 *
 * /home/root/settings/azmod-ledraw makes pad_leds.c publish `swatch` verbatim instead, for an A/B
 * on hardware without a rebuild. */
typedef struct {
    uint32_t swatch;
    uint32_t led;
} azmod_pad_color;
#define AZMOD_PADMODE_COUNT 4
/* Pad Colors rows: each button's primary page and its secondary page (HOT CUE 2,
 * BEAT LOOP 2, SLIP LOOP 2, BEAT JUMP 2). Pick index
 * sel = 2*j + page; page 0 -> padmode_color[j], page 1 -> padmode_color2[j]. */
#define AZMOD_PADPICK_COUNT (2 * AZMOD_PADMODE_COUNT)
#define AZMOD_PAD_PALETTE_COUNT 28
/* {on-screen sRGB, LED linear}. Index-stable: azmod.cfg line 2 stores these indices, so
 * entries are only ever appended; the on-screen order is AZMOD_PAD_GRID_ORDER below, never
 * this list. `led` = sRGB->linear of `swatch` (((v+.055)/1.055)^2.4 for v>.04045, else
 * v/12.92); use the same formula for additions. */
#define AZMOD_PAD_PALETTE                                                   \
    {{0xFF0000u, 0xFF0000u}, /*  0 RED        */                            \
     {0xFF6000u, 0xFF1E00u}, /*  1 ORANGE     */                            \
     {0xFF9E00u, 0xFF5700u}, /*  2 AMBER      */                            \
     {0xFFE000u, 0xFFBE00u}, /*  3 YELLOW     */                            \
     {0xA8FF00u, 0x64FF00u}, /*  4 LIME       */                            \
     {0x00FF00u, 0x00FF00u}, /*  5 GREEN      */                            \
     {0x00FF8Au, 0x00FF41u}, /*  6 MINT       */                            \
     {0x00F0FFu, 0x00DEFFu}, /*  7 CYAN       */                            \
     {0x00A8FFu, 0x0064FFu}, /*  8 SKY        */                            \
     {0x0038FFu, 0x000AFFu}, /*  9 BLUE       */                            \
     {0x5000FFu, 0x1400FFu}, /* 10 INDIGO     */                            \
     {0xA000FFu, 0x5A00FFu}, /* 11 VIOLET     */                            \
     {0xFF00D0u, 0xFF00A1u}, /* 12 MAGENTA    */                            \
     {0xFF4080u, 0xFF0D37u}, /* 13 PINK       */                            \
     {0xFFFFFFu, 0xFFFFFFu}, /* 14 WHITE      */                            \
     {0xFFC000u, 0xFF8600u}, /* 15 GOLD       */                            \
     {0x60FF00u, 0x1EFF00u}, /* 16 CHARTREUSE */                            \
     {0x00FF40u, 0x00FF0Du}, /* 17 SPRING     */                            \
     {0x00FFC8u, 0x00FF93u}, /* 18 TEAL       */                            \
     {0x0080FFu, 0x0037FFu}, /* 19 AZURE      */                            \
     {0x2020FFu, 0x0404FFu}, /* 20 ROYAL      */                            \
     {0x7800FFu, 0x3000FFu}, /* 21 PURPLE     */                            \
     {0xD040FFu, 0xA10DFFu}, /* 22 ORCHID     */                            \
     {0xFF00A0u, 0xFF005Au}, /* 23 FUCHSIA    */                            \
     {0xFF2060u, 0xFF041Eu}, /* 24 ROSE       */                            \
     {0xFFB080u, 0xFF6F37u}, /* 25 PEACH      */                            \
     {0xCBFFCCu, 0x98FF9Au}, /* 26 PALE MINT  (EP147's own HOT CUE tone) */ \
     {0xB0A0FFu, 0x6F5AFFu}} /* 27 LAVENDER   */
/* Picker grid (AZ_COLOR_PICK): 6 wide. Cell k -> palette index: four rows of the saturated
 * wheel in hue order, then the pales and white, then one trailing native cell (leave EP147's
 * own color alone). */
#define AZMOD_PAD_GRID_COLS 6
#define AZMOD_PAD_GRID_ORDER                        \
    {0, 1, 2, 15, 3, 4, 16, 5, 17, 6, 18, 7, 8, 19, \
     9, 20, 10, 21, 11, 22, 12, 23, 13, 24, 14, 25, 26, 27}
/* Swatch-grid cells = the palette plus one trailing native cell. */
#define AZMOD_PAD_SWATCH_CELLS (AZMOD_PAD_PALETTE_COUNT + 1)
/* Labels for the 4 pad-mode buttons in j order (ERP mode mask byte13 0x01/0x02/0x04/0x08),
 * matching the panel silkscreen. */
#define AZMOD_PADMODE_LABELS {"Hot Cue", "Beat Loop", "Slip Loop", "Beat Jump"}
#endif
