/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file playlist_actions.c
 * @brief Playlist editing requests: remove, add, paste, undo, folder delete and Tag List copy.
 *
 * Message-thread request functions behind the MENU popup's playlist actions. Each checks
 * eligibility (no drag in progress, the move slot idle, the selected row belonging to the
 * list on screen) and arms one operation in the shared g_mv slot; the library thread then
 * proves the change against the stick's database before and after writing. Results come
 * back through pl_edit_notify(), which reports errors and out-of-sight results in EP147's
 * gray banner strip and stays silent when the list already shows the outcome.
 *
 * Tag List contents are captured by patching the TAGEDIT_ADD_SLOT and TAGEDIT_CLEAR_SLOT
 * vtable slots (tag_capture_install), which also lets tag_clear() run EP147's own clear.
 */
/* The ADD TO target, one per stick, keyed by media id: with two sticks in, a single
 * target would make ADD on one stick name a playlist on the other. A different stick in
 * the same port is caught by the name re-check on every add (-26). Held in memory only:
 * playlist ids are per stick. Message thread only.
 * PL_TGT_NAME_MAX, PL_TGT_N and struct pl_addtgt are defined with the cross-stick copy. */
static struct pl_addtgt g_addtgt[PL_TGT_N];
static int g_paste_row =
    -1; /* PASTE on a TRACK row of a playlist sorted by #: that row (0-based), else -1 */
static _Atomic unsigned long g_cg_w; /* the playlist list showing the cut row, 0 = none */
static _Atomic int g_cg_row = -1;
static struct pl_addtgt *pl_target_for(unsigned media) {
    if (!media)
        return 0;
    for (int i = 0; i < PL_TGT_N; i++)
        if (g_addtgt[i].pl && g_addtgt[i].media == media)
            return &g_addtgt[i];
    return 0;
}
/* Fingerprint the target's row if it is the highlighted row of the list on screen. */
static void mk_capture(unsigned media, unsigned pl, unsigned attr) {
    memset(&g_mk, 0, sizeof g_mk); /* mk_tick moves the edge (and repaints) */
    uintptr_t w = __atomic_load_n(&g_hl_widget, __ATOMIC_ACQUIRE);
    int sel = __atomic_load_n(&g_hl_sel, __ATOMIC_RELAXED),
        rows = __atomic_load_n(&g_hl_rows, __ATOMIC_RELAXED);
    struct plsnap sn = plsnap_get();
    if (attr != PL_ATTR_LIST)
        return;
    if (!w || sel < 0 || sn.sel_pl != pl || sn.any_media != media ||
        !plist_name_at(w, sel, g_mk.name, sizeof g_mk.name) ||
        (sel > 0 && !plist_name_at(w, sel - 1, g_mk.prev, sizeof g_mk.prev)) ||
        (sel + 1 < rows && !plist_name_at(w, sel + 1, g_mk.next, sizeof g_mk.next))) {
        /* Not the highlighted row, or its names are not readable yet: arm the mark with
         * no fingerprint, so mk_tick asks the DB where the playlist is (MV_OP_MKREAD) and
         * fingerprints the answer. Leaving it off would show no brackets at all. */
        memset(&g_mk, 0, sizeof g_mk);
        g_mk.pl = pl;
        g_mk.media = media;
        g_mk.rows = 0;
        g_mk.on = 1;
        drag_log("MARK armed without a fingerprint: pl %u (the DB will place it)\n", pl);
        return;
    }
    g_mk.pl = pl;
    g_mk.media = media;
    g_mk.rows = rows;
    g_mk.on = 1;
    drag_log("MARK captured: pl %u row %d/%d \"%s\" between \"%s\" and \"%s\"\n", pl, sel, rows,
             g_mk.name, g_mk.prev, g_mk.next);
}
static void pl_target_store(unsigned media, unsigned pl, const char *name, const char *mount,
                            unsigned attr) {
    struct pl_addtgt *t = pl_target_for(media);
    if (!t) {
        static unsigned rr;
        t = &g_addtgt[rr++ % PL_TGT_N];
        for (int i = 0; i < PL_TGT_N; i++)
            if (!g_addtgt[i].pl) {
                t = &g_addtgt[i];
                break;
            }
    }
    t->pl = pl;
    t->media = media;
    utf8_copy(t->name, sizeof t->name, name);
    snprintf(t->mount, sizeof t->mount, "%s", mount ? mount : "");
    t->attr = attr;
    mk_capture(media, pl, attr);
}
/* After a RENAME: the ADD TO target proves itself by name (mv_add), so it takes the
 * new one; the clipboard's name is only shown. */
static void nl_rename_fixups(unsigned media, unsigned pl, const char *name) {
    _Static_assert(sizeof g_mv.name <= PL_TGT_NAME_MAX, "a name nm_tick accepts fits here");
    size_t n = strlen(name);
    if (n >= PL_TGT_NAME_MAX)
        return; /* nm_tick refuses a name this long; a name cut short proves nothing */
    struct pl_addtgt *t = pl_target_for(media);
    if (t && t->pl == pl)
        memcpy(t->name, name, n + 1);
    if (g_clip.kind && g_clip.media == media && g_clip.kind != CLIP_TRACK && g_clip.id == pl)
        memcpy(g_clip.name, name, n + 1);
}
/* [SELECTED] tapped again: forget that stick's selection. Message thread. */
static void pl_target_clear(unsigned media) {
    struct pl_addtgt *t = pl_target_for(media);
    if (t) {
        drag_log("TARGET cleared: [SELECTED] (%u \"%s\")\n", t->pl, t->name);
        memset(t, 0, sizeof *t);
    }
    memset(&g_mk, 0, sizeof g_mk);
}
/* The one target on another stick (cross-stick copy): exactly one, so an ADD never
 * guesses between two. 0 = none or ambiguous. */
static struct pl_addtgt *pl_target_other(unsigned media) {
    struct pl_addtgt *o = 0;
    int n = 0;
    for (int i = 0; i < PL_TGT_N; i++)
        if (g_addtgt[i].pl && g_addtgt[i].media != media && g_addtgt[i].mount[0]) {
            o = &g_addtgt[i];
            n++;
        }
    return n == 1 ? o : 0;
}
static const char *pl_name_short(const char *nm) {
    static char b[48];
    if (!nm || !*nm)
        return "the playlist";
    if (strlen(nm) <= 40)
        return nm;
    snprintf(b, sizeof b, "%.37s...", nm);
    return b;
}
/* What the DJ is told when an edit ends, in EP147's own banner strip. Message thread. */
static void pl_edit_notify(int op, int rc) {
    if (rc == LB_BAD || rc == LB_FAIL) { /* the backup refused the edit: nothing was changed */
        if (op == MV_OP_NEWPL || op == MV_OP_RENAME)
            __atomic_store_n(&g_nm.state, NM_IDLE, __ATOMIC_RELEASE);
        az_banner_strip(rc == LB_BAD
                            ? "This USB's library failed a check \xc2\xb7 editing is off for it"
                            : "Couldn't back up this USB's library \xc2\xb7 nothing was changed",
                        PL_MSG_READ);
        return;
    }
    if (op == MV_OP_NEWPL || op == MV_OP_RENAME) {
        nm_done(rc);
        return;
    } /* result or expiry: never stuck in "creating" */
    static char buf[160];
    const char *t = buf;
    int ms = rc == 0 ? PL_MSG_BRIEF : PL_MSG_READ;
    if (op == MV_OP_REMOVE)
        t = rc == 0 ? "Removed" : "Couldn't remove this track";
    else if (op == MV_OP_ADD) {
        if (rc == 0)
            snprintf(buf, sizeof buf, "Added to %s", pl_name_short(g_mv.name));
        else
            t = rc == -30   ? "COPY a track, then PASTE it on a playlist"
                : rc == -31 ? "That playlist is on the other USB"
                : rc == -26 ? "That playlist changed \xc2\xb7 paste again"
                : rc == -21 ? "Folders hold playlists, not tracks"
                : rc == -10 ? "Couldn't add to this playlist"
                            : "Couldn't add this track";
    } else if (op == MV_OP_CPREAD) { /* rc 1 = copying started; the result comes later */
        int fold = g_cp.kind == CPK_FOLDER;
        if (rc == 1 && fold) {
            snprintf(buf, sizeof buf, "Copying %s to the other USB...",
                     pl_name_short(g_cp.src_pl_name));
            ms = PL_MSG_HOLD;
        } else if (rc == 1) {
            snprintf(buf, sizeof buf, "Copying %d track%s to the other USB...", g_cp.ntrk,
                     g_cp.ntrk == 1 ? "" : "s");
            ms = PL_MSG_HOLD;
        } else if (rc == 2) {
            snprintf(buf, sizeof buf, "Copying %d/%d to the other USB...",
                     __atomic_load_n(&g_cp.done_n, __ATOMIC_ACQUIRE), g_cp.ntrk);
            ms = PL_MSG_HOLD;
        } else if (rc == 0 && fold)
            snprintf(buf, sizeof buf, "Copied %s to the other USB (%d track%s)",
                     pl_name_short(g_cp.src_pl_name), g_cp.ntrk, g_cp.ntrk == 1 ? "" : "s");
        else if (rc == 0 && (g_cp.kind == CPK_PLAYLIST || g_cp.kind == CPK_TAGGED))
            snprintf(buf, sizeof buf, "Copied %d tracks to the other USB", g_cp.ntrk);
        else if (rc == 0)
            snprintf(buf, sizeof buf, "Copied to the other USB");
        else
            t = rc == -50                ? "Not copied \xc2\xb7 a different file with that name is there"
                : rc == -52              ? "Not copied \xc2\xb7 its analysis clashes on the other USB"
                : rc == -58              ? "Copied \xc2\xb7 finishes on the other USB in a moment"
                : rc == -56 || rc == -57 ? "Copied, but the other USB's library refused it"
                : rc == -2               ? "Still copying"
                : rc == -45 && fold      ? "A list inside it can't be copied"
                : rc == -45              ? "Only playlists can be copied"
                : rc == -46              ? "Too many tracks (300 max)"
                : rc == -49              ? "Too much inside (500 max)"
                : rc == -47              ? "That playlist is empty"
                : rc == -60              ? "Copy a whole playlist into a folder"
                                         : "Couldn't copy this track";
    } else if (op == MV_OP_RSREAD && g_rs_cancel) { /* MENU > CANCEL STEMS */
        ms = PL_MSG_READ;
        g_rs_cancel = 0;
        if (rc == 0 && g_rs_cancel_n > 1)
            snprintf(buf, sizeof buf, "Stems canceled for %d tracks", g_rs_cancel_n);
        else if (rc == 0)
            t = "Stems canceled";
        else
            t = rc == -74   ? "Nothing in the queue to cancel"
                : rc == -71 ? "Could not cancel: this USB cannot be written"
                : rc == -2  ? "Busy \xc2\xb7 try again"
                            : "Could not cancel";
    } else if (op == MV_OP_RSREAD) { /* MENU > MAKE STEMS */
        ms = PL_MSG_READ;
        if (rc == 0)
            return; /* queued: the row animates, no banner */
        else
            t = rc == -72 /* no MODS/azstems.pair on the stick */
                    ? "Plug this USB into AZ-MODS once, with Remote Stems on"
                    : rc == -47   ? "That playlist is empty"
                      : rc == -73 ? "Already queued for stems"
                      : rc == -71 ? "Could not queue: this USB cannot be written"
                      : rc == -2  ? "Busy \xc2\xb7 try again"
                                  : "This track could not be queued for stems";
    } else if (op == MV_OP_FDEL) { /* REMOVE on a folder */
        ms = PL_MSG_READ;
        if (rc == 0) {
            drag_log("NOTIFY folder removed (silent: on screen)\n");
            return;
        }
        t = rc == -26   ? "That changed \xc2\xb7 try again"
            : rc == -46 ? "Too much inside (500 max)"
            : rc == -2  ? "Busy \xc2\xb7 try again"
                        : "Couldn't remove it";
    } else if (op == MV_OP_TAGREAD) { /* TAG LIST > COPY (n TAGGED) */
        ms = PL_MSG_READ;
        if (rc == 0 && g_tag_skipped)
            snprintf(buf, sizeof buf,
                     "Copied %d tagged track%s \xc2\xb7 %d on the other USB left out \xc2\xb7 "
                     "PASTE on a playlist",
                     g_tag_n, g_tag_n == 1 ? "" : "s", g_tag_skipped);
        else if (rc == 0)
            snprintf(buf, sizeof buf, "Copied %d tagged track%s \xc2\xb7 PASTE on a playlist",
                     g_tag_n, g_tag_n == 1 ? "" : "s");
        else
            t = rc == -90 || rc == -95   ? "The Tag List is empty"
                : rc == -91 || rc == -93 ? "Open TAG LIST, then MENU > COPY"
                : rc == -92              ? "Too many tagged tracks (100 max)"
                : rc == -94              ? "A tagged track isn't on this USB"
                : rc == -2               ? "Busy \xc2\xb7 try again"
                                         : "Couldn't read the Tag List";
    } else if (op == MV_OP_ADDN) { /* PASTE of the tagged tracks */
        if (rc == 0)
            snprintf(buf, sizeof buf, "Added %d track%s to %s", g_tag_added,
                     g_tag_added == 1 ? "" : "s", pl_name_short(g_mv.name));
        else if (g_tag_added)
            snprintf(buf, sizeof buf, "Added %d of %d to %s \xc2\xb7 the rest failed", g_tag_added,
                     g_tag_n, pl_name_short(g_mv.name));
        else
            t = rc == -26   ? "That playlist changed \xc2\xb7 paste again"
                : rc == -21 ? "Folders hold playlists, not tracks"
                            : "Couldn't add the tagged tracks";
        if (rc && g_tag_added && __atomic_load_n(&g_jr.ready, __ATOMIC_ACQUIRE)) {
            pl_undo_offer(buf);
            return;
        }
    } else if (op == MV_OP_PASTE) { /* the PASTE router refused before writing */
        ms = PL_MSG_READ;
        t = rc == -66   ? "Nothing to paste"
            : rc == -67 ? "Paste tracks onto a playlist"
            : rc == -68 ? "Cut and paste works within one USB \xc2\xb7 use COPY"
            : rc == -69 ? "Highlight where it goes, then PASTE"
            : rc == -96 ? "Paste tagged tracks on the same USB"
            : rc == -62 ? "Can't paste it onto itself"
            : rc == -2  ? "Busy \xc2\xb7 try again"
                        : "Highlight a playlist or folder";
    } else if (op == MV_OP_PLCOPY || op == MV_OP_PLMOVE ||
               op == MV_OP_PLINTO) { /* PASTE / drop into a folder, same USB */
        int mvp = op != MV_OP_PLCOPY;
        const char *where = g_mv.name[0] ? g_mv.name : "the top level";
        if (rc == 0 && mvp)
            snprintf(buf, sizeof buf, "Moved into %s", where);
        else if (rc == 0)
            snprintf(buf, sizeof buf, "Copied into %s", where);
        else
            t = rc == -61   ? "Name already used there"
                : rc == -62 ? "Already there"
                : rc == -63 ? "A folder can't go inside itself"
                : rc == -26 ? "That list changed \xc2\xb7 try again"
                : rc == -47 ? "That playlist is empty"
                : rc == -46 ? "Too much inside (500 max)"
                : rc == -45 ? (mvp ? "Only playlists can be moved" : "Only playlists can be copied")
                : rc == -2  ? "Busy \xc2\xb7 try again"
                : mvp       ? "Couldn't move this playlist"
                            : "Couldn't copy this playlist";
    } else if (op == MV_OP_PLORDER) { /* knob drag on the playlist list */
        if (rc == 0)
            snprintf(buf, sizeof buf, "Moved to #%u", g_mv.to);
        else
            t = rc == -10                ? "Couldn't move this row"
                : rc == -11 || rc == -12 ? "The list changed. Try again"
                : rc == -2               ? "Busy \xc2\xb7 try again"
                                         : "Couldn't move this row";
    } else if (op == MV_OP_TARGET) {
        ms = PL_MSG_READ; /* two sentences to read, even on success */
        if (rc == 0)
            return; /* the [ ] brackets and ADD TO [name] show the selection */
        else
            t = rc == -21 ? "Folders hold playlists, not tracks" : "Can't add tracks here";
    } else if (rc == 0)
        snprintf(buf, sizeof buf, "Moved to #%u", g_mv.to); /* also ends the held drag line */
    else if (rc == -10)
        t = "Couldn't move this track"; /* mv_execute: not 1..N */
    else if (rc == -11 || rc == -12)
        t = "The playlist changed \xc2\xb7 try again";
    else
        t = "Couldn't move this track";
    if (op == MV_OP_UNDO) {
        ms = PL_MSG_READ; /* a gray strip saying what */
        if (rc == 0)
            snprintf(buf, sizeof buf, "Undone: %s", g_js_undone[0] ? g_js_undone : "last edit");
        else if (rc == -80)
            snprintf(buf, sizeof buf, "Can't undo %s \xc2\xb7 it changed since",
                     g_js_undone[0] ? g_js_undone : "that");
        else
            t = rc == -82 ? "Nothing to undo" : "Couldn't undo";
    }
    /* A result the list already shows (moved, removed, reordered) says nothing; errors
     * and out-of-sight results (added to another playlist, copied into a folder) do, and
     * so does UNDO, to say what was undone. */
    if (rc == 0 && (op == MV_OP_MOVE || op == MV_OP_PLORDER || op == MV_OP_REMOVE)) {
        /* Silent, but a knob drag's held "Turn to move ... BACK to cancel" line must
         * still come down, or it stays up after the drop. pl_tb_tick re-posts its own
         * held line if it has one. */
        if (op == MV_OP_MOVE || op == MV_OP_PLORDER) {
            /* Clear ours whether held or timed: the held drag line reads back as BN_OURS,
             * not BN_OURS_HELD. Nothing is posted between the grab and a silent success,
             * so ours is the drag line. */
            int st = az_banner_strip_state(), cr = az_banner_strip_clear();
            drag_log("NOTIFY: drag line state=%d clear=%d\n", st, cr);
        }
        drag_log("NOTIFY op=%d rc=0 \"%s\" (silent: on screen)\n", op, t);
        return;
    }
    if (rc == 0 && op != MV_OP_UNDO && mv_journaled(op) && !g_paste_remove &&
        __atomic_load_n(&g_jr.ready, __ATOMIC_ACQUIRE)) {
        pl_undo_offer(t); /* brief strip; UNDO lives in the MENU */
        drag_log("NOTIFY op=%d rc=0 \"%s\" (UNDO offered)\n", op, t);
        return;
    }
    int b = az_banner_strip(t, ms);
    drag_log("NOTIFY op=%d rc=%d \"%s\" strip=%d\n", op, rc, t, b);
}
/* REMOVE the selected track from its playlist (MENU > Remove from playlist). Same
 * eligibility as a drag (a USB playlist in "#" ascending order, EP147's hierarchy
 * agreeing on the selected row) and the same library-thread proof before and after the
 * write. Message thread. Returns 0 when armed, <0 when refused (nothing written). */
static int pl_request_remove(void) {
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1; /* mid-drag */
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    int sel = tracklist_selected_row(); /* JUCE's selection; see tracklist_selected_row */
    struct plsnap sn = plsnap_get();
    if (!sn.playlist_id || sel < 0 || (unsigned) sel != sn.row) {
        drag_log("REMOVE refused: not an eligible playlist row (sel=%d snap pl=%u row=%u)\n", sel,
                 sn.playlist_id, sn.row);
        return -3;
    }
    g_mv.op = MV_OP_REMOVE;
    g_mv.pl = sn.playlist_id;
    g_mv.cid = sn.content_id;
    g_mv.media = sn.media;
    g_mv.from = (unsigned) sel + 1;
    g_mv.to = 0;
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("REMOVE armed: pl=%u cid=%u seq %u (media %04x)\n", g_mv.pl, g_mv.cid, g_mv.from,
             g_mv.media);
    return 0;
}
/* ADD content_id `cid` to the end of playlist `pl`, both on stick `media`.
 * Message thread. The library thread proves everything against the DB before
 * writing (mv_add). Returns 0 when armed, <0 when refused (nothing written). */
static int pl_request_add(unsigned pl, unsigned cid, unsigned media, const char *name) {
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    if (!pl || !cid || !media) {
        drag_log("ADD refused: pl=%u cid=%u media=%04x\n", pl, cid, media);
        return -3;
    }
    g_mv.op = MV_OP_ADD;
    g_mv.pl = pl;
    g_mv.cid = cid;
    g_mv.media = media;
    g_mv.from = g_mv.to = 0;
    utf8_copy(g_mv.name, sizeof g_mv.name, name ? name : "");
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("ADD armed: pl=%u cid=%u (media %04x)\n", pl, cid, media);
    return 0;
}
/* The add target for the stick being browsed (the hierarchy's media), else the one
 * target on another stick (cross-stick copy). Every add re-checks the target's name on
 * the library thread. Message thread only. */
static struct pl_addtgt *pl_add_target_any(void) {
    unsigned m = plsnap_get().any_media;
    struct pl_addtgt *t = pl_target_for(m);
    if (t)
        return t;
    return pl_target_other(m);
}
static int pl_add_target_set(void) {
    return pl_add_target_any() != 0;
}
static const char *pl_add_target_name(void) {
    static char b[PL_TGT_NAME_MAX + 16];
    struct pl_addtgt *t = pl_add_target_any();
    if (!t)
        return "";
    if (t->media == plsnap_get().any_media)
        return t->name;
    snprintf(b, sizeof b, "%s (other USB)", t->name);
    return b;
}
/* The banner's DONE and UNDO buttons: pl_tb_tick runs them (DONE forgets every stick's
 * target and the clipboard, menu.c). No line is held in EP147's gray strip: the [ ] brackets
 * and ADD TO [name] show the selection and the menu's PASTE shows the clipboard, so g_tb_up
 * (our held strip is on screen, DONE drawn over it) stays 0. Message thread. */
static _Atomic int g_tb_screen_ok, g_tb_up, g_tb_done;
static long long g_undo_until;
static _Atomic int g_tb_undo_mode;
void azmod_tb_done(void) {
    __atomic_store_n(&g_tb_done, 1, __ATOMIC_RELEASE);
}
static _Atomic int g_tb_undo_req, g_tb_done_btn;
void azmod_tb_undo(void) {
    __atomic_store_n(&g_tb_undo_req, 1, __ATOMIC_RELEASE);
}
static void pl_undo_offer(const char *msg) {
    int b = az_banner_strip(msg, PL_MSG_BRIEF);
    drag_log("NOTIFY \"%s\" (UNDO in MENU) strip=%d\n", msg, b);
}
static int pl_undo_live(void) {
    return g_undo_until && drag_now_ms() < g_undo_until &&
           __atomic_load_n(&g_jr.ready, __ATOMIC_ACQUIRE);
}
static int pl_request_undo(void) {
    int nst = __atomic_load_n(&g_js_n, __ATOMIC_ACQUIRE);
    if (nst < 1)
        return -82; /* "Nothing to undo" */
    if (g_tagclr.on) {
        g_tagclr.on = 0;
        drag_log("TAGLIST kept: UNDO\n");
    } /* the pasted tags stay tagged */
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    g_mv.op = MV_OP_UNDO;
    g_mv.media = g_js[nst - 1].media;
    g_mv.pl = g_mv.cid = g_mv.from = g_mv.to = 0;
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("UNDO armed: [%d] \"%s\" (media %04x)\n", nst, g_js[nst - 1].desc, g_mv.media);
    return 0;
}
static void pl_tb_tick(void) {
    static unsigned tick;
    int done = __atomic_exchange_n(&g_tb_done, 0, __ATOMIC_ACQ_REL);
    if ((tick++ & 7) && !done && !__atomic_load_n(&g_tb_undo_req, __ATOMIC_ACQUIRE) &&
        !(g_undo_until && !pl_undo_live()))
        return;
    if (g_undo_until && !pl_undo_live())
        g_undo_until = 0; /* the window closed: back to the clipboard / nothing */
    int undo = pl_undo_live(), ureq = __atomic_exchange_n(&g_tb_undo_req, 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_tb_undo_mode, undo, __ATOMIC_RELAXED);
    {
        int any = g_clip.kind != 0;
        for (int i = 0; i < PL_TGT_N && !any; i++)
            if (g_addtgt[i].pl)
                any = 1;
        __atomic_store_n(&g_tb_done_btn, any, __ATOMIC_RELAXED);
    }
    tag_clear_tick(ureq && undo);
    if (ureq && undo) { /* the UNDO button (left of DONE) */
        g_undo_until = 0;
        int rc = pl_request_undo();
        if (rc < 0)
            pl_edit_notify(MV_OP_UNDO, rc);
    }
    if (done && az_banner_strip_state() != BN_OURS_HELD) {
        /* DONE is only real while our strip is up: a tap on that corner must not drop the
         * selection */
        drag_log("DONE ignored: our banner is not up\n");
        done = 0;
    }
    if (done) {
        memset(g_addtgt, 0, sizeof g_addtgt);
        memset(&g_clip, 0, sizeof g_clip);
        drag_log("TARGET + CLIP cleared: DONE\n");
    }
}
/* MENU > PASTE: the destination is the highlighted row: a playlist/folder row on
 * the playlist list, or (on a track row inside a playlist) that open playlist. Its
 * attribute, name, parent and mount are read on its stick's library thread first
 * (MV_OP_TARGET), then pl_paste_route() picks the write. Message thread. */
static int pl_request_paste(void) {
    if (!g_clip.kind)
        return -66;
    if (g_clip.kind == CLIP_FILES) { /* the FOLDER view: files to the other stick */
        struct plsnap sn = plsnap_get();
        if (!sn.fsel_media) {
            az_banner_strip("Paste in the other USB's FOLDER view", PL_MSG_READ);
            return 0; /* said: no second message */
        }
        fc_request(g_clip.media, g_clip.path, sn.fsel_media, sn.fsel_dir, sn.fsel_dir_id,
                   g_clip.name);
        return 0; /* fc_request shows its own messages */
    }
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    struct plsnap sn = plsnap_get();
    unsigned r = sn.sel_pl ? sn.sel_pl : sn.open_pl;
    if (!r || !sn.any_media) {
        drag_log("PASTE refused: no playlist or folder highlighted (sel %u open %u)\n", sn.sel_pl,
                 sn.open_pl);
        return -3;
    }
    g_mv.op = MV_OP_TARGET;
    g_mv.pl = r;
    g_mv.cid = 0;
    g_mv.media = sn.any_media;
    g_mv.from = g_mv.to = 0;
    g_mv.name[0] = 0;
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    g_paste_pending = 1;
    /* on a track row of a "#"-sorted playlist, remember which row: a CUT pasted back
     * into its own playlist moves there */
    g_paste_row = (!sn.sel_pl && sn.playlist_id && sn.playlist_id == sn.open_pl &&
                   tracklist_selected_row() == (int) sn.row)
                      ? (int) sn.row
                      : -1;
    /* Where the flash goes. On the playlist list: the row PASTE was pressed on, which
     * pl_paste_route moves to where a pasted playlist lands. In a track list tracks are
     * appended, so the first new row is the current row count, known only in a "#"-sorted
     * list of this playlist (g_paste_row >= 0); otherwise no flash. A CUT pasted into its
     * own playlist flashes its MOVE's target instead. */
    if (sn.sel_pl) {
        g_fl_paste.w = __atomic_load_n(&g_hl_widget, __ATOMIC_RELAXED);
        g_fl_paste.row = (int) sn.sel_row;
        g_fl_paste.plist = 1;
    } else {
        g_fl_paste.w = __atomic_load_n(&g_tracklist_widget, __ATOMIC_RELAXED);
        g_fl_paste.row = g_paste_row >= 0 ? drag_num_rows() : -1;
        g_fl_paste.plist = 0;
    }
    g_fl_paste.cutrow = (g_clip.move && g_cg_w && g_cg_w == g_fl_paste.w)
                            ? __atomic_load_n(&g_cg_row, __ATOMIC_RELAXED)
                            : -1;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("PASTE armed: %s %u (%s) onto row %u (media %04x)\n",
             g_clip.kind == CLIP_TRACK  ? "track"
             : g_clip.kind == CLIP_LIST ? "playlist"
                                        : "folder",
             g_clip.id, g_clip.move ? "move" : "copy", r, sn.any_media);
    return 0;
}
/* The TaglistEditor EP147 last called our tag hooks with. It is not a component: its owner keeps
 * it raw at +0x90 and deletes it in the owner's destructor (1.30 0x9eeac8, also reached when
 * the owner is reset at run time). That delete is devirtualized behind a guard: the owner
 * compares the editor's deleting-destructor slot (vtable +8) with the expected function and
 * calls through the slot when it differs. So patching vtable slots 0 and 1 puts every
 * destruction through tc_forget, which unpublishes the editor and waits for a reader inside
 * tc_hold/tc_drop (message thread: a vtable check and EP147's own add/clear call) before the
 * memory goes back. Without both patches the editor is never cached and RETAG / clear after a
 * paste do nothing. */
static _Atomic uintptr_t g_tc_self;
static _Atomic int g_tc_readers;
static int g_tc_del_ok;
static void (*g_tc_orig_d0)(void *), (*g_tc_orig_d1)(void *);
static void tc_forget(void *self) {
    uintptr_t e = (uintptr_t) self;
    __atomic_compare_exchange_n(&g_tc_self, &e, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
    /* readers are message-thread only: on the message thread a reader inside is our own caller
     * (EP147's add/clear destroyed the editor), which re-checks g_tc_self after the call */
    if (!on_msg_thread())
        ep_wait_readers(&g_tc_readers);
}
static void tc_d0_hook(void *self) {
    tc_forget(self);
    g_tc_orig_d0(self);
}
static void tc_d1_hook(void *self) {
    tc_forget(self);
    g_tc_orig_d1(self);
}
/* Between tc_hold and tc_drop only, message thread only: the live editor (vtable checked) or 0. */
static uintptr_t tc_hold(void) {
    __atomic_fetch_add(&g_tc_readers, 1, __ATOMIC_SEQ_CST);
    uintptr_t t = __atomic_load_n(&g_tc_self, __ATOMIC_SEQ_CST), vt = 0;
    if (!on_msg_thread() || !t || !own_q(t, &vt) || vt != TAGEDIT_VT)
        return 0;
    return t;
}
static void tc_drop(void) {
    __atomic_fetch_sub(&g_tc_readers, 1, __ATOMIC_RELEASE);
}
static void tag_readd(void) {
    uintptr_t vt = 0, fn = 0;
    uint64_t msid = 0;
    g_retag.on = 0;
    for (int k = 0; k < 4; k++)
        if (g_tc_msid[k].media == g_retag.media)
            msid = g_tc_msid[k].msid;
    uintptr_t ed = tc_hold();
    if (!msid || !ed || !img_q(TAGEDIT_VT + 0x10, &fn) || !fn) {
        tc_drop();
        drag_log("RETAG refused: msid %llx self %lx\n", (unsigned long long) msid,
                 (unsigned long) ed);
        return;
    }
    for (int i = 0; i < g_retag.n; i++) {
        uint64_t id[2] = {msid, (uint64_t) g_retag.cids[i]};
        ((void *(*) (void *, void *, void *, void *, void *) ) fn)((void *) ed, id, 0, 0, 0);
        if (__atomic_load_n(&g_tc_self, __ATOMIC_SEQ_CST) != ed)
            break; /* the editor went away inside EP147's call */
    }
    tc_drop();
    (void) vt;
    drag_log("RETAG: %d tracks tagged again (media %04x)\n", g_retag.n, g_retag.media);
}
static void tag_clear_tick(int undo_pressed) {
    if (!g_tagclr.on)
        return;
    if (undo_pressed || (g_tagclr.at && __atomic_load_n(&g_tag_touched, __ATOMIC_ACQUIRE))) {
        drag_log("TAGLIST kept: %s\n", undo_pressed ? "UNDO" : "tags changed since the paste");
        g_tagclr.on = 0;
        return;
    }
    if (drag_now_ms() < g_tagclr.at)
        return;
    g_tagclr.on = 0;
    tag_clear(g_tagclr.media);
}
static int fd_arm(int op) {
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    g_mv.op = op;
    g_mv.media = g_fd.media;
    g_mv.pl = g_fd.folder;
    g_mv.cid = 0;
    g_mv.from = g_mv.to = 0;
    g_mv.name[0] = 0;
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    return 0;
}
/* MENU > REMOVE on a folder row (message thread): count first; the menu stays open for the box */
static int fd_request_count(unsigned media, unsigned folder, const char *name, uintptr_t ctrl) {
    if (!media || !folder || !name || !name[0])
        return -3;
    g_fd.media = media;
    g_fd.folder = folder;
    g_fd.ctrl = ctrl;
    utf8_copy(g_fd.name, sizeof g_fd.name, name);
    return fd_arm(MV_OP_FDCOUNT);
}
static int fd_request_delete(void) {
    return fd_arm(MV_OP_FDEL);
}
static int tag_request_copy(int rows) {
    g_tag_stems = 0;
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    struct plsnap sn = plsnap_get();
    if (!sn.any_media || rows < 1) {
        drag_log("TAGCOPY refused: media %04x rows %d\n", sn.any_media, rows);
        return -90;
    }
    g_tag_want = rows;
    g_mv.op = MV_OP_TAGREAD;
    g_mv.pl = 0;
    g_mv.cid = 0;
    g_mv.media = sn.any_media;
    g_mv.from = g_mv.to = 0;
    g_mv.name[0] = 0;
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("TAGCOPY armed: %d rows (media %04x)\n", rows, sn.any_media);
    return 0;
}
static int
tag_request_stems(int rows) { /* MAKE STEMS on the Tag List: read the tags, then queue them */
    g_rs_cancel = 0;
    int rc = tag_request_copy(rows);
    if (rc == 0)
        g_tag_stems = 1;
    return rc;
}
static void *(*g_tc_orig_add)(void *, void *, void *, void *, void *);
static void *(*g_tc_orig_clear)(void *, void *, void *, void *, void *);
static void tc_note(void *self, const void *id) {
    uintptr_t vt = 0;
    uint64_t m = 0;
    __atomic_store_n(&g_tag_touched, 1, __ATOMIC_RELEASE);
    if (!self || !own_q((uintptr_t) self, &vt) || vt != TAGEDIT_VT || !id || /* hook args */
        !own_q((uintptr_t) id, &m))
        return;
    if (g_tc_del_ok) /* cached only while its destruction is tracked */
        __atomic_store_n(&g_tc_self, (uintptr_t) self, __ATOMIC_SEQ_CST);
    unsigned media = (unsigned) ((m >> 32) & 0xffff);
    if (!media)
        return;
    int k = 0;
    for (; k < 4; k++)
        if (g_tc_msid[k].media == media || !g_tc_msid[k].media)
            break;
    if (k < 4) {
        g_tc_msid[k].media = media;
        g_tc_msid[k].msid = m;
    }
}
static void *tc_add_hook(void *s, void *a, void *b, void *c, void *d) {
    tc_note(s, a);
    return g_tc_orig_add(s, a, b, c, d);
}
static void *tc_clear_hook(void *s, void *a, void *b, void *c, void *d) {
    tc_note(s, a);
    return g_tc_orig_clear(s, a, b, c, d);
}
static int tag_capture_install(void) {
    int a = azm_patch_vtable(TAGEDIT_ADD_SLOT, (void *) tc_add_hook, (void **) &g_tc_orig_add);
    int c = a == 1 ? azm_patch_vtable(TAGEDIT_CLEAR_SLOT, (void *) tc_clear_hook,
                                      (void **) &g_tc_orig_clear)
                   : -9;
    int x0 = azm_patch_vtable(TAGEDIT_VT, (void *) tc_d0_hook, (void **) &g_tc_orig_d0);
    int x1 = x0 == 1 ? azm_patch_vtable(TAGEDIT_VT + 8, (void *) tc_d1_hook, (void **) &g_tc_orig_d1)
                     : -9;
    g_tc_del_ok = x0 == 1 && x1 == 1;
    drag_log("TAGLIST capture install add=%d clear=%d dtors=%d/%d\n", a, c, x0, x1);
    return a == 1 && c == 1;
}
/* Clear the Tag List of `media` the way stock REMOVE ALL TRACKS does. */
static int tag_clear(unsigned media) {
    uintptr_t fn = 0, ed = tc_hold();
    if (!ed) {
        tc_drop();
        return -1;
    }
    for (int k = 0; k < 4; k++)
        if (g_tc_msid[k].media == media) {
            uint64_t m[4] = {g_tc_msid[k].msid, 0, 0, 0};
            if (!img_q(TAGEDIT_CLEAR_SLOT, &fn) || !fn) {
                tc_drop();
                return -2;
            }
            ((void *(*) (void *, void *, void *, void *, void *) ) fn)((void *) ed, m, 0, 0, 0);
            tc_drop();
            drag_log("TAGLIST cleared (media %04x msid %016llx)\n", media,
                     (unsigned long long) m[0]);
            return 0;
        }
    tc_drop();
    drag_log("TAGLIST not cleared: no ManagementSystemID seen for media %04x\n", media);
    return -3;
}
/* 1 = the PASTE's destination row is the copied row itself. Playlist ids are per stick, and two
 * sticks exported from one library share them, so a row on the other stick with the copied
 * row's id is a different row. */
static int pl_paste_is_self(int same_stick, unsigned dest_row, unsigned clip_id) {
    return same_stick && dest_row == clip_id;
}
/* The destination row is resolved (g_mv: pl, media, name, attribute in n, parent,
 * pname, mount): arm the write. Called from msg_tick with the slot idle. Returns
 * <0 when refused (nothing written) and the op whose message explains it. */
static int pl_paste_route(int *op_out) {
    unsigned r = g_mv.pl, media = g_mv.media, attr = (unsigned) g_mv.n, parent = g_mv.parent;
    char name[PL_TGT_NAME_MAX], pname[PL_TGT_NAME_MAX], mount[128];
    utf8_copy(name, sizeof name, g_mv.name);
    utf8_copy(pname, sizeof pname, g_mv.pname);
    snprintf(mount, sizeof mount, "%s", g_mv.mount);
    int same = media == g_clip.media;
    *op_out = MV_OP_PASTE;
    if (g_clip.move && !same)
        return -68; /* MOVE stays on one stick */
    if (g_clip.kind == CLIP_TAGGED) {
        if (attr == PL_ATTR_FOLDER)
            return -67; /* tracks go into a playlist */
        /* a PASTE does not move the selection: it stays where [SELECT] put it */
        if (!same) { /* the other USB: the cross-stick copy */
            struct pl_addtgt t;
            memset(&t, 0, sizeof t);
            t.pl = r;
            t.media = media;
            t.attr = PL_ATTR_LIST;
            utf8_copy(t.name, sizeof t.name, name);
            snprintf(t.mount, sizeof t.mount, "%s", mount);
            *op_out = MV_OP_CPREAD;
            return cp_request_tagged(g_clip.media, &t);
        }
        *op_out = MV_OP_ADDN;
        g_mv.op = MV_OP_ADDN;
        g_mv.pl = r;
        g_mv.cid = 0;
        g_mv.media = media;
        g_mv.from = g_mv.to = 0;
        utf8_copy(g_mv.name, sizeof g_mv.name, name);
        g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
        g_mv.rc = 0;
        __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
        drag_log("PASTE -> ADDN %d tagged tracks into %u \"%s\"\n", g_tag_n, r, name);
        return 0;
    }
    if (g_clip.kind == CLIP_TRACK) {
        if (attr == PL_ATTR_FOLDER)
            return -67; /* a track goes into a playlist */
        if (g_clip.move &&
            r == g_clip.src_pl) { /* CUT into its own playlist: MOVE to the highlighted row */
            if (g_paste_row < 0)
                return -69;
            unsigned to = (unsigned) g_paste_row + 1;
            if (to == g_clip.src_seq)
                return -62; /* that is where it is */
            *op_out = MV_OP_MOVE;
            g_mv.op = MV_OP_MOVE;
            g_mv.pl = r;
            g_mv.cid = g_clip.id;
            g_mv.media = media;
            g_mv.from = g_clip.src_seq;
            g_mv.to = to;
            g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
            g_mv.rc = 0;
            __atomic_store_n(&g_drag_origin, (int) g_clip.src_seq - 1,
                             __ATOMIC_RELAXED); /* the selection pin after it */
            __atomic_store_n(&g_drag_target, (int) to - 1, __ATOMIC_RELAXED);
            g_paste_cutmove = 1;
            __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
            __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
            drag_log("PASTE -> MOVE cid %u seq %u -> %u in %u\n", g_clip.id, g_clip.src_seq, to, r);
            return 0;
        }
        /* no pl_target_store: the selection stays where [SELECT] put it */
        if (same) {
            *op_out = MV_OP_ADD;
            int rc = pl_request_add(r, g_clip.id, media, name);
            if (!rc && g_clip.move)
                g_paste_remove = 1;
            return rc;
        }
        struct pl_addtgt t;
        memset(&t, 0, sizeof t);
        t.pl = r;
        t.media = media;
        t.attr = PL_ATTR_LIST;
        utf8_copy(t.name, sizeof t.name, name);
        snprintf(t.mount, sizeof t.mount, "%s", mount);
        *op_out = MV_OP_CPREAD;
        return cp_request(g_clip.media, g_clip.id, &t);
    }
    /* a playlist or folder: into a folder row, or right below a playlist row */
    unsigned dest = attr == PL_ATTR_FOLDER ? r : parent, after = attr == PL_ATTR_FOLDER ? 0 : r;
    int self = pl_paste_is_self(same, r, g_clip.id);
    if (g_fl_paste.plist &&
        g_fl_paste.row >= 0) { /* the flash: the folder it went into, or the row below */
        int base = g_fl_paste.row;
        if (!same && attr != PL_ATTR_FOLDER)
            g_fl_paste.row = -1; /* the other stick's top level: unknown */
        else
            g_fl_paste.row = (attr == PL_ATTR_FOLDER && !self ? base : base + 1) -
                             (g_fl_paste.cutrow >= 0 && g_fl_paste.cutrow < base
                                  ? 1
                                  : 0); /* the cut row left from above */
    }
    const char *dname = attr == PL_ATTR_FOLDER ? name : pname;
    if (self) { /* onto itself */
        if (g_clip.move)
            return -62; /* a CUT: nothing to do */
        dest = parent;
        after = r;
        dname = pname; /* a COPY: right below its original */
    }
    if (same) {
        *op_out = g_clip.move ? MV_OP_PLMOVE : MV_OP_PLCOPY;
        g_mv.op = *op_out;
        g_mv.pl = dest;
        g_mv.from = g_clip.id;
        g_mv.to = after;
        g_mv.media = media;
        g_mv.cid = 0;
        utf8_copy(g_mv.name, sizeof g_mv.name, dname);
        g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
        g_mv.rc = 0;
        __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
        drag_log("PASTE -> %s %u into %u \"%s\" after %u\n", g_clip.move ? "PLMOVE" : "PLCOPY",
                 g_clip.id, dest, dname, after);
        return 0;
    }
    struct pl_addtgt t;
    memset(&t, 0, sizeof t); /* a playlist or folder onto the other stick: into that folder / top */
    t.pl = dest;
    t.media = media;
    t.attr = PL_ATTR_FOLDER;
    utf8_copy(t.name, sizeof t.name, dname);
    snprintf(t.mount, sizeof t.mount, "%s", mount);
    *op_out = MV_OP_CPREAD;
    return g_clip.kind == CLIP_FOLDER ? cp_request_folder(g_clip.media, g_clip.id, &t)
                                      : cp_request_playlist(g_clip.media, g_clip.id, &t);
}
/* A stock DELETE was confirmed on the playlist list (menu_popup.c): drop the target and
 * clipboard if the deleted row was them, or was a folder (which may hold them), so nothing
 * keeps naming a deleted list. Message thread. */
static void pl_target_forget_deleted(unsigned media, unsigned pl, unsigned folder) {
    if (g_clip.kind && g_clip.media == media &&
        (folder || (g_clip.kind != CLIP_TRACK && g_clip.id == pl) || g_clip.src_pl == pl)) {
        drag_log("CLIP cleared: DELETE of %u\n", pl);
        memset(&g_clip, 0, sizeof g_clip);
    }
    struct pl_addtgt *t = pl_target_for(media);
    if (!t || (!folder && t->pl != pl))
        return;
    drag_log("TARGET cleared: DELETE of %s %u (target %u \"%s\")\n", folder ? "folder" : "playlist",
             pl, t->pl, t->name);
    memset(t, 0, sizeof *t);
}
static int pl_request_set_target(void) {
    if (__atomic_load_n(&g_drag_on, __ATOMIC_RELAXED))
        return -1;
    if (__atomic_load_n(&g_mv_state, __ATOMIC_ACQUIRE) != MV_IDLE)
        return -2;
    struct plsnap sn = plsnap_get();
    if (!sn.sel_pl || !sn.any_media) {
        drag_log("TARGET refused: no playlist row selected (sel_pl=%u media=%04x)\n", sn.sel_pl,
                 sn.any_media);
        return -3;
    }
    g_mv.op = MV_OP_TARGET;
    g_mv.pl = sn.sel_pl;
    g_mv.cid = 0;
    g_mv.media = sn.any_media;
    g_mv.from = g_mv.to = 0;
    g_mv.name[0] = 0;
    g_mv.deadline = drag_now_ms() + MV_DEADLINE_MS;
    g_mv.rc = 0;
    __atomic_store_n(&g_mv_state, MV_ARMED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_mv_kick, 1, __ATOMIC_RELEASE);
    drag_log("TARGET armed: pl=%u (media %04x)\n", g_mv.pl, g_mv.media);
    return 0;
}
/* The captured track list (g_tracklist_widget), or 0. Usable only on the message thread and
 * only while its slot is a tracked wref range: then its destructor clears it, so a non-zero
 * value is a live widget (ep_mem.h). An untracked slot proves nothing: unknown. */
static int pa_wref_tracked(_Atomic uintptr_t *slot) {
    int k = atomic_load_explicit(&g_wref_n, memory_order_acquire);
    for (int i = 0; i < k; i++)
        if (slot >= g_wref[i].base && slot < g_wref[i].base + g_wref[i].n)
            return 1;
    return 0;
}
static uintptr_t tracklist_live_widget(void) {
    _Atomic uintptr_t *slot = (_Atomic uintptr_t *) &g_tracklist_widget;
    if (!on_msg_thread() || !pa_wref_tracked(slot))
        return 0;
    return wref_get(slot);
}
/* JUCE's own selected row of the track list (message thread), or -1. Not g_sel_row:
 * that comes from the row painter and lags behind a touch selection. */
static int tracklist_selected_row_any(void) {
    uintptr_t w = tracklist_live_widget();
    uintptr_t vt = 0, lb = 0;
    if (!w || (w & 7) || !own_q(w, &vt) || vt != TRACKLIST_WIDGET_VT || !own_q(w + 0x48, &lb) ||
        lb < 0x10000)
        return -1;
    return ((int (*)(void *, int)) LISTBOX_GETSELECTED)((void *) lb, 0);
}
static int tracklist_showing(void) {
    uintptr_t w = tracklist_live_widget();
    uintptr_t vt = 0, c = 0;
    if (!w || (w & 7) || !own_q(w, &vt) || vt != TRACKLIST_WIDGET_VT || !own_q(w + 0x48, &c))
        return 0;
    return lb_showing(c);
}
/* The selected row, for every caller: a list that is not on screen has none.
 * The drag, the MENU popup's REMOVE / ADD TO eligibility and the remove/add
 * requests all ask this, so none of them can act on a list behind WAVEFORM. */
static int tracklist_selected_row(void) {
    return tracklist_showing() ? tracklist_selected_row_any() : -1;
}
static int pl_request_add_selected(void) {
    struct plsnap sn = plsnap_get();
    struct pl_addtgt *t = pl_target_for(sn.any_media);
    /* No target on this stick but one on another: copy the track there (the library
     * spool writes the target's rows in place; ls_tick runs it during a copy). */
    struct pl_addtgt *o = t ? 0 : pl_target_other(sn.any_media);
    if (!t && !o)
        return -30;
    int sel = tracklist_selected_row();
    if (!sn.any_cid || sel < 0 || (unsigned) sel != sn.any_row) {
        drag_log("ADD refused: no track row selected (cid=%u row=%u sel=%d)\n", sn.any_cid,
                 sn.any_row, sel);
        return -3;
    }
    if (!t)
        return cp_request(sn.any_media, sn.any_cid, o);
    return pl_request_add(t->pl, sn.any_cid, sn.any_media, t->name);
}
