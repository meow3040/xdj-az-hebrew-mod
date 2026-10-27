/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file track_list_rows.c
 * @brief Track-list row plumbing: TrackID-to-path table, touch-scroll row cache, shared paint slot.
 *
 * A fixed ring of TrackID -> file path pairs is filled by reqan_hook, a hook on
 * AnalyzeManager::requestAnalyze, on EP147's loader thread. It is memory only and
 * published per entry with a seqlock so azstem's flusher can read it without locks;
 * lookups rank matches by write ordinal so the newest path wins (azmod_trackid_path).
 * reqan_hook also logs the call to /tmp/azmod-anlz.log.
 *
 * The row cache buffers each browse-list row (meow::TouchableTableListBox::RowComp and
 * meow::TouchableListBox::Row) to an image through juce::Component::setBufferedToImage,
 * and memoizes identical refreshComponentForCell calls, but only while a touch-scroll
 * session lasts. Outside a session each row cache is invalidated before it paints, so
 * idle behavior matches stock. Row pointers are vtable-checked at every use. It is
 * installed when the 60 FPS UI mod is active in azmod.cfg, or when azmod-rowcache.on or
 * azmod-touchaccum.on is present. The file also provides the single install of the
 * StandardCachedComponentImage paint slot (shared with Day Mode) and of the paintCell
 * slot (shared with Playlist Editing and the stems mark).
 *
 * Entry points: azmod_trackid_path, install_rowcache, install_scci_hook,
 * install_paintcell_slot.
 */
/* Analyzer-call loggers for /tmp/azmod-anlz.log (unflagged, cheap). */
static void anlz_lograw(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    FILE *f = azlog_open("/tmp/azmod-anlz.log", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}
static void anlz_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *f = azlog_open("/tmp/azmod-anlz.log", "a");
    if (f) {
        fprintf(f, "[%6ld.%03ld] %s\n", ts.tv_sec % 100000, ts.tv_nsec / 1000000, buf);
        fclose(f);
    }
}
/* ---- TrackID -> path ---------------------------------------------------------
 * azstem needs the track's file name to find its stems, but EP147's per-deck
 * LoadedTrackInfo carries only the title and the 16-byte TrackID.
 * AnalyzeManager::requestAnalyze fires on every load with both the TrackID and the
 * full path, so this table maps one to the other.
 *
 * Memory only, on purpose: the 16 bytes name a library slot, not a stick, so after
 * a stick swap the same id can mean a different song. A persisted table would load
 * the wrong stems with full confidence.
 *
 * Written on EP147's loader thread, read by azstem's flusher. Each entry is a
 * seqlock (odd seq = write in flight); a reader that sees seq move re-reads, and an
 * entry it cannot read whole makes the answer unknown. */
#define TIDPATH_N 64
struct tidpath_e {
    _Atomic unsigned seq;
    unsigned ord;
    unsigned char id[16];
    char path[300];
};
static struct tidpath_e g_tidpath[TIDPATH_N];
static _Atomic unsigned g_tidpath_w;

static void tidpath_put(const unsigned char id16[16], const char *path) {
    /* `w` is the monotonic write ordinal, not the slot: the lookup ranks matches
     * by age, which the slot index cannot express once the ring wraps. */
    unsigned w = __atomic_fetch_add(&g_tidpath_w, 1, __ATOMIC_RELAXED);
    unsigned i = w % TIDPATH_N;
    struct tidpath_e *e = &g_tidpath[i];
    unsigned s0 = __atomic_load_n(&e->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&e->seq, s0 | 1u, __ATOMIC_RELAXED); /* odd: writing */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e->ord = w;
    memcpy(e->id, id16, 16);
    snprintf(e->path, sizeof e->path, "%s", path);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&e->seq, (s0 | 1u) + 1u, __ATOMIC_RELEASE); /* even: stable */
}

/* Exported for azstem (via dlsym). Returns 1 and fills `out` on a hit; 0 means
 * unknown, never a guess, never another track's path. */
int azmod_trackid_path(const unsigned char id16[16], char *out, size_t n);
int azmod_trackid_path(const unsigned char id16[16], char *out, size_t n) {
    if (!id16 || !out || !n)
        return 0;
    out[0] = 0;
    /* The newest matching write wins, not the lowest index. After a stick swap or
     * re-export the same id can name a different song, and only the write ordinal
     * tells the stale entry from the current one. requestAnalyze fires on every
     * load that changes a deck's TrackID, so a real load always leaves a newer
     * entry. A same-song reload may not fire it, which is harmless (the TrackID is
     * unchanged). Link/cloud/streaming sources are untested. */
    unsigned best_ord = 0;
    int have = 0;
    char best[300];
    for (int i = 0; i < TIDPATH_N; i++) {
        struct tidpath_e *e = &g_tidpath[i];
        int settled = 0;
        for (int try = 0; try < 3 && !settled; try++) {
            unsigned s1 = __atomic_load_n(&e->seq, __ATOMIC_ACQUIRE);
            if (s1 & 1u)
                continue; /* write in flight */
            /* The id compare, like the copy, counts only once seq is seen unchanged: a torn
             * id can mismatch the entry that is being written for this very track. */
            int match = memcmp(e->id, id16, 16) == 0;
            char tmp[300];
            unsigned ord = e->ord;
            tmp[0] = 0;
            if (match)
                snprintf(tmp, sizeof tmp, "%s", e->path);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&e->seq, __ATOMIC_RELAXED) != s1)
                continue; /* moved: re-read */
            settled = 1;
            if (!match || !tmp[0])
                break;
            if (!have || ord - best_ord < 0x80000000u) { /* wrap-safe "newer than" */
                best_ord = ord;
                have = 1;
                snprintf(best, sizeof best, "%s", tmp);
            }
        }
        /* Still being written after three looks: it may be this track's newest entry, and
         * an older one for the same id can name another song. Unknown; the caller asks
         * again on a later tick. */
        if (!settled)
            return 0;
    }
    if (!have)
        return 0;
    snprintf(out, n, "%s", best);
    return 1;
}

static long (*g_orig_reqan)(long, long, long, long);
/* AnalyzeManager::requestAnalyze(this, TrackID const&, juce::File const&,
 * AnalyzeRequesterID const&), manager vtable slot 2. Safe to hook: exactly four
 * args, no sret, not variadic. Logs the TrackID bytes, path, 16-byte requester
 * and the return value. */
static long reqan_hook(long self, long tid, long file, long req) {
    /* Fill the identity table first. The TrackID and the juce::File are this call's
     * arguments (const references EP147 holds for the call), so they are read with plain
     * loads (own_q / own_str). Stems depend on the table, so it accepts nothing it could not
     * validate: an aligned TrackID and an absolute path. */
    {
        unsigned char id16[16];
        char path[300];
        int have_id = 0;
        if (tid > 0x10000) {
            uintptr_t w0 = 0, w1 = 0;
            if (own_q((uintptr_t) tid, &w0) && own_q((uintptr_t) tid + 8, &w1)) {
                memcpy(id16, &w0, 8);
                memcpy(id16 + 8, &w1, 8);
                have_id = 1;
            }
        }
        if (have_id && file > 0x10000) {
            uintptr_t sp = 0; /* a juce::File is its path's juce::String: one char* */
            if (own_q((uintptr_t) file, &sp) && sp > 0x10000) {
                if (own_str(sp, path, (int) sizeof path)) {
                    int n = (int) strlen(path);
                    /* Publish only an absolute path; anything else is a guess. */
                    if (n > 1 && path[0] == '/')
                        tidpath_put(id16, path);
                }
            }
        }
    }
    anlz_log("[reqan 0xc85838] self=%lx tid=%lx file=%lx req=%lx", self, tid, file, req);
    if (tid > 0x10000) {
        unsigned char *p = (unsigned char *) tid;
        anlz_lograw(" tidb=");
        for (int i = 0; i < 16; i++)
            anlz_lograw("%02x", p[i]);
    }
    if (file > 0x10000) {
        long sp = *(long *) file; /* juce::String is one char* */
        anlz_lograw(" file0=%lx", sp);
        if (sp > 0x10000) { /* signed: also rules out the high (kernel, -errno) half */
            unsigned char *p = (unsigned char *) sp;
            char buf[288];
            int n = 0;
            while (n < 280 && p[n] && n >= 0) {
                buf[n] = (p[n] >= 32 && p[n] < 127) ? p[n] : '.';
                n++;
            }
            buf[n] = 0;
            anlz_lograw(" path=\"%s\"", buf);
        }
    }
    if (req > 0x10000) {
        unsigned char *p = (unsigned char *) req;
        anlz_lograw(" reqb=");
        for (int i = 0; i < 16; i++)
            anlz_lograw("%02x", p[i]);
    }
    anlz_lograw("\n");
    long r = g_orig_reqan(self, tid, file, req);
    anlz_log("[reqan ret] %lx", r);
    return r;
}
static void (*g_orig_paintcell)(void *, void *, int, int, int, int, int);
/* browse_fields.c: a column showing a library field, drawn by EP147's sub column painter (1 =
 * the cell was handled); unset without it */
static int (*g_tlr_field_paint)(void *model, void *g, int row, int col, int w, int h, int sel);
static int sm_reserve_w(void *model, int col, int w, int h); /* stem_marks.c */
/* The paintCell vtable slot is shared by Playlist Editing's drag view and the stems
 * mark. A slot can be patched only once, so whichever feature asks first installs
 * it and paintcell_hook serves both, each behind its own flag; neither depends on
 * the other being enabled. Returns 1 when installed. */
static int install_paintcell_slot(void) {
    static int rc = 0;
    if (rc == 0)
        rc = azm_patch_vtable(PAINTCELL_SLOT, (void *) paintcell_hook,
                              (void **) &g_orig_paintcell) == 1
                 ? 1
                 : -1;
    return rc;
}
static void paintcell_hook(void *model, void *g, int row, int col, int w, int h, int sel) {
    /* Same argument shape as the row painter: the last int is set on exactly the
     * selected row (w5 is 1 on every row, measured). */
    if (drag_is_widget((uintptr_t) model - 0x30)) {
        /* the plain selection, also while committing (amber is g_row_grabbed's) */
        sel = row == __atomic_load_n(&g_drag_target, __ATOMIC_RELAXED);
        row = drag_remap(row);
    }
    g_in_grabcell = sel ? 1 : 0;
    /* the track list, for "is a track row selected" (vtable-checked at use) */
    drag_capture_widget(
        model);
    /* the stems mark's column: EP147 lays its text out clear of the mark (its own Key text is
     * short and left-aligned, already clear of it) */
    int rs = sm_reserve_w(model, col, w, h);
    if (!g_tlr_field_paint || !g_tlr_field_paint(model, g, row, col, w - rs, h, sel))
        g_orig_paintcell(model, g, row, col, col == ep_off(7, 8) ? w : w - rs, h, sel);
    g_in_grabcell = 0;
    if (g_sm_on && model)
        sm_after_paint((char *) model - 0x30, g, row, col, w, h); /* model = widget+0x30 */
}

/* ==== Row cache: buffered list rows for smooth touch scrolling ===============
 * Without it a touch-scrolled list reaches only ~25 frames/s on the hardware: every
 * visible row redoes image scaling, background fills and glyph rendering on every frame
 * (~28 ms, over the 16.9 ms panel frame) although a scroll moves rows without
 * changing them.
 *
 * Three parts, all EP147's/JUCE's own machinery:
 *  1. Component::setBufferedToImage (0x243d200; cache at +0x70): a row renders
 *     once into an image and a scroll blits it.
 *  2. A memo on the model's refreshComponentForCell: EP147 re-sets every cell on
 *     every scroll frame, which would invalidate every cache; an identical refresh
 *     during a scroll is skipped.
 *  3. Scope: both apply only during a touch-scroll session (a real drag on a
 *     touchable list within RC_TOUCH_MS). Outside one, a row's cache is
 *     invalidated before every paint so idle is exactly stock, and a scroll ends
 *     with one full refresh (the settle).
 * Measured: 11-15 row repaints per scroll frame drop to 1.4-2.1. Every row/cell
 * pointer is vtable-checked at use; anything unexpected falls back to stock. */
#define ROWCACHE_SETBUF EP_243d200 /* juce::Component::setBufferedToImage(bool) */
#define ROWCOMP_VT EP_2e605b8      /* meow::TouchableTableListBox::RowComp */
#define ROW_VT EP_2e5fe90          /* meow::TouchableListBox::Row */
#define ROWCOMP_PAINT_ORIG EP_21ea450
#define ROW_PAINT_ORIG EP_21ea670
static int g_rowcache_on;
static void (*g_orig_rowcomp_paint)(void *, void *);
static void (*g_orig_row_paint)(void *, void *);
/* Rows seen painting. Each slot is a wref (ep_mem.h): juce::Component's destructor clears it
 * (Row and RowComp derive from juce::Component at offset 0, RTTI), so on the message thread a
 * non-zero slot is a live row. Every use still checks its vtable. */
#define RC_ROWS_N 1024
#define RC_REPAINT EP_2488570 /* juce::Component::repaint() */
#ifndef SCCI_VT
#define SCCI_VT EP_314cae0 /* juce::StandardCachedComponentImage */
#endif
static _Atomic uintptr_t g_rc_rows[RC_ROWS_N];
static uintptr_t g_rc_rows_vt[RC_ROWS_N];
static int g_rc_scrolling; /* caches are on only while a scroll lasts */
/* Last time a finger was dragging a touchable list (set by touch_accum.c's hook on
 * the drag handler 0x21e1090). A scroll session may only begin within RC_TOUCH_MS
 * of it (covering the fling after release), so a knob scroll, a playlist edit or
 * any refresh EP147 makes on its own is never skipped. */
static _Atomic long long g_rc_touch_ms;
#define RC_TOUCH_MS 3000
static int ta_ensure_hook(void); /* touch_accum.c: the shared drag-handler hook */
static long g_rc_stat[8]; /* diag: [0] updateContent [1] on [2] off [3] invalidate [4] settles [5]
                             rows enabled */
/* Registers a row. Called from the row's own paint (message thread), so `self` is
 * the live object JUCE is painting: plain loads, no syscalls, since this runs for
 * every row paint. */
static void rowcache_enable(void *self, uintptr_t want_vt) {
    if (!g_rowcache_on || ((uintptr_t) self & 7) || *(uintptr_t *) self != want_vt)
        return;
    /* a row first seen during a scroll gets its cache now, or a long flick would
     * leave every newly exposed row uncached */
    if (g_rc_scrolling && !*(uintptr_t *) ((uintptr_t) self + 0x70)) {
        ((void (*)(void *, int)) ROWCACHE_SETBUF)(self, 1);
        g_rc_stat[5]++;
    }
    unsigned h = (unsigned) (((uintptr_t) self >> 4) * 2654435761u) % RC_ROWS_N;
    for (unsigned k = 0; k < 8; k++) {
        unsigned i = (h + k) % RC_ROWS_N;
        uintptr_t cur = wref_get(&g_rc_rows[i]);
        if (cur == (uintptr_t) self)
            return;
        if (!cur) {
            g_rc_rows_vt[i] = want_vt;
            wref_set(&g_rc_rows[i], (uintptr_t) self);
            return;
        }
    }
    g_rc_rows_vt[h] = want_vt; /* full: replace */
    wref_set(&g_rc_rows[h], (uintptr_t) self);
}

/* Idle is always fresh. JUCE invalidates a cache only when that component or a
 * child repaints, not an ancestor; EP147 refreshes BPM and Key by repainting the
 * list, so a cached row would keep stale values. Outside a scroll session a row's
 * cache is therefore invalidated right before it paints, so nothing the registry
 * missed can stay stale. */
static void (*g_orig_scci_paint)(void *, void *);
static void scci_paint_hook(void *self, void *g) {
    dm_scci_pre(self); /* Day Mode: a cache painted before the last switch re-renders (day_mode.c) */
    /* self is the live cache JUCE is painting and [self+0x28] its owner (the
     * component being painted): plain loads, as in the row hooks. */
    if (g_rowcache_on && !g_rc_scrolling && *(uintptr_t *) self == SCCI_VT) {
        uintptr_t owner = *(uintptr_t *) ((uintptr_t) self + 0x28);
        if (owner && !(owner & 7)) {
            uintptr_t ovt = *(uintptr_t *) owner;
            if (ovt == ROWCOMP_VT || ovt == ROW_VT)
                ((int (*)(void *))(*(void ***) self)[3])(self); /* invalidateAll (slot 3) */
        }
    }
    g_orig_scci_paint(self, g);
}
/* The single install of the StandardCachedComponentImage::paint slot, shared by
 * the row cache and Day Mode. The slot can be patched once; each feature's work in
 * scci_paint_hook is gated on its own flag. Returns 1 when the slot is ours. */
static int install_scci_hook(void) {
    static int done = 0, ok = 0;
    if (done)
        return ok;
    done = 1;
    uintptr_t sp = 0;
    if (img_q(SCCI_VT + 0x10, &sp) && sp == EP_24f5bc0)
        ok = azm_patch_vtable(SCCI_VT + 0x10, (void *) scci_paint_hook,
                              (void **) &g_orig_scci_paint) == 1;
    return ok;
}
/* op 1: cache on, 0: cache off, 2: invalidate (repaint the row itself) */
static void rowcache_all(int op) {
    g_rc_stat[op == 1 ? 1 : op == 0 ? 2
                                    : 3]++;
    if (!on_msg_thread())
        return; /* rows live and die on the message thread: nowhere else may touch them */
    for (int i = 0; i < RC_ROWS_N; i++) {
        uintptr_t r = wref_get(&g_rc_rows[i]), vt = 0, cache = 0;
        if (!r)
            continue; /* never seen, or destroyed since */
        if (!own_q(r, &vt) || vt != g_rc_rows_vt[i] || !own_q(r + 0x70, &cache)) {
            wref_set(&g_rc_rows[i], 0);
            continue;
        }
        if (op == 2) {
            if (cache)
                ((void (*)(uintptr_t)) RC_REPAINT)(r);
        } else if (op == 1) {
            if (!cache) {
                ((void (*)(void *, int)) ROWCACHE_SETBUF)((void *) r, 1);
                g_rc_stat[5]++;
            }
        } else if (cache) {
            uintptr_t cvt = 0;
            /* switch off only a cache azmod made (JUCE's StandardCachedComponentImage) */
            if (own_q(cache, &cvt) && cvt == SCCI_VT) /* the live row's own cache object */
                ((void (*)(void *, int)) ROWCACHE_SETBUF)((void *) r, 0);
        }
    }
}
static _Atomic long g_rc_paints; /* real row paints (cache misses once the cache is on) */
static void rowcomp_paint_hook(void *self, void *g) {
    g_rc_paints++;
    g_orig_rowcomp_paint(self, g);
    rowcache_enable(self, ROWCOMP_VT);
}
static void row_paint_hook(void *self, void *g) {
    g_rc_paints++;
    g_orig_row_paint(self, g);
    rowcache_enable(self, ROW_VT);
}
static void rowcache_tick(void) {
    static long last;
    static int armed = -1;
    if (armed < 0)
        armed = AZ_PROBE_ON("/home/root/settings/azmod-rowcache.count");
    if (!armed)
        return;
    long n = g_rc_paints;
    FILE *f = azlog_open("/tmp/azmod-rowcache.cnt", "w");
    if (f) {
        fprintf(f,
                "%ld upd=%ld on=%ld off=%ld inval=%ld settle=%ld enabled=%ld hits=%ld misses=%ld\n",
                n - last, g_rc_stat[0], g_rc_stat[1], g_rc_stat[2], g_rc_stat[3], g_rc_stat[4],
                g_rc_stat[5], g_rc_stat[6], g_rc_stat[7]);
        fclose(f);
    }
    last = n;
}

/* ---- Refresh memo ------------------------------------------------------------
 * On every scroll frame the list re-runs refreshComponentForCell for every visible
 * cell, and EP147's cell setter (0x1badae8) re-sets the text and re-lays-out the
 * label, which repaints the cell and invalidates the row's cache. So a refresh for
 * the same component, row, column and selection, with no ListBox::updateContent
 * since (it bumps a generation), returns the component untouched. Entries expire
 * after RC_MEMO_MS so late data (artwork, preview) still lands a few times a
 * second. Never active during a playlist drag (rows are remapped). */
#define RC_MEMO_N 512
#define RC_MEMO_MS 200
static struct {
    void *comp;
    int row, col, sel;
    unsigned gen;
    long long t;
} g_rcm[RC_MEMO_N];
static _Atomic unsigned g_rc_gen = 1;
static _Atomic long long g_rc_last_hit; /* last time the memo skipped a refresh */
static void drag_update_content(void);
/* Settle: a data change whose only refresh was skipped by the memo would stay
 * unapplied. Once scrolling stops (RC_SETTLE_MS without a skip), refresh the list
 * once via EP147's ListBox::updateContent. Message thread. */
#define RC_SETTLE_MS 300
static void rowcache_settle_tick(void) {
    long long t = __atomic_load_n(&g_rc_last_hit, __ATOMIC_RELAXED);
    if (!t || drag_now_ms() - t < RC_SETTLE_MS)
        return;
    __atomic_store_n(&g_rc_last_hit, 0, __ATOMIC_RELAXED);
    g_rc_stat[4]++;
    if (g_rc_scrolling) {
        g_rc_scrolling = 0;
        rowcache_all(0);
    } /* idle = stock behavior */
    if (!__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        drag_update_content();
}
static void *(*g_orig_refresh_memo)(void *, int, int, int, void *);
static void (*g_orig_lb_update)(void *);
static void lb_update_hook(void *lb) {
    g_rc_stat[0]++;
    __atomic_fetch_add(&g_rc_gen, 1, __ATOMIC_RELAXED);
    g_orig_lb_update(lb);
    if (g_rc_scrolling)
        rowcache_all(2);
} /* new data: no cached row may survive it */
static void *refresh_memo_hook(void *model, int row, int col, int sel, void *existing) {
    if (!existing || !g_rowcache_on || __atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return g_orig_refresh_memo(model, row, col, sel, existing);
    drag_capture_widget(model); /* the settle refresh needs the list (vtable-checked there) */
    unsigned h = (unsigned) (((uintptr_t) existing >> 4) * 2654435761u) % RC_MEMO_N;
    unsigned gen = __atomic_load_n(&g_rc_gen, __ATOMIC_RELAXED);
    long long now = drag_now_ms();
    long long tt = __atomic_load_n(&g_rc_touch_ms, __ATOMIC_RELAXED);
    if (tt && now - tt < RC_TOUCH_MS && g_rcm[h].comp == existing && g_rcm[h].row == row &&
        g_rcm[h].col == col && g_rcm[h].sel == !!sel && g_rcm[h].gen == gen &&
        now - g_rcm[h].t < RC_MEMO_MS) {
        g_rc_stat[6]++;
        __atomic_store_n(&g_rc_last_hit, now, __ATOMIC_RELAXED); /* rtick settles it later */
        if (!g_rc_scrolling) {
            g_rc_scrolling = 1;
            rowcache_all(1);
        } /* a scroll began: cache the rows */
        return existing;
    } /* nothing changed: leave it alone */
    g_rc_stat[7]++;
    void *r = g_orig_refresh_memo(model, row, col, sel, existing);
    if (r == existing) {
        g_rcm[h].comp = existing;
        g_rcm[h].row = row;
        g_rcm[h].col = col;
        g_rcm[h].sel = !!sel;
        g_rcm[h].gen = gen;
        g_rcm[h].t = now;
    } else
        g_rcm[h].comp = 0;
    return r;
}
/* Whether 60 FPS UI is on at EP147 start, for the parts patched at init (row cache,
 * slow-drag accumulate). azmod_init loads the settings (load_settings_early) before any hook
 * installs, so this is the row's value as the menu will show it. The live parts gate on the
 * value itself. */
static int g_ui60ui_installed = 0;
static int ui60ui_at_start(void) {
    return az_on(AZF_FPS60_UI);
}
static void install_rowcache(void) {
    g_ui60ui_installed = ui60ui_at_start();
    g_rowcache_on = g_ui60ui_installed;
    /* the rows are remembered only while their destructor can forget them */
    if (g_rowcache_on && !wref_track(g_rc_rows, RC_ROWS_N, 0))
        g_rowcache_on = 0;
    int count = AZ_PROBE_ON("/home/root/settings/azmod-rowcache.count");
    if (!g_rowcache_on && !count)
        return;
    /* Inline hook, not the vtable slot: RowComp::paint is not dispatched through
     * the slot, so a slot patch never sees it. */
    int r1 = azm_install_hook(ROWCOMP_PAINT_ORIG, (void *) rowcomp_paint_hook,
                              (void **) &g_orig_rowcomp_paint);
    /* Row::paint's prologue holds a pc-relative instruction, so azm_install_hook
     * declines it; its vtable slot is a plain data pointer and patches cleanly. */
    uintptr_t b = 0;
    int r2 = -8;
    if (img_q(ROW_VT + 0xd0, &b) && b == ROW_PAINT_ORIG)
        r2 = azm_patch_vtable(ROW_VT + 0xd0, (void *) row_paint_hook, (void **) &g_orig_row_paint);
    if (r1 != 1)
        g_rowcache_on = 0; /* RowComp is the one the browse lists use */
    if (g_rowcache_on && !ta_ensure_hook())
        g_rowcache_on = 0; /* no drag signal: no sessions */
    int r5 = -9;
    if (g_rowcache_on)
        r5 = install_scci_hook();
    if (r5 != 1)
        g_rowcache_on = 0; /* no idle guard, no cache: correctness first */
    /* the memo chains in front of the drag view's refreshcell_hook when that is
     * installed (install_reorder runs first) */
    int r3 = g_rowcache_on ? azm_patch_vtable(REFRESHCELL_SLOT, (void *) refresh_memo_hook,
                                              (void **) &g_orig_refresh_memo)
                           : -9;
    int r4 = r3 == 1 ? azm_install_hook(LISTBOX_UPDATECONTENT, (void *) lb_update_hook,
                                        (void **) &g_orig_lb_update)
                     : -9;
    if (r3 != 1 || r4 != 1) {
        g_rowcache_on = 0;
    }
    FILE *f = azlog_open("/tmp/azmod-rowcache.log", "w");
    if (f) {
        fprintf(f, "rowcache install: rowcomp=%d row=%d guard=%d memo=%d updatecontent=%d -> %s\n",
                r1, r2, r5, r3, r4, g_rowcache_on ? "ON" : "OFF");
        fclose(f);
    }
}
static int install_hier_refresh_slot(void);
/* EP147's browse-knob press handler for every browse list, FUN_019ac3c8(self,
 * int *ev): ev 0 = knob down, ev 1 = released before the threshold (the click),
 * ev 2 = held 500 ms (EP147's own timer) -> CursorEventFacade::enterJumpMode, the
 * quick scroll. After ev 2 the release sends nothing, so a hold never becomes a
 * click. Playlist Editing takes ev 2 when the hold is ours:
 *   Browse knob grab: PRESS          plain hold = ours, SHIFT + hold = stock quick scroll
 *   Browse knob grab: SHIFT + PRESS  SHIFT + hold = ours, plain hold = stock quick scroll
 * Ours: arm the grab on a row a drop can commit to (drag_try_grab proves it again on
 * the next tick) and never enter quick scroll, movable row or not. Off = stock. */
#define KNOBEV_FN EP_19ac3c8
/* EP147's browse-knob turn handler, slot +0x198 of BrowseView / BrowseListView /
 * TagListView: FUN_019ac568(view, unused, delta) -> cursor (view+0x330) slot +0x18.
 * While a row is held, and until its pin has moved EP147's cursor, the detents
 * belong to the drag (read() turns them into the target) and never reach EP147.
 * The view is remembered only from calls made during this drag and re-validated by
 * vtable before knob_cursor_to uses it. */
#define KNOBTURN_FN EP_19ac568
