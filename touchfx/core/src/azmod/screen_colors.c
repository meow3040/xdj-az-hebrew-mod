/* SPDX-License-Identifier: GPL-3.0-only */
/* screen_colors.c: Pad Colors (AZ_COLORS) and its swatch grid (AZ_COLOR_PICK): rows, picks, swatch geometry, row drawing.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* AZ_COLORS rows: 0 = the master toggle, 1-4 = the four buttons' primary modes,
 * a separator line, then 5-8 = their secondary modes. Pick
 * index sel = 2*j + page (j = button: Hot Cue, Beat Loop, Slip Loop, Beat Jump);
 * the secondary modes carry EP147's own names: HOT CUE's is GATE CUE and SLIP
 * LOOP's is KEY SHIFT. */
#define COLORS_ROW_MASTER 0
#define COLORS_ROWS (1 + AZMOD_PADPICK_COUNT)
#define COLORS_ROW_SECOND (1 + AZMOD_PADMODE_COUNT) /* first row below the separator */
/* clang-format off */
static const char *const PICK_LABEL[AZMOD_PADPICK_COUNT] = {
    "Hot Cue",   "Gate Cue",  "Beat Loop", "Beat Loop 2",
    "Slip Loop", "Key Shift", "Beat Jump", "Beat Jump 2",
};
/* clang-format on */
static int colors_row_to_sel(int row) {
    return row >= COLORS_ROW_SECOND ? 2 * (row - COLORS_ROW_SECOND) + 1 : 2 * (row - 1);
}
static int colors_sel_to_row(int sel) {
    return (sel & 1) ? COLORS_ROW_SECOND + (sel >> 1) : 1 + (sel >> 1);
}
/* The stored palette index behind pick sel (2*j + page). */
static int8_t *pick_slot(azmod_state *s, int sel) {
    if (sel < 0 || sel >= AZMOD_PADPICK_COUNT)
        sel = 0;
    return (sel & 1) ? &s->padmode_color2[sel >> 1] : &s->padmode_color[sel >> 1];
}
/* Plain stacked rows under the header. Not azmod_item_rect: that places a
 * main-menu item by its display order (menu_order, group gaps, hidden items), so a
 * sub-screen row index would land in the main menu's slots. */
static void colors_row_rect(const azmod_fb *fb, int i, int *x, int *y, int *w, int *h) {
    int rowh = g_full ? fb->h * 44 / 800 : menu_rowh(fb);
    *x = menu_bx(fb);
    *w = g_docked ? dock_w(fb) : menu_content_w(fb);
    *h = rowh;
    *y = menu_by(fb) + menu_hdrh(fb) + (g_full ? fb->h * 10 / 800 : 0) + rowh * i;
}
/* Pad Colors rows: colors_row_rect, with half a row of room for a separator
 * line below "Enable" and another above the secondary modes. Drawing and
 * hit-testing use this, so taps land on the rows as drawn. */
static void pc_row_rect(const azmod_fb *fb, int i, int *x, int *y, int *w, int *h) {
    colors_row_rect(fb, i, x, y, w, h);
    if (i >= 1)
        *y += *h / 2;
    if (i >= COLORS_ROW_SECOND)
        *y += *h / 2;
}
/* Grid cell k -> palette index (AZMOD_PAD_GRID_ORDER); the trailing cell is
 * NATIVE (-1). And the inverse, for putting the knob on the current pick. */
static int8_t colors_index_of_cell(int k) {
    static const int8_t ORDER[AZMOD_PAD_PALETTE_COUNT] = AZMOD_PAD_GRID_ORDER;
    return (k >= 0 && k < AZMOD_PAD_PALETTE_COUNT) ? ORDER[k] : (int8_t) -1;
}
static int colors_cell_of(int pi) {
    static const int8_t ORDER[AZMOD_PAD_PALETTE_COUNT] = AZMOD_PAD_GRID_ORDER;
    for (int k = 0; k < AZMOD_PAD_PALETTE_COUNT; k++)
        if (ORDER[k] == pi)
            return k;
    return AZMOD_PAD_PALETTE_COUNT; /* native = the last cell */
}
/* AZ_COLOR_PICK: swatch cell k in an AZMOD_PAD_GRID_COLS-wide grid that fills
 * the pane under the header. Cells are square, sized to fit both ways, and the
 * grid is centered. */
static void colors_swatch_rect(const azmod_fb *fb, int k, int *x, int *y, int *w, int *h) {
    int cols = AZMOD_PAD_GRID_COLS;
    int rows = (AZMOD_PAD_SWATCH_CELLS + cols - 1) / cols;
    int cw = g_docked ? dock_w(fb) : menu_content_w(fb);
    int ch = (g_docked ? dock_h(fb) : fb->h) - menu_hdrh(fb);
    int pad = cw / 24;
    if (pad < 4)
        pad = 4; /* outer margin */
    int gap = pad / 2;
    if (gap < 2)
        gap = 2; /* between cells */
    int cellw = (cw - 2 * pad - (cols - 1) * gap) / cols;
    int cellh = (ch - 2 * pad - (rows - 1) * gap) / rows;
    int cell = cellw < cellh ? cellw : cellh;
    if (cell < 8)
        cell = 8;
    int gridw = cols * cell + (cols - 1) * gap;
    int gx = menu_bx(fb) + (cw - gridw) / 2;
    int gy = menu_by(fb) + menu_hdrh(fb) + pad;
    int r = k / cols, c = k % cols;
    *x = gx + c * (cell + gap);
    *y = gy + r * (cell + gap);
    *w = cell;
    *h = cell;
}

/* A sub-screen row at a plain stacked position (colors_row_rect), with the
 * same highlight/flash rules as draw_row. draw_row itself is main-menu only:
 * it places rows through azmod_item_rect. */
static void draw_colors_row(azmod_state *s, const azmod_fb *fb, int i, int rowh, const char *label,
                            int scale, uint32_t col) {
    int x, y, w, h;
    colors_row_rect(fb, i, &x, &y, &w, &h);
    bool hi = s->menu_sel == i || (s->pressed_item == i && s->pressed_ticks > 0);
    if (hi && g_full) {
        fill_rect(fb, x + fb->w / 80, y, w - fb->w / 40, h, 0x00484848u);
        draw_rect_outline(fb, x + fb->w / 80, y, w - fb->w / 40, h, 2, C_HI);
        hi = false;
    }
    if (hi)
        fill_rect(fb, x, y, w, h, C_HI);
    (void) rowh;
    draw_text(fb, x + sub_lx(fb), y + (h - 7 * scale) / 2, label, scale, hi ? C_HI_TEXT : col);
}
static void draw_pc_row(azmod_state *s, const azmod_fb *fb, int i, int rowh, const char *label,
                        int scale, uint32_t col) {
    int x, y, w, h;
    pc_row_rect(fb, i, &x, &y, &w, &h);
    bool hi = s->menu_sel == i || (s->pressed_item == i && s->pressed_ticks > 0);
    if (hi && g_full) {
        fill_rect(fb, x + fb->w / 80, y, w - fb->w / 40, h, 0x00484848u);
        draw_rect_outline(fb, x + fb->w / 80, y, w - fb->w / 40, h, 2, C_HI);
        hi = false;
    }
    if (hi)
        fill_rect(fb, x, y, w, h, C_HI);
    (void) rowh;
    draw_text(fb, x + sub_lx(fb), y + (h - 7 * scale) / 2, label, scale, hi ? C_HI_TEXT : col);
}
