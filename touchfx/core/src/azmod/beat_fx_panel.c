/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file beat_fx_panel.c
 * @brief Beat FX panel observers and the X-PAD parameter row that shows the drum roll finger zone.
 *
 * Two groups of hooks on the Beat FX panel widgets. The ctor hooks (BeatFxParamWidget,
 * time, length, FX widget and FX body) call the original constructor and remember the
 * BeatFxParamWidget. In a development build they also run an opt-in capture of each
 * widget's embedded ImagesComponents (FXDIAG_CTOR_BODY, empty in a release build).
 *
 * The X-PAD parameter row hooks (fxrow_ctor_hook, fxrow_update_hook) capture the row owner
 * widget and, while the drum roll is armed and a pad is held, rewrite the row's
 * ImagesComponent image index so the row follows the finger zone. The strip array is grown
 * once with the appended strips (fxrow_ensure_grown) and each change is repainted with
 * fxrow_paint. On disarm fxrow_restore hands back EP147's own index. The cached owner's
 * component is validated against the ImagesComponent vtable at every use and dropped when
 * stale. All of it runs on the JUCE message thread.
 *
 * Entry points: the five ctor hooks, fxrow_ctor_hook(), fxrow_update_hook().
 */
static void (*g_orig_fxparam_ctor)(long, long, long);
static void fxparam_ctor_hook(long self, long a1, long a2) {
    g_orig_fxparam_ctor(self, a1, a2);
    disp_wref_init();
    wref_set(&g_fxparam_w, (uintptr_t) self); /* cleared by the widget's destructor */
    FXDIAG_CTOR_BODY("param")
}
static void (*g_orig_fxtime_ctor)(long, long, long);
static void fxtime_ctor_hook(long self, long a1, long a2) {
    g_orig_fxtime_ctor(self, a1, a2);
    FXDIAG_CTOR_BODY("time")
}
static void (*g_orig_fxlength_ctor)(long, long, long);
static void fxlength_ctor_hook(long self, long a1, long a2) {
    g_orig_fxlength_ctor(self, a1, a2);
    FXDIAG_CTOR_BODY("length")
}
static void (*g_orig_fxwidget_ctor)(long, long, long);
static void fxwidget_ctor_hook(long self, long a1, long a2) {
    g_orig_fxwidget_ctor(self, a1, a2);
    FXDIAG_CTOR_BODY("fxw")
}
static void (*g_orig_fxbody_ctor)(long, long, long);
static void fxbody_ctor_hook(long self, long a1, long a2) {
    g_orig_fxbody_ctor(self, a1, a2);
    FXDIAG_CTOR_BODY("fxb")
}
static _Atomic int g_fxrow_zone = -1;
static int fxrow_ensure_grown(long rc) {
    uintptr_t v; /* rc: the live row comp of a live owner, proven by vtable by the caller */
    if (!own_q((uintptr_t) rc + 0xd0, &v))
        return 0;
    uintptr_t arr = v;
    if (!own_q((uintptr_t) rc + 0xe0, &v))
        return 0;
    uint32_t cnt = (uint32_t) v;
    if (cnt >= FX_ROW_STRIP_BASE + FX_ROW_STRIP_CNT)
        return arr > 0x10000;
    if (cnt != FX_ROW_STRIP_BASE || arr <= 0x10000 || !g_fxroll_art_ready)
        return 0;
    enum {
        NEWCNT = FX_ROW_STRIP_BASE + FX_ROW_STRIP_CNT
    };
    void *grown = malloc(NEWCNT * 8);
    if (!grown)
        return 0;
    memcpy(grown, (void *) arr, FX_ROW_STRIP_BASE * 8);
    for (int s = 0; s < FX_ROW_STRIP_CNT; s++)
        fx_img_assign((char *) grown + (FX_ROW_STRIP_BASE + s) * 8, &g_fxart_strip[s]);
    *(volatile uintptr_t *) ((uintptr_t) rc + 0xd0) = (uintptr_t) grown;
    *(volatile uint32_t *) ((uintptr_t) rc + 0xe0) = NEWCNT;
    dbg("fxart: row comp %#lx grown to %d strips (idle=%d, zones +%d)\n", (unsigned long) rc,
        NEWCNT, FX_ROW_STRIP_BASE, 1);
    return 1;
}
/* EP147's own index for the current FX type: what the row shows disarmed,
 * and what a disarm must hand back. */
static int fxrow_native_idx(long self) {
    uintptr_t v; /* self: a live row owner (the writer's argument or the wref slot) */
    if (!own_q((uintptr_t) self + FX_ROW_STATE, &v) || v <= 0x10000)
        return -1;
    uintptr_t st = v;
    if (!own_q(st + FX_ROW_TYPEBYTE, &v))
        return -1;
    uint32_t type = (uint32_t) (v & 0xff);
    if (type > 0xd)
        return -1;
    uint32_t idx = 0;
    if (!img_q(FX_ROW_TBL + 4u * type, &v)) /* EP147's static type table (an odd type: unaligned, -1) */
        return -1;
    idx = (uint32_t) v;
    return idx < FX_ROW_STRIP_BASE ? (int) idx : -1;
}
static void fxrow_paint(long comp, int want) {
    *(volatile uint32_t *) ((uintptr_t) comp + 0xc8) = (uint32_t) want;
    ((void (*)(long)) PAD_REPAINT)(comp);
}
/* Hand the row back to EP147 on disarm. The writer itself only rewrites on
 * change, so if we leave our index in +0xc8 and no sync fires, the roll art
 * would stick; restoring here makes the return unconditional. */
static void fxrow_restore(long self) {
    uintptr_t v; /* self: a live row owner (the writer's argument or the wref slot) */
    if (!own_q((uintptr_t) self + FX_ROW_COMP, &v) || v <= 0x10000)
        return;
    long comp = (long) v;
    int nat = fxrow_native_idx(self);
    __atomic_store_n(&g_fxrow_zone, -1, __ATOMIC_RELAXED);
    if (nat < 0)
        return;
    if (!own_q((uintptr_t) comp + 0xc8, &v))
        return;
    if ((uint32_t) v == (uint32_t) nat)
        return;
    fxrow_paint(comp, nat);
    dbg("fxrow: disarmed -> native strip %d restored\n", nat);
}
static void fxrow_apply_tick(void) {
    /* per-tick re-assert while armed: the zone changes at bus rate but EP147's writer
     * runs only on its own state syncs. Change-only, so the steady state is one compare. */
    /* Message thread. The owner is a wref slot: EP147 rebuilds this widget, and its
     * destructor clears the slot, so a non-zero owner is alive. Its row comp is proven by
     * vtable before anything calls through it. */
    uintptr_t owner = wref_get(&g_fxrow_owner);
    if (!owner)
        return;
    int zone = __atomic_load_n(&g_fxrow_zone, __ATOMIC_RELAXED);
    int want = FX_ROW_STRIP_BASE + (zone >= 0 ? 1 + zone : 0);
    uintptr_t v;
    if (!own_q(owner + FX_ROW_COMP, &v) || v <= 0x10000)
        return;
    long comp = (long) v;
    if ((comp & 7) || !own_q((uintptr_t) comp, &v) || v != IMAGESCOMP_VT)
        return; /* not the row comp: leave it */
    if (!own_q((uintptr_t) comp + 0xc8, &v))
        return;
    if ((uint32_t) v == (uint32_t) want)
        return;
    if (!fxrow_ensure_grown(comp))
        return;
    fxrow_paint(comp, want);
}
static void (*g_orig_fxrow_update)(long);
/* After either strip writer has run (the writer's this is the row widget). */
static void fxrow_after_update(long self) {
    disp_wref_init();
    wref_set(&g_fxrow_owner, (uintptr_t) self); /* the writer's this is the row widget */
    if (!g_fxroll_art_ready)
        return;
    int arm = __atomic_load_n(&g_drum_arm, __ATOMIC_RELAXED);
    int held = 0;
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (__atomic_load_n(&g_drum_held[d], __ATOMIC_RELAXED))
            held = 1;
    if (!(arm && held)) {
        static int was_applied;
        if (was_applied) {
            was_applied = 0;
            fxrow_restore(self);
        }
        return;
    }
    fxrow_apply_tick();
}
static void fxrow_update_hook(long self) {
    g_orig_fxrow_update(self);
    fxrow_after_update(self);
}
/* The twin writer FX_ROW_UPDATE2 calls its own original, never FX_ROW_UPDATE's: the two
 * bodies are instruction-identical on 2.00, but that is not a contract. */
static void fxrow_update2_hook(long self) {
    g_orig_fxrow_update2(self);
    fxrow_after_update(self);
}
/* Owner capture at panel (re)build. The writer hook recaptures on every call; this
 * only gives the per-tick path a handle before the writer's first run. */
static void (*g_orig_fxrow_ctor)(long, long, long);
static void fxrow_ctor_hook(long self, long a1, long a2) {
    g_orig_fxrow_ctor(self, a1, a2);
    disp_wref_init();
    wref_set(&g_fxrow_owner, (uintptr_t) self);
}
