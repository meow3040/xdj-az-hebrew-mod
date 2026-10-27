/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file clock_native.c
 * @brief The Clock in the place of EP147's recording time readout ("0:00:00", top right of the
 * status bar): while the Clock is on and EP147 is not recording, EP147's label is hidden and the
 * overlay draws the time of day exactly where it was (screen_overlays.c azmod_draw_clockn), in
 * EP147's own face, size and ink. The moment EP147 lights its recording icon, its label shows
 * again and nothing is drawn (clock_native.h cn_take). While EP147's caution strip (the gray banner
 * over the status bar, ours or its own) is up, nothing is drawn either: the time never sits on it.
 *
 * This file runs on EP147's message thread (the widget's constructor, its controller's update,
 * calltimers_hook). It publishes to the overlay thread, through atomics only: the label's screen
 * rect (0 = draw nothing), EP147's ink for it and the status bar's ground behind it, as EP147
 * has them: the overlay's drawing themes them (Day and the dark themes), as it themes all it draws.
 *
 * Entry points: install_clock_native, cn_tick.
 */
/* EP147 v1.30 (v2.00 in the address table; offsets checked equal there):
 * gui::status_area::RecordingStatusWidget (vtable 0x2c667f8), ctor FUN_01ca31a8(this, ...): a
 *   100x20 component; +0xf0 the juce::Label "recordingTime" (text "0:00:00", bounds 27,0,68,20);
 *   +0xf8 "recordingStatusIcon" (gui::OnOffImageComponent): its image index at +0xc8, 0 the dark
 *   dot, 1 the red dot; +0xe0 a dim flag, +0xe1 / +0xe5 the label's text colour (juce::Colour,
 *   ARGB) when dim / not dim (FUN_01ca2f78 sets it on the label).
 * RecordingStatusWidgetController update FUN_01ca4650(ctrl): [ctrl+0x18] its widget; asks the
 *   mixer use case whether it records (slot +0x28) and writes that to the icon's image index, then
 *   sets the label to the recording time (juce::Label::setText 0x24d33a0). EP147 calls it while it
 *   records; it is how the icon lights at the start.
 * juce::Component: the visible flag is bit 1 of the byte at +0xc0 (setVisible, vtable slot +0x60,
 *   tests it first on both builds); bounds and the parent pointer are found on the label itself
 *   (the bounds its constructor gave it, the widget as its parent) before anything is hidden. */
#include "clock_native.h"

#define CN_WIDGET_CTOR EP_1ca31a8
#define CN_WIDGET_VT EP_2c667f8
#define CN_UPDATE EP_1ca4650
#define CN_LABEL_VT EP_31592e0 /* juce::Label */
#define CN_LABEL 0xf0
#define CN_ICON 0xf8
#define CN_ICON_IDX 0xc8
#define CN_DIM 0xe0
#define CN_INK_DIM 0xe1
#define CN_INK 0xe5
#define CN_CTRL_WIDGET 0x18
#define CN_FLAGS 0xc0
#define CN_SETVIS 0x60
#define CN_MAX 16 /* status bars EP147 builds (13 measured on v1.30) */
#define CN_DEPTH 12
#define CN_GROUND 0xff000000u /* the status bar behind the readout (stock: black) */

static void cn_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-clock.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* published to the overlay thread through menu.h's g_azmod_clockn_* (defined in
 * screen_overlays.c, with the Mod Menu that reads them; __atomic only) */

/* ---- the readouts (message thread) --------------------------------------------------------- */
static struct {
    uintptr_t w;
    int hidden; /* its label is hidden by the clock */
} g_cn[CN_MAX];
static int g_cn_on;              /* installed */
static int g_cn_strip;           /* EP147's caution strip (a gray banner, ours or its own) covers the status bar */
static int g_cn_bounds_off = -1; /* juce::Component: boundsRelativeToParent */
static int g_cn_parent_off = -1; /* juce::Component: parentComponent */
static int cn_clock_on(void) { return azmod_stat_on(&g_state, AZF_CLOCK); }
/* the widget's label, checked to be the juce::Label EP147 made */
/* p if it is an aligned object whose vtable is vt, else 0 (checked at every use) */
static uintptr_t cn_obj(uintptr_t p, uintptr_t vt) {
    uintptr_t v = 0;
    return p && !(p & 7) && own_q(p, &v) && v == vt ? p : 0;
}
static uintptr_t cn_label(uintptr_t w) {
    uintptr_t lb = 0;
    return own_q(w + CN_LABEL, &lb) ? cn_obj(lb, CN_LABEL_VT) : 0;
}
/* the recording icon's image index, -1 when it cannot be read */
static int cn_icon(uintptr_t w) {
    uintptr_t icon = 0, v = 0;
    if (!own_q(w + CN_ICON, &icon) || !icon || (icon & 7) || !own_q(icon + CN_ICON_IDX, &v))
        return -1;
    return (int) (int32_t) v;
}
static int cn_visible(uintptr_t c) {
    uintptr_t v = 0;
    return own_q(c + CN_FLAGS, &v) && (v >> 1 & 1);
}
/* the four bytes at an unaligned offset of a live object */
static uint32_t cn_u32(uintptr_t a) {
    uintptr_t lo = 0, hi = 0, b = a & ~(uintptr_t) 7;
    if (!own_q(b, &lo) || !own_q(b + 8, &hi))
        return 0;
    unsigned s = (unsigned) (a & 7) * 8;
    return (uint32_t) (s ? (lo >> s | hi << (64 - s)) : lo);
}
/* bounds and parent offsets, learned on a label just built: its bounds are 27,0,68,20 and its
 * parent is the widget (the constructor's own setBounds and addAndMakeVisible) */
static void cn_learn(uintptr_t w, uintptr_t lb) {
    if (g_cn_bounds_off > 0 && g_cn_parent_off > 0)
        return;
    for (int o = 0x8; o < 0xc0; o += 8) {
        uintptr_t a = 0, b = 0, p = 0;
        if (g_cn_bounds_off < 0 && own_q(lb + (uintptr_t) o, &a) && own_q(lb + (uintptr_t) o + 8, &b) &&
            a == ((uintptr_t) 0 << 32 | 27) && b == ((uintptr_t) 20 << 32 | 68))
            g_cn_bounds_off = o;
        if (g_cn_parent_off < 0 && own_q(lb + (uintptr_t) o, &p) && p == w)
            g_cn_parent_off = o;
    }
    cn_log("component offsets: bounds +%x, parent +%x\n", g_cn_bounds_off, g_cn_parent_off);
}
/* the label's rect on the screen, and whether every component up to the top is visible (the
 * label itself may be hidden by us); 0 when anything on the way cannot be read */
static uint64_t cn_screen_rect(uintptr_t w, uintptr_t lb) {
    if (g_cn_bounds_off <= 0 || g_cn_parent_off <= 0)
        return 0;
    uintptr_t a = 0, b = 0;
    if (!own_q(lb + (uintptr_t) g_cn_bounds_off, &a) || !own_q(lb + (uintptr_t) g_cn_bounds_off + 8, &b))
        return 0;
    int x = (int) (int32_t) a, y = (int) (int32_t) (a >> 32), wd = (int) (int32_t) b, ht = (int) (int32_t) (b >> 32);
    uintptr_t c = w;
    for (int d = 0; d < CN_DEPTH; d++) {
        uintptr_t p = 0;
        if (!c || (c & 7) || !cn_visible(c) || !own_q(c + (uintptr_t) g_cn_bounds_off, &a))
            return 0;
        x += (int) (int32_t) a;
        y += (int) (int32_t) (a >> 32);
        if (!own_q(c + (uintptr_t) g_cn_parent_off, &p))
            return 0;
        if (!p)
            return cn_rect_pack(x, y, wd, ht); /* c is the top-level window's component */
        c = p;
    }
    return 0;
}
static void cn_set_visible(uintptr_t lb, int vis) {
    uintptr_t vt = 0, fn = 0;
    if (own_q(lb, &vt) && img_q(vt + CN_SETVIS, &fn) && fn)
        ((void (*)(uintptr_t, int)) fn)(lb, vis);
}
static void cn_apply(int i) {
    uintptr_t w = cn_obj(g_cn[i].w, CN_WIDGET_VT), lb = w ? cn_label(w) : 0;
    if (!w) {
        g_cn[i].w = 0; /* gone: its label went with it */
        return;
    }
    int take = cn_take(cn_clock_on(), cn_icon(w), lb && g_cn_bounds_off > 0 && g_cn_parent_off > 0);
    if (take != g_cn[i].hidden && lb) {
        cn_set_visible(lb, !take);
        g_cn[i].hidden = take;
    }
}
/* the rect the overlay draws in: the readout on screen whose label the clock hid */
static void cn_publish(void) {
    uint64_t r = 0;
    uint32_t ink = 0, ground = CN_GROUND;
    for (int i = 0; i < CN_MAX && !r; i++) {
        uintptr_t w = g_cn[i].hidden ? cn_obj(g_cn[i].w, CN_WIDGET_VT) : 0, lb = w ? cn_label(w) : 0;
        if (!lb || !(r = cn_screen_rect(w, lb)))
            continue;
        uintptr_t dim = 0;
        own_q(w + (CN_DIM & ~7u), &dim);
        ink = cn_u32(w + ((dim >> (8 * (CN_DIM & 7)) & 0xff) ? CN_INK_DIM : CN_INK));
    }
    if (g_cn_strip)
        r = 0; /* under a banner: the readout it covers is not drawn over it */
    __atomic_store_n(&g_azmod_clockn_ink, ink, __ATOMIC_RELAXED);
    __atomic_store_n(&g_azmod_clockn_ground, ground, __ATOMIC_RELAXED);
    __atomic_store_n(&g_azmod_clockn_rect, r, __ATOMIC_RELEASE);
}
static void (*g_o_cn_ctor)(long, long, long, long, long, long, long, long);
static void cn_ctor_hook(long w, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
    g_o_cn_ctor(w, a1, a2, a3, a4, a5, a6, a7);
    uintptr_t lb = cn_obj((uintptr_t) w, CN_WIDGET_VT) ? cn_label((uintptr_t) w) : 0;
    if (!lb)
        return;
    cn_learn((uintptr_t) w, lb);
    for (int i = 0; i < CN_MAX; i++)
        if (!cn_obj(g_cn[i].w, CN_WIDGET_VT)) {
            g_cn[i].w = (uintptr_t) w;
            g_cn[i].hidden = 0;
            return;
        }
}
static void (*g_o_cn_update)(long);
static void cn_update_hook(long ctrl) {
    g_o_cn_update(ctrl); /* EP147 lights its icon (or not) and writes its own text */
    uintptr_t w = 0;
    if (!own_q((uintptr_t) ctrl + CN_CTRL_WIDGET, &w) || !cn_obj(w, CN_WIDGET_VT))
        return;
    for (int i = 0; i < CN_MAX; i++)
        if (g_cn[i].w == w) {
            cn_apply(i);
            cn_publish();
        }
}
/* message thread (calltimers_hook): screen changes and the Clock row, 4 times a second */
static void cn_tick(void) {
    static long long last;
    long long now = drag_now_ms();
    if (!g_cn_on)
        return;
    /* a banner is checked every tick (a few loads), so the time never sits on one that just came up */
    int strip = az_banner_strip_state() > BN_NONE;
    if (strip != g_cn_strip) {
        g_cn_strip = strip;
        cn_publish();
    }
    if (now - last < 250)
        return;
    last = now;
    for (int i = 0; i < CN_MAX; i++)
        if (g_cn[i].w)
            cn_apply(i);
    cn_publish();
}
static void install_clock_native(void) {
    int a = CN_WIDGET_VT && CN_LABEL_VT && azm_install_hook(CN_WIDGET_CTOR, (void *) cn_ctor_hook, (void **) &g_o_cn_ctor) == 1;
    int b = a && azm_install_hook(CN_UPDATE, (void *) cn_update_hook, (void **) &g_o_cn_update) == 1;
    g_cn_on = a && b; /* without the update hook a recording start would not show EP147's label */
    cn_log("clock in the readout: widgets=%d update=%d -> %s\n", a, b, g_cn_on ? "on" : "off");
}
