/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file jog_display.c
 * @brief The deck's stem state on its jog display, through EP147's own jog records
 *
 * The jog display is drawn by the deck's controller, not by EP147: EP147 sends it records
 * (gui::sub_display, per deck 0..3 from the constructors: PlayerInfoData [+0x350],
 * CurrentHotCueCollect [+0x08]). With "Pad Info Display" On the jog shows the pad mode; the
 * stems page has no pad mode of its own (it is BEAT LOOP to EP147). While a deck's pads are its
 * stem toggles this file shows that deck's stem state on the jog's HOT CUE pad page instead
 * (decisions and table: jog_stems_logic.h):
 *
 *   - jog_padinfo_hook, after EP147 fills the deck's pad-mode record
 *     (PlayerInfoData 0x1ff1250(this, PlayerInfo*)): the record asks for the HOT CUE page,
 *     and publishes what it asked for (g_jog_active).
 *   - jog_cue_update_hook, around CurrentHotCueCollect's update (0x1ffbc60(this, ...), the only
 *     sender of the deck's pad table): the deck is known for the duration of the call
 *     (t_jog_deck), so a table EP147 sends inside it (jog_cue_slot_hook, TrackInfoQueue
 *     vtable slot +0x58) is replaced by the stem table while the deck shows stems. After it,
 *     a changed stem state is sent at once through the same slot, on the same thread, and
 *     when the deck stops showing stems EP147's own table ([this+0x8d0]) is sent again.
 *
 * The pad table is one record that the controller applies whole; it does not touch the
 * artwork or the waveform preview, so the page keeps both and a toggle redraws one tile.
 *
 * Inputs, each atomic and per deck: g_jog_src (overlay thread: Stems engaged on the deck's
 * track, and its stem mask) and g_jog_page (the ErP LED path: the deck's pads are its stem
 * toggles, the same answer the pad LEDs use). A deck not driven by a side keeps its last page
 * answer.
 *
 * The waveform preview strip on the same page shows four stem blocks while the deck shows
 * stems (jog_prev_slot_hook, TrackInfoQueue vtable slot +0x20, record 0x2c): EP147's own strip
 * for the deck is kept and passed through, the stem strip goes in its place, once per change
 * of the stem state, and EP147's latest strip goes back when the deck stops showing stems. The
 * controller decodes a new strip off screen and swaps it in whole, so it never blanks. (Not the
 * artwork: the controller marks a deck's artwork invalid from the first piece of a new JPEG to
 * its decode, and draws its no-artwork disc meanwhile.)
 *
 * Safety: the hooks run inside EP147's own calls on the objects EP147 passed them; nothing is
 * cached but the queue address a collector update named, which is only compared. The
 * collector and the queue are vtable-checked before use, the deck must be 0..3, and an unknown
 * deck is left alone. Nothing acts until all three table hooks are in; the kill switch
 * settings/azmod-jogstems.off installs none of them, and settings/azmod-jogstems-strip.off
 * leaves the strip to EP147. With diagnostics on (azmod-logs.on) page changes, strip sends and
 * each strip or artwork transfer in the deck frames (timed on the ioctl, first to last piece)
 * go to /tmp/azmod-jogart.log.
 */
#include "jog_stems_logic.h"

#define JOG_PADINFO_FILL EP_1ff1250 /* PlayerInfoData: fill the pad-mode record (this, info) */
#define JOG_PID_DECK 0x350L        /* PlayerInfoData: its deck 0..3 (constructor argument) */
#define JOG_INFO_PADINFO 0xd4L     /* PlayerInfo: Pad Info Display is On */
#define JOG_INFO_PADMODE 0xd5L     /* PlayerInfo: the pad mode the jog shows */
#define JOG_CUE_UPDATE EP_1ffbc60  /* CurrentHotCueCollect::update(this, bool, bool) */
#define JOG_CUE_VT EP_2df7ff8      /* gui::sub_display::CurrentHotCueCollect */
#define JOG_CUE_DECK 0x08L         /* its deck 0..3 (constructor argument, u32) */
#define JOG_CUE_VIEW 0x10L         /* the deck's ITrackInfoView: its TrackInfoQueue */
#define JOG_CUE_TABLE 0x8d0L       /* std::vector: the pad table EP147 last built */
#define JOG_TIQ_VT EP_2e509c8      /* device_adapter::subcpu_comm::deck_ucom::TrackInfoQueue */
#define JOG_TIQ_PADS 0x58L         /* (this, const vector*, header): send the pad table, 0x40 */
#define JOG_TIQ_PADS_FN EP_218a890 /* that slot's function, checked before patching */
#define JOG_TIQ_PREV 0x20L         /* (this, const vector*, u8, u8, u8): the preview strip, 0x2c */
#define JOG_TIQ_PREV_FN EP_218a6d8 /* that slot's function, checked before patching */

static int g_jog_hooks;                           /* atomics only: all three hooks are in */
static unsigned g_jog_active[AZMOD_STEM_DECKS];   /* atomics only: what the pad-mode hook asked */
static unsigned g_jog_sent[AZMOD_STEM_DECKS];     /* under g_jog_busy[deck]: what the jog has */
static long g_jog_hdr[AZMOD_STEM_DECKS];          /* atomics only: EP147's last table header */
static int g_jog_busy[AZMOD_STEM_DECKS];          /* atomics only */
static _Thread_local int t_jog_deck = -1;         /* the deck whose collector is running here */

/* The preview strip (HOT CUE page style): per deck, EP147's last strip (a copy of its columns
 * and flags, for putting it back) and what the jog has. Under g_jog_pbusy[deck]. The deck of a
 * queue is the one whose collector update last named that queue (g_jog_q, compared only). */
struct jog_prev_st {
    int have;               /* EP147 has sent this deck a strip */
    unsigned n, shown;      /* its column count; 0 = EP147's on the jog, else the word shown */
    long a, b, c;           /* EP147's flags for the strip */
    struct jog_prev_col ep[JOG_PREV_MAX];
    struct jog_prev_col ours[JOG_PREV_MAX]; /* the stem strip being sent */
};
struct jog_prev_vec {
    const struct jog_prev_col *begin, *end, *cap;
};
static struct jog_prev_st g_jp[AZMOD_STEM_DECKS];
static int g_jog_pbusy[AZMOD_STEM_DECKS]; /* atomics only */
static int g_jog_pstale[AZMOD_STEM_DECKS]; /* atomics only: EP147 sent a strip while busy */
static uintptr_t g_jog_q[AZMOD_STEM_DECKS]; /* atomics only: the deck's queue, by address */
static int g_jog_strip;                     /* atomics only: the strip is in use */
static int g_jog_timing;                    /* atomics only: transfer timing (diagnostics on) */

/* Events for the timing log, written by EP147's threads and the bus thread,
 * read by the message thread (jogstems_tick). Each slot is claimed by a counter and
 * published by its sequence word; every field is atomic. */
enum {
    JN_ACTIVE = 1, /* a: the deck's word (0 = stems not shown) */
    JN_XFER,       /* deck = bus 0/1, a: display | record << 8, b: pieces, c: gaps, e: JPEG
                    * bytes (artwork only), t2: last */
    JN_STRIP       /* a: 1 = ours (b: mask), 0 = EP147's again, 2 = EP147 sent one */
};
#define JOG_EV_N 64
static struct {
    unsigned seq; /* slot number + 1 once complete */
    int kind, deck;
    unsigned a, b, c, e;
    int64_t t, t2;
} g_jog_ev[JOG_EV_N];
static unsigned g_jog_ev_head;
static int64_t jog_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void jog_ev_put(int kind, int deck, unsigned a, unsigned b, unsigned c, unsigned e,
                       int64_t t, int64_t t2) {
    unsigned n = __atomic_fetch_add(&g_jog_ev_head, 1, __ATOMIC_RELAXED);
    unsigned i = n % JOG_EV_N;
    __atomic_store_n(&g_jog_ev[i].seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].kind, kind, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].deck, deck, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].a, a, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].b, b, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].c, c, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].e, e, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].t, t, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].t2, t2, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_ev[i].seq, n + 1, __ATOMIC_RELEASE);
}
static void jog_note(int kind, int deck, unsigned a) {
    jog_ev_put(kind, deck, a, 0, 0, 0, jog_now_ns(), 0);
}

/* Deck-frame ioctl (bus_interpose.c), before EP147's frame goes out: time each strip or
 * artwork transfer (diagnostics on). bus = 0/1 for the two deck buses; each is driven by one thread, and a busy flag
 * skips a frame rather than share the state. */
static struct jog_xfer g_jog_x[2];
static int g_jog_x_busy[2];
static void jog_tx_note(int bus, const void *arg) {
    if (bus < 0 || bus > 1 || !arg || !__atomic_load_n(&g_jog_timing, __ATOMIC_RELAXED))
        return;
    uint32_t size = 0;
    uint64_t ptr = 0;
    memcpy(&size, (const unsigned char *) arg + 4, 4);
    memcpy(&ptr, (const unsigned char *) arg + 8, 8);
    struct jog_msg m;
    if (ptr < 0x1000 || size > 256 ||
        !jog_msg_parse((const unsigned char *) (uintptr_t) ptr, size, &m))
        return;
    if (__atomic_exchange_n(&g_jog_x_busy[bus], 1, __ATOMIC_ACQUIRE))
        return;
    struct jog_xfer *x = &g_jog_x[bus];
    if (jog_xfer_feed(x, &m, jog_now_ns()))
        jog_ev_put(JN_XFER, bus, m.display | x->id << 8, x->pieces, x->gaps, x->len, x->first,
                   x->last);
    __atomic_store_n(&g_jog_x_busy[bus], 0, __ATOMIC_RELEASE);
}

/* The deck's pad-mode record, right after EP147 filled it. */
static void jog_padinfo_apply(long pid, long info) {
    unsigned d = *(const unsigned char *) (pid + JOG_PID_DECK);
    if (d >= AZMOD_STEM_DECKS)
        return; /* unknown deck: EP147's record as is */
    unsigned word = 0;
    if (__atomic_load_n(&g_jog_hooks, __ATOMIC_ACQUIRE)) {
        unsigned src = __atomic_load_n(&g_jog_src[d], __ATOMIC_RELAXED);
        word = jog_word((src & JOG_ON) != 0, __atomic_load_n(&g_jog_page[d], __ATOMIC_RELAXED),
                        src);
    }
    unsigned char *d4 = (unsigned char *) (info + JOG_INFO_PADINFO),
                  *d5 = (unsigned char *) (info + JOG_INFO_PADMODE);
    unsigned now = jog_padinfo_override(d4, d5, word);
    if (__atomic_exchange_n(&g_jog_active[d], now, __ATOMIC_ACQ_REL) != now &&
        __atomic_load_n(&g_jog_timing, __ATOMIC_RELAXED))
        jog_note(JN_ACTIVE, d, now);
}
static void (*g_orig_jog_padinfo)(long, long);
static void jog_padinfo_hook(long pid, long info) {
    g_orig_jog_padinfo(pid, info);
    jog_padinfo_apply(pid, info);
}

/* TrackInfoQueue slot +0x58: a pad table EP147 sends. Inside the deck's collector update, while
 * the deck shows stems, the stem table goes instead. */
static void (*g_orig_jog_pads)(long, const void *, long);
static void jog_cue_slot_hook(long queue, const void *vec, long hdr) {
    int d = t_jog_deck;
    if (d >= 0 && d < AZMOD_STEM_DECKS) {
        __atomic_store_n(&g_jog_hdr[d], hdr, __ATOMIC_RELAXED);
        unsigned active = __atomic_load_n(&g_jog_active[d], __ATOMIC_ACQUIRE);
        if (active & JOG_ON) {
            struct jog_cue cues[JOG_PADS];
            struct jog_cue_vec v = {cues, cues + JOG_PADS, cues + JOG_PADS};
            jog_cues_build(active, cues);
            g_orig_jog_pads(queue, &v, hdr);
            __atomic_store_n(&g_jog_sent[d], active, __ATOMIC_RELAXED);
            return;
        }
        __atomic_store_n(&g_jog_sent[d], 0, __ATOMIC_RELAXED); /* EP147's own is on the jog */
    }
    g_orig_jog_pads(queue, vec, hdr);
}

/* After the deck's collector update: send the stem table when the stem state changed, or
 * EP147's own table when the deck stopped showing stems. */
static void jog_cue_after(long self, int d) {
    long view = *(const long *) (self + JOG_CUE_VIEW);
    if (view <= 0x10000 || (view & 7) || *(const uintptr_t *) view != (uintptr_t) JOG_TIQ_VT)
        return;
    if (__atomic_exchange_n(&g_jog_busy[d], 1, __ATOMIC_ACQUIRE))
        return; /* this deck's step is running elsewhere: the next update catches up */
    unsigned sent = __atomic_load_n(&g_jog_sent[d], __ATOMIC_RELAXED);
    unsigned active = __atomic_load_n(&g_jog_active[d], __ATOMIC_ACQUIRE);
    long hdr = __atomic_load_n(&g_jog_hdr[d], __ATOMIC_RELAXED);
    switch (jog_cue_step(&sent, active)) {
    case JC_OURS: {
        struct jog_cue cues[JOG_PADS];
        struct jog_cue_vec v = {cues, cues + JOG_PADS, cues + JOG_PADS};
        jog_cues_build(active, cues);
        g_orig_jog_pads(view, &v, hdr);
        break;
    }
    case JC_EP147:
        g_orig_jog_pads(view, (const void *) (self + JOG_CUE_TABLE), hdr);
        break;
    default:
        break;
    }
    __atomic_store_n(&g_jog_sent[d], sent, __ATOMIC_RELAXED);
    __atomic_store_n(&g_jog_busy[d], 0, __ATOMIC_RELEASE);
}
/* TrackInfoQueue slot +0x20: a preview strip EP147 sends. Kept (columns and flags) for its
 * deck; while the deck shows stems, the stem strip goes instead. */
static void (*g_orig_jog_prev)(long, const void *, long, long, long);
static void jog_prev_ours(long queue, int d, unsigned word) {
    struct jog_prev_col *buf = g_jp[d].ours;
    int n = jog_prev_build(word, buf, jog_prev_ncols((int) g_jp[d].n));
    if (!n)
        return;
    struct jog_prev_vec v = {buf, buf + n, buf + n};
    g_orig_jog_prev(queue, &v, g_jp[d].a, g_jp[d].b, g_jp[d].c);
    g_jp[d].shown = word;
    jog_ev_put(JN_STRIP, d, 1, word & 0xfu, 0, 0, jog_now_ns(), 0);
}
static void jog_prev_slot_hook(long queue, const void *vec, long a, long b, long c) {
    int d = -1;
    for (int i = 0; i < AZMOD_STEM_DECKS; i++)
        if (__atomic_load_n(&g_jog_q[i], __ATOMIC_RELAXED) == (uintptr_t) queue)
            d = i;
    const struct jog_prev_vec *v = vec;
    long n = v ? (long) (v->end - v->begin) : -1;
    if (d < 0 || !__atomic_load_n(&g_jog_strip, __ATOMIC_ACQUIRE) || n <= 0 || n > JOG_PREV_MAX) {
        g_orig_jog_prev(queue, vec, a, b, c);
        return;
    }
    if (__atomic_exchange_n(&g_jog_pbusy[d], 1, __ATOMIC_ACQUIRE)) {
        g_orig_jog_prev(queue, vec, a, b, c); /* the jog shows EP147's; settled next update */
        __atomic_store_n(&g_jog_pstale[d], 1, __ATOMIC_RELEASE);
        return;
    }
    memcpy(g_jp[d].ep, v->begin, sizeof(struct jog_prev_col) * (size_t) n);
    g_jp[d].n = (unsigned) n;
    g_jp[d].a = a;
    g_jp[d].b = b;
    g_jp[d].c = c;
    g_jp[d].have = 1;
    jog_ev_put(JN_STRIP, d, 2, 0, 0, 0, jog_now_ns(), 0);
    unsigned active = __atomic_load_n(&g_jog_active[d], __ATOMIC_ACQUIRE);
    if (active & JOG_ON) {
        jog_prev_ours(queue, d, active);
    } else {
        g_orig_jog_prev(queue, vec, a, b, c);
        g_jp[d].shown = 0;
    }
    __atomic_store_n(&g_jog_pbusy[d], 0, __ATOMIC_RELEASE);
}
/* After the deck's collector update (the deck's queue, validated): the stem strip on an edge
 * of the stem state, EP147's strip back when the deck stops showing stems. */
static void jog_prev_after(long view, int d) {
    if (!__atomic_load_n(&g_jog_strip, __ATOMIC_ACQUIRE))
        return;
    if (__atomic_exchange_n(&g_jog_pbusy[d], 1, __ATOMIC_ACQUIRE))
        return;
    if (__atomic_exchange_n(&g_jog_pstale[d], 0, __ATOMIC_ACQ_REL))
        g_jp[d].shown = 0;
    unsigned active = __atomic_load_n(&g_jog_active[d], __ATOMIC_ACQUIRE);
    if (g_jp[d].have) {
        if ((active & JOG_ON) && g_jp[d].shown != active) {
            jog_prev_ours(view, d, active);
        } else if (!(active & JOG_ON) && g_jp[d].shown) {
            struct jog_prev_vec v = {g_jp[d].ep, g_jp[d].ep + g_jp[d].n, g_jp[d].ep + g_jp[d].n};
            g_orig_jog_prev(view, &v, g_jp[d].a, g_jp[d].b, g_jp[d].c);
            g_jp[d].shown = 0;
            jog_ev_put(JN_STRIP, d, 0, 0, 0, 0, jog_now_ns(), 0);
        }
    }
    __atomic_store_n(&g_jog_pbusy[d], 0, __ATOMIC_RELEASE);
}

static void (*g_orig_jog_cue_update)(long, long, long);
static void jog_cue_update_hook(long self, long changed, long reset) {
    int d = -1;
    if (self > 0x10000 && !(self & 7) && *(const uintptr_t *) self == (uintptr_t) JOG_CUE_VT &&
        __atomic_load_n(&g_jog_hooks, __ATOMIC_ACQUIRE)) {
        unsigned u = *(const unsigned *) (self + JOG_CUE_DECK);
        if (u < AZMOD_STEM_DECKS)
            d = (int) u;
    }
    t_jog_deck = d;
    g_orig_jog_cue_update(self, changed, reset);
    t_jog_deck = -1;
    if (d >= 0) {
        jog_cue_after(self, d);
        long view = *(const long *) (self + JOG_CUE_VIEW);
        if (view > 0x10000 && !(view & 7) && *(const uintptr_t *) view == (uintptr_t) JOG_TIQ_VT) {
            __atomic_store_n(&g_jog_q[d], (uintptr_t) view, __ATOMIC_RELAXED);
            jog_prev_after(view, d);
        }
    }
}

/* Constructor phase (install_static_hooks). The pad-mode hook goes in last: without the table
 * hooks it would show EP147's hot cues on a stems page. Nothing acts until all three are in. */
static void install_jogstems(void) {
    if (access("/home/root/settings/azmod-jogstems.off", F_OK) == 0)
        return;
    if (access("/home/root/settings/azmod-logs.on", F_OK) == 0)
        __atomic_store_n(&g_jog_timing, 1, __ATOMIC_RELAXED);
    uintptr_t slot = (uintptr_t) JOG_TIQ_VT + JOG_TIQ_PADS;
    if (!JOG_TIQ_VT || *(const uintptr_t *) slot != (uintptr_t) JOG_TIQ_PADS_FN)
        return; /* not the slot this file was written against */
    if (azm_install_hook(JOG_CUE_UPDATE, (void *) jog_cue_update_hook,
                         (void **) &g_orig_jog_cue_update) != 1)
        return;
    if (azm_patch_vtable(slot, (void *) jog_cue_slot_hook, (void **) &g_orig_jog_pads) != 1)
        return;
    if (azm_install_hook(JOG_PADINFO_FILL, (void *) jog_padinfo_hook,
                         (void **) &g_orig_jog_padinfo) != 1)
        return;
    __atomic_store_n(&g_jog_hooks, 1, __ATOMIC_RELEASE);
    /* The preview strip: its own kill switch, and only with the slot this was written for. */
    uintptr_t pslot = (uintptr_t) JOG_TIQ_VT + JOG_TIQ_PREV;
    if (access("/home/root/settings/azmod-jogstems-strip.off", F_OK) != 0 && JOG_TIQ_PREV_FN &&
        *(const uintptr_t *) pslot == (uintptr_t) JOG_TIQ_PREV_FN &&
        azm_patch_vtable(pslot, (void *) jog_prev_slot_hook, (void **) &g_orig_jog_prev) == 1)
        __atomic_store_n(&g_jog_strip, 1, __ATOMIC_RELEASE);
}

/* Message thread (rtick_hook), diagnostics on: write the events to /tmp/azmod-jogart.log. */
static void jogstems_tick(void) {
    static unsigned tail;
    if (!__atomic_load_n(&g_jog_timing, __ATOMIC_RELAXED) ||
        !__atomic_load_n(&g_jog_hooks, __ATOMIC_ACQUIRE))
        return;
    unsigned head = __atomic_load_n(&g_jog_ev_head, __ATOMIC_ACQUIRE);
    if (tail == head)
        return;
    if (head - tail > JOG_EV_N)
        tail = head - JOG_EV_N; /* overwritten: the log says how many were lost */
    FILE *f = azlog_open("/tmp/azmod-jogart.log", "a");
    for (; tail != head; tail++) {
        unsigned i = tail % JOG_EV_N;
        if (__atomic_load_n(&g_jog_ev[i].seq, __ATOMIC_ACQUIRE) != tail + 1)
            break; /* not complete yet: next tick */
        int kind = __atomic_load_n(&g_jog_ev[i].kind, __ATOMIC_RELAXED);
        int deck = __atomic_load_n(&g_jog_ev[i].deck, __ATOMIC_RELAXED);
        unsigned a = __atomic_load_n(&g_jog_ev[i].a, __ATOMIC_RELAXED),
                 b = __atomic_load_n(&g_jog_ev[i].b, __ATOMIC_RELAXED),
                 c = __atomic_load_n(&g_jog_ev[i].c, __ATOMIC_RELAXED),
                 e = __atomic_load_n(&g_jog_ev[i].e, __ATOMIC_RELAXED);
        int64_t t = __atomic_load_n(&g_jog_ev[i].t, __ATOMIC_RELAXED),
                t2 = __atomic_load_n(&g_jog_ev[i].t2, __ATOMIC_RELAXED);
        if (!f)
            continue;
        double ms = (double) t / 1e6;
        switch (kind) {
        case JN_ACTIVE:
            fprintf(f, "%.3f deck %d page %s mask %x\n", ms, deck + 1,
                    (a & JOG_ON) ? "stems" : "stock", a & 0xfu);
            break;
        case JN_XFER:
            if ((a >> 8) == JOG_MSG_PREV)
                fprintf(f, "%.3f bus %d display %#x strip out: %u pieces, %u gaps, %.1f ms\n", ms,
                        deck, a & 0xffu, b, c, (double) (t2 - t) / 1e6);
            else
                fprintf(f, "%.3f bus %d display %#x artwork out: %u pieces, %u gaps, %u B, %.1f ms\n",
                        ms, deck, a & 0xffu, b, c, e, (double) (t2 - t) / 1e6);
            break;
        case JN_STRIP:
            fprintf(f, "%.3f deck %d strip: %s %x\n", ms, deck + 1,
                    a == 1 ? "stems" : a == 2 ? "EP147 sent"
                                              : "EP147's back",
                    b);
            break;
        default:
            break;
        }
        fflush(f);
    }
    if (f)
        fclose(f);
}
