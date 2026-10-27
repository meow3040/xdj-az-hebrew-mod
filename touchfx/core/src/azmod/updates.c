/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file updates.c
 * @brief Update Mods screen: live mods version, update status, update requests, changelog.
 *
 * Everything here runs on the overlay thread and does plain file I/O. No network access and
 * no fork ever happens inside EP147: the loader's request watcher does the update work, and
 * this file only reads its status file and hands it request files.
 */
/* The running mods version for the footer and Sys Info, from the set the loader staged
 * (/tmp/az-deck/version). Re-read every ~2 s because an on-deck update rewrites it. */
const char *azmod_live_version(void) {
    static char v[32];
    static long last;
    long now = (long) time(NULL);
    if (!v[0] || now - last >= 2) {
        last = now;
        FILE *f = fopen("/tmp/az-deck/version", "re");
        char b[32] = "";
        if (f) {
            if (fgets(b, sizeof b, f))
                b[strcspn(b, "\r\n \t")] = 0;
            fclose(f);
        }
        /* No compiled-in fallback: a missing file shows as empty, never as a
         * version that may not be the one running. */
        snprintf(v, sizeof v, "%s", b);
    }
    return v;
}
/* Renders /tmp/az-modupdate/status (key=value, written whole by az-modupdate under the
 * loader's watcher) into update_line[], the progress bar and the version tags.
 * Polled ~2/s while the screen is up. Returns 1 when anything changed. */
static int read_modupdate(azmod_state *s) {
    char st[32] = "idle", lo[32] = "", re[32] = "", msg[112] = "", dn[16] = "0", tt[16] = "0";
    FILE *f = fopen("/tmp/az-modupdate/status", "re");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "state=", 6))
                snprintf(st, sizeof st, "%s", line + 6);
            else if (!strncmp(line, "local=", 6))
                snprintf(lo, sizeof lo, "%s", line + 6);
            else if (!strncmp(line, "remote=", 7))
                snprintf(re, sizeof re, "%s", line + 7);
            else if (!strncmp(line, "done=", 5))
                snprintf(dn, sizeof dn, "%s", line + 5);
            else if (!strncmp(line, "total=", 6))
                snprintf(tt, sizeof tt, "%s", line + 6);
            else if (!strncmp(line, "msg=", 4))
                snprintf(msg, sizeof msg, "%s", line + 4);
        }
        fclose(f);
    }
    if (!lo[0]) {
        FILE *v = fopen("/tmp/az-deck/version", "re");
        if (v) {
            if (fgets(lo, sizeof lo, v))
                lo[strcspn(lo, "\r\n")] = 0;
            fclose(v);
        }
    }
    /* The loader staged an installed set and did not restart EP147 (its restart policy): the
     * screen says so until a Reload loads it. "installed" is the updater's own final state;
     * after the loader's re-run it reports "uptodate" (what an older azmod understands) and
     * /run/az-reload-pending (line 1 "update", removed by the next EP147 start) carries it. */
    int staged = !strcmp(st, "installed");
    if (!staged && !strcmp(st, "uptodate")) {
        FILE *p = fopen("/run/az-reload-pending", "re");
        char why[16] = "";
        if (p) {
            if (fgets(why, sizeof why, p))
                why[strcspn(why, "\r\n")] = 0;
            fclose(p);
        }
        staged = !strcmp(why, "update");
    }
    char nl[AZMOD_UPDATE_LINES][AZMOD_UPDATE_LINE_LEN];
    snprintf(nl[0], AZMOD_UPDATE_LINE_LEN, "Installed:  v%s", lo[0] ? lo : "?");
    if (staged)
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Reload to apply."); /* line 0 names the installed set; a longer
                                                                   sentence is cut off beside the button */
    else if (!strcmp(st, "available"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Available:  v%s", re);
    else if (!strcmp(st, "uptodate"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Up to date");
    else if (!strcmp(st, "checking"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Checking...");
    else if (!strcmp(st, "offline"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "No internet");
    else if (!strcmp(st, "error"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Failed%s%.*s", msg[0] ? ": " : "",
                 AZMOD_UPDATE_LINE_LEN - 10, msg); /* the reason on the same line */
    /* an older updater restarted EP147 itself: announce it, or the screen vanishing while
     * EP147 restarts reads as a crash */
    else if (!strcmp(st, "reloading"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Installed v%s. Reloading...", re[0] ? re : lo);
    else if (!strcmp(st, "idle"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Not checked yet");
    else if (!strcmp(st, "downloading"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Downloading v%s", re);
    else if (!strcmp(st, "verifying"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Verifying v%s", re);
    else if (!strcmp(st, "installing"))
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "Installing v%s", re);
    else
        snprintf(nl[1], AZMOD_UPDATE_LINE_LEN, "%s v%s", st, re);
    /* Phases map onto 0..100. An update begins with the updater's own check, so
     * "checking" and "available" are early phases of a running update. While
     * update_busy the bar only moves forward and stays up until a final state. */
    {
        int d = atoi(dn), t = atoi(tt);
        int pct = -1;
        if (!strcmp(st, "checking"))
            pct = 4;
        else if (!strcmp(st, "available"))
            pct = 8;
        else if (!strcmp(st, "downloading"))
            pct = 15;
        else if (!strcmp(st, "verifying"))
            pct = t > 0 ? 25 + 60 * d / t : 25;
        else if (!strcmp(st, "installing"))
            pct = 90;
        else if (!strcmp(st, "reloading"))
            pct = 100;
        int final_ = !strcmp(st, "uptodate") || !strcmp(st, "error") || !strcmp(st, "offline") ||
                     !strcmp(st, "installed");
        /* A check (update_busy == 1) ends at "available"; only for an update (2)
         * is it an early phase. Otherwise a check that found a newer set would leave
         * the screen busy, and a busy screen ignores presses. */
        if (s->update_busy == 1 && !strcmp(st, "available"))
            final_ = 1;
        if (s->update_busy) {
            if (pct > s->update_pct)
                s->update_pct = pct; /* forward only */
            if (final_) {
                s->update_busy = 0;
                s->update_pct = -1;
            }
        } else
            s->update_pct = -1;
    }
    nl[2][0] = 0; /* the bar takes this row */
    /* row 3 stays empty: an error's reason rides line 1 ("Failed: ...") */
    int problem = !strcmp(st, "error");
    (void) problem;
    nl[3][0] = 0;
    int busy = s->update_busy || !strcmp(st, "checking") || !strcmp(st, "downloading") ||
               !strcmp(st, "verifying") || !strcmp(st, "installing") || !strcmp(st, "reloading");
    s->update_avail = !strcmp(st, "available");
    s->update_uptodate = !strcmp(st, "uptodate");
    snprintf(nl[4], AZMOD_UPDATE_LINE_LEN, "%s",
             busy              ? "Please wait"
             : s->update_avail ? "Update now"
                               : "Check for updates");
    /* the changelog box tags its versions from these: INSTALLED / UPDATE AVAILABLE */
    char ml[32], mr[32];
    snprintf(ml, sizeof ml, "%s", lo);
    snprintf(mr, sizeof mr, "%s", (!strcmp(st, "available") || !strcmp(st, "uptodate")) ? re : "");
    int tags = strcmp(ml, s->mods_local) || strcmp(mr, s->mods_remote);
    memcpy(s->mods_local, ml, sizeof ml);
    memcpy(s->mods_remote, mr, sizeof mr);
    int changed = tags || memcmp(nl, s->update_line, sizeof nl) != 0;
    memcpy(s->update_line, nl, sizeof nl);
    return changed;
}
/* Hands "check" or "update" to the loader's watcher as a request file
 * (tmp + rename, so the watcher never reads a partial request). */
static void write_modupdate_req(int want) {
    FILE *f = fopen("/run/.az-modupdate.req.tmp", "we");
    if (!f)
        return;
    fprintf(f, "%s\n", want == 2 ? "update" : "check");
    fclose(f);
    rename("/run/.az-modupdate.req.tmp", "/run/az-modupdate.req");
    dbg("modupdate: requested %s\n", want == 2 ? "update" : "check");
}
/* Loads the changelog box text when the screen opens, so menu.c stays
 * syscall-free (it rewraps once per changelog_gen). */
static void read_changelog(azmod_state *s) {
    /* Prefer the changelog az-modupdate fetched on its last online check
     * (signature-verified), else the one staged with the installed set.
     * An empty file counts as absent. */
    static const char *src[] = {"/tmp/az-modupdate/CHANGELOG.txt", "/tmp/az-deck/CHANGELOG.txt"};
    s->changelog_len = 0;
    s->changelog[0] = 0;
    for (size_t k = 0; k < sizeof src / sizeof src[0] && !s->changelog_len; k++) {
        FILE *f = fopen(src[k], "re");
        if (!f)
            continue;
        size_t n = fread(s->changelog, 1, AZMOD_CHANGELOG_MAX - 1, f);
        fclose(f);
        s->changelog[n] = 0;
        s->changelog_len = (int) n;
    }
    s->changelog_gen++;
    s->changelog_scroll = 0;
    s->changelog_pos = 0;
}
