/* SPDX-License-Identifier: GPL-3.0-only */
/* ==== touch_accum.c: slow touch drags scroll (part of the 60 FPS UI mod) =========
 * #included by azmod.c.
 *
 * EP147's drag handler for touch-scrollable components (0x21e1090, runs while the
 * mouse is down, [this+0x120] == 1):
 *     d = pos - prev;  prev = pos;               prev: doubles at this+0x100/+0x108
 *     [this+0x110/+0x118] = d;                   (0x21e1144, always, before the test)
 *     if (([this+0xf0] & 2) && (|dx| >= 2.0 || |dy| >= 2.0)) this->vt[+0x188](this, dx, dy);
 *     [this+0x125] |= |distance from drag start| > [this+0x128];      the "is a drag" flag
 * A move under 2 px is dropped, not accumulated, so a slow finger never scrolls.
 *
 * Behavior: after the stock handler, if it overwrote prev but did not scroll, restore prev so
 * the next report measures from the last delivered point. Only once the gesture is
 * already a drag ([this+0x125]), so a jittery tap stays a tap and the tap/drag threshold
 * is untouched. Only fields the handler just wrote are touched, on its thread, inside
 * the same call. */
#define TA_DRAG_FN EP_21e1090
#define TA_FLAG "/home/root/settings/azmod-touchaccum.on"
static void (*g_ta_orig)(void *, void *);
static int g_ta_on;
static void ta_drag_hook(void *self, void *ev) {
    double *prev = (double *) ((char *) self + 0x100), *d = (double *) ((char *) self + 0x110);
    double px = prev[0], py = prev[1];
    g_ta_orig(self, ev);
    if (*((uint8_t *) self + 0x125)) /* a real drag: the row cache's scroll session */
        __atomic_store_n(&g_rc_touch_ms, drag_now_ms(), __ATOMIC_RELAXED);
    if (!g_ta_on)
        return;
    if ((prev[0] != px || prev[1] != py) && fabs(d[0]) < 2.0 && fabs(d[1]) < 2.0 &&
        *((uint8_t *) self + 0x125)) {
        prev[0] = px;
        prev[1] = py; /* dropped: keep measuring from the last delivered point */
    }
}
/* Shared by the slow-drag accumulation (g_ta_on) and the row cache's scroll-session
 * timestamp: installed once, by whichever asks first. Returns 1 when it is in. */
static int g_ta_hooked;
static int ta_ensure_hook(void) {
    if (g_ta_hooked)
        return g_ta_hooked == 1;
    g_ta_hooked =
        azm_install_hook(TA_DRAG_FN, (void *) ta_drag_hook, (void **) &g_ta_orig) == 1 ? 1 : -1;
    FILE *f = azlog_open("/tmp/azmod-touchaccum.log", "a");
    if (f) {
        fprintf(f, "install drag hook: %s (accumulate=%d)\n", g_ta_hooked == 1 ? "ok" : "DECLINED",
                g_ta_on);
        fclose(f);
    }
    return g_ta_hooked == 1;
}
static void install_touchaccum(void) {
    g_ta_on = ui60ui_at_start(); /* 60 FPS UI mod; TA_FLAG only migrates into it */
    if (!g_ta_on)
        return;
    if (!ta_ensure_hook())
        g_ta_on = 0;
}
