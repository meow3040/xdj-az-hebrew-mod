/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file vinyl_brake.c
 * @brief Stepless vinyl brake (Fine Vinyl Speed Adjust): backend of the SHORTCUT slider
 *
 * Hooks dj_player::VinylSpeedAdjustor's setter 0x14754e8(this, v, kind), one adjustor per deck:
 *   idx = clamp((int)(v * 255), 0, 255); rate = tbl[0x2896ba0][idx]
 *   kind 1 TOUCH -> +0x18/+0x20, kind 2 RELEASE -> +0x1c/+0x24
 * SHORTCUT step n sends v = n/5; the unused kind, and every kind with the deck's VINYL
 * SPEED ADJUST button off, get v = 0. Substituting only v > 0 therefore leaves the button
 * and the TOUCH/RELEASE mode stock.
 *
 * The hook is always installed (so the Mod Menu row acts without a restart) and records
 * the adjustors it sees in g_vb_adj for vinyl_slider.c. With the row off or no valid value
 * it passes EP147's value through. The adjustor carries no deck number, so one value
 * applies to every deck. A new value takes effect at the setter's next call.
 *
 * Value: azmod.conf's "vinyl.brake", one number 0.00..1.00, saved when the slider is
 * released and loaded into g_vb_milli with the other settings (azmod.c conf_apply_small):
 * the setter runs on the player thread and must do no I/O.
 */
#define VB_SETTER EP_14754e8
static void (*g_orig_vb_set)(long, float, int);
static _Atomic int g_vb_milli = -1; /* v * 1000, -1 = no valid value: pass through */
static int g_vb_hooked;             /* the hook went in at start; else the slider does nothing */
#define VB_ADJ_VT EP_2896b90 /* dj_player::VinylSpeedAdjustor (RTTI) */
#define VB_NADJ 8
static _Atomic long g_vb_adj[VB_NADJ]; /* VinylSpeedAdjustor instances seen by the setter */
/* Adjustor lifetime. EP147 replaces a deck's adjustor at run time (its owner deletes the old
 * one through the vtable, or through an inlined copy that first compares the vtable's
 * deleting-destructor slot with the original). Both destructor slots are patched, so every
 * delete passes vb_forget first: it unpublishes the adjustor and waits for a slider apply
 * that is using it (vs_apply holds g_vb_readers for a few loads and one leaf call) before
 * the memory goes back. A cached adjustor is therefore live for as long as the reader holds
 * the count. Without both patches nothing is cached and the slider takes effect at the
 * setter's next call. */
static int g_vb_dtor_ok;
static _Atomic int g_vb_readers;
static void (*g_orig_vb_d1)(long);
static void (*g_orig_vb_d0)(long);
static void vb_forget(long self) {
    for (int i = 0; i < VB_NADJ; i++) {
        long e = self;
        __atomic_compare_exchange_n(&g_vb_adj[i], &e, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
    }
    if (!on_msg_thread()) /* readers are message-thread only: else a reader is our own caller */
        ep_wait_readers(&g_vb_readers);
}
static void vb_d1_hook(long self) {
    vb_forget(self);
    g_orig_vb_d1(self);
}
static void vb_d0_hook(long self) {
    vb_forget(self);
    g_orig_vb_d0(self);
}
/* Constructor phase. Patch only when both slots still hold EP147's own destructors (each
 * starts with `str wzr, [x0, #8]`, clearing the object's guard word). */
static void install_vb_dtor(void) {
    uintptr_t d1 = 0, d0 = 0, i1 = 0, i0 = 0;
    if (!img_q(VB_ADJ_VT, &d1) || !img_q(VB_ADJ_VT + 8, &d0) || (d1 & 3) || (d0 & 3) ||
        !img_q(d1 & ~7UL, &i1) || !img_q(d0 & ~7UL, &i0))
        return;
    uint32_t w1 = (uint32_t) (i1 >> ((d1 & 4) * 8)), w0 = (uint32_t) (i0 >> ((d0 & 4) * 8));
    if (w1 != 0xb900081fu || w0 != 0xb900081fu)
        return;
    if (azm_patch_vtable(VB_ADJ_VT, (void *) vb_d1_hook, (void **) &g_orig_vb_d1) != 1)
        return;
    if (azm_patch_vtable(VB_ADJ_VT + 8, (void *) vb_d0_hook, (void **) &g_orig_vb_d0) != 1)
        return; /* d1 alone: nothing is cached (g_vb_dtor_ok stays 0) */
    g_vb_dtor_ok = 1;
}
static void vb_set_hook(long self, float v, int kind) {
    /* remember the adjustors (EP147 calls this for all four at start) so the slider
     * can apply a new value at once; vinyl_slider.c vtable-checks them before use */
    for (int i = 0; g_vb_dtor_ok && i < VB_NADJ; i++) {
        long e = __atomic_load_n(&g_vb_adj[i], __ATOMIC_RELAXED);
        if (e == self)
            break;
        if (!e) {
            long z = 0;
            /* SEQ_CST, a release: vs_apply on the message thread must see the constructed
             * object it loads (as g_tc_self and g_mixch are published) */
            if (__atomic_compare_exchange_n(&g_vb_adj[i], &z, self, 0, __ATOMIC_SEQ_CST,
                                            __ATOMIC_RELAXED))
                break;
        }
    }
    int m = __atomic_load_n(&g_vb_milli, __ATOMIC_RELAXED);
    if (v > 0.f && m >= 0 && az_on(AZF_FINE_VINYL_SPEED))
        v = (float) m / 1000.f; /* row off or no value: stock */
    g_orig_vb_set(self, v, kind);
}
