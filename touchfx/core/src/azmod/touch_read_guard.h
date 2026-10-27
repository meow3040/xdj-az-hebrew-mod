/* SPDX-License-Identifier: GPL-3.0-only */
/* Guard for EP147's own touch-panel reader.
 *
 * The reader opens /dev/input/by-id/gt928, reads up to 0x600 bytes into a buffer on its
 * stack, and hands the byte count it got back from read() to the parser as a signed int.
 * The parser treats any count above 23 as (count / 24) input events and walks that many
 * 24-byte records. A count of 0 or of 1..23 ends the pass with no events, and a count that is
 * not a multiple of 24 simply ignores the tail. A count of -1 (read() failing) is widened to
 * 64 bits unsigned, becomes about 7.7e17 "events", and the parser reads far past the end of
 * the stack buffer until it faults. The same code is in both firmware builds.
 *
 * So the one value the reader cannot survive is a failed read. For that descriptor only,
 * a failed read is reported as 0 bytes (no events), which the reader handles as "nothing
 * arrived" and goes back to waiting. Every other descriptor, and every successful read of
 * this one, passes through untouched.
 *
 * The descriptor is recognized when it is opened, by the exact path, and forgotten when it
 * is closed, so a number reused by an unrelated file is never treated as the touch reader.
 * The table is a few atomic ints; the success path of read() does not touch it, and a failed read costs one short scan
 * of it. A device that keeps failing makes the reader loop on its select/read pair without
 * crashing; nothing is slept here because this runs on the bus read path. */
#ifndef TOUCH_READ_GUARD_H
#define TOUCH_READ_GUARD_H

#include <string.h>
#include <sys/types.h>

#define TG_PATH "/dev/input/by-id/gt928"
#define TG_SLOTS 4

static int g_tg_fd[TG_SLOTS] = {-1, -1, -1, -1}; /* __atomic only */

/* Forget a descriptor number (close, dup2/dup3 onto it, or an open that returned it). */
static void tg_forget(int fd) {
    if (fd < 0)
        return;
    for (int i = 0; i < TG_SLOTS; i++) {
        int cur = fd;
        __atomic_compare_exchange_n(&g_tg_fd[i], &cur, -1, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
    }
}

/* Record fd when path is the touch reader's device. */
static void tg_note(int fd, const char *path) {
    tg_forget(fd);
    if (fd < 0 || !path || strcmp(path, TG_PATH) != 0)
        return;
    for (int i = 0; i < TG_SLOTS; i++) {
        int cur = -1;
        if (__atomic_compare_exchange_n(&g_tg_fd[i], &cur, fd, 0, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED))
            return;
    }
}

static int tg_tracked(int fd) {
    if (fd < 0)
        return 0;
    for (int i = 0; i < TG_SLOTS; i++)
        if (__atomic_load_n(&g_tg_fd[i], __ATOMIC_ACQUIRE) == fd)
            return 1;
    return 0;
}

/* The count to hand back for a read() on fd that returned r: a failed read on the touch
 * reader's descriptor becomes 0, anything else is unchanged. Only called when r < 0. */
static ssize_t tg_result(int fd, ssize_t r) {
    return (r < 0 && tg_tracked(fd)) ? 0 : r;
}

#endif
