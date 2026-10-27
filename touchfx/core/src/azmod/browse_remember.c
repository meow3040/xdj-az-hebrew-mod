/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file browse_remember.c
 * @brief EP147's own PREVIEW button and sub column choice survive a restart.
 *
 * EP147 keeps the Browse PREVIEW button's state and the sub column choice (the track list
 * header's sort menu button, the up-down arrows) only in memory, so every start shows the
 * defaults again. PREVIEW is saved to settings/azmod-preview when the button sets it and set
 * again on the Browse function buttons right after they are built. The sub column choice is
 * saved to settings/azmod-subcolumn when it is made and chosen again after the first track list
 * has its choices. Both go through EP147's own setters; nothing here changes which columns a
 * list shows or how wide they are, and there is no Mod Menu row.
 * Kill switches: settings/azmod-preview.off, settings/azmod-subcolumn.off.
 *
 * Entry points: install_browse_remember, br_tick (message thread).
 */
/* EP147, gui::browse::TrackListHeader (a juce::TableHeaderComponent):
 *   - columns (v1.30 / v2.00 ids): 1 Preview, 2 artwork, 3 track number, title 4 / 5, the sub
 *     column 5 / 6, BPM 6 / 7, Key 7 / 8 (v2.00 added a status icon column as 4);
 *   - layout FUN_01bd06b8(header, row height): widths from which columns are visible, the
 *     title column gets the rest; mode at +0x134 (0 full-width list, 1 compact pane). Every
 *     change of a column's visibility EP147 makes is followed by it, so the sub column choice
 *     is checked there (the hook calls the original unchanged);
 *   - juce::TableHeaderComponent::isColumnVisible FUN_024597e0(header, id).
 * gui::status_area::browse::FunctionButtonWidgetController (the Browse function buttons):
 *   - ctor FUN_01c6e420(this, ...); PREVIEW set FUN_01c6e788(this, on), called only by the
 *     buttons' handler (through FUN_01c70fc8); the state is the button's index, 1 = on.
 * gui::browser_logic::TrackListDisplayFormatEventFacade:
 *   - ctor FUN_01f25838(this, ...); vtable slot +0x18 changeSubCateory(this, index): posts
 *     the change; an index the current list does not have is logged and ignored. */
#define BR_LAYOUT EP_1bd06b8
#define BR_ISVIS EP_24597e0
#define BR_HDR_VT EP_2b7d8d0
#define BR_HDR_MODE 0x134
#define BR_FB_CTOR EP_1c6e420
#define BR_FB_VT EP_2c51010
#define BR_PV_SET EP_1c6e788
#define BR_FC_CTOR EP_1f25838
#define BR_FC_VT EP_2dc7888
#define BR_FC_SUB 0x18
#ifndef BR_PV_PATH
#define BR_PV_PATH "/home/root/settings/azmod-preview"
#define BR_PV_OFF "/home/root/settings/azmod-preview.off"
#define BR_SC_PATH "/home/root/settings/azmod-subcolumn"
#define BR_SC_OFF "/home/root/settings/azmod-subcolumn.off"
#endif
#define BR_FC_MAX 8
#define BR_SC_MAX 64      /* a sub column index past this is not EP147's */
#define BR_SC_TRIES 16    /* restore attempts (one per layout) before giving up */
#define BR_SC_GAP_MS 300  /* at least this between attempts */

static void br_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-browse.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
/* a kill switch file, read once: *cache -1 unread, 0 absent, 1 present (__atomic only) */
static int br_killed(const char *path, int *cache) {
    int v = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
    if (v < 0) {
        v = access(path, F_OK) == 0;
        __atomic_store_n(cache, v, __ATOMIC_RELEASE);
    }
    return v;
}
/* a small decimal setting in [0, max], or -1 */
static int br_read_int(const char *path, int max) {
    FILE *f = fopen(path, "re");
    int n = -1;
    if (!f)
        return -1;
    if (fscanf(f, "%d", &n) != 1)
        n = -1;
    fclose(f);
    return n >= 0 && n <= max ? n : -1;
}
static void br_write_int(const char *path, int v) {
    char line[16];
    int n = snprintf(line, sizeof line, "%d\n", v);
    settings_write(path, line, (size_t) n, 1);
}
/* p if it is aligned, readable and its vtable is vt, else 0 */
static uintptr_t br_obj(uintptr_t p, uintptr_t vt) {
    uintptr_t v = 0;
    return p && !(p & 7) && own_q(p, &v) && v == vt ? p : 0;
}

/* ---- PREVIEW --------------------------------------------------------------------------- */
static int g_pv_off = -1;
static int g_pv_saved = -2; /* -2 unread, -1 none, else the saved state (__atomic only) */
static void (*g_o_pv_set)(long, int);
static void (*g_o_fb_ctor)(long, long, long, long);
static void br_pv_set_hook(long fb, int on) {
    g_o_pv_set(fb, on);
    if (br_killed(BR_PV_OFF, &g_pv_off) || !br_obj((uintptr_t) fb, BR_FB_VT))
        return;
    on = (on & 0xff) != 0;
    if (__atomic_exchange_n(&g_pv_saved, on, __ATOMIC_ACQ_REL) == on)
        return;
    br_write_int(BR_PV_PATH, on);
    br_log("PREVIEW saved %d\n", on);
}
/* the Browse function buttons were just built: PREVIEW as it was saved */
static void br_fb_ctor_hook(long fb, long a1, long a2, long a3) {
    g_o_fb_ctor(fb, a1, a2, a3);
    if (!br_obj((uintptr_t) fb, BR_FB_VT) || br_killed(BR_PV_OFF, &g_pv_off))
        return;
    int v = __atomic_load_n(&g_pv_saved, __ATOMIC_ACQUIRE);
    if (v == -2) {
        v = br_read_int(BR_PV_PATH, 1);
        __atomic_store_n(&g_pv_saved, v, __ATOMIC_RELEASE);
    }
    if (v >= 0)
        g_o_pv_set(fb, v); /* the original: not a press, nothing is written back */
    br_log("function buttons %lx: PREVIEW %d\n", (unsigned long) fb, v);
}

/* ---- the sub column choice ------------------------------------------------------------ */
static int g_sc_off = -1;
static int g_sc_saved = -1;     /* the saved index, -1 none (__atomic only) */
static int g_sc_pending;        /* a saved choice still to be made (__atomic only) */
static int g_sc_want;           /* a full-width list is up without it (__atomic only) */
static int g_sc_tries;          /* attempts so far (__atomic only) */
static long long g_sc_next_ms;  /* message thread */
static uintptr_t g_sc_fc[BR_FC_MAX]; /* the facades built (__atomic only) */
static void (*g_o_sc)(long, int);
static void (*g_o_fc_ctor)(long, long, long, long);
static void (*g_o_br_layout)(long, int);
static int (*g_br_isvis)(long, int);
static int br_sub_id(void) { return ep_off(5, 6); }
/* a full-width track list's header (not a compact pane), or 0 */
static uintptr_t br_full_hdr(uintptr_t h) {
    uintptr_t m = 0;
    if (!br_obj(h, BR_HDR_VT) || !own_q(h + (BR_HDR_MODE & ~7u), &m))
        return 0;
    return (int) (m >> (8 * (BR_HDR_MODE & 7))) == 0 ? h : 0;
}
static void br_sc_hook(long fc, int idx) {
    g_o_sc(fc, idx);
    if (br_killed(BR_SC_OFF, &g_sc_off) || !br_obj((uintptr_t) fc, BR_FC_VT) || idx < 0 ||
        idx > BR_SC_MAX)
        return;
    __atomic_store_n(&g_sc_pending, 0, __ATOMIC_RELEASE); /* the DJ's choice wins */
    if (__atomic_exchange_n(&g_sc_saved, idx, __ATOMIC_ACQ_REL) == idx)
        return;
    br_write_int(BR_SC_PATH, idx);
    br_log("sub column saved %d (facade %lx)\n", idx, (unsigned long) fc);
}
static void br_fc_ctor_hook(long fc, long a1, long a2, long a3) {
    g_o_fc_ctor(fc, a1, a2, a3);
    br_log("facade %lx\n", (unsigned long) fc);
    for (int i = 0; i < BR_FC_MAX; i++) {
        uintptr_t z = 0;
        if (__atomic_compare_exchange_n(&g_sc_fc[i], &z, (uintptr_t) fc, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE) ||
            z == (uintptr_t) fc)
            return;
    }
}
/* A full-width list was laid out. Its sub column is hidden: EP147 has set the list up with
 * its default choice, so ask for the saved one. It shows after an ask: done. (A header just
 * built shows every column, before EP147 has set anything up.) */
static void br_sc_check(uintptr_t h) {
    if (!__atomic_load_n(&g_sc_pending, __ATOMIC_ACQUIRE))
        return;
    if (!g_br_isvis((long) h, br_sub_id())) {
        __atomic_store_n(&g_sc_want, 1, __ATOMIC_RELEASE);
    } else if (__atomic_load_n(&g_sc_tries, __ATOMIC_ACQUIRE) > 0) {
        __atomic_store_n(&g_sc_pending, 0, __ATOMIC_RELEASE);
        br_log("sub column restored\n");
    }
}
/* the header's layout, unchanged: only looked at for the sub column */
static void br_layout_hook(long hdr, int rowh) {
    uintptr_t h = br_full_hdr((uintptr_t) hdr);
    if (h)
        br_sc_check(h);
    g_o_br_layout(hdr, rowh);
}
static void br_sc_tick(long long now) {
    if (!__atomic_load_n(&g_sc_pending, __ATOMIC_ACQUIRE) || !__atomic_load_n(&g_sc_want, __ATOMIC_ACQUIRE) ||
        now < g_sc_next_ms)
        return;
    /* one ask per layout without it: EP147 refuses it until a track list's tab exists */
    __atomic_store_n(&g_sc_want, 0, __ATOMIC_RELEASE);
    int tries = __atomic_add_fetch(&g_sc_tries, 1, __ATOMIC_ACQ_REL);
    if (tries > BR_SC_TRIES) {
        __atomic_store_n(&g_sc_pending, 0, __ATOMIC_RELEASE);
        br_log("sub column: gave up\n");
        return;
    }
    g_sc_next_ms = now + BR_SC_GAP_MS;
    int idx = __atomic_load_n(&g_sc_saved, __ATOMIC_ACQUIRE), n = 0;
    for (int i = 0; i < BR_FC_MAX; i++) {
        uintptr_t fc = br_obj(__atomic_load_n(&g_sc_fc[i], __ATOMIC_ACQUIRE), BR_FC_VT);
        if (fc) {
            g_o_sc((long) fc, idx); /* the original: not the DJ's choice, nothing is written */
            n++;
        }
    }
    br_log("sub column %d asked of %d (try %d)\n", idx, n, tries);
}

/* message thread (calltimers_hook) */
static void br_tick(void) {
    if (g_o_sc && g_o_br_layout)
        br_sc_tick(drag_now_ms());
}
static void install_browse_remember(void) {
    int b = 0, c = 0, a = 0;
    if (!br_killed(BR_PV_OFF, &g_pv_off) && BR_FB_VT &&
        azm_install_hook(BR_PV_SET, (void *) br_pv_set_hook, (void **) &g_o_pv_set))
        b = azm_install_hook(BR_FB_CTOR, (void *) br_fb_ctor_hook, (void **) &g_o_fb_ctor);
    if (!br_killed(BR_SC_OFF, &g_sc_off) && BR_FC_VT && BR_ISVIS && BR_HDR_VT &&
        azm_install_hook(BR_FC_CTOR, (void *) br_fc_ctor_hook, (void **) &g_o_fc_ctor)) {
        g_br_isvis = (int (*)(long, int)) BR_ISVIS;
        c = azm_patch_vtable(BR_FC_VT + BR_FC_SUB, (void *) br_sc_hook, (void **) &g_o_sc);
        if (c == 1)
            a = azm_install_hook(BR_LAYOUT, (void *) br_layout_hook, (void **) &g_o_br_layout);
        if (c == 1 && a == 1) {
            int v = br_read_int(BR_SC_PATH, BR_SC_MAX);
            __atomic_store_n(&g_sc_saved, v, __ATOMIC_RELEASE);
            __atomic_store_n(&g_sc_pending, v > 0, __ATOMIC_RELEASE); /* 0 = EP147's default */
        }
    }
    br_log("browse remember: preview=%d subcolumn=%d layout=%d (saved %d)\n", b, c, a,
           __atomic_load_n(&g_sc_saved, __ATOMIC_ACQUIRE));
}
