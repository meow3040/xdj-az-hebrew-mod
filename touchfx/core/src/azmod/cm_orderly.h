/* SPDX-License-Identifier: GPL-3.0-only */
/* cm_orderly.h: an orderly stop is not a failed start (the community quarantine, community.c).
 *
 * Every start with community mods enabled counts itself in CM_STARTING, and 90 s of running
 * clears it; two starts in a row that end before that hold every community mod back. EP147
 * does not catch SIGTERM, so systemd's stop (a Reload, an update, a restart from a shell, the
 * power button) ends it at once, and before this every such stop in the first 90 s counted as
 * a start that failed. A stop is orderly when EP147 was alive when it came: its message thread
 * ran its repaint timer (rtick_hook, ~60/s, also on an idle screen) within the last
 * CMO_ALIVE_MS. Then the stop clears the count. A deck that froze (the case the quarantine is
 * for: the DJ can only switch it off), a crash, a SIGKILL and a power cut still count.
 *
 * The overlay thread notes the heartbeat (cmo_note, every tick); the SIGTERM handler decides
 * with async-signal-safe calls only, restores the previous disposition and raises the signal
 * again, so EP147 ends exactly as it did without the handler. */
#ifndef AZMOD_CM_ORDERLY_H
#define AZMOD_CM_ORDERLY_H
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CMO_ALIVE_MS 1500 /* the repaint timer's last tick at most this long before the stop */

static char g_cmo_path[256];             /* the count file to clear; set before the handler */
static long g_cmo_seen = -1;             /* the heartbeat count last seen (overlay thread) */
static long g_cmo_alive_ms = -1;         /* when it last moved, CLOCK_MONOTONIC ms; -1 = never */
static struct sigaction g_cmo_old;
static int g_cmo_installed;

static long cmo_now_ms(void) { /* async-signal-safe */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* alive at `now`: the heartbeat moved within CMO_ALIVE_MS; never seen moving = not alive */
static int cmo_alive(long alive_ms, long now) {
    return alive_ms >= 0 && now >= alive_ms && now - alive_ms <= CMO_ALIVE_MS;
}
/* overlay thread, every tick: the heartbeat's count now (EP147's repaint timer ticks) */
static void cmo_note(long count, long now) {
    if (count != g_cmo_seen) {
        if (g_cmo_seen >= 0)
            __atomic_store_n(&g_cmo_alive_ms, now, __ATOMIC_RELEASE);
        g_cmo_seen = count;
    }
}
static void cmo_term(int sig) {
    int e = errno;
    if (g_cmo_path[0] && cmo_alive(__atomic_load_n(&g_cmo_alive_ms, __ATOMIC_ACQUIRE), cmo_now_ms()))
        unlink(g_cmo_path);
    sigaction(sig, &g_cmo_old, NULL); /* EP147's own disposition (the default: end the process) */
    errno = e;
    raise(sig);
}
/* while a start is being counted in `path`: an orderly stop clears it. 0 = installed. */
static int cmo_install(const char *path) {
    size_t n = strlen(path);
    if (g_cmo_installed || n >= sizeof g_cmo_path)
        return -1;
    memcpy(g_cmo_path, path, n + 1);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = cmo_term;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, &g_cmo_old) != 0)
        return -1;
    g_cmo_installed = 1;
    return 0;
}
#endif
