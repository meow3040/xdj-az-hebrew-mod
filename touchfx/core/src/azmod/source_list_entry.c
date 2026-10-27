/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file source_list_entry.c
 * @brief Native "MOD MENU" row appended to EP147's SOURCE list.
 *
 * The row is a real entry in the row table of gui::source_select::SourceTableModel,
 * so EP147 selects, scrolls, highlights and paints it itself. srcrow_ensure keeps
 * exactly one such row, always last, re-appending it after EP147's add path
 * (intercepted by addrow_hook) sorts the table. Removals match by MediaID and never
 * touch it.
 *
 * All work runs on the JUCE message thread through vtable-slot hooks: the cell refresh
 * sets the row's color flags and draws pinned rows (azmod-pinrows.on), cellClicked
 * forwards a tap when the row is not selected and focuses the docked menu when it is,
 * and selectedRowsChanged mirrors the selection into g_srcrow_want for the overlay and
 * bus threads. While the Mod Menu owns the knob, the source list's knob handlers are
 * neutralized so the cursor cannot leave the row. On by default; azmod-srcrow.off
 * disables the feature. Trace output goes to /tmp/azmod-srcrow.log.
 */
/* SourceTableModel keeps its rows in a juce::Array<Row> (this+0x08 rows, +0x10
 * numAllocated, +0x18 numUsed; Row = 0x28 bytes; malloc/realloc/free), from the
 * model ctor 0x1acdae0 and its add path 0x1ad0d40. Because the knob handlers
 * 0x1acc7b0/0x1acc820 read the count straight from this table, a real appended
 * Row is the only way the knob can reach our row.
 *
 * EP147's add appends and then std::sorts; addrow_hook pops our row first and the
 * next getNumRows re-appends it. Removals (0x1acdb98) match by MediaID. Pinned rows
 * are synthetic rows past numUsed. Nothing here runs on the RT bus/LED paths. */
#define SRC_MODEL_VT EP_2a92ae0 /* SourceTableModel vtable */
#define SRC_ROW_SZ 0x28
#define SRC_ROW_DEFAULT \
    EP_1ac7848 /* Row::Row(): kind 0x0101, empty MediaID, empty strings, flags 0 */
#define SRC_MODEL_ADDROW \
    EP_1ad0d40 /* SourceTableModel::add(this, const Event&, u8): append + std::sort */
/* SourceListWidgetController's knob handlers: UP (this, n) selects max(sel - n, 0),
 * DOWN (this, n) selects min(sel + n, count - 1) via ListBox::selectRow, then
 * tail-call SRC_CTRL_PUBSEL when the row moved. Freezing bus byte 34 does not stop
 * them on the deck (EP147 catches up on frozen detents later), so a buffered UP
 * would select a real source and leave the Mod Menu. While the Mod Menu owns the
 * knob (focused + open) both do nothing; the detents still drive azmod's menu. */
#define SRC_CTRL_KNOBUP EP_1acc7b0
#define SRC_CTRL_KNOBDOWN EP_1acc820
#define SRC_CTRL_ADD \
    EP_1acd240 /* SourceListWidgetController::onDeviceAdded(this, const Event&, u8): add, fit,     \
                  select (model at this+0x80) */
#define SRC_MODEL_MARK \
    EP_1ace798 /* SourceTableModel::mark(this, which 0..4, row): byte +0x20+which = (i==row) on    \
                  every row */
#define SRC_LISTW_FIT \
    EP_1ac9cb0 /* SourceListWidget::fit(this): setSize(640, min(rows*90, h)) +                     \
                  ListBox::updateContent */
#define SRC_LISTWIDGET_CTOR EP_1ac9268
#define LISTBOX_UPDATECONTENT EP_21e48b0 /* juce::ListBox::updateContent */
#define LISTBOX_GETSELECTED EP_21e6de0   /* juce::ListBox::getSelectedRow(int index) */
#define LISTBOX_SELECTROW EP_21e8590 /* juce::ListBox::selectRow(row, dontScroll, deselectOthers)  \
                                      */
#define SRC_VIEW_VT EP_29e2858   /* SourceSelectView vtable; slot 12 = juce::Component::setVisible \
                                  */
#define SRC_COMP_SIZE 0x220      /* sizeof(SourceComponent), from refreshComponentForCell */
#define SRC_COMP_CTOR EP_1ac7e50 /* SourceComponent::SourceComponent(this, ctx) */
#define SRC_COMP_SETDATA EP_1ac7998 /* SourceComponent::setData(this, const Row&) */
#define JSTR_FROM_CSTR EP_2333ce0   /* juce::String::String(const char*) */
#define JSTR_COPY EP_230f530        /* juce::String::String(const String&) */
#define JSTR_DTOR EP_22fc600        /* juce::String::~String */
#define SRC_MARK_FOCUS \
    3 /* Row+0x23: the focused row (light gray); EP147 sets it on every selection change */
#define SRC_MARK_SOURCE 4 /* Row+0x24: the selected SOURCE (blue); EP147 sets it on activate */
#define SRCROW_NAME "MOD MENU"
typedef int (*src_rows_fn)(void *m);
typedef void *(*src_refresh_fn)(void *m, int row, int col, int sel, void *existing);
typedef void (*src_click_fn)(void *m, int row, int col, void *ev);
typedef void (*src_selrow_fn)(void *m, int row);
typedef int (*src_addrow_fn)(void *m, void *ev, unsigned flag);
typedef void (*src_ctrladd_fn)(void *ctrl, void *ev, unsigned flag);
static src_rows_fn g_o_src_rows;
static src_refresh_fn g_o_src_refresh;
static src_click_fn g_o_src_click;
static src_selrow_fn g_o_src_selrow;
static src_addrow_fn g_o_src_addrow;
static src_ctrladd_fn g_o_src_ctrladd;
static volatile int g_srcrow_hold; /* inside EP147's device-added handler: do not re-append yet */
static src_setvis_fn g_o_src_setvis;
static void *(*g_orig_slw_ctor)(void *);
/* set by the JUCE/bus threads, drained by overlay_thread: focus the menu */
static volatile int g_srcrow_open;
static volatile int g_srcrow_close; /* another source row selected / SOURCE view hidden */
static volatile int g_srcrow_n =
    -1; /* model index of our row (for the icon window); -1 = not in the table */
/* EP147's list selection is our row. Written on the JUCE thread by the selection
 * hook; read by the overlay (PREVIEW) and the bus thread (click). */
static volatile int g_srcrow_want;
static volatile int g_src_visible =
    0; /* SOURCE screen up (mirrored from g_state for the interposer thread) */
/* Menu visible and at its top level (AZ_OPEN, not a submenu). Published for the
 * bus thread, which decides whether to swallow BACK before the overlay acts. */
static _Atomic int g_menu_top;
static _Atomic int
    g_srcrow_repaint; /* focus changed -> EP147 must re-render our row (blue <-> gray) */
static _Atomic int
    g_srcrow_unfocus; /* tap on the gray (focused) row -> hand the cursor back to it (blue) */
static _Atomic int
    g_srcrow_fit; /* our row was appended outside EP147's own add path: size the list */
static _Atomic int
    g_srcrow_pin_ours; /* focus release: snap EP147's selection back to our row (bus->JUCE) */
static _Atomic unsigned g_drain_calls; /* how many times the drain has run (trace) */
static void menu_drop_intent(void) {
    __atomic_store_n(&g_srcrow_want, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_srcrow_menu, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED);
}
static int srcrow_trace_on(void) {
    static int on = -1;
    static unsigned calls;
    if (on < 0 || (++calls & 63u) == 0)
        on = AZ_PROBE_ON("/home/root/settings/azmod-srcrow.log");
    return on;
}

/* ---- the TableListBox behind the model ------------------------------------
 * Captured from the SourceListWidget ctor (the juce::TableListBox subobject at
 * this+0x30). The ctor runs more than once, so recent candidates are kept and the
 * one whose `model` member matches is resolved on every use; a rebuilt SOURCE view
 * can never leave us holding the old list. A candidate is used only while its
 * vtable still matches the one recorded at construction. */
#define TBL_CAND_MAX 4
static void *g_tbl_cand[TBL_CAND_MAX];
static volatile int g_tbl_cand_n;
static void *g_tbl;              /* last resolved TableListBox (diagnostic + setVisible path) */
static int g_tbl_model_off = -1; /* offset of ListBox::model inside it, learned at first resolve */
static uintptr_t g_tbl_vt;       /* the TableListBox vtable, recorded at construction */
static volatile int g_slw_ctor_ok =
    -1; /* diag: did the SourceListWidget ctor hook install (1) / decline (0) */
static void slw_ctor_hook(void *self) {
    g_orig_slw_ctor(self);
    void *sub = (void *) ((char *) self + 0x30);
    if (!g_tbl_vt)
        g_tbl_vt = *(uintptr_t *) sub;
    int i = __atomic_load_n(&g_tbl_cand_n, __ATOMIC_RELAXED);
    if (i < TBL_CAND_MAX) {
        g_tbl_cand[i] = sub;
        __atomic_store_n(&g_tbl_cand_n, i + 1, __ATOMIC_RELEASE);
    } else {
        g_tbl_cand[TBL_CAND_MAX - 1] = sub;
    } /* keep the most recent */
}
static int tbl_looks_live(void *c) {
    return c && (((uintptr_t) c & 7) == 0) && g_tbl_vt && *(uintptr_t *) c == g_tbl_vt;
}
/* The TableListBox bound to model m, or NULL. JUCE thread only. */
static void *srcrow_tbl_for(void *m) {
    if (!m)
        return NULL;
    void *t = __atomic_load_n(&g_tbl, __ATOMIC_ACQUIRE);
    if (t && g_tbl_model_off >= 0 && tbl_looks_live(t) &&
        *(void **) ((char *) t + g_tbl_model_off) == m)
        return t;
    int n = __atomic_load_n(&g_tbl_cand_n, __ATOMIC_ACQUIRE);
    for (int i = n - 1; i >= 0; i--) {
        unsigned char *c = g_tbl_cand[i];
        if (!tbl_looks_live(c))
            continue;
        for (int off = 0x40; off < 0x200; off += 8) {
            if (*(void **) (c + off) == m) {
                g_tbl_model_off = off;
                __atomic_store_n(&g_tbl, c, __ATOMIC_RELEASE);
                return c;
            }
        }
    }
    return NULL;
}

/* ---- the row table --------------------------------------------------------- */
static inline unsigned char *src_rows(void *m) {
    return *(unsigned char **) ((char *) m + 8);
}
static inline int src_used(void *m) {
    return *(int *) ((char *) m + 0x18);
}
/* Ours = name "MOD MENU" with an empty MediaID. Matched by content, never by
 * position, because EP147 sorts and memmoves its table. The row's +0x10 String
 * points straight at UTF-8 char data. */
static int srcrow_is_ours(const unsigned char *r) {
    const char *s = *(const char *const *) (r + 0x10);
    if (!s || (uintptr_t) s < 0x1000)
        return 0;
    /* Row::Row zeroes +4..+0xd only (stur xzr,[+4]; strh wzr,[+0xc]); bytes +0xe/+0xf
     * are never written, so never read the MediaID as a whole 8-byte pointer. */
    if (*(const unsigned *) (r + 4) != 0 || *(const unsigned *) (r + 8) != 0)
        return 0;
    return strcmp(s, SRCROW_NAME) == 0;
}
static int srcrow_find_ours(void *m) {
    unsigned char *rows = src_rows(m);
    int n = src_used(m);
    if (!rows || n <= 0)
        return -1;
    for (int i = 0; i < n; i++)
        if (srcrow_is_ours(rows + (size_t) i * SRC_ROW_SZ))
            return i;
    return -1;
}
static void srcrow_destroy(unsigned char *r) {
    ((void (*)(void *)) JSTR_DTOR)(r + 0x18);
    ((void (*)(void *)) JSTR_DTOR)(r + 0x10);
}
/* Append with the allocator EP147 frees the table with (the process allocator;
 * logged at install). */
static int srcrow_append(void *m) {
    unsigned char **prows = (unsigned char **) ((char *) m + 8);
    int *pall = (int *) ((char *) m + 0x10), *pused = (int *) ((char *) m + 0x18);
    int n = *pused;
    if (n < 0)
        return -1;
    if (n + 1 > *pall || !*prows) {
        int cap = n + 1 + 8;
        unsigned char *nr = realloc(*prows, (size_t) cap * SRC_ROW_SZ);
        if (!nr)
            return -1;
        *prows = nr;
        *pall = cap;
    }
    unsigned char *r = *prows + (size_t) n * SRC_ROW_SZ;
    ((void (*)(void *)) SRC_ROW_DEFAULT)(r);
    void *name;
    ((void (*)(void *, const char *)) JSTR_FROM_CSTR)(&name, SRCROW_NAME);
    ((void (*)(void *)) JSTR_DTOR)(r + 0x10);
    *(void **) (r + 0x10) = name; /* move: a juce::String is one pointer */
    *pused = n + 1;
    return n;
}
static void srcrow_remove_at(void *m, int i) {
    unsigned char *rows = src_rows(m);
    int n = src_used(m);
    if (!rows || i < 0 || i >= n)
        return;
    srcrow_destroy(rows + (size_t) i * SRC_ROW_SZ);
    if (i < n - 1)
        memmove(rows + (size_t) i * SRC_ROW_SZ, rows + (size_t) (i + 1) * SRC_ROW_SZ,
                (size_t) (n - 1 - i) * SRC_ROW_SZ);
    *(int *) ((char *) m + 0x18) = n - 1;
}
/* Diagnostic (azmod-rowlate.on): hold our row back until EP147 has announced at
 * least one real source row, so MOD MENU is never the first row in the table. */
static int rowlate_on(void) {
    static int on = -1;
    static unsigned calls;
    if (on < 0 || (++calls & 31u) == 0)
        on = AZ_PROBE_ON("/home/root/settings/azmod-rowlate.on");
    return on;
}
/* Make sure our row exists and is the last row. Returns its index, -1 if absent. */
static int srcrow_ensure(void *m) {
    int n = src_used(m);
    int i = srcrow_find_ours(m);
    int i0 = i;
    if (i >= 0 && i == n - 1)
        return i;
    if (i < 0 && rowlate_on()) {
        unsigned char *rows = src_rows(m);
        int real = 0;
        for (int k = 0; k < n && rows; k++)
            if (!srcrow_is_ours(rows + (size_t) k * SRC_ROW_SZ)) {
                real = 1;
                break;
            }
        if (!real) {
            SRCROW_TRACE(8, "ENSURE: HELD BACK (no real source row yet, n=%d)\n", n);
            return -1;
        }
    }
    if (g_srcrow_hold)
        return i; /* EP147 is mid-add: it decides on its pristine table first */
    if (i >= 0)
        srcrow_remove_at(m, i); /* a sort moved it: put it back at the end */
    i = srcrow_append(m);
    if (i >= 0)
        __atomic_store_n(&g_srcrow_fit, 1, __ATOMIC_RELEASE);
    SRCROW_TRACE(40, "ENSURE: n=%d -> ours=%d (before: n=%d idx=%d)\n", src_used(m), i, n, i0);
    return i;
}
static void srcrow_fit(void *m); /* defined with the list-refresh helpers below */
/* EP147's add: append + sort. Pop ours first so the sort never sees it; it is
 * re-appended later, so our row stays last and EP147's ordering is its own. */
static int addrow_hook(void *m, void *ev, unsigned flag) {
    int had = 0;
    {
        int n = src_used(m);
        if (n > 0 && srcrow_is_ours(src_rows(m) + (size_t) (n - 1) * SRC_ROW_SZ)) {
            srcrow_remove_at(m, n - 1);
            had = 1;
        }
    }
    int rc = g_o_src_addrow(m, ev, flag);
    /* Not re-appended here: the controller (0x1acd240) next selects the new device
     * if it is the only row, else re-finds the selection by MediaID, and our empty
     * MediaID would match and auto-select MOD MENU. ctrladd_hook re-appends after
     * that decision. */
    SRCROW_TRACE(
        30, "ADD:   rc=%d popped=%d -> n=%d (ours re-appended after the controller's handler)\n",
        rc, had, src_used(m));
    return rc;
}
/* The controller's device-added handler: add -> fit -> "only row? select it, else
 * re-find the selection by MediaID" -> right pane. Its fit() calls getNumRows,
 * which would re-append our row before that decision and let the empty-MediaID
 * lookup select it. Hold the re-append until the handler returns, then put the row
 * back and size the list. */
static void ctrladd_hook(void *ctrl, void *ev, unsigned flag) {
    g_srcrow_hold = 1;
    g_o_src_ctrladd(ctrl, ev, flag);
    g_srcrow_hold = 0;
    void *m = (char *) ctrl + 0x80;
    int i = srcrow_ensure(m);
    __atomic_store_n(&g_srcrow_fit, 0, __ATOMIC_RELEASE);
    srcrow_fit(m);
    SRCROW_TRACE(30, "CTRLADD: done -> n=%d ours=%d\n", src_used(m), i);
}

/* ---- the blue "selected source" flag ---------------------------------------
 * Only one row is blue at a time. EP147 moves the flag with its own writer,
 * mark(SOURCE, row); we use the same writer, remember whose blue we took (by
 * MediaID, since the table is re-sorted) and hand it back when the selection
 * leaves, unless EP147 has re-marked a real source in between. */
static unsigned g_blue_id_a, g_blue_id_b;
static int g_blue_saved; /* MediaID words +4 / +8 */
/* Diagnostic (azmod-noblue.on, read live): never take the blue. Taking it writes
 * EP147's own "selected source" flag onto our row without the rest of activation,
 * the only model write unique to our row. */
static int noblue_on(void) {
    static int on = -1;
    static unsigned calls;
    if (on < 0 || (++calls & 31u) == 0)
        on = AZ_PROBE_ON("/home/root/settings/azmod-noblue.on");
    return on;
}
static void srcrow_take_blue(void *m, int ours) {
    unsigned char *rows = src_rows(m);
    int n = src_used(m);
    g_blue_saved = 0;
    if (noblue_on()) {
        SRCROW_TRACE(10, "BLUE: take SUPPRESSED (azmod-noblue.on)\n");
        return;
    }
    if (!rows || ours < 0 || ours >= n)
        return;
    for (int i = 0; i < n; i++) {
        unsigned char *r = rows + (size_t) i * SRC_ROW_SZ;
        if (i != ours && r[0x24]) {
            g_blue_id_a = *(unsigned *) (r + 4);
            g_blue_id_b = *(unsigned *) (r + 8);
            g_blue_saved = 1;
            break;
        }
    }
    ((void (*)(void *, int, int)) SRC_MODEL_MARK)(m, SRC_MARK_SOURCE, ours);
}
static void srcrow_release_blue(void *m, int ours) {
    unsigned char *rows = src_rows(m);
    int n = src_used(m);
    if (!rows || ours < 0 || ours >= n) {
        g_blue_saved = 0;
        return;
    }
    if (!rows[(size_t) ours * SRC_ROW_SZ + 0x24]) {
        g_blue_saved = 0;
        return;
    } /* EP147 already moved it: nothing to undo */
    int back = -1;
    if (g_blue_saved)
        for (int i = 0; i < n; i++) {
            unsigned char *r = rows + (size_t) i * SRC_ROW_SZ;
            if (i != ours && *(unsigned *) (r + 4) == g_blue_id_a &&
                *(unsigned *) (r + 8) == g_blue_id_b) {
                back = i;
                break;
            }
        }
    /* back = -1 marks nobody, as EP147's own deactivate does */
    ((void (*)(void *, int, int)) SRC_MODEL_MARK)(m, SRC_MARK_SOURCE, back);
    g_blue_saved = 0;
}

/* ---- list refresh (JUCE thread) -------------------------------------------- */
static void *g_srcrow_model; /* the model the hooks last ran for; used only via srcrow_tbl_for */
static void srcrow_update_content(void *m) {
    void *tbl = srcrow_tbl_for(m);
    if (tbl)
        ((void (*)(void *)) LISTBOX_UPDATECONTENT)(tbl);
}
static void srcrow_fit(void *m) {
    void *tbl = srcrow_tbl_for(m);
    if (tbl)
        ((void (*)(void *)) SRC_LISTW_FIT)(
            (char *) tbl - 0x30); /* EP147's own sizing: rows*90, then updateContent */
}
/* Drain a pending repaint / fit on the JUCE thread. EP147 rebuilds the row
 * component only on updateContent(), so a focus change (blue <-> gray) needs one,
 * and a newly appended row needs the list re-sized. Both call back into
 * getNumRows, hence the reentrancy guard; a request raised inside our own refresh
 * is kept for the next call. */
static void srcrow_repaint_if_needed(void *m) {
    static _Atomic int busy;
    int had = __atomic_exchange_n(&g_srcrow_repaint, 0, __ATOMIC_ACQ_REL);
    int fit = __atomic_exchange_n(&g_srcrow_fit, 0, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&g_drain_calls, 1, __ATOMIC_RELAXED);
    if (!had && !fit)
        return;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQ_REL)) {
        if (had)
            __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
        if (fit)
            __atomic_store_n(&g_srcrow_fit, 1, __ATOMIC_RELEASE);
        return;
    }
    SRCROW_TRACE(40, "DRAIN: call#%u had=%d fit=%d want=%d foc=%d open=%d\n",
                 __atomic_load_n(&g_drain_calls, __ATOMIC_RELAXED), had, fit,
                 __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED),
                 __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED),
                 __atomic_load_n(&g_menu_open, __ATOMIC_RELAXED));
    if (fit)
        srcrow_fit(m);
    else
        srcrow_update_content(m);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

/* The pinned rows, in display order (azfeatures.c az_pinned_items). */
static int pin_list(int *out) {
    int n = az_pinned_items(out, AZI_COUNT), m = 0;
    for (int k = 0; k < n; k++) {
        /* A hidden row must never appear as a SOURCE-list row, even if an older cfg
         * pinned it. */
        if (az_hidden(out[k]))
            continue;
        out[m++] = out[k];
    }
    return m;
}
static int pin_count(void) {
    int l[AZI_COUNT];
    return g_state.pinrows_on ? pin_list(l) : 0;
}
static int pin_nth(int k) {
    int l[AZI_COUNT], n = pin_list(l);
    return k >= 0 && k < n ? l[k] : -1;
}

/* ---- the four model hooks (JUCE message thread) ----------------------------- */
static int src_rows_hook(void *m) {
    g_srcrow_model = m;
    int ours = srcrow_ensure(m);
    __atomic_store_n(&g_srcrow_n, ours, __ATOMIC_RELAXED);
    /* Re-sync the selection mirror with the ListBox. The overlay thread can clear
     * want on its own closes without EP147's selection moving (e.g. a device
     * change leaving our row as the only one). Never while focused: the knob is
     * frozen, so the selection cannot move. */
    if (ours >= 0 && !__atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED)) {
        void *tbl = srcrow_tbl_for(m);
        if (tbl) {
            int sel = ((int (*)(void *, int)) LISTBOX_GETSELECTED)(tbl, 0);
            int now = (sel == ours), want = __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED);
            if (now && !want) {
                __atomic_store_n(&g_srcrow_want, 1, __ATOMIC_RELEASE);
                srcrow_take_blue(m, ours);
                __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
                SRCROW_TRACE(20, "RESYNC: selection is ours (sel=%d) -> want=1\n", sel);
            } else if (!now && want) {
                __atomic_store_n(&g_srcrow_want, 0, __ATOMIC_RELEASE);
                srcrow_release_blue(m, ours);
                __atomic_store_n(&g_srcrow_menu, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&g_srcrow_close, 1, __ATOMIC_RELEASE);
                __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
                SRCROW_TRACE(20, "RESYNC: selection left ours (sel=%d) -> want=0\n", sel);
            }
            /* Preview knob nav: in preview (our row selected, menu docked, not
             * focused) EP147 does not move its own selection when the knob turns.
             * azmod moves it to the adjacent row via ListBox::selectRow, which
             * fires selectedRowsChanged -> src_selrow_hook -> want=0, and further
             * turns are native. Our row is always last, so up steps to the row
             * above and down is clamped. */
            if (__atomic_exchange_n(&g_srcrow_pin_ours, 0, __ATOMIC_ACQ_REL) && tbl && ours >= 0) {
                /* Focus just released: byte 34 unfroze to the real knob position and
                 * EP147 jumped its selection there. Snap it back onto MOD MENU so
                 * preview starts on our row; byte 34 is passthrough from here. */
                if (((int (*)(void *, int)) LISTBOX_GETSELECTED)(tbl, 0) != ours) {
                    SRCROW_TRACE(20, "PINOURS: focus released -> selectRow(%d)\n", ours);
                    ((void (*)(void *, int, int, int)) LISTBOX_SELECTROW)(tbl, ours, 0, 1);
                }
            }
        }
    }
    srcrow_repaint_if_needed(m);
    int open = __atomic_load_n(&g_srcrow_menu, __ATOMIC_RELAXED);
    return src_used(m) + (open ? 0 : pin_count());
}
static void *src_refresh_hook(void *m, int row, int col, int sel, void *existing) {
    int n = src_used(m);
    if (row < n) {
        int ours = srcrow_find_ours(m);
        if (row == ours) {
            /* Our row's color flags, written right before EP147 paints it.
             * 0x24 = blue (selected source), 0x23 alone = light-gray focus. Blue
             * in PREVIEW, gray once focus moves into the docked menu. */
            unsigned char *r = src_rows(m) + (size_t) row * SRC_ROW_SZ;
            int want = __atomic_load_n(&g_srcrow_want, __ATOMIC_ACQUIRE);
            int foc = __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED);
            r[0x23] = want ? 1 : 0;
            /* Second writer of the blue flag (besides srcrow_take_blue), so
             * azmod-noblue.on must gate it here too. */
            r[0x24] = (!noblue_on() && want && !foc) ? 1 : 0;
            SRCROW_TRACE(60,
                         "ROWRENDER: drain#%u row=%d sel=%d want=%d foc=%d -> 0x23=%d 0x24=%d\n",
                         __atomic_load_n(&g_drain_calls, __ATOMIC_RELAXED), row, sel, want, foc,
                         r[0x23], r[0x24]);
        }
        return g_o_src_refresh(m, row, col, sel, existing);
    }
    /* Pinned rows past EP147's table (menu closed). Reuse the component JUCE hands
     * back (always one of the model's SourceComponents), else build one the way
     * refreshComponentForCell does. */
    void *comp = existing;
    if (!comp) {
        comp = malloc(SRC_COMP_SIZE); /* process allocator == EP147's operator new */
        if (!comp)
            return NULL;
        ((void (*)(void *, void *)) SRC_COMP_CTOR)(comp, *(void **) ((char *) m + 0x20));
    }
    unsigned char rowbuf[SRC_ROW_SZ];
    ((void (*)(void *)) SRC_ROW_DEFAULT)(rowbuf);
    int open = __atomic_load_n(&g_srcrow_menu, __ATOMIC_RELAXED);
    int j = row - n;
    const char *line1 = NULL;
    if (!open && j >= 0 && j < pin_count()) {
        int item = pin_nth(j);
        if (item >= 0 && az_hidden(item))
            item = -1; /* defensive: never label a hidden mod */
        if (item >= 0 && az_has_value(item)) {
            static const char *sfx[2] = {" Off", " On"};
            static char lab[AZ_VAL_COUNT][40]; /* written only here = JUCE thread */
            const char *base = az_label(item);
            char *p = lab[item], *e = p + sizeof(lab[item]) - 1;
            while (*base && p < e)
                *p++ = *base++;
            const char *s = sfx[az_on(item) ? 1 : 0];
            while (*s && p < e)
                *p++ = *s++;
            *p = 0;
            line1 = lab[item];
        } else if (item >= 0)
            line1 = az_label(item);
    }
    /* else: a stale cell past the count: render it blank and let the next
     * list rebuild take the component back. */
    void *name;
    if (line1) {
        ((void (*)(void *, const char *)) JSTR_FROM_CSTR)(&name, line1);
        ((void (*)(void *)) JSTR_DTOR)(rowbuf + 0x10);
        ((void (*)(void *, void *)) JSTR_COPY)(rowbuf + 0x10, &name);
    }
    ((void (*)(void *, void *)) SRC_COMP_SETDATA)(comp, rowbuf);
    if (line1)
        ((void (*)(void *)) JSTR_DTOR)(&name);
    ((void (*)(void *)) JSTR_DTOR)(rowbuf + 0x18);
    ((void (*)(void *)) JSTR_DTOR)(rowbuf + 0x10);
    return comp;
}
static void src_click_hook(void *m, int row, int col, void *ev) {
    int n = src_used(m);
    int ours = srcrow_find_ours(m);
    int open = __atomic_load_n(&g_srcrow_menu, __ATOMIC_RELAXED);
    int want = __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED);
    /* Ask the ListBox whether this row is selected; fall back to the mirror when
     * the list is not resolved. */
    int selected = want;
    {
        void *tbl = srcrow_tbl_for(m);
        if (tbl)
            selected = (((int (*)(void *, int)) LISTBOX_GETSELECTED)(tbl, 0) == row);
    }
    SRCROW_TRACE(30, "CLICK: row=%d col=%d | n=%d ours=%d open=%d want=%d selected=%d\n", row, col,
                 n, ours, open, want, selected);
    if (row == ours && ours >= 0) {
        if (!selected) {
            /* First tap: EP147 selects the row natively (-> src_selrow_hook -> PREVIEW). */
            g_o_src_click(m, row, col, ev);
            return;
        }
        /* A tap on the already-selected row toggles focus: blue (preview) -> gray
         * focuses the menu; gray -> blue hands the cursor back to the row. Never
         * forwarded: EP147's activate of an empty MediaID takes its "no source"
         * branch and leaves the SOURCE screen. */
        if (!__atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED)) {
            if (!want)
                __atomic_store_n(&g_srcrow_want, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_menu_focused, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&g_menu_open, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&g_srcrow_open, 1, __ATOMIC_RELEASE);
            SRCROW_TRACE(20, "FWD:   row=%d SKIPPED (tap -> focus)\n", row);
        } else {
            __atomic_store_n(&g_menu_focused, 0, __ATOMIC_RELAXED); /* gray -> blue */
            /* overlay: collapse to preview */
            __atomic_store_n(&g_srcrow_unfocus, 1, __ATOMIC_RELEASE);
            SRCROW_TRACE(20, "FWD:   row=%d SKIPPED (tap -> unfocus)\n", row);
        }
        __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
        return;
    }
    if (row < n) {
        g_o_src_click(m, row, col, ev);
        return;
    } /* a real row: EP147's business; the selection hook folds us */
    /* Pinned row (menu closed): activate the mod via the overlay thread, so
     * every g_state write stays on azmod_input's thread. */
    int j = row - n;
    if (!open && j >= 0 && j < pin_count()) {
        int item = pin_nth(j);
        __atomic_store_n(&g_srcrow_item, item, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_act, item + 1, __ATOMIC_RELEASE);
    }
}
/* EP147's selection moved. Always forwarded: EP147 moves the focus flag, refreshes
 * the list and updates the right pane (an empty MediaID shows "no device" under
 * our docked menu). */
static void src_selrow_hook(void *m, int row) {
    int n = src_used(m);
    int ours = srcrow_find_ours(m);
    int want = __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED);
    SRCROW_TRACE(30, "SEL:   row=%d | n=%d ours=%d want=%d\n", row, n, ours, want);
    if (row == ours && ours >= 0) {
        if (!want) {
            /* before EP147's refresh, so it paints blue */
            __atomic_store_n(&g_srcrow_want, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_srcrow_item, -1, __ATOMIC_RELAXED);
            srcrow_take_blue(m, ours);
        }
        g_o_src_selrow(m, row);
        return;
    }
    /* A real source picked: the Mod Menu is not reopened when SOURCE comes back. */
    mmr_forget(&g_mm_resume);
    mmr_page_disarm(&g_mm_page_armed);
    if (want) {
        __atomic_store_n(&g_srcrow_want, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_srcrow_item, -1, __ATOMIC_RELAXED);
        srcrow_release_blue(m, ours);
        __atomic_store_n(&g_srcrow_menu, 0, __ATOMIC_RELEASE);
        /* a real source took the right pane back */
        __atomic_store_n(&g_srcrow_close, 1, __ATOMIC_RELEASE);
    }
    g_o_src_selrow(m, row);
}
/* ---- Source publish on selection (opt-in: azmod-srcfix.on) -----------------
 * MediaID+4 is a source-type enum (1 USB, 2 software control, 3 cloud); ours is 0.
 * 0x1aca178 runs on every selection change (tail of the cellClicked listener
 * 0x1acc8b0 and of the dataset-change handlers 0x1accf78 / 0x1acd070 / 0x1acd158 /
 * 0x1acd240) and does three things:
 *   1. mark(FOCUS, selectedRow)
 *   2. ListBox::updateContent
 *   3. [this+0x50]->vt[0x20](MediaID-of-that-row, 0): publish the current source
 * Our row's kind bytes are 0x01,0x01 like every row, so selecting it publishes
 * MediaID(0,0). With the flag, step 3 is skipped while our row is selected;
 * anything unresolved falls through to EP147 untouched. */
#define SRC_CTRL_PUBSEL \
    EP_1aca178 /* SourceListWidgetController: publish the selected row as the source */
static void (*g_o_src_pubsel)(void *ctrl);
/* Off by default to stay close to stock: suppressing this publish does not prevent
 * the SOURCE -> SHORTCUT jump (type 0 publishes are routine and harmless). */
static int srcfix_on(void) {
    static int on = -1;
    static unsigned calls;
    if (on < 0 || (++calls & 63u) == 0)
        on = AZ_PROBE_ON("/home/root/settings/azmod-srcfix.on");
    return on;
}
/* There are at least two publishers with the same body: 0x1aca178 reads the
 * selected row itself, its sibling 0x1aca09c takes it as an argument (the
 * descriptor builder 0x1aaba50 has seven callers). All end in
 *     x19 = [controller+0x50];  [x19]->vt[0x20](x19, &desc, 0)
 * so the sink slot is hooked instead of the call sites. Its class is not known
 * statically: the vtable is captured the first time a publisher runs, validated,
 * and slot 4 swapped once; anything unexpected leaves EP147 stock.
 *
 * A publish is skipped only when our row is selected and the descriptor is type 0.
 * A real "no source" publish (last USB ejected) is also type 0, and a real source
 * published while the cursor is on our row is not, so both conditions are needed.
 * Every publish is logged with its type and caller. */
#define SRC_CTRL_PUBSEL2 EP_1aca09c /* sibling: same body, row passed in */
static void (*g_o_src_pubsel2)(void *ctrl, int row);
static void (*g_o_srcpub)(void *self, void *desc, long flag, long x3);
static _Atomic int g_srcpub_hooked; /* 0 not yet, 1 installed, -1 refused */

/* Is our row the one the cursor is on right now? JUCE thread only. */
static int srcrow_ours_is_selected(void) {
    void *m = g_srcrow_model;
    if (!m)
        return 0;
    int ours = srcrow_find_ours(m);
    if (ours < 0)
        return 0;
    void *tbl = srcrow_tbl_for(m);
    if (!tbl)
        return 0;
    return ((int (*)(void *, int)) LISTBOX_GETSELECTED)(tbl, 0) == ours;
}
static void srcpub_hook(void *self, void *desc, long flag, long x3) {
    unsigned type = 0xffffffffu;
    if (desc && (((uintptr_t) desc & 3) == 0))
        type = *(unsigned char *) ((char *) desc + 4);
    /* Ask the listbox only when a skip is possible: LISTBOX_GETSELECTED is a JUCE
     * call and this runs on whatever thread publishes. -1 = not asked. */
    int fix = srcfix_on();
    int ours_sel = (fix && type == 0) ? srcrow_ours_is_selected() : -1;
    int skip = fix && ours_sel == 1 && type == 0;
    SRCROW_TRACE(60, "SRCPUB: type=%u ours_selected=%d -> %s (lr=%lx)\n", type, ours_sel,
                 skip ? "SKIPPED" : "forwarded",
                 (unsigned long) (uintptr_t) __builtin_return_address(0));
    if (skip)
        return;
    void (*o)(void *, void *, long, long) = __atomic_load_n(&g_o_srcpub, __ATOMIC_ACQUIRE);
    if (o) /* installed at run time: never branch to an original not yet visible here */
        o(self, desc, flag, x3);
}
/* Capture [controller+0x50]'s vtable and swap slot 4 (byte offset 0x20), once,
 * and only with azmod-srcfix.on.
 *
 * The installer is claimed with a CAS (0 -> 2 "installing"): with a plain
 * load-then-store two threads could both see 0, and the second would capture
 * srcpub_hook as the original and recurse until the stack overflowed. An original
 * that is already our hook is refused too.
 *
 * The vtable must lie in EP147's .rodata; a whole-image range would accept .data,
 * which the PROT_READ restore would then leave read-only. An aligned 8-byte slot
 * never straddles a page, so one page is patched. */
#define EP147_RODATA_LO EP_25c0aa0
#define EP147_RODATA_HI EP_3480128
static void srcpub_install_from(void *ctrl) {
    if (__atomic_load_n(&g_srcpub_hooked, __ATOMIC_ACQUIRE) != 0)
        return;
    if (!srcfix_on())
        return;
    if (!ctrl || ((uintptr_t) ctrl & 7))
        return;
    void *pub = *(void **) ((char *) ctrl + 0x50);
    if (!pub || ((uintptr_t) pub & 7))
        return;
    uintptr_t vt = *(uintptr_t *) pub;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&g_srcpub_hooked, &expect, 2, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;
    if (vt < EP147_RODATA_LO || vt + 0x28 > EP147_RODATA_HI || (vt & 7)) {
        __atomic_store_n(&g_srcpub_hooked, -1, __ATOMIC_RELEASE);
        SRCROW_TRACE(2, "SRCPUB: refusing vt=%lx (not in EP147 .rodata)\n", (unsigned long) vt);
        return;
    }
    void **slot = (void **) (vt + 0x20);
    void *orig = *slot;
    if (orig == (void *) srcpub_hook) {
        __atomic_store_n(&g_srcpub_hooked, -1, __ATOMIC_RELEASE);
        SRCROW_TRACE(2, "SRCPUB: refusing -- slot already holds our hook\n");
        return;
    }
    /* runs on the message thread while EP147 may be calling through this slot: the original
     * is stored before the slot (azm_patch_vtable, release) */
    if (azm_patch_vtable((uintptr_t) slot, (void *) srcpub_hook, (void **) &g_o_srcpub) != 1) {
        __atomic_store_n(&g_srcpub_hooked, -1, __ATOMIC_RELEASE);
        SRCROW_TRACE(2, "SRCPUB: slot not writable for vt=%lx\n", (unsigned long) vt);
        return;
    }
    __atomic_store_n(&g_srcpub_hooked, 1, __ATOMIC_RELEASE);
    SRCROW_TRACE(2, "SRCPUB: sink hooked -- pub=%p vt=%lx orig=%p\n", pub, (unsigned long) vt,
                 (void *) g_o_srcpub);
    dbg("srcrow: source-publish SINK hooked (vt=%lx)\n", (unsigned long) vt);
}
static void src_pubsel_hook(void *ctrl) {
    srcpub_install_from(ctrl);
    g_o_src_pubsel(ctrl);
}
static void src_pubsel2_hook(void *ctrl, int row) {
    srcpub_install_from(ctrl);
    g_o_src_pubsel2(ctrl, row);
}
/* ---- Screen-change trace (opt-in: azmod-scrntrace.on) ----------------------
 * Records who changes the screen away from SOURCE. EP147 switches screens with
 * juce::Component::setVisible on the top-level views, so hooking ShortcutView's
 * slot 12 catches the arrival, and the saved-x29 chain names the caller in
 * absolute EP147 text addresses (non-PIE: file offset = VA - 0x400000).
 * SHORTCUT-SHOW means the Shortcut screen is the destination; SRC-HIDE alone means
 * something else dismissed SOURCE. Each line carries azmod's own state.
 *
 * ShortcutView vtable 0x029e0f18 (RTTI "N3gui8shortcut12ShortcutViewE" 0x29e0ed0
 * -> typeinfo 0x29e0ef0); slot 12 is 0x249daf0, the same Component::setVisible as
 * SourceSelectView's slot 12. Observe-only, message thread, budgeted. */
#define SHORTCUT_VIEW_VT EP_29e0f18
static src_setvis_fn g_o_shortcut_setvis;
static int scrntrace_on(void) {
    static int on = -1;
    static unsigned calls;
    if (on < 0 || (++calls & 15u) == 0)
        on = AZ_PROBE_ON("/home/root/settings/azmod-scrntrace.on");
    return on;
}
static void scrn_log(long fp, const char *what, int visible) {
    static _Atomic int budget = 60;
    if (!scrntrace_on())
        return;
    if (__atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED) <= 0)
        return;
    FILE *f = azlog_open("/tmp/azmod-scrn.log", "a");
    if (!f)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "[%ld.%03ld] %-13s visible=%d | src_vis=%d want=%d ours=%d menu=%d foc=%d\n",
            (long) ts.tv_sec, (long) (ts.tv_nsec / 1000000), what, visible, g_src_visible,
            __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED),
            __atomic_load_n(&g_srcrow_n, __ATOMIC_RELAXED), g_menu_open, g_menu_focused);
    for (int i = 0; i < 14 && fp > 0x10000; i++) {
        long next = *(long *) fp;     /* saved x29 */
        long lr = *(long *) (fp + 8); /* saved x30 */
        if (lr < 0x400000 || lr > 0x4000000)
            break; /* outside EP147 .text */
        fprintf(f, "    [%2d] 0x%08lx\n", i, lr);
        if (next <= fp)
            break; /* the chain must climb */
        fp = next;
    }
    fclose(f);
}
/* Logs hides as well as shows: EP147 calls setVisible(false) on its views at
 * start-up, so a SHORTCUT-HIDE line proves the hook is live and a missing
 * SHORTCUT-SHOW is meaningful. (SHORTCUT is mixer byte 33 bit 0x10, held ~1 s.) */
static void shortcut_setvis_hook(void *c, int visible) {
    long fp;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    scrn_log(fp, (visible & 1) ? "SHORTCUT-SHOW" : "SHORTCUT-HIDE", visible);
    g_o_shortcut_setvis(c, visible);
}
static void install_scrntrace_hook(void) {
    void **vv = (void **) SHORTCUT_VIEW_VT;
    if (azm_patch_vtable((uintptr_t) &vv[12], (void *) shortcut_setvis_hook,
                         (void **) &g_o_shortcut_setvis) != 1) {
        dbg("scrntrace: slot not writable -- NOT installed\n");
        return;
    }
    /* Log the install so a silently skipped installer is visible. */
    dbg("scrntrace: ShortcutView::setVisible hooked (orig=%p); flag azmod-scrntrace.on\n",
        (void *) g_o_shortcut_setvis);
}
/* SOURCE is back and the Mod Menu was up when it was left: select MOD MENU again (EP147's
 * selection hook then marks it ours and the overlay docks the preview), and for a menu that
 * was focused, focus it the way a tap on the selected row does. Message thread. */
static void srcrow_resume(void *m, int level) {
    void *tbl = srcrow_tbl_for(m);
    int ours = srcrow_find_ours(m);
    if (!tbl || ours < 0)
        return;
    /* before the selection: the overlay's open can follow it at once, and takes the page then */
    mmr_page_arm(&g_mm_page_armed);
    if (((int (*)(void *, int)) LISTBOX_GETSELECTED)(tbl, 0) != ours)
        ((void (*)(void *, int, int, int)) LISTBOX_SELECTROW)(tbl, ours, 0, 1);
    if (!__atomic_load_n(&g_srcrow_want, __ATOMIC_ACQUIRE)) { /* the selection did not land */
        mmr_page_disarm(&g_mm_page_armed);
        SRCROW_TRACE(12, "RESUME: level=%d, MOD MENU not selected, nothing reopened\n", level);
        return;
    }
    if (level == MMR_FOCUSED) {
        __atomic_store_n(&g_menu_focused, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu_open, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_srcrow_open, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_srcrow_repaint, 1, __ATOMIC_RELEASE);
    SRCROW_TRACE(12, "RESUME: level=%d ours=%d\n", level, ours);
}
/* SourceSelectView::setVisible. Hide: fold everything. Show: make sure the row is
 * in the table and the list is sized for it; EP147 sizes the list only on its own
 * device events, so a SOURCE view entered before any of them would clip our row. */
static void src_setvis_hook(void *c, int visible) {
    /* Control for the shortcut trace: whatever dismisses SOURCE passes here first.
     * Reads state, writes nothing. */
    {
        long fp;
        __asm__ volatile("mov %0, x29" : "=r"(fp));
        scrn_log(fp, (visible & 1) ? "SRC-SHOW" : "SRC-HIDE", visible);
    }
    int resume = MMR_NONE; /* show: the Mod Menu level to bring back (mm_resume.h) */
    if (!(visible & 1)) {
        /* Remember what the menu showed, before the folding below clears it. A real source
         * picked first has already cleared want, so a deliberate pick remembers nothing. */
        mmr_page_disarm(&g_mm_page_armed); /* a resume whose open never came */
        mmr_hide(&g_mm_resume, __atomic_load_n(&g_menu_open, __ATOMIC_RELAXED),
                 __atomic_load_n(&g_srcrow_want, __ATOMIC_RELAXED),
                 __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED));
        __atomic_store_n(&g_srcrow_close, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_srcrow_menu, 0, __ATOMIC_RELEASE);
        if (__atomic_exchange_n(&g_srcrow_want, 0, __ATOMIC_ACQ_REL)) {
            void *m = g_srcrow_model;
            if (m && srcrow_tbl_for(m))
                srcrow_release_blue(m, srcrow_find_ours(m));
        }
    }
    if (visible & 1) /* taken before EP147's show, whose own selection moves would clear it */
        resume = mmr_take(&g_mm_resume);
    g_o_src_setvis(c, visible);
    if (visible & 1) {
        void *m = g_srcrow_model;
        SRCROW_TRACE(12, "SHOW: model=%p tbl=%p cand_n=%d ctor_ok=%d\n", m,
                     __atomic_load_n(&g_tbl, __ATOMIC_ACQUIRE), g_tbl_cand_n, g_slw_ctor_ok);
        if (m) { /* trace: every row's kind, MediaID words (m4/m8) and flags (f23 focus,
                    f24 selected source) */
            unsigned char *rows = src_rows(m);
            int rn = src_used(m);
            for (int i = 0; i < rn && i < 12; i++) {
                const unsigned char *r = rows + (size_t) i * SRC_ROW_SZ;
                const char *nm = *(const char *const *) (r + 0x10);
                SRCROW_TRACE(
                    24, "ROWDUMP: [%d/%d] kind=%04x m4=%08x m8=%08x f23=%d f24=%d name=\"%s\"\n", i,
                    rn, *(unsigned short *) r, *(const unsigned *) (r + 4),
                    *(const unsigned *) (r + 8), r[0x23], r[0x24],
                    (nm && (uintptr_t) nm > 0x1000) ? nm : "(null)");
            }
        }
        if (m && srcrow_tbl_for(m)) {
            srcrow_ensure(m);
            srcrow_fit(m);
            if (resume != MMR_NONE)
                srcrow_resume(m, resume);
        }
    }
}
static void (*g_o_src_knobup)(void *, int), (*g_o_src_knobdown)(void *, int);
static _Atomic unsigned g_kn_eaten; /* EP147 SOURCE-list turns swallowed (diagnostics) */
static int src_knob_owned(void) {
    return __atomic_load_n(&g_menu_focused, __ATOMIC_RELAXED) &&
           __atomic_load_n(&g_menu_open, __ATOMIC_RELAXED);
}
static void src_knobup_hook(void *self, int n) {
    if (src_knob_owned()) {
        __atomic_add_fetch(&g_kn_eaten, 1, __ATOMIC_RELAXED);
        return;
    }
    g_o_src_knobup(self, n);
}
static void src_knobdown_hook(void *self, int n) {
    if (src_knob_owned()) {
        __atomic_add_fetch(&g_kn_eaten, 1, __ATOMIC_RELAXED);
        return;
    }
    g_o_src_knobdown(self, n);
}
static void install_srcrow_hook(void) {
    /* Each slot through azm_patch_vtable: the original is stored before its slot points at
     * the hook (release), so a call already in flight never reaches a hook whose original is
     * still empty. The model slots are checked as one mapped range first, so the four writes
     * go in together (the rows hook alone would add a row the click and refresh hooks do
     * not know about). */
    void **vt = (void **) SRC_MODEL_VT;
    if (azprot_query((uintptr_t) &vt[2], (uintptr_t) &vt[13]) < 0) { /* slots 2..12 */
        dbg("srcrow: model vtable not mapped as one range -- NOT installed\n");
        return;
    }
    if (azm_patch_vtable((uintptr_t) &vt[2], (void *) src_rows_hook, (void **) &g_o_src_rows) !=
            1 ||
        azm_patch_vtable((uintptr_t) &vt[5], (void *) src_refresh_hook,
                         (void **) &g_o_src_refresh) != 1 ||
        azm_patch_vtable((uintptr_t) &vt[6], (void *) src_click_hook, (void **) &g_o_src_click) !=
            1 ||
        azm_patch_vtable((uintptr_t) &vt[12], (void *) src_selrow_hook,
                         (void **) &g_o_src_selrow) != 1) {
        dbg("srcrow: a model slot could not be written -- NOT installed\n");
        return;
    }
    void **vv = (void **) SRC_VIEW_VT;
    if (azm_patch_vtable((uintptr_t) &vv[12], (void *) src_setvis_hook,
                         (void **) &g_o_src_setvis) != 1)
        dbg("srcrow: view slot could not be written\n");
    g_srcrow_active = 1;
    /* Capture the juce::TableListBox when the SOURCE view is built (this+0x30).
     * azm_install_hook returns 1 on success and a negative code on decline. */
    {
        int rc = azm_install_hook(SRC_LISTWIDGET_CTOR, (void *) slw_ctor_hook,
                                  (void **) &g_orig_slw_ctor);
        g_slw_ctor_ok = (rc == 1) ? 1 : 0;
        if (rc != 1)
            dbg("srcrow: SourceListWidget ctor hook declined -- list sizing degraded\n");
    }
    /* Both publishers are hooked only to capture the sink object; suppression
     * happens at the sink. */
    {
        int rc1 =
            azm_install_hook(SRC_CTRL_PUBSEL, (void *) src_pubsel_hook, (void **) &g_o_src_pubsel);
        int rc2 = azm_install_hook(SRC_CTRL_PUBSEL2, (void *) src_pubsel2_hook,
                                   (void **) &g_o_src_pubsel2);
        dbg("srcrow: publisher hooks rc=%d/%d (sink is captured from whichever runs)\n", rc1, rc2);
        SRCROW_TRACE(1, "INSTALL: pubsel rc=%d pubsel2 rc=%d\n", rc1, rc2);
    }
    {
        int ru =
            azm_install_hook(SRC_CTRL_KNOBUP, (void *) src_knobup_hook, (void **) &g_o_src_knobup);
        int rd = azm_install_hook(SRC_CTRL_KNOBDOWN, (void *) src_knobdown_hook,
                                  (void **) &g_o_src_knobdown);
        dbg("srcrow: SOURCE knob up/down hooks rc=%d/%d\n", ru, rd);
        if (ru != 1 || rd != 1)
            dbg("srcrow: a SOURCE knob hook DECLINED -- the list can move under the Mod Menu\n");
    }
    /* EP147's add path sorts the table: pop/re-append around it. If this declines,
     * srcrow_ensure still moves a mis-sorted row back to the end on the next
     * getNumRows, at the cost of one odd refresh. */
    {
        int rc =
            azm_install_hook(SRC_MODEL_ADDROW, (void *) addrow_hook, (void **) &g_o_src_addrow);
        if (rc != 1)
            dbg("srcrow: addRow hook declined (%d) -- relying on ensure()\n", rc);
        rc = azm_install_hook(SRC_CTRL_ADD, (void *) ctrladd_hook, (void **) &g_o_src_ctrladd);
        if (rc != 1)
            dbg("srcrow: controller add hook declined (%d) -- MOD MENU may auto-select on device "
                "add\n",
                rc);
    }
    /* We grow EP147's table with realloc and EP147 frees it: both must be the
     * process allocator. Log which one we bound. */
    {
        void *ra = (void *) realloc, *fr = (void *) free;
        dbg("srcrow: realloc=%p free=%p (%s)\n", ra, fr,
            ((uintptr_t) ra >= 0x400000 && (uintptr_t) ra < 0x4000000) ? "inside EP147 image"
                                                                       : "libc");
        SRCROW_TRACE(1, "INSTALL: realloc=%p free=%p ctor_ok=%d\n", ra, fr, g_slw_ctor_ok);
    }
    dbg("srcrow: SourceTableModel vtable slots 2/5/6/12 + SourceSelectView::setVisible swapped; "
        "addRow inline\n");
}
