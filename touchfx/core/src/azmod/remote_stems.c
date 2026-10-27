/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file remote_stems.c
 * @brief MENU > MAKE STEMS: queue the selected track (or playlist) for stem separation on a PC/Mac.
 *
 * The row's (media, content_id) is read on the library thread of the stick whose
 * own database carries that media (mlserver_onmsg_hook), never guessed across
 * sticks. The request is written to the stick as <mount>/MODS/azstems-queue/<n>.req
 * on the message thread, so the queue travels with the USB. The transfer itself is
 * done by the azrs.sh helper script, a systemd unit outside EP147 that is started here
 * when there is work; an EP147 restart does not stop it.
 *
 * rs_tick() runs on the message thread from msg_tick: it turns the helper's
 * /tmp/azrs/status line into EP147's top strip when the helper marks it as a
 * problem, restarts the helper when a stick holds queued work, and tracks the Mod
 * Menu row Stems > Remote Stems (AZF_REMOTE_STEMS) live. The hooks are installed only when the feature
 * is enabled at start. A loaded track is not updated; the stems load next time.
 * Playlists are queued by a detached worker thread. The AZ-MODS desktop app makes
 * the stems and writes them back to that USB's STEMS folder.
 */
/* The helper ships in MODS/deck/lib, which the loader keeps in the stage, so it
 * is present with or without the stick. */
#define RS_HELPER_STAGE "/home/root/settings/az-usbload/deck/lib/azrs.sh"
static int az_banner_strip(const char *text, int dur_ms); /* banner.c */
static int g_rs_on;   /* the feature is on now (installed + Mod Menu toggle) */
static int g_rs_inst; /* its hooks are in (decided once, at start) */
static int (*g_has_stems_fn)(const char *); /* azstem_has_stems, resolved by the stems mark */
static struct {
    unsigned cid, pl;
    char mount[128], path[600], title[200];
    struct rs_trk {
        unsigned cid;
        char path[600], title[200];
    } *trk;
    int ntrk;
} g_rs;
#define RS_PL_MAX 2000
static _Atomic int g_rs_busy; /* a playlist is being queued on its worker */
static int rs_arm(unsigned media, unsigned cid, unsigned pl) { /* message thread */
    if (!__atomic_load_n(&g_rs_on, __ATOMIC_RELAXED))
        return -70;
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE ||
        __atomic_load_n(&g_rs_busy, __ATOMIC_ACQUIRE))
        return -2;
    if (!media || (!cid && !pl))
        return -3;
    g_rs.cid = cid;
    g_rs.pl = pl;
    g_rs.mount[0] = g_rs.path[0] = g_rs.title[0] = 0;
    g_mv.op = MV_OP_RSREAD;
    g_mv.media = media;
    g_mv.cid = cid;
    g_mv.pl = 0;
    g_mv.from = g_mv.to = 0;
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("MAKE STEMS armed: cid=%u pl=%u media %04x\n", cid, pl, media);
    return 0;
}
/* MENU > CANCEL STEMS on a queued row: the same read as MAKE STEMS (the stick,
 * and a playlist's tracks), then rs_enqueue cancels instead of queueing. */
static int g_rs_cancel;   /* message thread: the armed read is a cancel */
static int g_rs_cancel_n; /* how many requests it canceled */
static int rs_request(unsigned media, unsigned cid) {
    g_rs_cancel = 0;
    return rs_arm(media, cid, 0);
}
static int rs_cancel_request(unsigned media, unsigned cid, unsigned pl) {
    g_rs_cancel = 1;
    int rc = rs_arm(media, cid, pl);
    if (rc)
        g_rs_cancel = 0;
    return rc;
}
/* MENU > MAKE STEMS on the TAG LIST: every tagged track. The content_ids are read
 * as COPY n TAGGED reads them (MV_OP_TAGREAD, g_tag_cids); this marker then takes
 * the playlist path, with rs_read_tagged in place of the playlist SQL. */
#define RS_TAGGED 0xfffffffeu
static int rs_request_tagged(unsigned media) {
    return rs_arm(media, 0, RS_TAGGED);
}
/* MENU > MAKE STEMS on a playlist row: every track in it, and in everything
 * under it for a folder, read on the owning stick. */
static int rs_request_playlist(unsigned media, unsigned pl) {
    g_rs_cancel = 0;
    return rs_arm(media, 0, pl);
}
static int rs_read_playlist(uintptr_t conn, FILE *lg) {
    static int (*coltype)(void *, int);
    void *st = 0;
    free(g_rs.trk);
    g_rs.trk = 0;
    g_rs.ntrk = 0;
    if (!sq_text((void *) conn, "SELECT name FROM playlist WHERE playlist_id = ?1", g_rs.pl,
                 g_rs.title, sizeof g_rs.title))
        return -20;
    if (g_sq.prepare((void *) conn,
                     "WITH RECURSIVE sub(id) AS (SELECT ?1 UNION SELECT p.playlist_id FROM "
                     "playlist p JOIN sub ON p.playlist_id_parent = sub.id) "
                     "SELECT DISTINCT c.content_id, c.path, c.title FROM playlist_content pc JOIN "
                     "sub ON pc.playlist_id = sub.id "
                     "JOIN content c ON c.content_id = pc.content_id",
                     -1, &st, 0) != 0 ||
        !st)
        return -42;
    (void) coltype;
    g_sq.bind_int64(st, 1, g_rs.pl);
    g_rs.trk = calloc(RS_PL_MAX, sizeof *g_rs.trk);
    while (g_rs.trk && g_rs.ntrk < RS_PL_MAX && g_sq.step(st) == SQLITE_ROW) {
        struct rs_trk *t = &g_rs.trk[g_rs.ntrk];
        const unsigned char *p = g_sq.column_text(st, 1), *ti = g_sq.column_text(st, 2);
        if (!p || p[0] != '/')
            continue;
        t->cid = (unsigned) g_sq.column_int64(st, 0);
        snprintf(t->path, sizeof t->path, "%s", (const char *) p);
        snprintf(t->title, sizeof t->title, "%s", ti ? (const char *) ti : "");
        for (char *c = t->title; *c; c++)
            if (*c == '\n' || *c == '\r')
                *c = ' ';
        g_rs.ntrk++;
    }
    g_sq.finalize(st);
    if (lg)
        fprintf(lg, "MAKE STEMS read playlist %u \"%s\": %d tracks\n", g_rs.pl, g_rs.title,
                g_rs.ntrk);
    return g_rs.trk ? (g_rs.ntrk ? 0 : -47) : -42;
}
/* the tagged tracks (g_tag_cids, read by MV_OP_TAGREAD just before): path + title each */
static int rs_read_tagged(uintptr_t conn, FILE *lg) {
    free(g_rs.trk);
    g_rs.trk = 0;
    g_rs.ntrk = 0;
    snprintf(g_rs.title, sizeof g_rs.title, "the Tag List");
    g_rs.trk = calloc(RS_PL_MAX, sizeof *g_rs.trk);
    for (int i = 0; g_rs.trk && i < g_tag_n && g_rs.ntrk < RS_PL_MAX; i++) {
        void *st = 0;
        if (g_sq.prepare((void *) conn, "SELECT path, title FROM content WHERE content_id = ?1", -1,
                         &st, 0) != 0 ||
            !st)
            continue;
        g_sq.bind_int64(st, 1, g_tag_cids[i]);
        if (g_sq.step(st) == SQLITE_ROW) {
            const unsigned char *p = g_sq.column_text(st, 0), *ti = g_sq.column_text(st, 1);
            if (p && p[0] == '/') {
                struct rs_trk *t = &g_rs.trk[g_rs.ntrk++];
                t->cid = g_tag_cids[i];
                snprintf(t->path, sizeof t->path, "%s", (const char *) p);
                snprintf(t->title, sizeof t->title, "%s", ti ? (const char *) ti : "");
                for (char *c = t->title; *c; c++)
                    if (*c == '\n' || *c == '\r')
                        *c = ' ';
            }
        }
        g_sq.finalize(st);
    }
    if (lg)
        fprintf(lg, "MAKE STEMS read the Tag List: %d of %d tracks\n", g_rs.ntrk, g_tag_n);
    return g_rs.trk ? (g_rs.ntrk ? 0 : -47) : -42;
}
/* library thread, the owning stick's: its mount and the row's file (read only) */
static int rs_read(uintptr_t conn, FILE *lg) {
    if (!cp_mount_of((void *) conn, g_rs.mount, sizeof g_rs.mount))
        return -41;
    if (g_rs.pl == RS_TAGGED)
        return rs_read_tagged(conn, lg);
    if (g_rs.pl)
        return rs_read_playlist(conn, lg);
    if (!sq_text((void *) conn, "SELECT path FROM content WHERE content_id = ?1", g_rs.cid,
                 g_rs.path, sizeof g_rs.path) ||
        g_rs.path[0] != '/')
        return -22;
    /* The title, for the banners: a file name can be an opaque id. Optional. */
    if (!sq_text((void *) conn, "SELECT title FROM content WHERE content_id = ?1", g_rs.cid,
                 g_rs.title, sizeof g_rs.title))
        g_rs.title[0] = 0;
    for (char *c = g_rs.title; *c; c++)
        if (*c == '\n' || *c == '\r')
            *c = ' '; /* one line in the .req */
    if (lg)
        fprintf(lg, "MAKE STEMS read: %s%s \"%s\"\n", g_rs.mount, g_rs.path, g_rs.title);
    return 0;
}
static int rs_helper_running(void) {
    char b[32] = "";
    FILE *f = fopen("/tmp/azrs/pid", "re");
    if (!f)
        return 0;
    int ok = fgets(b, sizeof b, f) != 0;
    fclose(f);
    int pid = ok ? atoi(b) : 0;
    return pid > 1 && kill(pid, 0) == 0;
}
static void rs_start_helper(void) {
    if (rs_helper_running())
        return;
    const char *h = access(RS_HELPER_STAGE, R_OK) == 0 ? RS_HELPER_STAGE : 0;
    if (!h) {
        drag_log("MAKE STEMS: no helper (%s)\n", RS_HELPER_STAGE);
        return;
    }
    /* LD_PRELOAD cleared: the unit must not inherit azmod/azstem. Not waited on: the
     * message thread never blocks on systemd (a small thread reaps systemd-run). Started
     * with posix_spawn, so EP147 is not forked and no shell runs. */
    char *const argv[] = {"systemd-run", "--unit=azrs", "--collect", "-p", "Nice=19",
                          "-p", "CPUAffinity=0-3", "/bin/sh", (char *) h, NULL};
    int rc = azspawn_detached(argv, AZSPAWN_QUIET | AZSPAWN_NO_PRELOAD);
    drag_log("MAKE STEMS: helper %s start rc=%d%s%s\n", h, rc, rc ? " " : "", rc ? strerror(errno) : "");
}
/* Writes one request file into the track's own stick queue (tmp + rename).
 * `queued` = the file= lines already queued (a playlist reads them once); NULL =
 * read them here. The helper reads key=value lines (the first of each key), so the library's
 * text never adds a line: a title's control bytes are written as spaces, and a path that would
 * be split or cut is not queued. Returns 0 written, -73 already queued, -71 I/O error, -74 a
 * path that cannot be written as one line. */
static int rs_write_req(const char *q, unsigned cid, const char *path, const char *title,
                        const char *queued) {
    char want[640], fn[260], tmp[270];
    if (!path || strpbrk(path, "\r\n") ||
        snprintf(want, sizeof want, "file=%s\n", path) >= (int) sizeof want)
        return -74;
    if (queued && strstr(queued, want))
        return -73;
    if (!queued) {
        DIR *d = opendir(q);
        if (!d)
            return -71;
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 5 || strcmp(de->d_name + l - 4, ".req"))
                continue;
            char p2[460], line[640] = "";
            snprintf(p2, sizeof p2, "%s/%s", q, de->d_name);
            FILE *f = fopen(p2, "re");
            if (f) {
                int same = fgets(line, sizeof line, f) && !strcmp(line, want);
                fclose(f);
                if (same) {
                    closedir(d);
                    return -73;
                }
            }
        }
        closedir(d);
    }
    static unsigned seq;
    snprintf(fn, sizeof fn, "%s/%lld%03u-%u.req", q, drag_now_ms(), seq++ % 1000, cid);
    snprintf(tmp, sizeof tmp, "%s.tmp", fn);
    FILE *f = fopen(tmp, "we");
    if (!f)
        return -71;
    int ok = fputs(want, f) >= 0; /* file= first: the duplicate check reads line 1 */
    if (ok && title && title[0]) {
        ok = fputs("title=", f) >= 0;
        for (const char *t = title; ok && *t; t++)
            ok = fputc((unsigned char) *t < 0x20 ? ' ' : *t, f) != EOF;
        ok = ok && fputc('\n', f) != EOF;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, fn) != 0) {
        unlink(tmp);
        return -71;
    }
    return 0;
}
static int rs_queue_dir(char *q, size_t n) {
    char pair[200];
    snprintf(pair, sizeof pair, "%s/MODS/azstems.pair", g_rs.mount);
    if (access(pair, F_OK) != 0)
        return -72; /* not paired in AZ-MODS */
    snprintf(q, n, "%s/MODS/azstems-queue", g_rs.mount);
    mkdir(q, 0777);
    return 0;
}
/* Worker thread for a whole playlist. Skips tracks that already have stems
 * (azstem's own rule, every stick) or are already queued. The stems check walks
 * the stems folders and can take seconds, so it never runs on the message thread. */
static _Atomic int g_rs_pl_n, g_rs_pl_have, g_rs_pl_rc, g_rs_pl_done;
static void *rs_playlist_worker(void *arg) {
    (void) arg;
    struct sched_param sp = {0};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    char q[200];
    int n = 0, have = 0, rc = rs_queue_dir(q, sizeof q);
    char *queued = 0;
    size_t ql = 0;
    if (rc == 0) { /* the queue's file= lines, once */
        FILE *m = open_memstream(&queued, &ql);
        DIR *d = m ? opendir(q) : 0;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            size_t l = strlen(de->d_name);
            if (l < 5 || strcmp(de->d_name + l - 4, ".req"))
                continue;
            char p2[460], line[640] = "";
            snprintf(p2, sizeof p2, "%s/%s", q, de->d_name);
            FILE *f = fopen(p2, "re");
            if (f) {
                if (fgets(line, sizeof line, f))
                    fputs(line, m);
                fclose(f);
            }
        }
        if (d)
            closedir(d);
        if (m)
            fclose(m);
    }
    for (int i = 0; rc == 0 && i < g_rs.ntrk; i++) {
        struct rs_trk *t = &g_rs.trk[i];
        char abs[800];
        snprintf(abs, sizeof abs, "%s%s", g_rs.mount, t->path);
        if (g_has_stems_fn && g_has_stems_fn(abs) == 1) {
            have++;
            continue;
        }
        int r = rs_write_req(q, t->cid, t->path, t->title, queued ? queued : "");
        if (r == 0)
            n++;
        else if (r == -71) {
            rc = -71;
            break;
        }
    }
    free(queued);
    if (n)
        rs_start_helper();
    drag_log("MAKE STEMS playlist %u \"%s\": %d tracks, %d queued, %d already have stems, rc=%d\n",
             g_rs.pl, g_rs.title, g_rs.ntrk, n, have, rc);
    __atomic_store_n(&g_rs_pl_n, n, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rs_pl_have, have, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rs_pl_rc, rc, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rs_pl_done, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_rs_busy, 0, __ATOMIC_RELEASE);
    return 0;
}
/* Message thread: cancel by renaming every queued request of this track (or of
 * this playlist's tracks) to <n>.cancel. The mark stops at once, since azmod reads
 * only *.req; the helper cancels the job on the computer and removes the file.
 * Requests are named <ms><seq>-<content_id>.req, so the match is on the id. */
static int rs_cancel_reqs(const char *q) {
    int n = 0;
    DIR *d = opendir(q);
    if (!d)
        return -71;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcmp(de->d_name + l - 4, ".req"))
            continue;
        const char *dash = strchr(de->d_name, '-');
        unsigned cid = dash ? (unsigned) strtoul(dash + 1, 0, 10) : 0;
        int hit = cid && !g_rs.pl && cid == g_rs.cid;
        for (int i = 0; !hit && g_rs.pl && cid && i < g_rs.ntrk; i++)
            hit = g_rs.trk[i].cid == cid;
        if (!hit)
            continue;
        char from[460], to[460];
        snprintf(from, sizeof from, "%s/%s", q, de->d_name);
        snprintf(to, sizeof to, "%.*s.cancel", (int) (strlen(from) - 4), from);
        if (rename(from, to) == 0)
            n++;
    }
    closedir(d);
    g_rs_cancel_n = n;
    if (n)
        rs_start_helper(); /* it tells the computer and cleans up */
    drag_log("CANCEL STEMS %s %u: %d request(s)\n", g_rs.pl ? "playlist" : "cid",
             g_rs.pl ? g_rs.pl : g_rs.cid, n);
    return n ? 0 : -74;
}
/* Message thread, after rs_read: queue the track, or start the playlist worker. */
static int rs_enqueue(void) {
    char q[200];
    int rc = rs_queue_dir(q, sizeof q);
    if (rc)
        return rc;
    if (g_rs_cancel)
        return rs_cancel_reqs(q);
    if (g_rs.pl) {
        pthread_t th;
        __atomic_store_n(&g_rs_busy, 1, __ATOMIC_RELEASE);
        if (az_thread_create(&th, NULL, rs_playlist_worker, NULL) != 0) {
            __atomic_store_n(&g_rs_busy, 0, __ATOMIC_RELEASE);
            return -71;
        }
        pthread_detach(th);
        return 0;
    }
    rc = rs_write_req(q, g_rs.cid, g_rs.path, g_rs.title, NULL);
    if (rc == 0)
        rs_start_helper();
    return rc;
}
/* The helper's status file (azrs.sh): line 1 the message, line 2 "banner=1" when it
 * reports a problem the rows cannot show, "banner=0" otherwise. Cuts buf at the end of
 * line 1 and returns 1 when the message belongs in the banner strip. Without line 2 the
 * file comes from an older helper, and its wording decides. */
static int rs_status_banner(char *buf) {
    char *nl = strchr(buf, '\n');
    const char *mark = NULL;
    if (nl) {
        *nl = 0;
        mark = nl + 1;
    }
    if (mark && !strncmp(mark, "banner=", 7))
        return mark[7] == '1';
    return strstr(buf, "failed") || strstr(buf, "not found") || strstr(buf, "Turn on") ||
           strstr(buf, "missing") || strstr(buf, "cannot") || strstr(buf, "already");
}
/* Does any stick under <root> hold a queued request, a .req file in
 * <root>/<stick>/MODS/azstems-queue?
 * Directory reads on a USB stick can block for as long as the stick is busy, so this runs on
 * the stem-marks worker, never on the message thread. */
static int rs_queue_has_req(const char *root) {
    DIR *rd = opendir(root);
    if (!rd)
        return 0;
    struct dirent *de;
    int work = 0;
    while (!work && (de = readdir(rd))) {
        if (de->d_name[0] == '.')
            continue;
        char q[300];
        if (snprintf(q, sizeof q, "%s/%s/MODS/azstems-queue", root, de->d_name) >= (int) sizeof q)
            continue;
        DIR *d = opendir(q);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcmp(e->d_name + l - 4, ".req")) {
                work = 1;
                break;
            }
        }
        closedir(d);
    }
    closedir(rd);
    return work;
}
/* The scan's result, from the worker to the message thread: 1 = a request is queued. */
static int g_rs_work; /* __atomic only */
static void rs_scan_publish(const char *root) {
    __atomic_store_n(&g_rs_work, rs_queue_has_req(root), __ATOMIC_RELEASE);
}
static int rs_work_take(void) {
    return __atomic_exchange_n(&g_rs_work, 0, __ATOMIC_ACQ_REL);
}
/* The scan's own thread: every RS_SCAN_MS while the feature is on, one scan. It belongs to
 * Remote Stems alone (started from its install path), so it runs whether or not any other mod's
 * worker exists. It runs at idle priority: a busy stick delays only this thread. */
#define RS_SCAN_MS 10000
static void rs_scan_loop(const char *root, const int *stop, const int *on, int period_ms) {
    long long next = 0;
    while (!__atomic_load_n(stop, __ATOMIC_ACQUIRE)) {
        long long now = drag_now_ms();
        if (now >= next && __atomic_load_n(on, __ATOMIC_RELAXED)) {
            next = now + period_ms;
            rs_scan_publish(root);
        }
        usleep(100000);
    }
}
static int g_rs_scan_stop; /* __atomic only; never set in production, the loop runs for the process */
static void *rs_scan_thread(void *arg) {
    (void) arg;
    struct sched_param sp = {0};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    rs_scan_loop("/media/usb", &g_rs_scan_stop, &g_rs_on, RS_SCAN_MS);
    return NULL;
}
static void rs_scan_start(void) {
    static int started; /* __atomic only: install may run more than once */
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL))
        return;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = az_thread_create(&t, &at, rs_scan_thread, NULL);
    pthread_attr_destroy(&at);
    if (e)
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
}
/* Message thread, from msg_tick: surfaces the helper's status line on EP147's
 * top strip and restarts the helper when a stick holds queued work (after an
 * EP147 restart, or a stick from another deck). One stat a second; a directory
 * scan every 10 s. */
static void rs_tick(void) {
    if (!g_rs_inst)
        return;
    { /* Mod Menu > Stems > Remote Stems, followed live: no restart to switch it */
        static long long at;
        long long now = drag_now_ms();
        if (now - at >= 500) {
            at = now;
            __atomic_store_n(&g_rs_on,
                             az_on(AZF_REMOTE_STEMS) && az_active(AZF_REMOTE_STEMS),
                             __ATOMIC_RELAXED);
        }
    }
    if (!__atomic_load_n(&g_rs_on, __ATOMIC_RELAXED))
        return;
    if (__atomic_exchange_n(&g_rs_pl_done, 0,
                            __ATOMIC_ACQ_REL)) { /* a playlist finished queueing */
        int n = __atomic_load_n(&g_rs_pl_n, __ATOMIC_RELAXED),
            have = __atomic_load_n(&g_rs_pl_have, __ATOMIC_RELAXED);
        int rc = __atomic_load_n(&g_rs_pl_rc, __ATOMIC_RELAXED);
        static char msg[160];
        if (rc)
            snprintf(msg, sizeof msg, "Could not queue: this USB cannot be written");
        else if (!n)
            snprintf(msg, sizeof msg,
                     have ? "Every track in %.80s already has stems"
                          : "Everything in %.80s is already queued",
                     g_rs.title);
        if (rc || !n)
            az_banner_strip(msg, RS_MSG_MS); /* queued: the rows animate, no banner */
    }
    static long long next;
    static time_t mt;
    static off_t sz;
    long long now = drag_now_ms();
    if (now < next)
        return;
    next = now + 1000;
    struct stat st;
    if (stat("/tmp/azrs/status", &st) == 0 && (st.st_mtime != mt || st.st_size != sz)) {
        int first = (mt == 0 && sz == 0);
        mt = st.st_mtime;
        sz = st.st_size;
        char buf[1024] = "", line[160];
        FILE *f = fopen("/tmp/azrs/status", "re");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
        }
        /* Routine progress shows on the row itself; only what the row cannot
         * say (why nothing is happening) reaches the strip. */
        int problem = rs_status_banner(buf);
        snprintf(line, sizeof line, "%s", buf);
        if (line[0] && !first && problem)
            az_banner_strip(line, RS_MSG_MS);
    }
    /* A queued request is found by the scan thread (rs_scan_publish), so no directory on
     * a stick is read here. Taking the flag clears it; the next scan sets it again while the
     * request is still queued. */
    if (rs_work_take())
        rs_start_helper();
}


/* Library-thread playlist operations, defined in the playlist editor. */
static int nl_create(uintptr_t conn, uintptr_t cache, FILE *lg); /* NEW PLAYLIST, below */
static int nl_rename(uintptr_t conn, uintptr_t cache, FILE *lg); /* RENAME, below */
static int fd_collect(uintptr_t conn, FILE *lg);                 /* folder REMOVE, below */
static int fd_delete(uintptr_t conn, uintptr_t cache, FILE *lg);
static void lib_invalidate(uintptr_t cache, void *conn, FILE *lg);
static int lp_exec(void *conn, const char *sql, FILE *lg);
/* MENU > MAKE STEMS needs the selection hooks (which row, which stick), the
 * library hooks (the file, read on the owning stick) and paintCell (whether a
 * track row is selected). */
static void install_remotestems(void) {
    int sel = install_selection_hooks(), lib = install_library_hooks(),
        pc = install_paintcell_slot();
    /* its own lookup: the stems mark, which also resolves it, may be off */
    if (!g_has_stems_fn)
        *(void **) &g_has_stems_fn = dlsym(RTLD_DEFAULT, "azstem_has_stems");
    g_rs_inst = sel == 1 && lib == 1 && pc == 1; /* g_rs_on follows the Mod Menu toggle (rs_tick) */
    if (g_rs_inst)
        rs_scan_start();
    drag_log("remote stems: selection=%d library=%d paintCell=%d -> %s\n", sel, lib, pc,
             g_rs_inst ? "installed" : "OFF");
}
/* ---- Tracks being stemmed: the queue on each stick plus the helper's in-flight
 * jobs. Matched to rows by (content_id, title hash), the mark's own identity.
 * Rebuilt by the worker once a second; any change repaints what is on screen. */
#define SM_PEND 32
static struct {
    unsigned cid, h;
    int pct, ph;
} g_smp[SM_PEND]; /* pct -1 = waiting; g_sm_mu */
static int g_smp_n;
static void sm_pend_scan(void) {
    if (!__atomic_load_n(&g_rs_on, __ATOMIC_RELAXED))
        return;
    struct {
        unsigned cid, h;
        int pct, ph;
    } n[SM_PEND];
    int k = 0;
    /* In flight, per track: /tmp/azrs/jobs, one "<cid> <pct> <ph>" per line. An
     * older helper writes only /tmp/azrs/job ("cid=<id> pct=<n> ph=<ph>"), the one
     * track it stepped last. */
    struct {
        unsigned cid;
        int pct, ph;
    } fl[SM_PEND];
    int nfl = 0;
    FILE *jf = fopen("/tmp/azrs/jobs", "re");
    if (jf) {
        unsigned c;
        int pc;
        char ph[4];
        while (nfl < SM_PEND && fscanf(jf, "%u %d %3s", &c, &pc, ph) == 3) {
            fl[nfl].cid = c;
            fl[nfl].pct = pc;
            fl[nfl].ph = !strcmp(ph, "up")   ? SM_PH_UP
                         : !strcmp(ph, "dn") ? SM_PH_DN
                         : !strcmp(ph, "wt") ? SM_PH_WT
                                             : SM_PH_ST;
            nfl++;
        }
        fclose(jf);
    } else if ((jf = fopen("/tmp/azrs/job", "re"))) {
        char ph[4] = "";
        unsigned c = 0;
        int pc = -1;
        int got = fscanf(jf, "cid=%u pct=%d ph=%3s", &c, &pc, ph);
        if (got >= 2) {
            fl[0].cid = c;
            fl[0].pct = pc;
            fl[0].ph = got == 3 && !strcmp(ph, "up")   ? SM_PH_UP
                       : got == 3 && !strcmp(ph, "dn") ? SM_PH_DN
                                                       : SM_PH_ST;
            nfl = 1;
        }
        fclose(jf);
    }
    unsigned dones[SM_PEND];
    int ndone = 0; /* the helper's finished tracks, newest last */
    FILE *df = fopen("/tmp/azrs/done", "re");
    if (df) {
        unsigned c;
        unsigned all[256];
        int na = 0;
        while (na < 256 && fscanf(df, "%u", &c) == 1)
            all[na++] = c;
        fclose(df);
        for (int i = na > SM_PEND ? na - SM_PEND : 0; i < na; i++)
            dones[ndone++] = all[i];
    }
    DIR *rd = opendir("/media/usb");
    struct dirent *de;
    while (rd && (de = readdir(rd))) {
        if (de->d_name[0] == '.')
            continue;
        char q[300];
        snprintf(q, sizeof q, "/media/usb/%s/MODS/azstems-queue", de->d_name);
        DIR *d = opendir(q);
        if (!d)
            continue;
        struct dirent *e;
        while (k < SM_PEND && (e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".req"))
                continue;
            const char *dash = strchr(e->d_name, '-');
            unsigned cid = dash ? (unsigned) strtoul(dash + 1, 0, 10) : 0;
            if (!cid)
                continue;
            char p2[600], line[260], title[200] = "";
            snprintf(p2, sizeof p2, "%s/%s", q, e->d_name);
            FILE *f = fopen(p2, "re");
            if (f) {
                while (fgets(line, sizeof line, f))
                    if (!strncmp(line, "title=", 6)) {
                        snprintf(title, sizeof title, "%s", line + 6);
                        title[strcspn(title, "\n")] = 0;
                    }
                fclose(f);
            }
            if (!title[0])
                continue; /* no identity to match a row by */
            n[k].cid = cid;
            n[k].h = sm_hash(title);
            n[k].pct = -1;
            n[k].ph = SM_PH_WT; /* queued */
            for (int f = 0; f < nfl; f++)
                if (fl[f].cid == cid) {
                    n[k].ph = fl[f].ph;
                    n[k].pct = fl[f].ph == SM_PH_WT ? -1 : (fl[f].pct < 0 ? 0 : fl[f].pct);
                    break;
                }
            k++;
        }
        closedir(d);
    }
    if (rd)
        closedir(rd);
    sm_lock();
    int changed = k != g_smp_n;
    for (int i = 0; !changed && i < k; i++)
        changed = n[i].cid != g_smp[i].cid || n[i].h != g_smp[i].h || n[i].pct != g_smp[i].pct ||
                  n[i].ph != g_smp[i].ph;
    int left = 0; /* did a job leave the queue? */
    struct {
        unsigned cid, h;
    } gone[SM_PEND];
    int ngone = 0;
    for (int i = 0; i < g_smp_n; i++) {
        int still = 0;
        for (int j = 0; j < k; j++)
            if (n[j].cid == g_smp[i].cid && n[j].h == g_smp[i].h) {
                still = 1;
                break;
            }
        if (!still) {
            left = 1;
            gone[ngone].cid = g_smp[i].cid;
            gone[ngone].h = g_smp[i].h;
            ngone++;
        }
    }
    /* A job leaves the queue once its stems are in place (or it failed). They
     * arrive by renaming .<folder>.part, which on FAT/exFAT does not touch STEMS'
     * mtime, so sm_world_sig cannot see it: a job leaving re-checks every row. */
    if (left)
        for (int i = 0; i < SM_CACHE; i++)
            if (g_smc[i].st)
                g_smc[i].stale = 1;
    /* A track the helper just finished (listed in /tmp/azrs/done before its request
     * is removed) is marked full now; otherwise its stale "no stems" answer would
     * blank the mark until the re-check lands. Canceled or failed tracks are not
     * listed and are re-checked. */
    for (int g = 0; g < ngone; g++) {
        int done = 0;
        for (int d = 0; d < ndone && !done; d++)
            done = dones[d] == gone[g].cid;
        if (!done)
            continue;
        struct sm_ent *e = sm_ent(gone[g].cid, gone[g].h, 1);
        if (e) {
            e->st = SM_ST_STEMS;
            e->stale = 0;
        }
    }
    /* A playlist row animates while a track in it is stemmed, and the cache holds
     * no track list for it, so any queue change re-checks the playlists. */
    int qchanged = k != g_smp_n; /* the jobs or which are in flight, not percentages */
    for (int i = 0; !qchanged && i < k; i++)
        qchanged = n[i].cid != g_smp[i].cid || n[i].h != g_smp[i].h ||
                   (n[i].pct >= 0) != (g_smp[i].pct >= 0);
    if (qchanged)
        for (int i = 0; i < SM_CACHE; i++)
            if (g_smc[i].st && (g_smc[i].cid & SM_PLF))
                g_smc[i].stale = 1;
    for (int i = 0; i < k; i++) {
        g_smp[i].cid = n[i].cid;
        g_smp[i].h = n[i].h;
        g_smp[i].pct = n[i].pct;
        g_smp[i].ph = n[i].ph;
    }
    g_smp_n = k;
    sm_unlock();
    if (changed) {
        __atomic_store_n(&g_sm_world, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_sm_dirty, 1, __ATOMIC_RELEASE);
    }
}
/* -2 = not being stemmed, -1 = waiting, 0..100 = in flight */
static int sm_pending(unsigned cid, const char *title, int *ph) {
    unsigned h = sm_hash(title);
    int r = -2;
    sm_lock();
    for (int i = 0; i < g_smp_n; i++)
        if (g_smp[i].cid == cid && g_smp[i].h == h) {
            r = g_smp[i].pct;
            if (ph)
                *ph = g_smp[i].ph;
            break;
        }
    sm_unlock();
    return r;
}
static int sm_queued_cid(unsigned cid) {
    int r = 0;
    sm_lock();
    for (int i = 0; i < g_smp_n && !r; i++)
        r = g_smp[i].cid == cid;
    sm_unlock();
    return r;
}
/* Repaint-timer tick (message thread): repaint the rows whose status landed.
 * Why a vtable check is enough for liveness: this runs on the message thread,
 * the only thread that destroys JUCE widgets, so none is ever half destroyed;
 * and glibc's free() overwrites the first 16 bytes of a freed block, where the
 * vptr lives, so a destroyed widget cannot still read TRACKLIST_WIDGET_VT at +0.
 * Only a new widget of the same class at the same address passes, and
 * repainting its rows is harmless. The ListBox at [w+0x48] must carry the vptr
 * it had when the widget painted. No time window: the first answer can land
 * ~1.3 s after the list paints (SQLCipher derives the key on the first read). */
/* One entry per track list on screen: the two-pane PLAYLIST view shows two
 * TrackListWidgets at once, and each needs its own widget and learned layout. */
#define SM_NW 4
static struct sm_w {
    unsigned long w, lb, lbvt;
    long long ms;                 /* message thread only (paint + tick) */
    int lastcol, best, pr, pc, n; /* the rightmost column, learned per list */
    int prow[SM_PROWS], vrow[SM_PROWS],
        arow[SM_PROWS]; /* awaiting / on screen / animating (row + 1) */
} g_smw[SM_NW];
static void sm_repaint_labels(int world); /* playlist rows, defined below */
static void sm_animate_labels(void);
static int smk_variant(void); /* the mark's style + ink, below */
/* The stem mark: the Mod Menu's waveform icon, bold, gray outline, the four stem
 * colors as bands; partial = a diagonal cut; being stemmed = FILL_0..8
 * (stemsmark.h). Straight-alpha PNGs through EP147's own ImageCache (cached by
 * data pointer: one decode each), drawn with Graphics::drawImage. */
static void mp_image_from_memory(uintptr_t *out, const void *data,
                                 int size); /* menu_popup.c: ImageCache::getFromMemory */
_Static_assert(SMK_I_N == sizeof SMK_MW / sizeof SMK_MW[0],
               "stemsmark.h's mono frames follow SMK_I_* order");
static int sm_mark_row(struct sm_w *e, uintptr_t self, void *g, int row, int x0, int y0);
