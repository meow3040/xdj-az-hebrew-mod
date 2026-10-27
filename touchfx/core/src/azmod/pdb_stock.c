/* SPDX-License-Identifier: GPL-3.0-only */
/* pdb_stock.c: when to re-sync export.pdb after EP147's own playlist writes.
 *
 * Stock TAG LIST > CREATE PLAYLIST and stock DELETE / ALL DELETE write exportLibrary.db
 * through EP147's device playlist writer and leave export.pdb as it was, so the stick's
 * two libraries drift apart. azmod notices those writes (the writer's vtable slots, inside
 * the library thread's message) and brings export.pdb back in line with the same pdb sync
 * its own edits use. This runs always, whether or not the Playlist Editing mod is on: a
 * stock playlist write is a stock playlist write, and the Device Library must mirror it
 * either way. (The Playlist Editing mod, meaning the drag gesture and the edit menu, stays
 * opt-in; only this stock-write sync is unconditional.)
 *
 * This file is only the schedule, libc only, so a host test compiles it natively:
 *   pst_note()     a library thread finished a message that wrote playlists for `media`;
 *                  the sync is due PST_DEBOUNCE_MS after the last such write, so a burst
 *                  (a create plus its N track rows, several deletes in a row) is one sync;
 *   pst_any_due()  the message thread asks whether to wake the library threads; entries
 *                  nobody serviced within PST_EXPIRE_MS (the stick went away) are dropped;
 *   pst_service()  a library thread at a message boundary takes its stick's due entry and
 *                  runs the sync once, if the stick's mount resolves.
 * An unknown stick (media 0, or a mount that does not resolve) is never synced. */
#include <pthread.h>
#include <stddef.h>
#include <string.h>

#define PST_N 4               /* sticks at once (two ports, with room for a re-insert) */
#define PST_DEBOUNCE_MS 1000  /* quiet time after the last stock write before the sync */
#define PST_EXPIRE_MS 15000   /* a due entry nobody took this long after it fell due */

struct pst {
    pthread_mutex_t mu;
    struct {
        unsigned media; /* 0 = free */
        long long due;
    } e[PST_N];
};
/* clang-format off */
#define PST_INIT {PTHREAD_MUTEX_INITIALIZER, {{0, 0}}}
/* clang-format on */

/* 1 = noted, 0 = ignored (unknown stick), -1 = table full */
static int pst_note(struct pst *p, unsigned media, long long now) {
    if (!media)
        return 0;
    int rc = -1;
    pthread_mutex_lock(&p->mu);
    int k = -1;
    for (int i = 0; i < PST_N; i++)
        if (p->e[i].media == media) {
            k = i;
            break;
        }
    for (int i = 0; k < 0 && i < PST_N; i++)
        if (!p->e[i].media)
            k = i;
    if (k >= 0) {
        p->e[k].media = media;
        p->e[k].due = now + PST_DEBOUNCE_MS; /* every write in the burst pushes it out */
        rc = 1;
    }
    pthread_mutex_unlock(&p->mu);
    return rc;
}

/* 1 = some stick's sync is due now (wake the library threads) */
static int pst_any_due(struct pst *p, long long now) {
    int any = 0;
    pthread_mutex_lock(&p->mu);
    for (int i = 0; i < PST_N; i++) {
        if (!p->e[i].media)
            continue;
        if (now - p->e[i].due > PST_EXPIRE_MS)
            p->e[i].media = 0; /* nobody took it in time: the stick went away */
        else if (now >= p->e[i].due)
            any = 1;
    }
    pthread_mutex_unlock(&p->mu);
    return any;
}

/* 1 = `media` was due and is now taken (cleared): the caller syncs it exactly once */
static int pst_take(struct pst *p, unsigned media, long long now) {
    int got = 0;
    if (!media)
        return 0;
    pthread_mutex_lock(&p->mu);
    for (int i = 0; i < PST_N; i++) {
        if (p->e[i].media != media)
            continue;
        if (now >= p->e[i].due) {
            p->e[i].media = 0;
            got = 1;
        }
    }
    pthread_mutex_unlock(&p->mu);
    return got;
}

/* On `media`'s library thread, at a message boundary. mount() fills the stick's root
 * ("/media/usb/sda") for this library, returning 0 when it cannot say; sync() runs the pdb
 * sync. 1 = synced, 0 = nothing due, -1 = due but the stick is unknown (dropped). */
static int pst_service(struct pst *p, unsigned media, long long now,
                       int (*mount)(void *ctx, char *out, size_t n),
                       void (*sync)(void *ctx, const char *root), void *ctx) {
    if (!pst_take(p, media, now))
        return 0;
    char root[300];
    root[0] = 0;
    if (!mount(ctx, root, sizeof root) || root[0] != '/')
        return -1;
    sync(ctx, root);
    return 1;
}
