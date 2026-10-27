/* SPDX-License-Identifier: GPL-3.0-only */
/* bus_cfg.h: what the overlay thread hands the panel-bus threads, without a lock.
 *
 * The read() and ioctl() interposers run on EP147's panel-bus threads and edit control and
 * LED frames according to Mod Menu settings that the overlay thread changes. A setting made
 * of several fields, written one store at a time, could be read half old and half new: an
 * x-pad position byte from the new config with the zone byte of the old one, or a pad color
 * with the new red and the old green. The bus threads must not wait, so there are no locks:
 *
 *   - the settings the bus reads are packed into one aligned 64-bit word (bus_cfg_pack),
 *     stored with release by the overlay thread and loaded once per frame with acquire: a
 *     reader gets one whole published combination;
 *   - a pad-mode color is one 32-bit word, 0x01RRGGBB (bit 24 = a pick is set);
 *   - a deck's 32-byte pad LED block goes through a sequence counter (seqblk): the writer
 *     makes it odd, writes, makes it even; a reader that sees it odd or changed keeps the
 *     block it copied last time instead of retrying.
 *
 * Header-only, no azmod dependencies, so the unit tests compile it as it ships. */
#ifndef AZMOD_BUS_CFG_H
#define AZMOD_BUS_CFG_H
#include <stdint.h>
#include <string.h>

/* ---- the settings word ---- */
#define BC_PLEDIT (1ull << 0)        /* Playlist Editing (az_on(AZF_PLAYLIST_EDITING)) */
#define BC_STEMSBL2 (1ull << 1)      /* "Stems on BEAT LOOP 2" (az_on(AZF_STEM_PADS_PAGE)) */
#define BC_REORDER_HOOKS (1ull << 2) /* every reorder hook installed at start */
#define BC_REORDER_OFF (1ull << 3)   /* a reorder hook declined: not again until a restart */
#define BC_XPOS_SH 8                 /* x-pad position byte, 0..127 */
#define BC_XTOUCH_SH 16              /* x-pad touch byte, 0xff = none */
#define BC_XMASK_SH 24               /* x-pad touch mask, 1..255 */
#define BC_XSPAN_SH 32               /* x-pad full scale - 1, 1..255 (span 2..256) */
#define BC_XZONE_SH 40               /* x-pad zone byte, 0xff = none */
#define BC_VALID (1ull << 63)        /* published at least once (or the default below) */

/* The x-pad's bytes on the mixer bus (overridable by /home/root/settings/azmod-xpad):
 * the position byte walks 0..124 (a 7-bit strip, so full scale 128) and the zone bitmap byte
 * follows it; 0x1c next to them is a once-per-boot "strip used" latch, never the position. */
#define XPAD_DEFAULT_POS 0x1d
#define XPAD_DEFAULT_SPAN 128
#define XPAD_DEFAULT_ZONE 0x1e
/* what the bus reads before the overlay's first publish: the defaults, no mod bits */
#define BC_DEFAULT                                                                        \
    (BC_VALID | ((uint64_t) XPAD_DEFAULT_POS << BC_XPOS_SH) | (0xffull << BC_XTOUCH_SH) | \
     (0xffull << BC_XMASK_SH) | ((uint64_t) (XPAD_DEFAULT_SPAN - 1) << BC_XSPAN_SH) |     \
     ((uint64_t) XPAD_DEFAULT_ZONE << BC_XZONE_SH))

struct bus_xpad {
    int pos, touch, mask, span, zone; /* touch / zone < 0 = not used */
};
static inline uint64_t bus_cfg_pack(int pledit, int stemsbl2, int reorder_hooks, int reorder_off,
                                    const struct bus_xpad *x) {
    uint64_t c = BC_VALID;
    if (pledit)
        c |= BC_PLEDIT;
    if (stemsbl2)
        c |= BC_STEMSBL2;
    if (reorder_hooks)
        c |= BC_REORDER_HOOKS;
    if (reorder_off)
        c |= BC_REORDER_OFF;
    c |= (uint64_t) (x->pos & 0x7f) << BC_XPOS_SH;
    c |= (uint64_t) (x->touch >= 0 && x->touch < 128 ? x->touch : 0xff) << BC_XTOUCH_SH;
    c |= (uint64_t) ((x->mask & 0xff) ? (x->mask & 0xff) : 0xff) << BC_XMASK_SH;
    c |= (uint64_t) ((x->span >= 2 && x->span <= 256 ? x->span : 128) - 1) << BC_XSPAN_SH;
    c |= (uint64_t) (x->zone >= 0 && x->zone < 128 ? x->zone : 0xff) << BC_XZONE_SH;
    return c;
}
static inline void bus_cfg_xpad(uint64_t c, struct bus_xpad *x) {
    unsigned t = (unsigned) (c >> BC_XTOUCH_SH) & 0xff, z = (unsigned) (c >> BC_XZONE_SH) & 0xff;
    x->pos = (int) ((c >> BC_XPOS_SH) & 0x7f);
    x->touch = t == 0xff ? -1 : (int) t;
    x->mask = (int) ((c >> BC_XMASK_SH) & 0xff);
    x->span = (int) ((c >> BC_XSPAN_SH) & 0xff) + 1;
    x->zone = z == 0xff ? -1 : (int) z;
}
/* Playlist Editing's reorder gesture is live: hooks in, none declined, the mod on */
static inline int bus_cfg_reorder(uint64_t c) {
    return (c & (BC_PLEDIT | BC_REORDER_HOOKS | BC_REORDER_OFF)) == (BC_PLEDIT | BC_REORDER_HOOKS);
}

/* ---- a pad-mode color ---- */
#define BC_RGB_SET (1u << 24)
static inline uint32_t bus_rgb_pack(int set, uint32_t rgb) {
    return set ? (BC_RGB_SET | (rgb & 0xffffffu)) : 0;
}

/* ---- a 32-byte block behind a sequence counter ---- */
struct seqblk {
    unsigned seq; /* odd while the writer is inside; only ever touched with __atomic builtins */
    uint64_t w[4]; /* the 32 bytes, as four words, likewise */
};
/* one writer (the overlay thread) */
static inline void seqblk_write(struct seqblk *b, const unsigned char src[32]) {
    unsigned s = __atomic_load_n(&b->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&b->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE); /* odd before any word changes */
    for (int i = 0; i < 4; i++) {
        uint64_t v;
        memcpy(&v, src + 8 * i, 8);
        __atomic_store_n(&b->w[i], v, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&b->seq, s + 2, __ATOMIC_RELEASE); /* even after every word */
}
/* any reader, never waits: 1 = dst holds one whole published block, 0 = the writer was
 * inside (dst unchanged; use the last good copy) */
static inline int seqblk_read(struct seqblk *b, unsigned char dst[32]) {
    unsigned s1 = __atomic_load_n(&b->seq, __ATOMIC_ACQUIRE);
    if (s1 & 1)
        return 0;
    uint64_t v[4];
    for (int i = 0; i < 4; i++)
        v[i] = __atomic_load_n(&b->w[i], __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE); /* the words before the second look at seq */
    if (__atomic_load_n(&b->seq, __ATOMIC_RELAXED) != s1)
        return 0;
    memcpy(dst, v, 32);
    return 1;
}
#endif
