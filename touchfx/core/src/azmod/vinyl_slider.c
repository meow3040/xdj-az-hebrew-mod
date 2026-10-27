/* SPDX-License-Identifier: GPL-3.0-only */
/* ==== VINYL SPEED ADJUST TIME as a slider on SHORTCUT ====
 * Included from azmod.c after menu_popup.c (FreeType face, mp_image_uncached).
 *
 * gui::shortcut::VinylSpeedAdjustTimeWidget (vt 0x2aa6448) is a label, five
 * gui::ToggleImagesButton steps (vt 0x29674c8, at local y 22, 103x58, 2 px apart,
 * x 2..525) and the knob icon. The steps only send the player v = n/5, and the player
 * maps any v in 0..1 through a 256-entry table, so a continuous value works. The steps
 * are hidden and the widget's paint draws one image in their place:
 * [ - ] [ bar: blue fill, ticks at the native steps, "NN %" ] [ + ], in the native
 * palette (button gray 60/60/60, blue 20/115/235, 2 px black gaps) and the button
 * labels' face (menulabels.h MP_FT_*). Touch uses this class's mouseDown/Drag/Up slots
 * (5/6/7, EP147's shared no-op, patched only in this class's vtable): - / + = 1 %, tap
 * or drag the bar.
 *
 * The value is g_vb_milli (0..1000, -1 = never set: EP147's own step stands). The setter
 * hook substitutes it whenever EP147 sends v > 0, so the button and the TOUCH/RELEASE mode
 * stay stock. A change is also applied at once to every adjustor kind EP147 has active
 * (index > 0) by calling the setter, a leaf that stores rate + index and is safe from the
 * message thread. Saved (azmod.conf vinyl.brake) when the touch is released. Until first used the
 * slider shows EP147's own step: the selected ToggleImagesButton has [btn+0x1e0] == 1. */
#include "vsbar.h"
#define VS_WIDGET_VT EP_2aa6448
#define VS_PAINT_SLOT (VS_WIDGET_VT + 26 * 8)
#define VS_PAINT_ORIG EP_1afde70
/* juce::MouseListener comes first in Component's vtable: mouseMove..mouseDoubleClick
 * = slots 2..8 (EP147's shared empty fn), mouseWheelMove/Magnify 9/10, then setName 11,
 * setVisible 12. So down/drag/up = 5/6/7. (31/33 are a Button state callback with
 * args (.., state 2 = down, 1 = over), not mouse events.) */
#define VS_MDOWN_SLOT (VS_WIDGET_VT + 5 * 8)
#define VS_MDRAG_SLOT (VS_WIDGET_VT + 6 * 8)
#define VS_MUP_SLOT (VS_WIDGET_VT + 7 * 8)
#define VS_MOUSE_NOOP EP_242d0a0
#define VS_TOGGLE_VT EP_29674c8
#define VS_ADJ_VT EP_2896b90
#define VS_SETVIS_SLOT 0x60 /* juce::Component::setVisible */
#define VS_X 2              /* the steps' area in the widget */
#define VS_Y 22
#define VS_CELL 103 /* - and + cells: one native step wide */
#define VS_GAP 2
#define VS_BAR_X (VS_CELL + VS_GAP)
#define VS_BAR_W (VS_W - 2 * (VS_CELL + VS_GAP))
#define VS_BASE 35 /* digit baseline row: native "3" ink 22..35 */

static void (*g_orig_vs_paint)(void *, void *);
static uintptr_t g_vs_img;                     /* our juce::Image (ImagePixelData*) */
static int g_vs_drawn = -2, g_vs_drawn_p = -1; /* what the image holds */
static int g_vs_press;                         /* 1 = -, 2 = +, 3 = bar (message thread) */
static uintptr_t g_vs_widget; /* the widget that painted last (identity checked at use) */

/* Every reader below runs inside one of this class's own vtable calls (paint, mouse) on the
 * message thread, so `w` is EP147's live `this` and its children are live for the call:
 * plain loads (own_q), identity-checked as before. */
static int vs_is_widget(uintptr_t w) {
    uintptr_t vt = 0;
    return w && !(w & 7) && own_q(w, &vt) && vt == VS_WIDGET_VT;
}
/* the five step buttons, in child order (= left to right); 0 if not exactly five */
static int vs_buttons(uintptr_t w, uintptr_t b[5]) {
    uintptr_t arr = 0, cnt = 0;
    int k = 0;
    /* juce::Array<Component*> childComponentList: elements +0x40, numAllocated +0x48,
     * numUsed +0x50 */
    if (!own_q(w + 0x40, &arr) || !arr || !own_q(w + 0x50, &cnt))
        return 0;
    int used = (int) (uint32_t) cnt;
    if (used < 5 || used > 16)
        return 0;
    for (int i = 0; i < used && k < 5; i++) {
        uintptr_t c = 0, vt = 0;
        if (!own_q(arr + 8 * (uintptr_t) i, &c) || !c || (c & 7) || !own_q(c, &vt))
            return 0;
        if (vt == VS_TOGGLE_VT)
            b[k++] = c;
    }
    return k == 5;
}
/* EP147's own step as milli (step n -> n*200), or -1 */
static int vs_native_milli(uintptr_t w) {
    uintptr_t b[5], v = 0;
    if (!vs_buttons(w, b))
        return -1;
    for (int i = 0; i < 5; i++)
        if (own_q(b[i] + 0x1e0, &v) && (v & 0xff) == 1)
            return (i + 1) * 200;
    return -1;
}
static int vs_shown_milli(uintptr_t w) {
    int m = __atomic_load_n(&g_vb_milli, __ATOMIC_RELAXED);
    return m >= 0 ? m : vs_native_milli(w);
}
/* apply to every adjustor kind EP147 has active right now. The reader count keeps each
 * published adjustor alive while it is used (vinyl_brake.c vb_forget waits for it). */
static void vs_apply(int m) {
    if (m < 0 || !g_orig_vb_set || !g_vb_dtor_ok || !on_msg_thread())
        return; /* readers are message-thread only (vb_forget relies on it) */
    float v = (float) m / 1000.f;
    __atomic_fetch_add(&g_vb_readers, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < VB_NADJ; i++) {
        uintptr_t a = (uintptr_t) __atomic_load_n(&g_vb_adj[i], __ATOMIC_SEQ_CST), vt = 0, idx = 0;
        if (!a || (a & 7) || !own_q(a, &vt) || vt != VS_ADJ_VT || !own_q(a + 0x20, &idx))
            continue;
        if ((int) (uint32_t) idx > 0)
            g_orig_vb_set((long) a, v, 1); /* TOUCH active */
        if ((int) (uint32_t) (idx >> 32) > 0)
            g_orig_vb_set((long) a, v, 2); /* RELEASE active */
    }
    __atomic_fetch_sub(&g_vb_readers, 1, __ATOMIC_RELEASE);
}
static void vs_save(int m) { /* azmod.conf's vinyl.brake, captured from g_vb_milli */
    if (m < 0)
        return;
    save_config(&g_state);
}
/* The save is durable (two fsyncs on p7), so EP147's message thread, which runs the
 * touch handlers, only posts the value; the overlay thread writes it on its next
 * tick. One slot, newest wins: an older value still waiting is superseded. */
static int g_vs_save_req = -1; /* accessed only through __atomic builtins */
static void vs_save_post(int m) {
    if (m >= 0)
        __atomic_store_n(&g_vs_save_req, m, __ATOMIC_RELEASE);
}
static void vs_save_flush(void) { /* overlay thread */
    vs_save(__atomic_exchange_n(&g_vs_save_req, -1, __ATOMIC_ACQ_REL));
}

/* ---- rendering: one opaque 523x58 image, pixels written once per change ---- */
static void vs_fill(uint8_t *px, int ls, int x0, int y0, int w, int h, int r, int g, int b) {
    for (int y = y0; y < y0 + h && y < VS_H; y++)
        for (int x = x0; x < x0 + w && x < VS_W; x++) {
            if (x < 0 || y < 0)
                continue;
            uint8_t *q = px + y * ls + x * 4;
            q[0] = (uint8_t) b;
            q[1] = (uint8_t) g;
            q[2] = (uint8_t) r;
            q[3] = 255;
        }
}
/* white text centered on cx; coverage is blended once over what is there, never
 * applied twice (double coverage reads as pixelated on the panel) */
static void vs_text(uint8_t *px, int ls, const char *s, int cx) {
    uint32_t cp[16];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *) s; *p && n < 15;) {
        uint32_t c = *p++;
        if (c >= 0xe0) {
            c = ((c & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0xc0) {
            c = ((c & 0x1f) << 6) | (p[0] & 0x3f);
            p += 1;
        }
        cp[n++] = c;
    }
    struct mp_gl gl[16];
    int lo = 0, hi = 0, k = mp_lay(cp, n, gl, 0);
    if (!mp_ink(gl, k, &lo, &hi))
        return;
    int X = (int) floor(cx - (lo + hi) / 2.0 + 0.5);
    for (int i = 0; i < k; i++) {
        FT_GlyphSlot gs = mp_glyph(gl[i].cp);
        if (!gs)
            continue;
        for (unsigned r = 0; r < gs->bitmap.rows; r++) {
            int y = VS_BASE + 1 - gs->bitmap_top + (int) r;
            if (y < 0 || y >= VS_H)
                continue;
            for (unsigned c = 0; c < gs->bitmap.width; c++) {
                int x = X + gl[i].x + gs->bitmap_left + (int) c;
                unsigned a = gs->bitmap.buffer[r * (unsigned) gs->bitmap.pitch + c];
                if (x < 0 || x >= VS_W || !a)
                    continue;
                uint8_t *q = px + y * ls + x * 4;
                for (int ch = 0; ch < 3; ch++)
                    q[ch] = (uint8_t) ((q[ch] * (255 - a) + 255 * a + 127) / 255);
            }
        }
    }
}
static int vs_render(int m, int press) {
    if (!mp_ft_ready())
        return 0;
    if (!g_vs_img)
        g_vs_img = mp_image_uncached(VS_BG, sizeof VS_BG);
    uintptr_t img = g_vs_img, fm = 0, wh = 0, pxa = 0, st = 0;
    /* our own image: azmod holds its only reference and never releases it */
    if (!img || !own_q(img + 0x08, &fm) || !own_q(img + 0x10, &wh) || !own_q(img + 0x48, &pxa) ||
        !own_q(img + 0x50, &st) || (int) (uint32_t) wh != VS_W ||
        (int) (uint32_t) (wh >> 32) != VS_H || (uint32_t) (fm >> 32) != 2 || (uint32_t) st != 4 ||
        (int) (st >> 32) < VS_W * 4 || !pxa)
        return 0;
    uint8_t *px = (uint8_t *) pxa;
    int ls = (int) (st >> 32);
    vs_fill(px, ls, 0, 0, VS_W, VS_H, 0, 0, 0); /* the gaps */
    int mb = press == 1 ? 1 : 0, pb = press == 2 ? 1 : 0;
    vs_fill(px, ls, 0, 0, VS_CELL, VS_H, mb ? 20 : 60, mb ? 115 : 60, mb ? 235 : 60); /* - */
    vs_fill(px, ls, VS_W - VS_CELL, 0, VS_CELL, VS_H, pb ? 20 : 60, pb ? 115 : 60,
            pb ? 235 : 60);                                   /* + */
    vs_fill(px, ls, VS_BAR_X, 0, VS_BAR_W, VS_H, 60, 60, 60); /* bar */
    int fw = m < 0 ? 0 : (m * VS_BAR_W + 500) / 1000;
    vs_fill(px, ls, VS_BAR_X, 0, fw, VS_H, 20, 115, 235); /* fill */
    for (int s = 1; s <= 4; s++)                          /* the native steps */
        vs_fill(px, ls, VS_BAR_X + (s * 200 * VS_BAR_W + 500) / 1000 - 1, VS_H - 8, 2, 8, 0, 0, 0);
    /* - and + are drawn as 16x2 px bars on the digits' center line (rows 22..35): the
     * face's "+" glyph is much smaller than its minus, so glyphs would not match */
    vs_fill(px, ls, VS_CELL / 2 - 8, 28, 16, 2, 255, 255, 255);
    vs_fill(px, ls, VS_W - VS_CELL / 2 - 8, 28, 16, 2, 255, 255, 255);
    vs_fill(px, ls, VS_W - VS_CELL / 2 - 1, 21, 2, 16, 255, 255, 255);
    char t[16];
    if (m >= 0)
        snprintf(t, sizeof t, "%d %%", (m + 5) / 10);
    else
        snprintf(t, sizeof t, "-");
    vs_text(px, ls, t, VS_BAR_X + VS_BAR_W / 2);
    g_vs_drawn = m;
    g_vs_drawn_p = press;
    return 1;
}

/* Mod Menu > Extras > Fine Vinyl Speed Adjust, read live. Off = stock: the five
 * steps shown again, our mouse handlers inert, EP147's values passed through. */
static int vs_on(void) {
    return az_on(AZF_FINE_VINYL_SPEED);
}
static void vs_show_steps(uintptr_t b[5], int vis) {
    for (int i = 0; i < 5; i++) { /* a no-op once in that state */
        uintptr_t vt = 0, fn = 0;
        if (own_q(b[i], &vt) && img_q(vt + VS_SETVIS_SLOT, &fn) && fn)
            ((void (*)(uintptr_t, int)) fn)(b[i], vis);
    }
}
static void vs_paint_hook(void *self, void *g) {
    g_orig_vs_paint(self, g);
    uintptr_t w = (uintptr_t) self, b[5];
    int iw = vs_is_widget(w), ib = iw && vs_buttons(w, b);
    if (!iw || !ib)
        return; /* not the expected shape: stock */
    g_vs_widget = w;
    if (!vs_on()) {
        vs_show_steps(b, 1);
        return;
    }                    /* switched off: the stock steps */
    vs_show_steps(b, 0); /* hide the steps */
    int m = vs_shown_milli(w);
    if ((m != g_vs_drawn || g_vs_press != g_vs_drawn_p) && !vs_render(m, g_vs_press))
        return;
    uintptr_t holder = g_vs_img;
    ((void (*)(void *, uintptr_t *, int, int, int, int, int, int, int, int, int)) MP_DRAW_IMAGE)(
        g, &holder, VS_X, VS_Y, VS_W, VS_H, 0, 0, VS_W, VS_H, 0);
}

/* ---- touch: juce::MouseEvent int x at +0x08, y at +0x0c (widget coordinates) ---- */
static void vs_set(uintptr_t w, int m) {
    if (m < 0)
        m = 0;
    if (m > 1000)
        m = 1000;
    __atomic_store_n(&g_vb_milli, m, __ATOMIC_RELAXED);
    vs_apply(m);
    ((void (*)(long)) PAD_REPAINT)((long) w);
}
static int vs_bar_milli(int x) {
    int m = (int) (((long) (x - VS_X - VS_BAR_X) * 1000 + VS_BAR_W / 2) / VS_BAR_W);
    m = (m + 5) / 10 * 10; /* 1 % steps */
    return m < 0 ? 0 : m > 1000 ? 1000
                                : m;
}
static void vs_mouse(void *self, const void *ev, int kind) { /* 0 down, 1 drag, 2 up */
    uintptr_t w = (uintptr_t) self, xy = 0;
    if (!vs_on()) {
        g_vs_press = 0;
        return;
    } /* stock: the steps take the touch */
    if (!vs_is_widget(w) || !own_q((uintptr_t) ev + 0x08, &xy)) /* the event: our argument */
        return;
    int x = (int) (uint32_t) xy, y = (int) (uint32_t) (xy >> 32);
    int lx = x - VS_X;
    if (kind == 0) {
        if (y < VS_Y || y >= VS_Y + VS_H || lx < 0 || lx >= VS_W)
            return;
        int base = vs_shown_milli(w);
        if (base < 0)
            base = 600;
        if (lx < VS_CELL) {
            g_vs_press = 1;
            vs_set(w, base - 10);
        } else if (lx >= VS_W - VS_CELL) {
            g_vs_press = 2;
            vs_set(w, base + 10);
        } else if (lx >= VS_BAR_X && lx < VS_BAR_X + VS_BAR_W) {
            g_vs_press = 3;
            vs_set(w, vs_bar_milli(x));
        }
        return;
    }
    if (kind == 1) {
        if (g_vs_press == 3)
            vs_set(w, vs_bar_milli(x));
        return;
    }
    if (g_vs_press) {
        g_vs_press = 0;
        vs_save_post(__atomic_load_n(&g_vb_milli, __ATOMIC_RELAXED));
        ((void (*)(long)) PAD_REPAINT)((long) w);
    }
}
static void vs_mdown(void *s, const void *e) {
    vs_mouse(s, e, 0);
}
static void vs_mdrag(void *s, const void *e) {
    vs_mouse(s, e, 1);
}
static void vs_mup(void *s, const void *e) {
    vs_mouse(s, e, 2);
}

/* Patch only when the slots still hold the exact functions expected, and only with the
 * setter hook in (otherwise the slider would move a value nothing uses). */
static void install_vinyl_slider(void) {
    uintptr_t p = 0, d0 = 0, d1 = 0, d2 = 0;
    void *o = 0;
    if (!g_vb_hooked)
        return;
    if (!img_q(VS_PAINT_SLOT, &p) || p != VS_PAINT_ORIG || !img_q(VS_MDOWN_SLOT, &d0) ||
        !img_q(VS_MDRAG_SLOT, &d1) || !img_q(VS_MUP_SLOT, &d2) || d0 != VS_MOUSE_NOOP ||
        d1 != VS_MOUSE_NOOP || d2 != VS_MOUSE_NOOP)
        return;
    if (azm_patch_vtable(VS_PAINT_SLOT, (void *) vs_paint_hook, (void **) &g_orig_vs_paint) != 1)
        return;
    azm_patch_vtable(VS_MDOWN_SLOT, (void *) vs_mdown, &o);
    azm_patch_vtable(VS_MDRAG_SLOT, (void *) vs_mdrag, &o);
    azm_patch_vtable(VS_MUP_SLOT, (void *) vs_mup, &o);
}
