/* SPDX-License-Identifier: GPL-3.0-only */
/* menu_layout.c: the Mod Menu's geometry (compact strip, docked pane) and chrome (header, backdrop), and the rects of the closed-state bars.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* Compact menu: menu screens occupy the left part of the display. Solid content
 * spans MENU_CONTENT_FRAC of the width, then a strip (MENU_FADE_FRAC) fades into
 * EP147's dark UI. The overlay window is sized to their sum (azmod.c wanted_rect)
 * so EP147 shows through on the right. */
#define MENU_CONTENT_FRAC 0.50f /* solid content (contains the MOD MENU header) */
#define MENU_FADE_FRAC 0.08f    /* gradient strip past the content */
#define MENU_WIN_FRAC (MENU_CONTENT_FRAC + MENU_FADE_FRAC)

/* Docked (right-pane) geometry: EP147's SourceSelectLayoutView puts the
 * per-source detail pane at (640,140,640,563) on 1280x800. */
#define DOCK_X 0.500f
/* The docked panel covers the whole right pane, header bar included: y=70 is the
 * top of the pane's content area (starting at y=140 would leave the USB panel's
 * header bar showing above the menu), and the height then matches the SOURCE
 * list's 633px extent (tbl_h=633). */
#define DOCK_Y 0.0875f
#define DOCK_W 0.500f
#define DOCK_H \
    0.78125f /* y 70..694, as EP147's own right pane: an 8 px gap above the deck strips */
static bool g_docked;
void azmod_set_docked(bool on) {
    g_docked = on;
}

bool azmod_docked(void) {
    return g_docked;
}

static int dock_x(const azmod_fb *fb) {
    return (int) (DOCK_X * fb->w);
}
static int dock_y(const azmod_fb *fb) {
    return (int) (DOCK_Y * fb->h);
}
static int dock_w(const azmod_fb *fb) {
    return (int) (DOCK_W * fb->w);
}
static int dock_h(const azmod_fb *fb) {
    return (int) (DOCK_H * fb->h);
}
/* Menu origin: (0,0) for the left strip, the pane's top-left when docked. */
static int menu_bx(const azmod_fb *fb) {
    return g_docked ? dock_x(fb) : 0;
}
static int menu_by(const azmod_fb *fb) {
    return g_docked ? dock_y(fb) : 0;
}
static int menu_content_w(const azmod_fb *fb) {
    return (int) (fb->w * MENU_CONTENT_FRAC);
}
static int menu_win_w(const azmod_fb *fb) {
    return (int) (fb->w * MENU_WIN_FRAC);
}

/* The persistent "MOD MENU" header. Drawn by every menu screen (main and every
 * sub-screen) so the title never disappears when you go a level deeper, on a
 * darker band than the scrim so it reads as chrome rather than another row.
 * `sub` names the sub-screen (NULL on the main menu) and sits right-aligned. */
static void draw_text(const azmod_fb *fb, int x, int y, const char *s, int scale, uint32_t c);
static void draw_close_x(const azmod_fb *fb, int cx, int cy, int hs, uint32_t c);
static int menu_content_w(const azmod_fb *fb);
static int menu_bx(const azmod_fb *fb);
static int menu_by(const azmod_fb *fb);
static int dock_w(const azmod_fb *fb);

/* Header band height = EP147's own right-pane header on the USB1 source screen:
 * 70px of 800 (band y 70-139). Tied to the screen height, not rowh, so adding menu
 * items cannot change it. */
static int menu_hdrh(const azmod_fb *fb) {
    return fb->h * 70 / 800;
}
static int menu_rowh(const azmod_fb *fb);
static void draw_rect_outline(const azmod_fb *fb, int x, int y, int w, int h, int t, uint32_t c);
/* Sub-screen BACK button: a square in the header's icon column. One rect for the
 * header draw and azmod_input's hit-test, so the two can never drift. */
static void back_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    int hh = menu_hdrh(fb), side = hh * 5 / 8;
    int cx = menu_bx(fb) + fb->w / 40 + (menu_rowh(fb) * 3 / 4) / 2; /* center of the icon column */
    *w = *h = side;
    *x = cx - side / 2;
    *y = menu_by(fb) + (hh - side) / 2;
}
static bool g_full;
static int g_full_mode;
/* Sub-screen rows in the full-screen menu: the detail pane's margin and a taller row. */
static int sub_lx(const azmod_fb *fb) {
    return g_full ? fb->w / 40 : fb->w / 12;
}
static void c_text(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px,
                   uint32_t c);
static void c_select(azmod_state *s, int it);
static void draw_menu_header(const azmod_fb *fb, int rowh, int scale, const char *sub) {
    int hx = menu_bx(fb), hy = menu_by(fb);
    int hw = g_docked ? dock_w(fb) : menu_content_w(fb);
    int hh = menu_hdrh(fb);
    if (g_full) { /* full-screen menu: the right pane's header names the screen */
        fill_rect(fb, hx, hy, hw, hh, C_HEADER_BG);
        int tx = hx + fb->w / 40;
        if (g_full_mode == AZ_COLOR_PICK || g_full_mode == AZ_CLOCK_TZ || g_full_mode == AZ_CLOCK_SET) { /* one level deeper: BACK */
            int bx, by, bw, bh;
            back_rect(fb, &bx, &by, &bw, &bh);
            draw_rect_outline(fb, bx, by, bw, bh, 2, C_CHECK_BOX);
            int asz = bw * 3 / 5;
            blit_icon(fb, bx + (bw - asz) / 2, by + (bh - asz) / 2, asz, C_TEXT, IC_BACK);
            tx = bx + bw + fb->w / 50;
        }
        if (sub)
            c_text(fb, tx, hy, 0, hh, sub, hh * 34 / 100, C_TEXT);
        fill_rect(fb, hx, hy + hh - 2, hw, 2, C_DIVIDER);
        (void) rowh;
        (void) scale;
        return;
    }
    fill_rect(fb, hx, hy, hw, hh, C_HEADER_BG);
    int ts = scale; /* title at row size (scale + 1 is too large) */
    draw_text(fb, hx + fb->w / 12, hy + (hh - 7 * ts) / 2, "MOD MENU", ts, C_TEXT);
    if (sub) { /* sub-screen: a tappable BACK square left of the title (same as the BACK button) */
        int bx, by, bw, bh;
        back_rect(fb, &bx, &by, &bw, &bh);
        draw_rect_outline(fb, bx, by, bw, bh, 2, C_CHECK_BOX);
        int asz = bw * 3 / 5;
        blit_icon(fb, bx + (bw - asz) / 2, by + (bh - asz) / 2, asz, C_TEXT, IC_BACK);
    }
    if (sub) {
        int tw = (int) strlen(sub) * 6 * scale;
        draw_text(fb, hx + hw - tw - fb->w / 24, hy + (hh - 7 * scale) / 2, sub, scale, C_TEXT_DIM);
    }
    if (!g_docked)
        draw_close_x(fb, hx + hw - rowh / 2, hy + hh / 2, rowh / 6, C_TEXT);
    fill_rect(fb, hx, hy + hh - 2, hw, 2, C_DIVIDER);
}

static void menu_backdrop(const azmod_fb *fb) {
    /* Solid gray across the whole window; azmod.c dithers an X-Shape mask over
     * the fade strip [content_w,win_w) so the gray dissolves into EP147 (clear)
     * toward the right. (If the X server lacks SHAPE the strip stays solid gray
     * with a soft right edge, still fine against the dark UI.) */
    if (g_docked) {
        fill_rect(fb, dock_x(fb), dock_y(fb), dock_w(fb), dock_h(fb), C_MENU_SCRIM);
        return;
    }
    fill_rect(fb, 0, 0, menu_win_w(fb), fb->h, C_MENU_SCRIM);
}

/* NEW PLAYLIST's bar: EP147's title row right of the source icon (x 105/1280,
 * 70/800 tall), CANCEL and CREATE at its right end. {x, y, w, h} each. */
void azmod_naming_rects(const azmod_fb *fb, int *bar, int *cancel, int *create) {
    int bx = fb->w * 105 / 1280, bh = fb->h * 70 / 800, bw = fb->w - bx;
    int pad = fb->h * 8 / 800, btw = fb->w * 150 / 1280, bth = bh - 2 * pad;
    bar[0] = bx;
    bar[1] = 0;
    bar[2] = bw;
    bar[3] = bh;
    create[0] = fb->w - btw - pad;
    create[1] = pad;
    create[2] = btw;
    create[3] = bth;
    cancel[0] = create[0] - btw - pad;
    cancel[1] = pad;
    cancel[2] = btw;
    cancel[3] = bth;
}

/* Overlay-window rect for an open menu screen: the compact left strip (content
 * + gradient), full height. azmod.c's wanted_rect() sizes the window to this so
 * EP147 shows through the uncovered right side. */
void azmod_menu_win_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    if (g_docked) {
        *x = dock_x(fb);
        *y = dock_y(fb);
        *w = dock_w(fb);
        *h = dock_h(fb);
        return;
    }
    *x = 0;
    *y = 0;
    *w = menu_win_w(fb);
    *h = fb->h;
}
/* Bounds of the fade strip (solid content ends at *content_w, window at *win_w).
 * azmod.c dithers an X-Shape mask across [content_w,win_w) so the strip fades to
 * clear (EP147 shows through the holes) instead of to black. */
void azmod_menu_fade_bounds(const azmod_fb *fb, int *content_w, int *win_w) {
    if (g_docked) {
        *content_w = *win_w = dock_w(fb);
        return;
    } /* no fade strip when docked */
    *content_w = menu_content_w(fb);
    *win_w = menu_win_w(fb);
}

/* SELECT PLAYLIST: DONE at the right end of EP147's gray top strip, which our
 * held banner occupies (azmod.c pl_tb_tick). Geometry at 1280x800. */
void azmod_tb_done_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    *w = (int) (0.105f * fb->w);
    *h = (int) (0.068f * fb->h); /* strip: y 0..70 @ 800 */
    *x = fb->w - *w - (int) (0.006f * fb->w);
    *y = (int) (0.010f * fb->h);
}
/* UNDO sits left of DONE, never on it, so a tap meant for DONE right after a
 * PASTE cannot undo the paste. */
void azmod_tb_undo_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    azmod_tb_done_rect(fb, x, y, w, h);
    *x -= *w + (int) (0.010f * fb->w);
}
