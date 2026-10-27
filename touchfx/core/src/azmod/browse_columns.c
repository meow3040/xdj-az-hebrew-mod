/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file browse_columns.c
 * @brief The Browse track lists' columns: EP147's own PREVIEW button and sub column choice
 * survive a restart, and Column Edit's hidden and shown columns.
 *
 * Column Edit (browse_edit.c) hides or shows the artwork, track number, BPM and Key columns of the
 * full-width track lists (AZMOD_BCOL_*). A column the DJ has set (AZMOD_BCOL_SET) applies in every
 * such list; one never set stays as EP147 has it, which differs by list (EP147 shows # in album,
 * playlist and history lists and hides it in TRACK). It goes through EP147's own header: the hook
 * on gui::browse::TrackListHeader's column layout sets those columns' visibility with
 * juce::TableHeaderComponent::setColumnVisible just before EP147 lays them out, so the widths stay
 * EP147's (the title column takes what the others leave). Every change of visibility EP147 makes
 * is followed by that layout, and so is every resize. The compact track panes (header mode 1:
 * artwork and title only) are left as they are. Saved in settings/azmod-colshow.
 * It applies only while the Mod Menu's Column Edit row is on. EP147's own visibility of each
 * column is recorded as EP147 sets it (the setColumnVisible hook; our own calls go around it),
 * so turning Column Edit off, and RESET, give each column back as EP147 last set it. A column
 * whose EP147 state was never seen is left as it is.
 *
 * EP147 keeps the PREVIEW button's state and the sub column choice (the header's sort menu
 * button) only in memory, so every start shows the defaults again. PREVIEW is saved to
 * settings/azmod-preview when the button sets it and set again on the Browse function buttons
 * right after they are built. The sub column choice is saved to settings/azmod-subcolumn when
 * it is made and chosen again after the first track list has its choices. Both go through
 * EP147's own setters, whatever Column Edit is set to.
 * Kill switches: settings/azmod-preview.off, settings/azmod-subcolumn.off.
 *
 * Every library field, with Column Edit on: EP147's sub column list (the header's sort menu
 * button, and Column Edit's field list) offers only the library's sort rows marked visible
 * (rekordbox's My Settings). While the Column Edit row is on, every row answers visible, and a
 * row the library hid sorts after the visible ones, in the library's own row order. EP147 reads
 * the rows when it loads a library, so turning the row on or off applies at the next Reload.
 *
 * Entry points: install_browse_columns, bc_tick (message thread).
 */
/* EP147, gui::browse::TrackListHeader (a juce::TableHeaderComponent):
 *   - columns (v1.30 / v2.00 ids): 1 Preview, 2 artwork, 3 track number, title 4 / 5, the sub
 *     column 5 / 6, BPM 6 / 7, Key 7 / 8 (v2.00 added a status icon column as 4);
 *   - layout FUN_01bd06b8(header, row height): widths from which columns are visible, the
 *     title column gets the rest; mode at +0x134 (0 full-width list, 1 compact pane);
 *   - juce::TableHeaderComponent::setColumnVisible FUN_0248a8f0(header, id, visible): returns
 *     at once when nothing changes; isColumnVisible FUN_024597e0(header, id).
 * gui::status_area::browse::FunctionButtonWidgetController (the Browse function buttons):
 *   - ctor FUN_01c6e420(this, ...); PREVIEW set FUN_01c6e788(this, on), called only by the
 *     buttons' handler (through FUN_01c70fc8); the state is the button's index, 1 = on.
 * music_library::SqliteSortEntry (one row of the library's sort table; vtable 0x284b090), read
 * on EP147's library thread: slot +0x10 sort_id, +0x20 sequenceNo, +0x28 isVisible.
 * gui::browser_logic::TrackListDisplayFormatEventFacade:
 *   - ctor FUN_01f25838(this, ...); vtable slot +0x18 changeSubCateory(this, index): posts
 *     the change; an index the current list does not have is logged and ignored. */
#define BC_LAYOUT EP_1bd06b8
#define BC_SETVIS EP_248a8f0
#define BC_ISVIS EP_24597e0
#define BC_HDR_VT EP_2b7d8d0
#define BC_HDR_MODE 0x134
#define BC_FB_CTOR EP_1c6e420
#define BC_FB_VT EP_2c51010
#define BC_PV_SET EP_1c6e788
#define BC_FB_MAX 8
#define BC_FC_CTOR EP_1f25838
#define BC_FC_VT EP_2dc7888
#define BC_FC_SUB 0x18
#define BC_SORT_VT EP_284b090
#define BC_SORT_ID 0x10
#define BC_SORT_SEQ 0x20
#define BC_SORT_VIS 0x28
#define BC_SORT_HIDDEN_SEQ 1000 /* a hidden row's place: after every visible one */
#ifndef BC_PV_PATH
#define BC_PV_PATH "/home/root/settings/azmod-preview"
#define BC_PV_OFF "/home/root/settings/azmod-preview.off"
#define BC_SC_PATH "/home/root/settings/azmod-subcolumn"
#define BC_SC_OFF "/home/root/settings/azmod-subcolumn.off"
#define BC_COLS_PATH "/home/root/settings/azmod-colshow"
#endif
#define BC_HDR_MAX 8
#define BC_EP_MAX 16      /* headers whose EP147 visibility is recorded */
#define BC_FC_MAX 8
#define BC_SC_MAX 64      /* a sub column index past this is not EP147's */
#define BC_SC_TRIES 16    /* restore attempts (one per layout) before giving up */
#define BC_SC_GAP_MS 300  /* at least this between attempts */

/* Column Edit's columns: one bit per column it hides or shows on the full-width track lists.
 * EP147's PREVIEW button and its sub column button keep their own columns. */
#define AZMOD_BCOL_ART 0x01u /* the artwork column */
#define AZMOD_BCOL_NUM 0x02u /* the track number (#) */
#define AZMOD_BCOL_BPM 0x04u
#define AZMOD_BCOL_KEY 0x08u
#define AZMOD_BCOL_ALL 0x0Fu
#define AZMOD_BCOL_DEFAULT (AZMOD_BCOL_ART | AZMOD_BCOL_BPM | AZMOD_BCOL_KEY) /* as EP147's TRACK list */
#define AZMOD_BCOL_SET(bits) ((unsigned) (bits) << 4) /* the same columns, as set by the DJ */
#define AZMOD_BCOL_SET_OF(v) (((unsigned) (v) >> 4) & AZMOD_BCOL_ALL)

static void bc_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-bcol.log", "a");
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
static int bc_killed(const char *path, int *cache) {
    int v = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
    if (v < 0) {
        v = access(path, F_OK) == 0;
        __atomic_store_n(cache, v, __ATOMIC_RELEASE);
    }
    return v;
}
/* a small decimal setting in [0, max], or -1 */
static int bc_read_int(const char *path, int max) {
    FILE *f = fopen(path, "re");
    int n = -1;
    if (!f)
        return -1;
    if (fscanf(f, "%d", &n) != 1)
        n = -1;
    fclose(f);
    return n >= 0 && n <= max ? n : -1;
}
static void bc_write_int(const char *path, int v) {
    char line[16];
    int n = snprintf(line, sizeof line, "%d\n", v);
    settings_write(path, line, (size_t) n, 1);
}
/* p if it is aligned, readable and its vtable is vt, else 0 */
static uintptr_t bc_obj(uintptr_t p, uintptr_t vt) {
    uintptr_t v = 0;
    return p && !(p & 7) && own_q(p, &v) && v == vt ? p : 0;
}

/* ---- Column Edit's columns -------------------------------------------------------------- */
/* EP147's column id for a column bit, 0 for none */
static int bc_col_id(unsigned bit) {
    switch (bit) {
    case AZMOD_BCOL_ART: return 2;
    case AZMOD_BCOL_NUM: return 3;
    case AZMOD_BCOL_BPM: return ep_off(6, 7);
    case AZMOD_BCOL_KEY: return ep_off(7, 8);
    default: return 0;
    }
}
static int bc_sub_id(void) { return ep_off(5, 6); }
static void (*g_o_bc_layout)(long, int);
static void (*g_bc_setvis)(long, int, int); /* setColumnVisible itself: never recorded as EP147's */
static int (*g_bc_isvis)(long, int);
/* a full-width track list's header (not a compact pane), or 0 */
static uintptr_t bc_full_hdr(uintptr_t h) {
    uintptr_t m = 0;
    if (!bc_obj(h, BC_HDR_VT) || !own_q(h + (BC_HDR_MODE & ~7u), &m))
        return 0;
    return (int) (m >> (8 * (BC_HDR_MODE & 7))) == 0 ? h : 0;
}
/* Set the columns in `bits` as `mask` has them. */
static void bc_set_cols(uintptr_t h, unsigned mask, unsigned bits) {
    for (unsigned b = 1; b <= AZMOD_BCOL_ALL; b <<= 1)
        if (bits & b)
            g_bc_setvis((long) h, bc_col_id(b), (mask & b) != 0);
}
/* the columns shown (low bits) and set by the DJ (AZMOD_BCOL_SET) (message thread) */
static unsigned g_bc_cols = AZMOD_BCOL_DEFAULT;
static unsigned bc_mask(void) { return g_bc_cols; }
/* Column Edit as applied: its row on and browse_edit.c installed. Written by bc_tick only; read
 * by paint hooks too, so __atomic only. */
static int g_bc_ce;
static int bc_ce_on(void) { return __atomic_load_n(&g_bc_ce, __ATOMIC_ACQUIRE); }
/* the full-width headers seen by the layout, with their last row height (message thread) */
static uintptr_t g_bc_hdr[BC_HDR_MAX];
static int g_bc_rowh[BC_HDR_MAX];
static void bc_remember(uintptr_t h, int rowh) {
    int slot = -1;
    for (int i = 0; i < BC_HDR_MAX; i++) {
        if (g_bc_hdr[i] == h) {
            g_bc_rowh[i] = rowh;
            return;
        }
        if (slot < 0 && !bc_full_hdr(g_bc_hdr[i]))
            slot = i; /* empty, or no longer a live header */
    }
    if (slot >= 0) {
        g_bc_hdr[slot] = h;
        g_bc_rowh[slot] = rowh;
    }
}

/* EP147's own visibility of each header's columns, as it last set them (bit id). Kept only while
 * the headers' lifetimes are followed (bc_hdr_built / bc_hdr_gone, from browse_fields.c's header
 * constructor and destructor hooks): a record of a header that was freed would answer for the
 * next object at that address. (message thread) */
static struct {
    uintptr_t h;
    unsigned known, vis;
} g_bc_ep[BC_EP_MAX];
static int g_bc_ep_ok; /* the lifetimes are followed (set once at install; __atomic only) */
static void (*g_o_bc_setvis)(long, int, int);
static int bc_ep_slot(uintptr_t h, int make) {
    int free_slot = -1;
    for (int i = 0; i < BC_EP_MAX; i++) {
        if (g_bc_ep[i].h == h)
            return i;
        if (free_slot < 0 && !g_bc_ep[i].h)
            free_slot = i;
    }
    if (make && free_slot >= 0) {
        g_bc_ep[free_slot].h = h;
        g_bc_ep[free_slot].known = g_bc_ep[free_slot].vis = 0;
    }
    return make ? free_slot : -1;
}
/* juce::TableHeaderComponent::setColumnVisible, as EP147 calls it: recorded for a track list
 * header, then EP147's call as made */
static void bc_setvis_hook(long hdr, int id, int vis) {
    if (__atomic_load_n(&g_bc_ep_ok, __ATOMIC_ACQUIRE) && id >= 1 && id < 32 && bc_obj((uintptr_t) hdr, BC_HDR_VT)) {
        int i = bc_ep_slot((uintptr_t) hdr, 1);
        if (i >= 0) {
            g_bc_ep[i].known |= 1u << id;
            g_bc_ep[i].vis = (vis & 0xff) ? g_bc_ep[i].vis | 1u << id : g_bc_ep[i].vis & ~(1u << id);
        }
    }
    g_o_bc_setvis(hdr, id, vis);
}
/* browse_fields.c: a header was just built (it shows every column until EP147 sets them up) */
static void bc_hdr_built(uintptr_t h) {
    if (!__atomic_load_n(&g_bc_ep_ok, __ATOMIC_ACQUIRE))
        return;
    int i = bc_ep_slot(h, 1);
    if (i < 0)
        return;
    g_bc_ep[i].known = g_bc_ep[i].vis = 0;
    for (unsigned b = 1; b <= AZMOD_BCOL_ALL; b <<= 1) {
        int id = bc_col_id(b);
        g_bc_ep[i].known |= 1u << id;
        if (g_bc_isvis((long) h, id))
            g_bc_ep[i].vis |= 1u << id;
    }
}
/* browse_fields.c: a header is being destroyed */
static void bc_hdr_gone(uintptr_t h) {
    int i = bc_ep_slot(h, 0);
    if (i >= 0)
        memset(&g_bc_ep[i], 0, sizeof g_bc_ep[i]);
}
/* EP147's own visibility of column id on h: 1, 0, or -1 unknown */
static int bc_ep_vis(uintptr_t h, int id) {
    int i = __atomic_load_n(&g_bc_ep_ok, __ATOMIC_ACQUIRE) && id >= 1 && id < 32 ? bc_ep_slot(h, 0) : -1;
    if (i < 0 || !(g_bc_ep[i].known >> id & 1u))
        return -1;
    return (int) (g_bc_ep[i].vis >> id & 1u);
}
/* The columns in `bits` as EP147 last set them on h; one never seen is left as it is. */
static void bc_give_back(uintptr_t h, unsigned bits) {
    for (unsigned b = 1; b <= AZMOD_BCOL_ALL; b <<= 1) {
        int v = (bits & b) ? bc_ep_vis(h, bc_col_id(b)) : -1;
        if (v >= 0)
            g_bc_setvis((long) h, bc_col_id(b), v);
    }
}

static void bc_sc_check(uintptr_t h);
/* browse_fields.c (Column Edit): a pick one of its buttons asked for; unset without it */
static int (*g_bc_take_pick)(int idx);
/* browse_edit.c (Column Edit), around EP147's layout of a full-width header, and told when
 * Column Edit goes on or off; unset without it */
static void (*g_bc_before_layout)(uintptr_t h), (*g_bc_after_layout)(uintptr_t h);
static void (*g_bc_ce_changed)(int on);
static void bc_layout_hook(long hdr, int rowh) {
    uintptr_t h = bc_full_hdr((uintptr_t) hdr);
    if (h) {
        bc_remember(h, rowh);
        /* only the columns the DJ has set; EP147 owns the rest */
        if (bc_ce_on())
            bc_set_cols(h, g_bc_cols, AZMOD_BCOL_SET_OF(g_bc_cols));
        bc_sc_check(h);
        if (g_bc_before_layout)
            g_bc_before_layout(h);
    }
    g_o_bc_layout(hdr, rowh);
    if (h && g_bc_after_layout)
        g_bc_after_layout(h);
}
/* every full-width header on screen laid out again */
static void bc_relayout_all(void) {
    for (int i = 0; i < BC_HDR_MAX; i++) {
        uintptr_t h = bc_full_hdr(g_bc_hdr[i]);
        if (h)
            bc_layout_hook((long) h, g_bc_rowh[i]);
    }
}
/* Column Edit's columns changed (a header tap, a Hide column pick, RESET): saved, and the headers
 * on screen take them. A column no longer set goes back to EP147's own. (message thread) */
static void bc_set_mask(unsigned m) {
    m &= 0xFFu;
    unsigned was = AZMOD_BCOL_SET_OF(g_bc_cols), now = AZMOD_BCOL_SET_OF(m);
    if (m == g_bc_cols)
        return;
    g_bc_cols = m;
    bc_write_int(BC_COLS_PATH, (int) m);
    if (bc_ce_on())
        for (int i = 0; i < BC_HDR_MAX; i++) {
            uintptr_t h = bc_full_hdr(g_bc_hdr[i]);
            if (h)
                bc_give_back(h, was & ~now);
        }
    bc_relayout_all();
    bc_log("columns %x\n", m);
}
/* The Mod Menu's Column Edit row went on or off: off, every column the DJ set is EP147's again
 * (browse_edit.c leaves edit mode and lets EP147's widths and names back first). (message thread) */
static void bc_ce_tick(void) {
    int want = g_bc_ce_changed && az_on(AZF_COLUMN_EDIT);
    if (want == bc_ce_on())
        return;
    __atomic_store_n(&g_bc_ce, want, __ATOMIC_RELEASE);
    if (g_bc_ce_changed)
        g_bc_ce_changed(want);
    if (!want)
        for (int i = 0; i < BC_HDR_MAX; i++) {
            uintptr_t h = bc_full_hdr(g_bc_hdr[i]);
            if (h)
                bc_give_back(h, AZMOD_BCOL_SET_OF(g_bc_cols));
        }
    bc_relayout_all();
    bc_log("column edit %d\n", want);
}

/* ---- PREVIEW --------------------------------------------------------------------------- */
static int g_pv_off = -1;
static int g_pv_saved = -2; /* -2 unread, -1 none, else the saved state (__atomic only) */
static void (*g_o_pv_set)(long, int);
static void (*g_o_fb_ctor)(long, long, long, long);
static void bc_pv_set_hook(long fb, int on) {
    g_o_pv_set(fb, on);
    if (bc_killed(BC_PV_OFF, &g_pv_off) || !bc_obj((uintptr_t) fb, BC_FB_VT))
        return;
    on = (on & 0xff) != 0;
    if (__atomic_exchange_n(&g_pv_saved, on, __ATOMIC_ACQ_REL) == on)
        return;
    bc_write_int(BC_PV_PATH, on);
    bc_log("PREVIEW saved %d\n", on);
}
/* the Browse function buttons were just built: PREVIEW as it was saved */
static void bc_fb_ctor_hook(long fb, long a1, long a2, long a3) {
    g_o_fb_ctor(fb, a1, a2, a3);
    if (bc_killed(BC_PV_OFF, &g_pv_off) || !bc_obj((uintptr_t) fb, BC_FB_VT))
        return;
    int v = __atomic_load_n(&g_pv_saved, __ATOMIC_ACQUIRE);
    if (v == -2) {
        v = bc_read_int(BC_PV_PATH, 1);
        __atomic_store_n(&g_pv_saved, v, __ATOMIC_RELEASE);
    }
    if (v >= 0)
        g_o_pv_set(fb, v); /* the original: not a press, nothing is written back */
    bc_log("function buttons %lx: PREVIEW %d\n", (unsigned long) fb, v);
}

/* ---- the sub column choice ------------------------------------------------------------ */
static int g_sc_off = -1;
static int g_sc_saved = -1;     /* the saved index, -1 none (__atomic only) */
static int g_sc_pending;        /* a saved choice still to be made (__atomic only) */
static int g_sc_want;           /* a full-width list is up without it (__atomic only) */
static int g_sc_tries;          /* attempts so far (__atomic only) */
static long long g_sc_next_ms;  /* message thread */
static uintptr_t g_sc_fc[BC_FC_MAX]; /* the facades built (__atomic only) */
static void (*g_o_sc)(long, int);
static void (*g_o_fc_ctor)(long, long, long, long);
static void bc_sc_hook(long fc, int idx) {
    if (g_bc_take_pick && g_bc_take_pick(idx))
        return; /* a column's field button opened the list: its pick, not EP147's */
    g_o_sc(fc, idx);
    if (bc_killed(BC_SC_OFF, &g_sc_off) || !bc_obj((uintptr_t) fc, BC_FC_VT) || idx < 0 ||
        idx > BC_SC_MAX)
        return;
    __atomic_store_n(&g_sc_pending, 0, __ATOMIC_RELEASE); /* the DJ's choice wins */
    if (__atomic_exchange_n(&g_sc_saved, idx, __ATOMIC_ACQ_REL) == idx)
        return;
    bc_write_int(BC_SC_PATH, idx);
    bc_log("sub column saved %d (facade %lx)\n", idx, (unsigned long) fc);
}
static void bc_fc_ctor_hook(long fc, long a1, long a2, long a3) {
    g_o_fc_ctor(fc, a1, a2, a3);
    bc_log("facade %lx\n", (unsigned long) fc);
    for (int i = 0; i < BC_FC_MAX; i++) {
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
static void bc_sc_check(uintptr_t h) {
    if (!__atomic_load_n(&g_sc_pending, __ATOMIC_ACQUIRE))
        return;
    if (!g_bc_isvis((long) h, bc_sub_id())) {
        __atomic_store_n(&g_sc_want, 1, __ATOMIC_RELEASE);
    } else if (__atomic_load_n(&g_sc_tries, __ATOMIC_ACQUIRE) > 0) {
        __atomic_store_n(&g_sc_pending, 0, __ATOMIC_RELEASE);
        bc_log("sub column restored\n");
    }
}
static void bc_sc_tick(long long now) {
    if (!__atomic_load_n(&g_sc_pending, __ATOMIC_ACQUIRE) || !__atomic_load_n(&g_sc_want, __ATOMIC_ACQUIRE) ||
        now < g_sc_next_ms)
        return;
    /* one ask per layout without it: EP147 refuses it until a track list's tab exists */
    __atomic_store_n(&g_sc_want, 0, __ATOMIC_RELEASE);
    int tries = __atomic_add_fetch(&g_sc_tries, 1, __ATOMIC_ACQ_REL);
    if (tries > BC_SC_TRIES) {
        __atomic_store_n(&g_sc_pending, 0, __ATOMIC_RELEASE);
        bc_log("sub column: gave up\n");
        return;
    }
    g_sc_next_ms = now + BC_SC_GAP_MS;
    int idx = __atomic_load_n(&g_sc_saved, __ATOMIC_ACQUIRE), n = 0;
    for (int i = 0; i < BC_FC_MAX; i++) {
        uintptr_t fc = bc_obj(__atomic_load_n(&g_sc_fc[i], __ATOMIC_ACQUIRE), BC_FC_VT);
        if (fc) {
            g_o_sc((long) fc, idx); /* the original: not the DJ's choice, nothing is written */
            n++;
        }
    }
    bc_log("sub column %d asked of %d (try %d)\n", idx, n, tries);
}

/* ---- every library field, with Column Edit on (EP147's library thread) ------------------ */
static int (*g_o_sort_id)(void *), (*g_o_sort_seq)(void *), (*g_o_sort_vis)(void *);
static int bc_allfields(void) { return az_on(AZF_COLUMN_EDIT); }
static int bc_sort_vis_hook(void *row) {
    int v = g_o_sort_vis(row);
    return bc_allfields() ? 1 : v;
}
static int bc_sort_seq_hook(void *row) {
    int seq = g_o_sort_seq(row);
    if (!bc_allfields() || g_o_sort_vis(row))
        return seq;
    int id = g_o_sort_id(row); /* a row the library hid: after the visible ones, in row order */
    return BC_SORT_HIDDEN_SEQ + (id >= 0 && id < BC_SORT_HIDDEN_SEQ ? id : BC_SORT_HIDDEN_SEQ - 1);
}

/* message thread (calltimers_hook) */
static void bc_tick(void) {
    if (!g_o_bc_layout)
        return;
    bc_ce_tick();
    if (g_o_sc)
        bc_sc_tick(drag_now_ms());
}
static void install_browse_columns(void) {
    g_bc_setvis = (void (*)(long, int, int)) BC_SETVIS;
    g_bc_isvis = (int (*)(long, int)) BC_ISVIS;
    int v = bc_read_int(BC_COLS_PATH, 0xFF);
    g_bc_cols = v >= 0 ? (unsigned) v : AZMOD_BCOL_DEFAULT;
    int a = BC_SETVIS && BC_ISVIS && BC_HDR_VT &&
            azm_install_hook(BC_LAYOUT, (void *) bc_layout_hook, (void **) &g_o_bc_layout);
    /* EP147's own visibility recorded; our calls go to the original */
    int e = a == 1 && azm_install_hook(BC_SETVIS, (void *) bc_setvis_hook, (void **) &g_o_bc_setvis) == 1;
    if (e)
        g_bc_setvis = g_o_bc_setvis;
    int b = 0, c = 0;
    if (!bc_killed(BC_PV_OFF, &g_pv_off) && BC_FB_VT &&
        azm_install_hook(BC_PV_SET, (void *) bc_pv_set_hook, (void **) &g_o_pv_set))
        b = azm_install_hook(BC_FB_CTOR, (void *) bc_fb_ctor_hook, (void **) &g_o_fb_ctor);
    if (!bc_killed(BC_SC_OFF, &g_sc_off) && BC_FC_VT &&
        azm_install_hook(BC_FC_CTOR, (void *) bc_fc_ctor_hook, (void **) &g_o_fc_ctor)) {
        c = azm_patch_vtable(BC_FC_VT + BC_FC_SUB, (void *) bc_sc_hook, (void **) &g_o_sc);
        if (c == 1 && a == 1) {
            int s = bc_read_int(BC_SC_PATH, BC_SC_MAX);
            __atomic_store_n(&g_sc_saved, s, __ATOMIC_RELEASE);
            __atomic_store_n(&g_sc_pending, s > 0, __ATOMIC_RELEASE); /* 0 = EP147's default */
        }
    }
    /* the visibility slot and the id getter first: the order hook calls both */
    uintptr_t idf = 0;
    int f = BC_SORT_VT && img_q(BC_SORT_VT + BC_SORT_ID, &idf) && idf &&
            azm_patch_vtable(BC_SORT_VT + BC_SORT_VIS, (void *) bc_sort_vis_hook, (void **) &g_o_sort_vis) == 1;
    if (f) {
        g_o_sort_id = (int (*)(void *)) idf;
        f = 1 + (azm_patch_vtable(BC_SORT_VT + BC_SORT_SEQ, (void *) bc_sort_seq_hook, (void **) &g_o_sort_seq) == 1);
    }
    bc_log("browse columns: layout=%d visibility=%d preview=%d subcolumn=%d (saved %d) columns %x all fields=%d\n",
           a, e, b, c, __atomic_load_n(&g_sc_saved, __ATOMIC_ACQUIRE), g_bc_cols, f);
}
