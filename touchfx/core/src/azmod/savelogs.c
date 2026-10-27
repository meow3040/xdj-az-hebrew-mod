/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file savelogs.c
 * @brief System Info > Save Logs to USB: the request file and the status line.
 *
 * Overlay thread only, plain file I/O. The copying is done outside EP147: the loader's request
 * watcher (az-usbload) runs deck/lib/az-savelogs, which writes /tmp/az-savelogs/status. A
 * request carries an id (the overlay's monotonic clock in ms), and the status echoes the id of
 * the request it answers, so a result left over from an earlier press is never shown as the
 * answer to this one.
 */
#define SAVELOGS_RUN "/run"
#define SAVELOGS_STATUS "/tmp/az-savelogs/status"
/* How long a request may go unanswered before the screen says so. The watcher serves one
 * request at a time, so a save can wait behind a mods check or a community fetch. */
#define SAVELOGS_WAIT_MS 30000LL

/* Hands one "save <id>" request to the watcher as <run>/az-savelogs.req, through a temp file
 * and a rename, so the watcher never reads a partial request. 0 = written, -1 = not (nothing
 * left behind). */
static int savelogs_write_req(const char *run, unsigned long long id) {
    char tmp[256], req[256];
    if (snprintf(tmp, sizeof tmp, "%s/.az-savelogs.req.tmp", run) >= (int) sizeof tmp ||
        snprintf(req, sizeof req, "%s/az-savelogs.req", run) >= (int) sizeof req)
        return -1;
    FILE *f = fopen(tmp, "we");
    if (!f)
        return -1;
    int bad = fprintf(f, "save %llu\n", id) < 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, req) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* The status line for request `id` (0 = no press this EP147 run) from the status file at
 * `path`, `age_ms` after the press. Returns 1 while the request is still running, else 0;
 * *tone: 0 neutral, 1 saved, 2 refused, 3 failed. The file is key=value lines (state, req,
 * msg), written whole by az-savelogs. */
static int savelogs_parse(const char *path, unsigned long long id, long long age_ms, char *line,
                          size_t n, int *tone) {
    char st[32] = "", msg[128] = "";
    unsigned long long rq = 0;
    FILE *f = fopen(path, "re");
    if (f) {
        char l[192];
        while (fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            if (!strncmp(l, "state=", 6))
                snprintf(st, sizeof st, "%s", l + 6);
            else if (!strncmp(l, "req=", 4))
                rq = strtoull(l + 4, NULL, 10);
            else if (!strncmp(l, "msg=", 4))
                snprintf(msg, sizeof msg, "%s", l + 4);
        }
        fclose(f);
    }
    int fin = 0; /* the final states, as tones */
    if (!strcmp(st, "saved"))
        fin = 1;
    else if (!strcmp(st, "refused"))
        fin = 2;
    else if (!strcmp(st, "error"))
        fin = 3;
    *tone = 0;
    line[0] = 0;
    if (!id) { /* no press yet: the last result of this boot, if there is one */
        if (fin) {
            *tone = fin;
            snprintf(line, n, "%s", msg);
        }
        return 0;
    }
    if (rq != id) { /* not answered yet (or the file answers an earlier press) */
        if (age_ms >= SAVELOGS_WAIT_MS) {
            *tone = 3;
            snprintf(line, n, "No answer from the deck. Reload, then try again.");
            return 0;
        }
        snprintf(line, n, "Saving logs...");
        return 1;
    }
    if (!fin) { /* this request, still running */
        snprintf(line, n, "%s", msg[0] ? msg : "Saving logs...");
        return 1;
    }
    *tone = fin;
    if (msg[0])
        snprintf(line, n, "%s", msg);
    else
        snprintf(line, n, "%s", fin == 1 ? "Saved" : "Not saved");
    return 0;
}

static unsigned long long g_savelogs_id; /* the last press's request id, 0 = none */
static long g_savelogs_ms;               /* when it was sent (now_ms) */
static int g_savelogs_sendfail;          /* the request file could not be written */

/* Renders the status into the screen's line. Returns 1 when anything changed. */
static int read_savelogs(azmod_state *s) {
    char nl[sizeof s->savelogs_line];
    int tone = 3, busy = 0;
    if (g_savelogs_sendfail)
        snprintf(nl, sizeof nl, "Could not send the request.");
    else
        busy = savelogs_parse(SAVELOGS_STATUS, g_savelogs_id,
                              (long long) (now_ms() - g_savelogs_ms), nl, sizeof nl, &tone);
    int changed =
        strcmp(nl, s->savelogs_line) || tone != s->savelogs_tone || busy != s->savelogs_busy;
    memcpy(s->savelogs_line, nl, sizeof nl);
    s->savelogs_tone = tone;
    s->savelogs_busy = busy;
    return changed;
}

/* The button was pressed (s->savelogs_want): send the request. */
static void savelogs_request(azmod_state *s) {
    s->savelogs_want = 0;
    g_savelogs_ms = now_ms();
    g_savelogs_id = (unsigned long long) g_savelogs_ms;
    g_savelogs_sendfail = savelogs_write_req(SAVELOGS_RUN, g_savelogs_id) != 0;
    dbg("savelogs: request %llu %s\n", g_savelogs_id,
        g_savelogs_sendfail ? "NOT written" : "written");
    read_savelogs(s);
}
