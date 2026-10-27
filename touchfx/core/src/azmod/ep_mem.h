/* SPDX-License-Identifier: GPL-3.0-only */
/* ep_mem.h: how azmod reads EP147's memory.
 *
 * Inside the process a read is a load. Whether it is safe depends on when and where it runs,
 * never on a system call turning a fault into an error code: freed memory usually stays
 * mapped, so a read through /proc/self/mem or process_vm_readv "succeeds" with whatever
 * object lives there now, and each such read takes EP147's mmap_sem, which a page-faulting
 * EP147 thread can then wait behind. So azmod reads live objects without a fault-free reader
 * (the one exception, crash_peek8, runs only after a crash). It has these:
 *
 *   own_q / own_qv   the address belongs to an object that is alive for the whole call:
 *                    a hook's argument (or an object reached from it through EP147's own
 *                    fields) on the thread that owns it, during EP147's call; or an object
 *                    whose lifetime azmod tracks (a wref slot below, the BpmState pin).
 *   img_q            EP147's own image: code, vtables, static data. Mapped for the life of
 *                    the process; the address must lie inside it.
 *   own_str          a NUL-terminated string owned by such a live object.
 *
 * All of them refuse a misaligned or implausible address (0 = unknown, the caller does
 * nothing); none of them can tell a live object from a dead one, which is why the caller
 * must already know.
 *
 * wref: a cached juce::Component pointer that cannot outlive its component. A registered
 * slot is cleared by comp_dtor_hook, the hook on juce::Component's destructor (every
 * component's destructor chain ends there), before the memory returns to the allocator.
 * JUCE creates and destroys components on the message thread, so message-thread code that
 * loads a non-zero slot holds a live component for the rest of its call. Other threads may
 * compare a slot's value; they never dereference it.
 *
 * Self-contained so the host tests compile it as is (epmem_test.c). */
#ifndef AZMOD_EP_MEM_H
#define AZMOD_EP_MEM_H
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>

static inline int ep_addr_ok(uintptr_t a) {
    return a >= 0x10000 && a <= 0x0000ffffffffffffUL && !(a & 7);
}
/* One aligned 8-byte load. The caller vouches that `a` is inside a live object. */
static inline int own_q(uintptr_t a, uintptr_t *out) {
    if (!ep_addr_ok(a))
        return 0;
    *out = *(const volatile uintptr_t *) a;
    return 1;
}
/* own_q for n addresses; ok[i] says whether out[i] holds a value (a refused address reads 0). */
static inline void own_qv(int n, const uintptr_t *a, uintptr_t *out, int *ok) {
    for (int i = 0; i < n; i++) {
        out[i] = 0;
        ok[i] = own_q(a[i], &out[i]);
    }
}

/* EP147's image, [lo, hi): filled once by ep_img_init() from the main program's PT_LOAD
 * segments, before any reader runs. Until then img_q refuses everything. */
static uintptr_t g_ep_img_lo, g_ep_img_hi;
static inline int img_q(uintptr_t a, uintptr_t *out) {
    if (!ep_addr_ok(a) || a < g_ep_img_lo || a + 8 > g_ep_img_hi)
        return 0;
    *out = *(const volatile uintptr_t *) a;
    return 1;
}

/* Copy a live object's NUL-terminated string into out[n]: at most n-1 bytes, and it must look
 * like text (a control byte below 0x09 before the NUL rejects it). 1 = copied, 0 = refused
 * (out is then ""). Reads stop at the first NUL, so nothing past the string is touched. */
#include "utf8_cut.h"
static inline int own_str(uintptr_t p, char *out, int n) {
    if (n < 1)
        return 0;
    out[0] = 0;
    if (p < 0x10000 || p > 0x0000ffffffffffffUL)
        return 0;
    const volatile unsigned char *s = (const volatile unsigned char *) p;
    for (int i = 0; i < n - 1; i++) {
        unsigned char c = s[i];
        if (!c) {
            out[i] = 0;
            return 1;
        }
        if (c < 0x09) {
            out[0] = 0;
            return 0;
        }
        out[i] = (char) c;
    }
    out[n - 1] = 0;
    utf8_cut(out); /* cut short: never half a character */
    return 1;
}

/* After a crash only: one 8-byte read for the crash reporter's frame walk (crash_guards.c,
 * community.c cm_blame_fault), inside a fatal-signal handler. A frame pointer of a crashed
 * thread may point anywhere, a plain load there would fault again inside the handler, and the
 * report would be lost, so this is the one fault-free read azmod keeps: process_vm_readv (a
 * system call that is async-signal-safe, needs no file descriptor, and reports an unmapped
 * page as an error). The objections to such reads do not apply here: the values only name
 * return addresses for a report, and EP147 is already going down. Nothing else may use it
 * (procmem-lint.sh). */
static inline int crash_peek8(uintptr_t a, uintptr_t *out) {
    if (!ep_addr_ok(a))
        return 0;
    struct iovec l = {out, 8}, r = {(void *) a, 8};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == 8;
}

/* ---- wref: cached juce::Component pointers, cleared by the component's destructor ---- */
#define WREF_RANGES 96
struct wref_range {
    _Atomic uintptr_t *base; /* the first slot */
    int n;                   /* slots in the range */
    int comp_off;            /* where juce::Component sits inside the cached object */
};
static struct wref_range g_wref[WREF_RANGES];
static _Atomic int g_wref_n;       /* ranges published (each fully written first) */
static _Atomic int g_wref_full;    /* registrations refused: the table was full */
static _Atomic unsigned g_wref_cleared; /* slots a destructor cleared (telemetry) */
static pthread_mutex_t g_wref_mu = PTHREAD_MUTEX_INITIALIZER; /* registration only */
static int g_compdtor_ok; /* the destructor hook is in: without it nothing may be cached */

/* Register `n` slots starting at `base`, holding objects whose juce::Component base is at
 * `comp_off`. Idempotent. Returns 1 when tracked; 0 when the table is full or the destructor
 * hook is not installed, and then the caller must not cache (wref_set refuses such a slot).
 * Register from the constructor phase where possible; the mutex only orders registrations. */
static int wref_track(_Atomic uintptr_t *base, int n, int comp_off) {
    if (!g_compdtor_ok)
        return 0;
    int k = atomic_load_explicit(&g_wref_n, memory_order_acquire);
    for (int i = 0; i < k; i++)
        if (g_wref[i].base == base)
            return 1;
    pthread_mutex_lock(&g_wref_mu);
    k = atomic_load_explicit(&g_wref_n, memory_order_relaxed);
    int found = 0;
    for (int i = 0; i < k && !found; i++)
        found = g_wref[i].base == base;
    if (!found) {
        if (k >= WREF_RANGES) {
            atomic_fetch_add_explicit(&g_wref_full, 1, memory_order_relaxed);
            pthread_mutex_unlock(&g_wref_mu);
            return 0;
        }
        g_wref[k].base = base;
        g_wref[k].n = n;
        g_wref[k].comp_off = comp_off;
        atomic_store_explicit(&g_wref_n, k + 1, memory_order_release); /* after the entry */
    }
    pthread_mutex_unlock(&g_wref_mu);
    return 1;
}
/* Store a component pointer into a slot of a tracked range (0 clears). A slot that is not
 * tracked is never written: an untracked cache could outlive its component. */
static int wref_set(_Atomic uintptr_t *slot, uintptr_t p) {
    int k = atomic_load_explicit(&g_wref_n, memory_order_acquire);
    for (int i = 0; i < k; i++)
        if (slot >= g_wref[i].base && slot < g_wref[i].base + g_wref[i].n) {
            atomic_store_explicit(slot, p, memory_order_release);
            return 1;
        }
    return 0;
}
static inline uintptr_t wref_get(_Atomic uintptr_t *slot) {
    return atomic_load_explicit(slot, memory_order_acquire);
}
/* The destructor side: clear every tracked slot that holds the component being destroyed.
 * `comp` is the juce::Component the destructor received. */
static void wref_on_destroy(uintptr_t comp) {
    int k = atomic_load_explicit(&g_wref_n, memory_order_acquire);
    for (int i = 0; i < k; i++) {
        uintptr_t want = comp - (uintptr_t) g_wref[i].comp_off;
        _Atomic uintptr_t *b = g_wref[i].base;
        for (int j = 0; j < g_wref[i].n; j++) {
            uintptr_t v = atomic_load_explicit(&b[j], memory_order_relaxed);
            if (v == want &&
                atomic_compare_exchange_strong_explicit(&b[j], &v, 0, memory_order_acq_rel,
                                                        memory_order_relaxed))
                atomic_fetch_add_explicit(&g_wref_cleared, 1, memory_order_relaxed);
        }
    }
}

/* A destructor hook's wait for readers that a reader count let in (tc_forget, mixch_forget,
 * vb_forget): the readers hold the count for a few loads and one leaf call, so the wait is that
 * long. It must still end if the destroying thread is real-time and the reader was preempted on
 * the same core (this kernel runs with RT throttling off): spin, then yield, then sleep, so a
 * lower-priority reader always gets the CPU back. */
static inline void ep_wait_readers(_Atomic int *readers) {
    for (unsigned spins = 0; atomic_load_explicit(readers, memory_order_seq_cst) != 0; spins++) {
        if (spins < 64)
            continue;
        if (spins < 256) {
            sched_yield();
            continue;
        }
        struct timespec ts = {0, 20000};
        nanosleep(&ts, NULL);
    }
    atomic_thread_fence(memory_order_acquire);
}
/* The message thread, recorded by the first JUCE timer callback (calltimers_hook / rtick_hook).
 * on_msg_thread() is a TLS compare, no system call. Until it is known, nothing is. */
static pthread_t g_msg_thread;
static _Atomic int g_msg_thread_known;
static inline void msg_thread_note(void) {
    if (!atomic_load_explicit(&g_msg_thread_known, memory_order_acquire)) {
        g_msg_thread = pthread_self();
        atomic_store_explicit(&g_msg_thread_known, 1, memory_order_release);
    }
}
static inline int on_msg_thread(void) {
    return atomic_load_explicit(&g_msg_thread_known, memory_order_acquire) &&
           pthread_equal(pthread_self(), g_msg_thread);
}
#endif
