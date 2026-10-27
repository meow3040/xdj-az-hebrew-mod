/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file beat_jump.c
 * @brief "Beat Jump 2 First" mod: swaps which jump sizes the two BEAT JUMP pad pages perform.
 *
 * Page 1 keeps EP147's own look, state and LEDs but its pads jump 8/16/32/64; page 2
 * does the short jumps. Only what each page does is swapped. Gated by the
 * AZF_BEAT_JUMP_2_FIRST Mod Menu toggle.
 *
 * Pad action. input_devices::DeckOperationPadHandler<BeatJumpOperation> runs every jump
 * as a queued meow::AsyncTask. A pad press posts a pressPad task (run 0x2086748,
 * vt 0x2e15028), which posts the shared changeOperation task (run 0x2084ef8) that calls
 * BeatJumpHandler press 0x2082a98; the release posts releasePad (run 0x2085768,
 * vt 0x2e15058). Every task holds the op at +0x20 = { u32 len = (den<<16)|num,
 * u32 dir (1 = back) }. The hardware BEAT JUMP < > buttons post changeOperation directly
 * with the panel's size and reach BeatJumpHandler with the same caller and op layout as
 * the pads, so the swap cannot live there without changing the buttons' size: it is
 * applied to the pad's own task, before it runs, so everything downstream (including the
 * held pad EP147 records) sees one consistent op. EP147's pad sets are disjoint
 * (page 1 = 1/2 1 2 4, page 2 = 8 16 32 64), so the mapping is its own inverse.
 *
 * A release is swapped only when its own press was (bj_held), so toggling the mod with a
 * pad held cannot leave EP147 holding an op its release does not match. Each task is
 * checked by alignment and exact vtable at use. No page or deck state is consulted, so
 * it is right for every deck by construction.
 *
 * Tiles. PadBeatJumpWidget's refresh 0x1deb208 picks each tile's art as
 * pad*2 + (mode != 5)*16 from the pad-mode getter, returning to 0x1deb250; modeget_hook
 * answers 5<->6 to that caller only (BJ_TILE_RA), with that deck's own mode. With the mod
 * off both pass through untouched.
 *
 * Jog display. The jog LCD draws each pad page itself, from one byte of the deck frame
 * (offset 0x46 = jog case << 1 | layout variant; BEAT JUMP is case 3, BEAT JUMP 2 case 7,
 * the AZ grid layout is variant 1). The deck frame builder's player-info writer 0x2178820
 * reads that pair from device_adapter::sublcd::PlayerInfo +0xd4 (variant) and +0xd5 (case), which
 * PlayerInfoData's get 0x1fefe18 hands it from the copy that PlayerInfoData's store 0x1fefa78
 * makes of its staging area (data + 0x4f8). bj_joginfo_hook wraps that store: with the mod on,
 * the store is handed a copy of the staging area whose case is swapped 3 <-> 7, so the jog shows
 * the lengths the pads perform. The staging case itself is never written (EP147 can store the
 * same staging area again without rereading it, and must find its own page there); the fields
 * the store fills into its argument (timing, flags) are copied back, so the staging area ends
 * as EP147's own store would leave it. The PlayerInfo is plain data (the store copies its two
 * colour words with a 4-byte load and store). Only the object's own staging area is replaced;
 * anything else, or a PlayerInfoData whose vtable does not match, passes through untouched.
 *
 * Entry points: bj_padpress_hook(), bj_padrel_hook(), bj_joginfo_hook().
 */
#define BJ_PADPRESS_RUN EP_2086748 /* AsyncTask<pressPad lambda>::run(task)   */
#define BJ_PADPRESS_VT EP_2e15028  /*   its vtable (address point)            */
#define BJ_PADREL_RUN EP_2085768   /* AsyncTask<releasePad lambda>::run(task) */
#define BJ_PADREL_VT EP_2e15058
static uint32_t bj_swap_len(uint32_t len) {
    switch (len) {
    case 0x00020001u:
        return 0x00010008u; /* 1/2 <-> 8  */
    case 0x00010008u:
        return 0x00020001u;
    case 0x00010001u:
        return 0x00010010u; /* 1   <-> 16 */
    case 0x00010010u:
        return 0x00010001u;
    case 0x00010002u:
        return 0x00010020u; /* 2   <-> 32 */
    case 0x00010020u:
        return 0x00010002u;
    case 0x00010004u:
        return 0x00010040u; /* 4   <-> 64 */
    case 0x00010040u:
        return 0x00010004u;
    default:
        return len; /* anything else: EP147's own */
    }
}
/* the task's op len, when task really is the expected EP147 task; NULL otherwise. Both hooks
 * are the task's own run(), on EP147's task thread: `task` is live for the call, plain loads. */
static uint32_t *bj_task_len(long task, uintptr_t vt) {
    uintptr_t v;
    if (!task || (task & 7) || !own_q((uintptr_t) task, &v) || v != vt)
        return NULL;
    if (!own_q((uintptr_t) task + 0x20, &v))
        return NULL;
    return (uint32_t *) ((uintptr_t) task + 0x20);
}
/* presses whose op we swapped, by (pad handler, swapped op): their releases get
 * the same swap. Tasks run on EP147's one task thread; the lock is for safety. */
#define BJ_HELD 16
static struct {
    uintptr_t h;
    uint64_t op;
} bj_held[BJ_HELD];
static pthread_mutex_t bj_held_mx = PTHREAD_MUTEX_INITIALIZER;
static void (*g_orig_bj_padpress)(long);
static void (*g_orig_bj_padrel)(long);
static void bj_padpress_hook(long task) {
    uint32_t *len =
        az_on(AZF_BEAT_JUMP_2_FIRST) ? bj_task_len(task, BJ_PADPRESS_VT) : NULL;
    uintptr_t h;
    if (len && own_q((uintptr_t) task + 0x18, &h)) { /* h: an identity token only */
        uint32_t sw = bj_swap_len(*len);
        if (sw != *len) {
            *len = sw; /* this pad task's own op */
            uint64_t op = *(uint64_t *) len;
            pthread_mutex_lock(&bj_held_mx);
            int i, f = -1;
            for (i = 0; i < BJ_HELD; i++) {
                if (bj_held[i].h == h && bj_held[i].op == op) {
                    f = i;
                    break;
                }
                if (f < 0 && !bj_held[i].h)
                    f = i;
            }
            if (f >= 0) {
                bj_held[f].h = h;
                bj_held[f].op = op;
            }
            pthread_mutex_unlock(&bj_held_mx);
        }
    }
    g_orig_bj_padpress(task);
}
static void bj_padrel_hook(long task) {
    uint32_t *len = bj_task_len(task, BJ_PADREL_VT); /* not gated on the mod: follow the press */
    uintptr_t h;
    if (len && own_q((uintptr_t) task + 0x18, &h)) { /* h: an identity token only */
        uint32_t w[2] = {bj_swap_len(len[0]), len[1]};
        uint64_t op;
        memcpy(&op, w, 8);
        pthread_mutex_lock(&bj_held_mx);
        for (int i = 0; i < BJ_HELD; i++)
            if (bj_held[i].h == h && bj_held[i].op == op) {
                len[0] = w[0];
                bj_held[i].h = 0;
                break;
            }
        pthread_mutex_unlock(&bj_held_mx);
    }
    g_orig_bj_padrel(task);
}
/* ---- Jog display ---- */
#define BJ_JOGINFO_STORE EP_1fefa78 /* PlayerInfoData store(data, staging) */
#define BJ_PLAYERINFODATA_VT EP_2df55e8
#define BJ_JOG_STAGING 0x4f8 /* PlayerInfoData's staging PlayerInfo */
#define BJ_JOG_INFO_SIZE 0xd6 /* PlayerInfo: its last field is the byte at +0xd5 */
#define BJ_JOG_VARIANT 0xd4  /* PlayerInfo: jog layout variant (1 = the AZ pad grid) */
#define BJ_JOG_CASE 0xd5     /* PlayerInfo: jog pad page */
/* the jog page that shows the other BEAT JUMP page's lengths; any other page as it is */
static uint8_t bj_jog_case(uint8_t variant, uint8_t jcase) {
    if (variant != 1)
        return jcase;
    if (jcase == 3) /* BEAT JUMP: 1/2 1 2 4 */
        return 7;
    if (jcase == 7) /* BEAT JUMP 2: 8 16 32 64 */
        return 3;
    return jcase;
}
static void (*g_orig_bj_joginfo)(long, unsigned char *);
/* Runs on the thread that builds the deck frames, inside PlayerInfoData's get, which fills the
 * staging area just before this call. Nothing here is shared: the copy is this call's own. */
static void bj_joginfo_hook(long data, unsigned char *info) {
    uintptr_t v;
    if (az_on(AZF_BEAT_JUMP_2_FIRST) && data && !(data & 7) &&
        (uintptr_t) info == (uintptr_t) data + BJ_JOG_STAGING && own_q((uintptr_t) data, &v) &&
        v == BJ_PLAYERINFODATA_VT) {
        uint8_t c = info[BJ_JOG_CASE], sw = bj_jog_case(info[BJ_JOG_VARIANT], c);
        if (sw != c) {
            _Alignas(8) unsigned char copy[BJ_JOG_INFO_SIZE];
            memcpy(copy, info, sizeof copy);
            copy[BJ_JOG_CASE] = sw;
            g_orig_bj_joginfo(data, copy);
            memcpy(info, copy, BJ_JOG_CASE); /* what the store filled in; the case stays EP147's */
            return;
        }
    }
    g_orig_bj_joginfo(data, info);
}
