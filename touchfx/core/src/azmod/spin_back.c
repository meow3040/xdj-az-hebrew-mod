/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file spin_back.c
 * @brief Extended Lead-In: the jog spins back far past the start of a track.
 *
 * EP147 lets a deck's position go only 2 s before the track: player_basis::PlayerCore keeps
 * that pre-roll in milliseconds, and the position a jog or scratch asks for is clamped to
 * [pre-roll x sample rate / 1000, end] by dj_player::PlayStartStopStrategy before it is used.
 * The same bound decides whether a deck may play (a deck sitting at the bound does not start).
 * With the row on, the pre-roll of the deck being moved is set to SB_CAP_MS just before
 * EP147's clamp runs, so the clamp and everything else that reads the bound use the longer
 * one; with the row off it goes back to the stock 2 s. Before the track a deck plays silence,
 * as it does in the stock 2 s, and runs into the track when it gets there.
 *
 * Entry points: install_spin_back.
 */
/* EP147:
 *   - PlayStartStopStrategy's jog clamp FUN_014a3e10(this, ..., position, readable, flag):
 *     the deck's PlayerCore at this+0x18; it computes the lower bound from that core, clamps
 *     the requested position and passes it on. It runs on the audio thread while a jog moves.
 *   - PlayerCore (vtable 0x2874158): the pre-roll at +0x2f0, int64 ms (-2000 on every player
 *     EP147 builds), read as ms x sample rate (+0x68) / 1000 by the clamp and by the play
 *     gate FUN_014a3f00. */
#define SB_CLAMP EP_14a3e10
#define SB_CORE_VT EP_2874158 /* player_basis::PlayerCore */
#define SB_CORE 0x18          /* the strategy's PlayerCore */
#define SB_PREROLL 0x2f0      /* PlayerCore: pre-roll before the track, ms (int64) */
#define SB_STOCK_MS (-2000L)
#define SB_CAP_MS (-300000L) /* 5 minutes: past any practical spin, far from any overflow */

/* The pre-roll to leave in place: the cap with the row on, stock with it off. Only the two
 * known values are ever replaced, so a player EP147 built with another pre-roll is left as
 * it is. Returns `cur` when nothing should change. */
static long sb_want(long cur, int on) {
    if (cur != SB_STOCK_MS && cur != SB_CAP_MS)
        return cur;
    return on ? SB_CAP_MS : SB_STOCK_MS;
}
/* The strategy's PlayerCore, or 0: aligned, readable and a PlayerCore. */
static uintptr_t sb_core(long strategy) {
    uintptr_t core = 0, vt = 0;
    if (!strategy || (strategy & 7) || !own_q((uintptr_t) strategy + SB_CORE, &core) || !core ||
        (core & 7) || !own_q(core, &vt) || vt != SB_CORE_VT)
        return 0;
    return core;
}
/* Audio thread: no system calls, one atomic load for the row. */
static void sb_apply(long strategy) {
    uintptr_t core = sb_core(strategy), cur = 0;
    if (!core || !own_q(core + SB_PREROLL, &cur))
        return;
    long want = sb_want((long) cur, az_value(AZF_EXTENDED_LEAD_IN) != 0);
    if (want != (long) cur)
        __atomic_store_n((long *) (core + SB_PREROLL), want, __ATOMIC_RELAXED);
}
static void (*g_o_sb_clamp)(long, long, long, long, long);
static void sb_clamp_hook(long strategy, long a1, long pos, long readable, long flag) {
    sb_apply(strategy);
    g_o_sb_clamp(strategy, a1, pos, readable, flag);
}

/* ---- BEAT JUMP back into the lead-in ---------------------------------------------------------
 * EP147's beat jump (dj_player::BeatJumpFunction, v1.30 0x14574b8) turns the position into a
 * beat position on the track's grid, subtracts the jump, and turns the result back into a
 * position (the grid extrapolates before its first beat with the first beat's tempo). It then
 * asks SB_JUMP_CHECK(function, target) whether the target may be jumped to, and that refuses any
 * negative target (`tbnz x1, #63`), so a jump that would land before the track does nothing.
 * With the row on, that question from the beat jump (its return address, SB_JUMP_RA) about a
 * target before the start is answered by leadin_jump.h: within the lead-in (SB_CAP_MS at the
 * track's sample rate) it is asked for the track's start instead, so EP147's other checks still
 * run, and the jump command EP147 builds next carries the real target (it reads it again from its
 * own frame); past the lead-in it is refused, as EP147 refuses one before the start. Other
 * callers, a target at or after the start, and the row off pass through unchanged.
 * At the call (the same on both builds) the caller's x20 is its beat grid, whose +0x18 is the
 * track's sample rate (the grid's own beat-to-position conversion reads it there), and its
 * position before the jump is at its sp + 0x118. The entry shim hands both to C. */
#include "leadin_jump.h"
#define SB_JUMP_CHECK EP_1453e80 /* v1.30 0x1453e80 */
#define SB_JUMP_RA EP_1457b4c    /* v1.30 0x1457b4c: the beat jump's return from SB_JUMP_CHECK */
#define SB_GRID_RATE 0x18
#define SB_FRAME_POS 0x118
#if defined(__aarch64__)
#if defined(__APPLE__)
#define SB_FN(n) ".globl _" #n "\n.private_extern _" #n "\n_" #n ":\n"
#define SB_B(n) "    b _" #n "\n"
#else
#define SB_FN(n) ".globl " #n "\n.hidden " #n "\n.type " #n ", %function\n" #n ":\n"
#define SB_B(n) "    b " #n "\n"
#endif
__attribute__((visibility("hidden"))) int (*g_o_sb_check)(long, long);
/* The C side: function and target as EP147 passes them, the caller's x20 and sp, and the
 * caller's return address (x30, unchanged by the shim). */
__attribute__((visibility("hidden"))) int sb_check_c(long function, long target, uintptr_t grid, uintptr_t sp,
                                                     uintptr_t ra) {
    if (target >= 0 || ra != SB_JUMP_RA || !az_value(AZF_EXTENDED_LEAD_IN))
        return g_o_sb_check(function, target);
    uintptr_t rate = 0, pos = 0;
    if (!grid || (grid & 7) || !own_q(grid + SB_GRID_RATE, &rate))
        rate = 0;
    int d = lj_decide(target, (long) (int32_t) rate, SB_CAP_MS), r = 0;
    if (d == LJ_ALLOW)
        r = g_o_sb_check(function, 0) & 0xff;
    FILE *f = azlog_open("/tmp/azmod-spinback.log", "a");
    if (f) {
        if (!own_q(sp + SB_FRAME_POS, &pos))
            pos = 0;
        fprintf(f, "[%ld] beat jump back: from %ld to %ld (%d Hz): %s\n", (long) time(NULL), (long) pos, target,
                (int) (int32_t) rate, d == LJ_ALLOW ? (r ? "into the lead-in" : "refused by EP147") : "past the lead-in");
        fclose(f);
    }
    return r;
}
/* A jump from the lead-in. Before computing anything the beat jump asks SB_JUMP_READY(function)
 * whether it may jump at all, and that answers no while the deck's position is before the track
 * (`tbnz x0, #63` on the position it copies from the deck's state). With the row on and EP147's
 * answer no, the question is asked again here with that one difference: the deck's state, where
 * EP147 takes it (the function's +8 context: its own +0x10 when its +0xc is not 2 and its +8 byte
 * is 0 and +9 byte is not, else its +0x50 object's vtable slot +0x30), passes the
 * same field tests (+8 nonzero, +0xc low five bits nonzero, +0xe nonzero, +0x10 nonzero) and its
 * position (+0x18) is before the track and not EP147's "none" (INT64_MAX). The grid check EP147
 * also makes is left to the jump itself, which does nothing without a grid. A deck is in the
 * lead-in only as far as a jog or a beat jump took it, both within the floor. Only for the beat
 * jump's own call (SB_READY_RA); its other caller keeps EP147's answer. (task thread) */
#define SB_JUMP_READY EP_14553d8 /* v1.30 0x14553d8 */
#define SB_READY_RA EP_14574f8   /* v1.30 0x14574f8: the beat jump's return from SB_JUMP_READY */
static int (*g_o_sb_ready)(long);
static int sb_ready_hook(long function) {
    int r = g_o_sb_ready(function) & 0xff;
    if (r || (uintptr_t) __builtin_return_address(0) != SB_READY_RA || !az_value(AZF_EXTENDED_LEAD_IN))
        return r;
    uintptr_t ctx = 0, mode = 0, obj = 0, vt = 0, fn = 0, st = 0, a = 0, b = 0, pos = 0;
    if (!function || (function & 7) || !own_q((uintptr_t) function + 8, &ctx) || !ctx || (ctx & 7) ||
        !own_q(ctx + 8, &mode))
        return 0;
    if ((uint32_t) (mode >> 32) != 2 && !(mode & 0xff) && ((mode >> 8) & 0xff)) {
        st = ctx + 0x10; /* the state EP147 keeps in place */
    } else {
        if (!own_q(ctx + 0x50, &obj) || !obj || (obj & 7) || !own_q(obj, &vt) || !vt || !img_q(vt + 0x30, &fn) ||
            !fn)
            return 0;
        st = ((uintptr_t (*)(uintptr_t)) fn)(obj);
    }
    if (!st || (st & 7) || !own_q(st + 8, &a) || !own_q(st + 0x10, &b) || !own_q(st + 0x18, &pos))
        return 0;
    int ok = (a & 0xff) && ((a >> 32) & 0x1f) && ((a >> 48) & 0xff) && (uint32_t) b && (long) pos < 0 &&
             (long) pos != (long) 0x7fffffffffffffffL;
    if (ok) {
        FILE *f = azlog_open("/tmp/azmod-spinback.log", "a");
        if (f) {
            fprintf(f, "[%ld] beat jump from the lead-in at %ld\n", (long) time(NULL), (long) pos);
            fclose(f);
        }
    }
    return ok;
}
/* EP147's entry: x0 = function, x1 = target; x20 and sp are still the caller's. */
__attribute__((visibility("hidden"))) void sb_check_hook(void);
__asm__(".text\n"
        ".p2align 2\n" SB_FN(sb_check_hook) "    mov x2, x20\n"
                                            "    mov x3, sp\n"
                                            "    mov x4, x30\n" SB_B(sb_check_c));
#define SB_HAVE_JUMP 1
#endif
static void install_spin_back(void) {
    int a = azm_install_hook(SB_CLAMP, (void *) sb_clamp_hook, (void **) &g_o_sb_clamp);
    int b = -9;
#ifdef SB_HAVE_JUMP
    b = azm_install_hook(SB_JUMP_CHECK, (void *) sb_check_hook, (void **) &g_o_sb_check);
    if (b == 1)
        b = azm_install_hook(SB_JUMP_READY, (void *) sb_ready_hook, (void **) &g_o_sb_ready) == 1 ? 1 : 2;
#endif
    FILE *f = azlog_open("/tmp/azmod-spinback.log", "a");
    if (f) {
        fprintf(f, "[%ld] spin back: hook clamp=%d jump=%d\n", (long) time(NULL), a, b);
        fclose(f);
    }
}
