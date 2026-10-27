/* SPDX-License-Identifier: GPL-3.0-only */
/* screen_sysinfo.c -- System Info (AZ_INFO): the info lines and the Save Logs to USB band.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there.
 * Pure drawing and hit-testing: azmod.c fills info_line[] and savelogs_line, and sends the
 * request a press asks for (savelogs_want). */
/* THE SAVE LOGS BAND, under the info lines: the button (the Updates screen's pill), then two
 * lines under it across the pane's width, what the button does and the last result, so a
 * result naming its folder is not cut. One geometry function for the draw and the touch test. */
typedef struct {
    int lx, top, lh, tw, bx, by, bw, bh;
} info_geo;
static void info_band(const azmod_fb *fb, info_geo *g) {
    int px = menu_text_px(fb), rowh = menu_rowh(fb), pad = fb->w / 40;
    int cw = g_docked ? dock_w(fb) : menu_content_w(fb), scale = fb->h / 220;
    if (scale < 1)
        scale = 1;
    g->lx = menu_bx(fb) + sub_lx(fb);
    g->top = menu_by(fb) + menu_hdrh(fb) + (g_full ? fb->h * 12 / 800 : 0) +
             rowh * AZMOD_INFO_LINES + rowh / 2;
    g->lh = px * 17 / 10;
    g->bh = px * 2;
    int widest = 0;
    static const char *const L[] = {"Save Logs to USB", "Please wait"};
    for (int i = 0; i < 2; i++) {
        int w = azmod_ttf_menu_width ? azmod_ttf_menu_width(L[i], (int) strlen(L[i]), px, 2) : 0;
        if (w <= 0)
            w = (int) strlen(L[i]) * 6 * scale; /* bitmap fallback */
        if (w > widest)
            widest = w;
    }
    g->bw = widest + g->bh * 2;
    if (g->bw > cw / 2)
        g->bw = cw / 2;
    g->bx = g->lx;
    g->by = g->top;
    g->tw = menu_bx(fb) + cw - pad - g->lx;
    if (g->tw < 0)
        g->tw = 0;
}
/* System Info's input: the knob press, or a tap on the button, asks for a save. Nothing to
 * press while one runs. */
static bool input_info(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched,
                       int enc_delta, bool enc_click) {
    (void) enc_delta;
    if (s->savelogs_busy)
        return true;
    bool hit = enc_click;
    if (touched) {
        info_geo g;
        info_band(fb, &g);
        hit = in_rect(touch_x, touch_y, g.bx - 8, g.by - 8, g.bw + 16, g.bh + 16);
    }
    if (hit) {
        s->savelogs_want = 1;
        s->savelogs_busy = 1;
        s->savelogs_tone = 0;
        snprintf(s->savelogs_line, sizeof s->savelogs_line, "Saving logs...");
    }
    return true;
}
static void compose_info(azmod_state *s, const azmod_fb *fb, int rowh, int scale) {
    draw_menu_header(fb, rowh, scale, "System Info");
    for (int i = 0; i < AZMOD_INFO_LINES; i++)
        draw_text(fb, menu_bx(fb) + sub_lx(fb),
                  menu_by(fb) + menu_hdrh(fb) + (g_full ? fb->h * 12 / 800 : 0) + rowh * i +
                      (rowh - 7 * scale) / 2,
                  s->info_line[i], scale, C_TEXT);
    info_geo g;
    info_band(fb, &g);
    int px = menu_text_px(fb);
    static const uint32_t tone_c[] = {C_TEXT_DIM, C_READOUT_OK, C_AVAIL, C_BAD};
    const char *lines[2] = {"Copies the deck's logs to MODS/logs on the USB.", s->savelogs_line};
    uint32_t cols[2] = {
        C_TEXT_DIM, tone_c[s->savelogs_tone >= 0 && s->savelogs_tone < 4 ? s->savelogs_tone : 0]};
    for (int i = 0; i < 2; i++) {
        char t[256];
        c_fit_str(t, sizeof t, lines[i], px, g.tw);
        int ly = g.by + g.bh + g.lh / 4 + g.lh * i;
        if (t[0] &&
            !(azmod_ttf_menu && azmod_ttf_menu(fb, g.lx, ly, 0, g.lh, t, px, 0, 2, cols[i])))
            draw_text(fb, g.lx, ly + (g.lh - 7 * scale) / 2, t, scale, cols[i]);
    }
    bool on = !s->savelogs_busy;
    const char *lbl = on ? "Save Logs to USB" : "Please wait";
    round_rect(fb, g.bx, g.by, g.bw, g.bh, g.bh / 2, 2, on ? C_HI : C_ROW_BG,
               on ? C_HI : C_CHECK_BOX);
    if (!(azmod_ttf_menu &&
          azmod_ttf_menu(fb, g.bx, g.by, g.bw, g.bh, lbl, px, 0, 2, on ? C_HI_TEXT : C_TEXT_DIM)))
        draw_text_centered(fb, g.bx, g.by + (g.bh - 7 * scale) / 2, g.bw, lbl, scale,
                           on ? C_HI_TEXT : C_TEXT_DIM);
}
