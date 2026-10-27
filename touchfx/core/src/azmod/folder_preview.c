/* SPDX-License-Identifier: GPL-3.0-only */
/* folder_preview.c: BROWSE, fill the preview pane when the page it shows holds no tracks.
 *
 * On stock EP147 2.00 (1.30 is not affected), the right pane stays blank (no rows, no "EMPTY")
 * whenever the page it should preview holds no track row: a selected FOLDER in PLAYLIST, an
 * artist's albums right after entering the library, the KEY / DATE ADDED / FOLDER categories,
 * an empty folder or playlist. Entering the item and pressing BACK fills it.
 *
 * The browse pane is drawn every display tick from two gui::disp_data::BrowseDispList objects,
 * rewritten only when an event asks its gui::disp_data::BrowseDispDataCreator (event+0x68) to
 * publish (creator vt+0x20). Every browse action is a gui::browser_logic event; the ones below
 * publish once immediately (lower list cleared, waiting) and use the base UpdateEventBase
 * lower-list reply (vt+0x88), which stores the page in the browser data (+0x250) and requests
 * the extras (art, waveform) of every TRACK row on it. The publish then comes from the extras
 * replies (UpdateEventBase vt+0x60). A page with no track row requests nothing, nothing replies,
 * and the page is never published. BACK's own lower-list reply handler publishes directly,
 * which is why BACK fills.
 *
 * The hook does what EP147's other reply handlers do: after the base handler has stored the
 * page, if it holds no track row (so no extras reply will publish it), ask the event's own
 * creator to publish, once, inside the reply handler on the message thread.
 *
 * The events, each reproduced blank on 2.00 and filled by this hook:
 *   CurrentListRowSelectEvent  a tap on a left-list row
 *   CursorMoveEvent            the browse knob
 *   MediaChangeEvent           entering a library (SOURCE > USB > Local Library)
 *   RootCategorySwitchEvent    a category on the left edge (ARTIST, KEY, FOLDER, DATE ADDED, ...)
 *   TabChangeEvent             the PLAYLIST button's tab, first shown on its first row
 *   LowerListRowSelectEvent    a tap on a right-pane row (the view moves into it)
 * 15 more classes share the base handler and stay stock: none of them was seen leaving a blank
 * pane (some never request a lower page, the rest could not be exercised).
 *
 * Identity, checked at use: only those classes' own vtable slots are patched, all of them or
 * none, and only over the base handler; the event's vptr must be one of them; the creator's
 * vptr must be BrowseDispDataCreator's and its publish slot the expected function; the page's
 * record vector must be well formed. Anything else: nothing (EP147 behaves as stock).
 * Independent of every other mod. 2.00 only. Off: /home/root/settings/azmod-folderpreview.off.
 * Log: /tmp/azmod-folderpreview.log. */
#define FP_OFF_FLAG "/home/root/settings/azmod-folderpreview.off"
/* 2.00 vtables (gui::browser_logic::...Event), resolved by fp_vt(): 0 on 1.30 keeps the feature out */
static const struct {
    int v200;
    const char *what;
} FP_EVENTS[] = {
    {0x3161800, "tap"},          /* CurrentListRowSelectEvent */
    {0x3161db8, "knob"},         /* CursorMoveEvent */
    {0x3163ae8, "library"},      /* MediaChangeEvent */
    {0x3163ef8, "category"},     /* RootCategorySwitchEvent */
    {0x3164a20, "tab"},          /* TabChangeEvent */
    {0x3163400, "right pane"},   /* LowerListRowSelectEvent */
};
#define FP_NEVENTS ((int) (sizeof FP_EVENTS / sizeof FP_EVENTS[0]))
static uintptr_t fp_vt(int i) { return (uintptr_t) ep_off(0, FP_EVENTS[i].v200); }
#define FP_LOWER_REPLY 0x88 /* vt slot: the lower list's reply */
#define FP_LOWER_REPLY_ORIG \
    ((uintptr_t) ep_off(0, 0x2016070)) /* UpdateEventBase's lower-list reply, not overridden */
#define FP_CREATOR_VT \
    ((uintptr_t) ep_off(0, 0x317f748)) /* gui::disp_data::BrowseDispDataCreator */
#define FP_CREATOR_PUBLISH \
    ((uintptr_t) ep_off(0, 0x20b37f0)) /* its vt+0x20: bump the version, rebuild the lists */
#define FP_EV_BROWSERDATA 0x58 /* event: browser data (base handler: ldr [x0,#0x58]) */
#define FP_EV_CREATOR 0x68     /* event: BrowseDispDataCreator (every publish: ldr [xN,#0x68]) */
#define FP_BD_LOWER_PAGE 0x250 /* browser data: the lower list's page (base handler stores it) */
#define FP_REC 0x78            /* page record size on 2.00 */
#define FP_MAX_RECS 100000     /* a sane page; more means we are not looking at a page */
static void (*g_fp_orig_lower_reply)(void *, void *, uintptr_t, uintptr_t); /* the base handler */
static _Atomic int g_fp_on;
static _Atomic unsigned g_fp_pages, g_fp_publishes; /* counters for the log */

static void fp_log(const char *fmt, unsigned long a, unsigned long b, unsigned long c) {
    FILE *f = azlog_open("/tmp/azmod-folderpreview.log", "a");
    if (f) {
        fprintf(f, fmt, a, b, c);
        fclose(f);
    }
}
/* The lower page EP147 just stored. 1 = there is one and no row on it is a track, so no extras
 * reply will publish it (an empty folder too: stock shows "EMPTY" once published). 0 = it holds
 * a track, or it is not there (the base handler dropped it to ask again) or not readable. */
static int fp_page_without_tracks(uintptr_t ev, unsigned long *nrecs) {
    uintptr_t bd = 0, pg = 0, vb = 0, ve = 0, q = 0;
    *nrecs = 0;
    if (!own_q(ev + FP_EV_BROWSERDATA, &bd) || bd < 0x10000 || (bd & 7) ||
        !own_q(bd + FP_BD_LOWER_PAGE, &pg) || pg < 0x10000 || (pg & 7) ||
        !own_q(pg + 8, &vb) || !own_q(pg + 0x10, &ve) || ve < vb || (ve - vb) % FP_REC ||
        (ve - vb) / FP_REC > FP_MAX_RECS || (ve > vb && vb < 0x10000))
        return 0;
    *nrecs = (ve - vb) / FP_REC;
    for (uintptr_t r = vb; r < ve; r += FP_REC) {
        if (!own_q(r + 8, &q))
            return 0;
        unsigned char type = q & 0xff; /* EP147 asks extras for 'e' and 4 (FUN_0200ec28) */
        if (type == 'e' || type == 4)
            return 0;
    }
    return 1;
}
static int fp_event_index(uintptr_t vt) { /* -1: not one of ours */
    for (int i = 0; i < FP_NEVENTS; i++)
        if (vt == fp_vt(i))
            return i;
    return -1;
}
/* vt+0x88 of the classes above, on the message thread: EP147 first, then publish a page EP147
 * will never publish itself. The event is this call's argument; its browser data, page and
 * creator are its own fields, read during EP147's call (own_q); vtable slots through img_q.
 * Every slot held the same base handler, so one original serves all. */
static void fp_lower_reply_hook(void *event, void *page, uintptr_t a2, uintptr_t a3) {
    g_fp_orig_lower_reply(event, page, a2, a3);
    uintptr_t ev = (uintptr_t) event, vt = 0, cr = 0, cvt = 0, pub = 0;
    unsigned long n = 0;
    int k;
    if (!__atomic_load_n(&g_fp_on, __ATOMIC_RELAXED) || ev < 0x10000 || (ev & 7) ||
        !own_q(ev, &vt) || (k = fp_event_index(vt)) < 0)
        return;
    if (!fp_page_without_tracks(ev, &n))
        return; /* EP147 publishes it itself (extras replies), or there is no page */
    unsigned long pages = __atomic_add_fetch(&g_fp_pages, 1, __ATOMIC_RELAXED);
    if (!own_q(ev + FP_EV_CREATOR, &cr) || cr < 0x10000 || (cr & 7) || !own_q(cr, &cvt) ||
        cvt != FP_CREATOR_VT || !img_q(cvt + 0x20, &pub) || pub != FP_CREATOR_PUBLISH) {
        fp_log("page %lu (%lu rows): no creator (%lx): nothing\n", pages, n, cr);
        return;
    }
    ((void (*)(void *)) pub)((void *) cr);
    unsigned long p = __atomic_add_fetch(&g_fp_publishes, 1, __ATOMIC_RELAXED);
    FILE *f = azlog_open("/tmp/azmod-folderpreview.log", "a");
    if (f) {
        fprintf(f, "page %lu (%lu rows, no tracks, %s): published #%lu\n", pages, n,
                FP_EVENTS[k].what, p);
        fclose(f);
    }
}
static void install_folderpreview(void) {
    if (g_ep147_build != EPB_V200 || !fp_vt(0) || access(FP_OFF_FLAG, F_OK) == 0)
        return; /* 2.00 only */
    /* every slot must hold the base handler before any is touched: all or nothing */
    for (int i = 0; i < FP_NEVENTS; i++) {
        uintptr_t cur = 0;
        if (!img_q(fp_vt(i) + FP_LOWER_REPLY, &cur) || cur != FP_LOWER_REPLY_ORIG) {
            fp_log("install folder preview: slot %lu was %lx: stock\n", (unsigned long) i, cur, 0);
            return;
        }
    }
    /* the original first: a reply can arrive the moment the first slot is patched */
    g_fp_orig_lower_reply = (void (*)(void *, void *, uintptr_t, uintptr_t)) FP_LOWER_REPLY_ORIG;
    int done = 0;
    for (; done < FP_NEVENTS; done++) {
        void *orig = 0;
        if (azm_patch_vtable(fp_vt(done) + FP_LOWER_REPLY, (void *) fp_lower_reply_hook,
                             &orig) != 1 ||
            (uintptr_t) orig != FP_LOWER_REPLY_ORIG)
            break;
    }
    if (done < FP_NEVENTS) { /* undo what was patched, stay stock */
        for (int i = 0; i <= done && i < FP_NEVENTS; i++) {
            uintptr_t cur = 0;
            void *mine = 0;
            if (img_q(fp_vt(i) + FP_LOWER_REPLY, &cur) && cur == (uintptr_t) fp_lower_reply_hook)
                azm_patch_vtable(fp_vt(i) + FP_LOWER_REPLY, (void *) FP_LOWER_REPLY_ORIG, &mine);
        }
        fp_log("install folder preview: slot %lu refused: undone, stock\n", (unsigned long) done, 0, 0);
        return;
    }
    __atomic_store_n(&g_fp_on, 1, __ATOMIC_RELEASE);
    fp_log("install folder preview: %lu event classes\n", (unsigned long) FP_NEVENTS, 0, 0);
}
