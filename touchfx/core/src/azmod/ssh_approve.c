/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file ssh_approve.c
 * @brief Remote access (SSH) for a key from a USB stick only after the DJ allows it on the deck
 *
 * The sshgate hook (deck/boot.d/10-sshgate.sh) holds every key a stick offers that this deck has
 * not approved in /run/az-ssh/pending and never installs it. Here, on the overlay thread, the core
 * asks about each one in its yes/no dialog (azcm_confirm: Cancel focused, BACK = no). Allow writes
 * the fingerprint to /run/az-ssh/approve; a path unit then runs the hook to record the key on the
 * deck and let it log in. Cancel writes nothing: the key stays pending and is asked about again at
 * the next start.
 *
 * The question never interrupts a set: it waits until no deck has a track loaded. The list is read
 * once a second at most.
 *
 * Included by azmod.c after sdk_ui.c.
 */
#include "ssh_approve_logic.h"

#ifndef SSHA_DIR
#define SSHA_DIR "/run/az-ssh" /* the unit test points this at a scratch dir */
#endif
#define SSHA_PENDING SSHA_DIR "/pending"
#define SSHA_APPROVE SSHA_DIR "/approve"

static struct ssha g_ssha;
static int g_ssha_open;               /* a question of ours is on screen */
static char g_ssha_cur[SSHA_FP];      /* the fingerprint it is about */
static long g_ssha_next_ms;

static void ssha_done(int yes, void *user) {
    (void) user;
    g_ssha_open = 0;
    if (!yes)
        return;
    int fd = open(SSHA_APPROVE ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    char line[SSHA_FP + 2];
    int len = snprintf(line, sizeof line, "%s\n", g_ssha_cur);
    int ok = write(fd, line, (size_t) len) == len;
    close(fd);
    if (ok)
        rename(SSHA_APPROVE ".new", SSHA_APPROVE);
    else
        unlink(SSHA_APPROVE ".new");
}

/* Overlay thread, every tick. */
static void ssh_approve_tick(void) {
    long now = now_ms();
    if (g_ssha_open || now < g_ssha_next_ms)
        return;
    g_ssha_next_ms = now + 1000;
    int fd = open(SSHA_PENDING, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    if (azcm_any_loaded())
        return; /* never in the middle of a set */
    char fps[SSHA_MAX][SSHA_FP];
    int cnt = ssha_parse(buf, fps, SSHA_MAX);
    int i = ssha_next(&g_ssha, fps, cnt);
    if (i < 0)
        return;
    char text[160];
    ssha_text(fps[i], text, sizeof text);
    snprintf(g_ssha_cur, sizeof g_ssha_cur, "%s", fps[i]);
    ssha_mark(&g_ssha, fps[i]);
    if (azcm_confirm(SSHA_TITLE, text, SSHA_YES, ssha_done, 0) == 0)
        g_ssha_open = 1;
    else
        ssha_unmark(&g_ssha, fps[i]); /* another question is open: ask later */
}
