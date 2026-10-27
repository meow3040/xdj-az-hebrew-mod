/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file cm_updall.h
 * @brief Community Mods > Updates: Update all, one install after another.
 *
 * The fetcher serves one request at a time and drops a request sent while it is busy, so a run
 * sends the next install only after the previous one is finished: the status file shows `done`
 * for that install (op `install`, the same id) and the catalog has been read again since the
 * request was sent (the fetcher rewrites the catalog before it writes `done`).
 *
 * The run takes the ids that had an update when it started, in the order shown. Before each send
 * it asks the caller whether the id still qualifies (an update this deck can take, not removed);
 * one that stopped qualifying is skipped. It ends:
 *   - CMU_FINISHED: every id was installed or skipped;
 *   - CMU_FAILED: the fetcher reported `error` (its message stays in the status file, so the
 *     screen keeps showing it); the ids after it are not tried;
 *   - CMU_STOPPED: the fetcher finished a different request than the one sent (another request
 *     replaced it before it was served);
 *   - CMU_QUIET: the status did not change for CMU_QUIET_MS after a send or between two steps
 *     of an install (no fetcher is answering);
 *   - cmu_cancel (a reload was asked for): it ends with no result.
 *
 * Inputs are the status fields and two counters the caller bumps each time it re-reads the
 * status file and the catalog. Pure logic: no globals, no allocation, no clock of its own.
 */
#ifndef AZMOD_CM_UPDALL_H
#define AZMOD_CM_UPDALL_H

#include <stdio.h>
#include <string.h>

#define CMU_MAX 128
#define CMU_IDLEN 33
#define CMU_QUIET_MS 120000L

enum {
    CMU_OFF = 0,  /* no run */
    CMU_WAIT,     /* a run is waiting for the fetcher */
    CMU_SEND,     /* send `install` for cmu_cur() now */
    CMU_FINISHED, /* the run ended: each id updated or skipped */
    CMU_FAILED,   /* the run ended on the fetcher's error */
    CMU_STOPPED,  /* the run ended: a different request was served */
    CMU_QUIET     /* the run ended: the fetcher did not answer */
};

typedef struct {
    int on;     /* a run is in progress */
    int n;      /* ids taken at the start */
    int pos;    /* the id sent, or the next one to look at */
    int sent;   /* the install for ids[pos] was sent and is not finished */
    int result; /* how the last run ended (CMU_FINISHED ... CMU_QUIET), CMU_OFF while running or cancelled */
    unsigned sgen0, cgen0, slast;
    long t; /* ms of the send or of the last status change since */
    char ids[CMU_MAX][CMU_IDLEN];
} cmu_run;

typedef struct {
    const char *state, *op, *id; /* the status file's fields ("" when absent) */
    unsigned sgen, cgen;         /* bumped on each re-read of the status file / the catalog */
    long now;                    /* ms, monotonic */
} cmu_in;

/* 1 = the id still has an update this deck can take */
typedef int (*cmu_ok_fn)(const char *id, void *user);

static inline int cmu_busy_state(const char *st) {
    return st && st[0] && strcmp(st, "idle") && strcmp(st, "done") && strcmp(st, "error");
}

static inline void cmu_start(cmu_run *r, const char *const *ids, int n) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < n && r->n < CMU_MAX; i++) {
        if (!ids[i] || !ids[i][0] || strlen(ids[i]) >= CMU_IDLEN) continue;
        snprintf(r->ids[r->n++], CMU_IDLEN, "%s", ids[i]);
    }
    r->on = r->n > 0;
}

static inline void cmu_cancel(cmu_run *r) {
    r->on = 0;
    r->sent = 0;
    r->result = CMU_OFF;
}

static inline int cmu_end(cmu_run *r, int result) {
    r->on = 0;
    r->sent = 0;
    r->result = result;
    return result;
}

/* the id the run sent or is about to send, or "" */
static inline const char *cmu_cur(const cmu_run *r) {
    return r->on && r->pos < r->n ? r->ids[r->pos] : "";
}

/* One step: call it whenever the status or the catalog may have changed (every tick is fine).
 * CMU_SEND: the caller sends `install` for cmu_cur() now. A finished run returns its result
 * once, then CMU_OFF. */
static inline int cmu_step(cmu_run *r, const cmu_in *in, cmu_ok_fn ok, void *user) {
    if (!r->on) return CMU_OFF;
    const char *st = in->state ? in->state : "", *op = in->op ? in->op : "", *id = in->id ? in->id : "";
    if (r->sent) {
        if (in->sgen != r->slast) {
            r->slast = in->sgen;
            r->t = in->now;
        }
        if (in->sgen == r->sgen0) /* nothing from the fetcher since the send */
            return in->now - r->t > CMU_QUIET_MS ? cmu_end(r, CMU_QUIET) : CMU_WAIT;
        if (!strcmp(st, "error")) return cmu_end(r, CMU_FAILED);
        if (!strcmp(st, "done")) {
            if (strcmp(op, "install") || strcmp(id, r->ids[r->pos])) return cmu_end(r, CMU_STOPPED);
            if (in->cgen == r->cgen0 && ok(r->ids[r->pos], user)) /* the catalog not read again yet */
                return in->now - r->t > CMU_QUIET_MS ? cmu_end(r, CMU_QUIET) : CMU_WAIT;
            r->sent = 0;
            r->pos++;
        } else
            return in->now - r->t > CMU_QUIET_MS ? cmu_end(r, CMU_QUIET) : CMU_WAIT;
    }
    while (r->pos < r->n && !ok(r->ids[r->pos], user)) r->pos++;
    if (r->pos >= r->n) return cmu_end(r, CMU_FINISHED);
    if (cmu_busy_state(st)) return CMU_WAIT; /* never sent while busy: the fetcher would drop it */
    r->sent = 1;
    r->sgen0 = r->slast = in->sgen;
    r->cgen0 = in->cgen;
    r->t = in->now;
    return CMU_SEND;
}

/* The button's text: "Updating 2 of 3..." while a run is on, else "Update all (N)" for the N
 * updates there are now. */
static inline void cmu_label(const cmu_run *r, int avail, char *buf, size_t n) {
    if (r->on) snprintf(buf, n, "Updating %d of %d...", (r->pos < r->n ? r->pos : r->n - 1) + 1, r->n);
    else snprintf(buf, n, "Update all (%d)", avail);
}

#endif
