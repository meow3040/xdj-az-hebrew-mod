/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file ssh_approve_logic.h
 * @brief Which pending SSH key the Mod Menu asks about next, and the question's text
 *
 * The deck's sshgate hook lists the keys a USB stick offers that this deck has not approved, one
 * line each in /run/az-ssh/pending: "<fingerprint> <type> <base64>". Each fingerprint is asked
 * about at most once per start; a key the DJ canceled stays pending and is asked again at the
 * next start. No file access here: ssh_approve.c reads the list and writes the answer.
 */
#ifndef SSH_APPROVE_LOGIC_H
#define SSH_APPROVE_LOGIC_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SSHA_MAX 8 /* keys tracked per start */
#define SSHA_FP 72 /* "SHA256:" + 43 base64 characters, with room to spare */

struct ssha {
    char asked[SSHA_MAX][SSHA_FP]; /* fingerprints already asked about this start */
    int nasked;
};

/* The first field of each line of a pending list: up to max fingerprints. */
static int ssha_parse(const char *buf, char fps[][SSHA_FP], int max) {
    int n = 0;
    while (buf && *buf && n < max) {
        const char *e = buf;
        while (*e && *e != ' ' && *e != '\n')
            e++;
        size_t len = (size_t) (e - buf);
        if (len > 0 && len < SSHA_FP) {
            memcpy(fps[n], buf, len);
            fps[n][len] = 0;
            n++;
        }
        while (*e && *e != '\n')
            e++;
        buf = *e ? e + 1 : e;
    }
    return n;
}

static int ssha_was_asked(const struct ssha *s, const char *fp) {
    for (int i = 0; i < s->nasked; i++)
        if (!strcmp(s->asked[i], fp))
            return 1;
    return 0;
}

/* The index of the first fingerprint not yet asked about this start, or -1. */
static int ssha_next(const struct ssha *s, char fps[][SSHA_FP], int n) {
    for (int i = 0; i < n; i++)
        if (!ssha_was_asked(s, fps[i]))
            return i;
    return -1;
}

static void ssha_mark(struct ssha *s, const char *fp) {
    if (ssha_was_asked(s, fp) || s->nasked >= SSHA_MAX)
        return;
    snprintf(s->asked[s->nasked++], SSHA_FP, "%s", fp);
}

/* Forget fp (the question could not be shown, so it is asked again later). */
static void ssha_unmark(struct ssha *s, const char *fp) {
    for (int i = 0; i < s->nasked; i++)
        if (!strcmp(s->asked[i], fp)) {
            memmove(s->asked[i], s->asked[i + 1], (size_t) (s->nasked - i - 1) * SSHA_FP);
            s->nasked--;
            return;
        }
}

#define SSHA_TITLE "Remote access"
#define SSHA_YES "Allow"
/* The question for one key. The fingerprint is shortened to what fits on a line; the full one is
 * in /run/az-ssh/pending and in the sshgate log. */
static void ssha_text(const char *fp, char *out, size_t n) {
    char shown[40];
    size_t len = strlen(fp);
    if (len > 32)
        snprintf(shown, sizeof shown, "%.32s...", fp);
    else
        snprintf(shown, sizeof shown, "%s", fp);
    snprintf(out, n, "Allow remote access (SSH) for key %s? Only allow a key you added yourself.",
             shown);
}

#endif
