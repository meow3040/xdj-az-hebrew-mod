/* SPDX-License-Identifier: GPL-3.0-only */
/* drag_handoff.h: how a playlist drag ends, handed from the mixer-bus read thread to
 * the message thread.
 *
 * The bus read thread sees the press that drops a held row and the BACK that cancels
 * it, but it runs at real-time priority and must never block or call into EP147's UI:
 * ending a drag posts a banner (the caution manager, with a table swap behind it),
 * starts the landing flash and arms the library write. So the bus thread only records
 * the end here, and the message tick performs it.
 *
 * One slot is enough, and that is enforced rather than assumed: every drag has exactly
 * one end. `state` is the per-drag latch, a generation count times two plus a CLOSED bit.
 * The first party to close the open generation owns the end: the bus thread (a drop or a
 * BACK, which then publishes its intent in the slot) or the message thread (the list
 * changed or left the screen, which ends the drag itself). Everyone else loses the
 * compare-and-swap and does nothing, so a drop followed by a BACK, two drops, or a drop
 * racing a list change can never act twice. The bus thread closes only the generation
 * it saw open at the start of its frame, so a stale frame can never end a newer drag.
 * The latch is reopened, as the next generation, only at GRAB, on the message thread,
 * and only once the slot is empty.
 *
 * Ordering: the poster writes the intent, then sets `full` with release; the taker
 * reads `full` with acquire before reading the intent, then clears it with release.
 * Single producer (the bus read thread), single consumer (the message thread). No
 * locks, no syscalls, no allocation: safe on the real-time thread. */
#ifndef AZMOD_DRAG_HANDOFF_H
#define AZMOD_DRAG_HANDOFF_H

enum {
    DH_NONE = 0,
    DH_COMMIT = 1,
    DH_CANCEL = 2
};

struct dh_intent {
    int op;        /* DH_COMMIT or DH_CANCEL */
    int origin;    /* the grabbed row (data row) */
    int target;    /* where it was dropped (display row) */
    int into;      /* the folder it drops into (data row), -1 = none */
    int into_disp; /* that folder's display row, -1 = none */
};

struct dh_slot {
    unsigned state; /* generation * 2 + 1 while closed: the drag's end is decided, or none open */
    int full;       /* 1 = `it` is published and not yet taken */
    struct dh_intent it;
    unsigned posted, refused, taken; /* counters: diagnostics and tests */
};

#define DH_SLOT_INIT {1u, 0, {DH_NONE, -1, -1, -1, -1}, 0, 0, 0}

/* Either thread: the latch as it is now. Read it once per frame and pass it to dh_post. */
static unsigned dh_state(struct dh_slot *s) {
    return __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
}
static int dh_is_open(unsigned st) {
    return !(st & 1u);
}
/* Either thread: the open drag's end is decided (or no drag is open). */
static int dh_closed(struct dh_slot *s) {
    return !dh_is_open(dh_state(s));
}

/* Bus read thread: record how the drag it saw open (`seen`, from dh_state) ends.
 * 1 = recorded; 0 = that drag's end was already decided (a drop or BACK earlier, or the
 * message thread ended it) or it is no longer the open drag, and nothing is recorded. */
static int dh_post(struct dh_slot *s, unsigned seen, const struct dh_intent *it) {
    if (!dh_is_open(seen) || !__atomic_compare_exchange_n(&s->state, &seen, seen | 1u, 0,
                                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&s->refused, 1, __ATOMIC_RELAXED);
        return 0;
    }
    s->it = *it;
    __atomic_add_fetch(&s->posted, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s->full, 1, __ATOMIC_RELEASE);
    return 1;
}

/* Message thread: take the recorded end, if any. 1 = *out holds it (taken exactly once). */
static int dh_take(struct dh_slot *s, struct dh_intent *out) {
    if (!__atomic_load_n(&s->full, __ATOMIC_ACQUIRE))
        return 0;
    *out = s->it;
    __atomic_add_fetch(&s->taken, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s->full, 0, __ATOMIC_RELEASE);
    return 1;
}

/* Message thread: end the open drag itself. 1 = this caller owns the end; 0 = the bus
 * thread already recorded one (take it instead), or no drag is open. */
static int dh_claim_local(struct dh_slot *s) {
    unsigned st = dh_state(s);
    return dh_is_open(st) && __atomic_compare_exchange_n(&s->state, &st, st | 1u, 0,
                                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Message thread, at GRAB, before the drag is published: open the next generation, which
 * may end once. 0 = an earlier end is still untaken, so the grab must not open a drag. */
static int dh_open(struct dh_slot *s) {
    if (__atomic_load_n(&s->full, __ATOMIC_ACQUIRE))
        return 0;
    unsigned st = dh_state(s);
    __atomic_store_n(&s->state, (st | 1u) + 1u, __ATOMIC_RELEASE);
    return 1;
}

#endif
