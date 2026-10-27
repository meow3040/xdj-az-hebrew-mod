/* SPDX-License-Identifier: GPL-3.0-only */
/* ==== banner.c: our text in EP147's own caution UI =======================
 * #included by azmod.c.
 *
 *   int az_banner_strip(const char *text, int dur_ms)          the gray strip across the top
 *   int az_banner_box(const char *l1, const char *l2, int dur_ms)  the centered box, 1-2 lines
 *
 * AZ_BANNER_HOLD, BN_NONE.. and the prototypes are declared in azmod.c, above
 * the playlist code that uses them (this file is #included near the end).
 *
 * Message thread only (call from rtick_hook / the calltimers tick). Return 0
 * when posted, <0 and nothing done when any link fails validation. dur_ms <= 0
 * keeps the stock duration (strip 3000, box 5000); AZ_BANNER_HOLD keeps the strip
 * up until something replaces it or az_banner_strip_clear() takes it down.
 *
 *   int az_banner_strip_state(void)  what the strip shows now: BN_NONE, BN_OURS,
 *                                    BN_OURS_HELD, BN_STOCK; <0 = unknown
 *   int az_banner_strip_clear(void)  take our strip down (never a stock one)
 *
 * How EP147 does it: a caution is posted by numeric id and its text lives in a
 * static Definition the id indexes, e.g. LibraryEditHandler's "COMPLETE! / NEW
 * PLAYLIST saved." is request([handler+0x50], 22). Two managers, both owned by
 * gui::GuiObjectManager:
 *   simple::CautionManager   gom+0x30 vt 0x2dfbe08        the strip
 *     request 0x1960f70(mgr, idx 0..36), Definition* table 0x29c3b38,
 *     Definition 0x20 = {int prio; int dur_ms; juce::String s[3]}
 *   browser::CautionManager  gom+0x28 vt 0x2dfbde0        the box
 *     request 0x195cc98(mgr, id 1..66; 3 is special), table 0x29c2b00,
 *     Definition 0x1c0 = {int prio; int dur_ms; int kind; ...; 18 per-language
 *     line lists at +0x10 stride 0x18: {String* elems; int alloc @+8; int used @+0x10}}
 * duration INT_MIN = stays until dismissed. The tables are r-x rodata; the
 * Definitions are writable .data.
 *
 * Our text: a private copy of a stock Definition whose strings point at our own
 * StringHolders ({int refCount; size_t alloc; char text[]}, refCount pinned at
 * 0x3fffffff like JUCE's own empty string at 0x312f250, so never freed), swapped
 * into the table for one synchronous request() and swapped straight back. The
 * Caution keeps the Definition POINTER, so each copy must outlive its display:
 * a static ring of BN_RING copies per kind. More than BN_RING posts in one display
 * period would reuse a copy still queued: wrong text, never a dangling pointer.
 *
 * The manager is reached by construction, never a raw cached pointer:
 *   EP147Application (vt 0x2968548) slot +0xd0 = 0x18d8d18 runs once at start-up
 *   and builds ui::UiLayerObjects at app+0x110 (vt 0x29676f0), whose ctor
 *   0x20eb3b0 builds gui::GuiObjectManager (vt 0x29676d0, ctor 0x20110a0) at +0x10.
 * The ctors cannot be inline-hooked (adrp in their first 4 instructions), so we
 * vtable-patch the app's +0xd0 slot from azmod's ELF constructor (before main)
 * and record `this`. Every link is vtable-checked at use.
 *
 * Known side effect: while any strip is up, including a stock one, the AZ MODS
 * icon on the SOURCE MOD MENU row is hidden. */

#define BN_APP_VT EP_2968548                /* EP147Application */
#define BN_APP_INIT_SLOT (BN_APP_VT + 0xd0) /* -> 0x18d8d18 */
#define BN_APP_INIT_FN EP_18d8d18
#define BN_ULO_VT EP_29676f0 /* ui::UiLayerObjects */
#define BN_GOM_VT EP_29676d0 /* gui::GuiObjectManager */
#define BN_BOX_VT EP_2dfbde0 /* gui::caution::browser::CautionManager */
#define BN_BOX_REQUEST EP_195cc98
#define BN_BOX_TABLE EP_29c2b00 /* [id-1] */
#define BN_BOX_ID 22            /* "COMPLETE! / NEW PLAYLIST saved.": timed, 5 s */
#define BN_BOX_DEF_SZ 0x1c0
#define BN_BOX_NLANG 18
#define BN_STRIP_VT EP_2dfbe08 /* gui::caution::simple::CautionManager */
#define BN_STRIP_REQUEST EP_1960f70
#define BN_STRIP_TABLE EP_29c3b38 /* [idx] */
#define BN_STRIP_IDX 13           /* "USB FULL": timed, 3 s */
#define BN_STRIP_DEF_SZ 0x20
#define BN_RING 8 /* a held strip stays referenced while showing */
#define BN_STRIP_EXPIRE \
    EP_1891d10 /* its juce::Timer callback: stop, tell listeners, drop current */
#define BN_FLAG "/home/root/settings/azmod-banner.on"
#define BN_TEXT "/tmp/azmod-banner.txt"
#define BN_LOG "/tmp/azmod-banner.log"

static void *(*g_orig_app_init)(void *, void *);
static _Atomic uintptr_t g_bn_app;
static int g_bn_harness; /* azmod-banner.on: file-driven test harness + log */

static void *app_init_hook(void *self, void *a1) {
    __atomic_store_n(&g_bn_app, (uintptr_t) self, __ATOMIC_RELAXED);
    return g_orig_app_init(self, a1);
}

static void bn_log(const char *fmt, ...) {
    if (!g_bn_harness)
        return;
    FILE *f = azlog_open(BN_LOG, "a"); /* open/close per line: never buffered */
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* azmod's ELF constructor: before EP147's main, so before initialize. Cheap and
 * inert (it only records `this`), so it is unconditional. */
static void install_banner(void) {
    g_bn_harness = AZ_PROBE_ON(BN_FLAG);
    uintptr_t cur = 0;
    if (!img_q(BN_APP_INIT_SLOT, &cur) || cur != BN_APP_INIT_FN) {
        bn_log("install: app slot holds %lx, not %lx -- banner off\n", (unsigned long) cur,
               (unsigned long) BN_APP_INIT_FN);
        return;
    }
    int rc = azm_patch_vtable(BN_APP_INIT_SLOT, (void *) app_init_hook, (void **) &g_orig_app_init);
    bn_log("install app-init slot rc=%d orig=%lx\n", rc,
           (unsigned long) (uintptr_t) g_orig_app_init);
}

/* app -> UiLayerObjects -> GuiObjectManager -> manager at gom+off, each vtable-checked.
 * The application object and these managers live for the whole process and are used on
 * the message thread, so plain loads there are safe; any other thread gets 0 (unknown). */
static uintptr_t bn_manager(int off, uintptr_t want) {
    uintptr_t app = __atomic_load_n(&g_bn_app, __ATOMIC_RELAXED), ulo = 0, gom = 0, mgr = 0, vt = 0;
    if (!on_msg_thread()) {
        bn_log("EXIT: not the message thread\n");
        return 0;
    }
    if (!app) {
        bn_log("EXIT: app not captured\n");
        return 0;
    }
    if (!own_q(app, &vt) || vt != BN_APP_VT) {
        bn_log("EXIT: app vt %lx\n", (unsigned long) vt);
        return 0;
    }
    if (!own_q(app + 0x110, &ulo) || !ulo || (ulo & 7) || !own_q(ulo, &vt) || vt != BN_ULO_VT) {
        bn_log("EXIT: UiLayerObjects %lx vt %lx\n", (unsigned long) ulo, (unsigned long) vt);
        return 0;
    }
    if (!own_q(ulo + 0x10, &gom) || !gom || (gom & 7) || !own_q(gom, &vt) || vt != BN_GOM_VT) {
        bn_log("EXIT: GuiObjectManager %lx vt %lx\n", (unsigned long) gom, (unsigned long) vt);
        return 0;
    }
    if (!own_q(gom + (uintptr_t) off, &mgr) || !mgr || (mgr & 7) || !own_q(mgr, &vt) ||
        vt != want) {
        bn_log("EXIT: manager +%x %lx vt %lx\n", off, (unsigned long) mgr, (unsigned long) vt);
        return 0;
    }
    return mgr;
}

struct bn_holder {
    int32_t ref;
    int32_t pad;
    uint64_t alloc;
    char text[160];
};
static void bn_set(struct bn_holder *h, const char *s, size_t len) {
    if (len >= sizeof h->text)
        len = sizeof h->text - 1;
    h->ref = 0x3fffffff;
    h->alloc = sizeof h->text;
    memcpy(h->text, s, len);
    h->text[len] = 0;
}

/* Swap table[slot] to `def`, request, swap back. */
static int bn_request(uintptr_t table_slot, void *def, uintptr_t fn, uintptr_t mgr, int arg) {
    void *orig = 0, *dummy = 0;
    if (azm_patch_vtable(table_slot, def, &orig) != 1) {
        bn_log("EXIT: table write\n");
        return -3;
    }
    ((void (*)(uintptr_t, int)) fn)(mgr, arg);
    azm_patch_vtable(table_slot, orig, &dummy);
    return 0;
}

static unsigned char g_bn_strip_def[BN_RING][BN_STRIP_DEF_SZ] __attribute__((aligned(16)));
static int az_banner_strip(const char *text, int dur_ms) {
    static struct bn_holder line[BN_RING];
    unsigned char (*def)[BN_STRIP_DEF_SZ] = g_bn_strip_def;
    static unsigned ring;
    if (!text || !*text)
        return -1;
    uintptr_t mgr = bn_manager(0x30, BN_STRIP_VT), slot = BN_STRIP_TABLE + BN_STRIP_IDX * 8,
              stock = 0;
    if (!mgr)
        return -2;
    if (!img_q(slot, &stock) || !stock) {
        bn_log("EXIT: strip table\n");
        return -2;
    }
    /* EP147 keeps one current caution at [mgr+0x30] (+8 = its Definition*), and
     * 0x1960f70 replaces it only for a strictly higher priority; otherwise the
     * request is dropped, not queued. So when the strip showing is ours, outrank
     * it by one and our newest message wins (a held line is ended by the next
     * result). A stock caution is never outranked: ours keeps stock priority and
     * is dropped, exactly as a stock one would be. */
    int32_t prio = *(int32_t *) stock;
    {
        uintptr_t cur = 0, cdef = 0, cp = 0;
        if (own_q(mgr + 0x30, &cur) && cur && !(cur & 7) && own_q(cur + 8, &cdef))
            for (int i = 0; i < BN_RING; i++)
                if (cdef == (uintptr_t) def[i] && own_q(cdef, &cp)) {
                    int32_t cprio = (int32_t) (uint32_t) cp;
                    if (cprio >= prio && cprio < 0x7fffff00)
                        prio = cprio + 1;
                    break;
                }
    }
    unsigned r = ring++ % BN_RING;
    memcpy(def[r], (void *) stock, BN_STRIP_DEF_SZ); /* priority, duration, empty s[1..2] */
    *(int32_t *) def[r] = prio;
    bn_set(&line[r], text, strlen(text));
    *(uintptr_t *) (def[r] + 8) = (uintptr_t) line[r].text;
    if (dur_ms > 0 || dur_ms == AZ_BANNER_HOLD)
        *(int32_t *) (def[r] + 4) = dur_ms;
    int rc = bn_request(slot, def[r], BN_STRIP_REQUEST, mgr, BN_STRIP_IDX);
    bn_log("STRIP rc=%d \"%s\" dur=%d prio=%d\n", rc, text, *(int32_t *) (def[r] + 4), prio);
    return rc;
}

/* The strip on screen now, read from the manager's current caution [mgr+0x30]
 * (+8 = its Definition*): ours when that is one of our ring copies. */
static int az_banner_strip_state(void) {
    uintptr_t mgr = bn_manager(0x30, BN_STRIP_VT), cur = 0, cdef = 0, cp = 0;
    if (!mgr || !own_q(mgr + 0x30, &cur) || (cur & 7))
        return -1;
    if (!cur)
        return BN_NONE;
    if (!own_q(cur + 8, &cdef))
        return -1;
    for (int i = 0; i < BN_RING; i++)
        if (cdef == (uintptr_t) g_bn_strip_def[i])
            return own_q(cdef, &cp) && (int32_t) (cp >> 32) == AZ_BANNER_HOLD ? BN_OURS_HELD
                                                                              : BN_OURS;
    return BN_STOCK;
}
/* Take our strip down the way its own timeout does (0x1891d10: stopTimer if
 * running, every listener's dismiss slot, then [mgr+0x30] = 0). A stock
 * caution is left alone. */
static int az_banner_strip_clear(void) {
    int st = az_banner_strip_state();
    if (st != BN_OURS && st != BN_OURS_HELD)
        return -1;
    ((void (*)(uintptr_t)) BN_STRIP_EXPIRE)(bn_manager(0x30, BN_STRIP_VT));
    bn_log("STRIP cleared (was %s)\n", st == BN_OURS_HELD ? "held" : "timed");
    return 0;
}

static int az_banner_box(const char *l1, const char *l2, int dur_ms) {
    static struct bn_holder line[BN_RING][2];
    static uintptr_t elems[BN_RING][2];
    static unsigned char def[BN_RING][BN_BOX_DEF_SZ] __attribute__((aligned(16)));
    static unsigned ring;
    if (!l1 || !*l1)
        return -1;
    uintptr_t mgr = bn_manager(0x28, BN_BOX_VT), slot = BN_BOX_TABLE + (BN_BOX_ID - 1) * 8,
              stock = 0;
    if (!mgr)
        return -2;
    if (!img_q(slot, &stock) || !stock) {
        bn_log("EXIT: box table\n");
        return -2;
    }
    unsigned r = ring++ % BN_RING;
    int n = (l2 && *l2) ? 2 : 1;
    memcpy(def[r], (void *) stock, BN_BOX_DEF_SZ); /* priority, duration, kind */
    bn_set(&line[r][0], l1, strlen(l1));
    elems[r][0] = (uintptr_t) line[r][0].text;
    if (n == 2) {
        bn_set(&line[r][1], l2, strlen(l2));
        elems[r][1] = (uintptr_t) line[r][1].text;
    }
    for (int l = 0; l < BN_BOX_NLANG; l++) { /* every UI language shows ours */
        unsigned char *rec = def[r] + 0x10 + l * 0x18;
        *(uintptr_t *) rec = (uintptr_t) elems[r];
        *(int32_t *) (rec + 8) = 2;
        *(int32_t *) (rec + 0x10) = n;
    }
    if (dur_ms > 0)
        *(int32_t *) (def[r] + 4) = dur_ms;
    int rc = bn_request(slot, def[r], BN_BOX_REQUEST, mgr, BN_BOX_ID);
    bn_log("BOX rc=%d \"%s\" / \"%s\" dur=%d\n", rc, l1, n == 2 ? l2 : "",
           *(int32_t *) (def[r] + 4));
    return rc;
}

/* Test harness (azmod-banner.on), message thread. /tmp/azmod-banner.txt is
 * consumed on read:
 *   "s[:ms] text"          az_banner_strip          "b[:ms] line1|line2"  az_banner_box
 *   "@sN"                  stock strip index N      "@N"                  stock box id N (controls)
 */
static void banner_harness_tick(void) {
    static unsigned tick;
    if (!g_bn_harness || !g_orig_app_init || (tick++ & 7))
        return;
    FILE *f = fopen(BN_TEXT, "re");
    if (!f)
        return;
    char buf[400] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(BN_TEXT);
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = 0;
    if (buf[0] == '@') {
        int strip = buf[1] == 's', id = atoi(buf + 1 + strip);
        uintptr_t mgr = strip ? bn_manager(0x30, BN_STRIP_VT) : bn_manager(0x28, BN_BOX_VT);
        if (!mgr || (strip ? (id < 0 || id > 36) : (id < 1 || id > 66 || id == 3))) {
            bn_log("EXIT: stock %s\n", buf);
            return;
        }
        ((void (*)(uintptr_t, int))(strip ? BN_STRIP_REQUEST : BN_BOX_REQUEST))(mgr, id);
        bn_log("STOCK %s %d\n", strip ? "strip" : "box", id);
        return;
    }
    int ms = 0;
    char *t = strchr(buf, ' ');
    if (!t) {
        bn_log("EXIT: harness syntax\n");
        return;
    }
    *t++ = 0;
    if (buf[1] == ':')
        ms = atoi(buf + 2);
    if (buf[0] == 's')
        az_banner_strip(t, ms);
    else if (buf[0] == 'b') {
        char *bar = strchr(t, '|');
        if (bar)
            *bar++ = 0;
        az_banner_box(t, bar, ms);
    } else
        bn_log("EXIT: harness syntax\n");
}
