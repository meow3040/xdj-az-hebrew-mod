/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file crash_guards.c
 * @brief Guards and diagnostics for stock EP147 faults: QR-login tick guard and crash reporter.
 *
 * install_qrlogin_guard hooks the DevicelessLoginWidgetController timer callback
 * (QRLOGIN_TICK) on the JUCE message thread. When the original would take its unchecked
 * path with a null shared_ptr from the cloud-login getter, the hook skips that one tick;
 * every other tick runs the original unchanged. The hook is installed only if the first
 * words of the target match the build it was read from, and the outcome is logged to
 * /tmp/azmod-qrlogin.log. Offsets that differ per firmware come from ep_off().
 *
 * install_crash_reporter installs async-signal-safe handlers for SIGSEGV, SIGBUS, SIGABRT,
 * SIGILL and SIGFPE that append one record per faulting pc (signal, fault address, thread
 * name, registers, frame-pointer chain, module maps) to /tmp/azmod-crash.log, then chain to
 * the previous action so the crash itself is unchanged. They are SA_ONSTACK: EP147's main
 * thread and every thread azmod/azstem start have an alternate signal stack
 * (common/azaltstack.h), so a stack overflow on those is still recorded; on EP147's other
 * threads the flag is a no-op and such a crash ends the process. A watcher thread re-checks
 * the handlers for the first minute and re-arms them if EP147 replaced them. az_install_segv is a
 * simpler observer-only SIGSEGV handler that writes /tmp/azmod-crash.txt; it installs only when the
 * crash reporter is not armed, so a fault is reported once and the two handlers never chain into
 * each other.
 *
 * Entry points: install_crash_reporter, install_qrlogin_guard, az_install_segv.
 */
static struct sigaction g_prev_segv;
static void az_hex64(char *out, unsigned long v) {
    static const char d[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = d[(v >> ((15 - i) * 4)) & 0xf];
    out[18] = 0;
}
/* write() from a fatal-signal handler: async-signal-safe, retries an interrupted or
 * partial write. Any other error is ignored on purpose: the process is going down and
 * the report is best effort, so there is nothing left to do about it. */
static void sig_write(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t) w;
        } else if (!(w < 0 && errno == EINTR)) {
            return;
        }
    }
}
static void az_segv_handler(int sig, siginfo_t *si, void *uc) {
    azlog_rotate("/tmp/azmod-crash.txt", AZLOG_CAP_BYTES); /* stat + rename: signal-safe */
    int fd = open("/tmp/azmod-crash.txt", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); /* logcap-ok: rotated above */
    if (fd >= 0) {
        char b[64];
        ucontext_t *u = (ucontext_t *) uc;
        unsigned long pc = u->uc_mcontext.pc;
        unsigned long lr = u->uc_mcontext.regs[30];
        unsigned long fp = u->uc_mcontext.regs[29];
        sig_write(fd, "\n=== SIGSEGV ===\nfault ", 23);
        az_hex64(b, (unsigned long) si->si_addr);
        sig_write(fd, b, 18);
        sig_write(fd, "\npc    ", 7);
        az_hex64(b, pc);
        sig_write(fd, b, 18);
        sig_write(fd, "\nlr    ", 7);
        az_hex64(b, lr);
        sig_write(fd, b, 18);
        sig_write(fd, "\nbacktrace:\n", 12);
        /* Walk the frame chain. Bounded, aligned and range-checked, because a second
         * fault inside a fault handler is fatal. (The range check rejects wild
         * pointers; it cannot prove a page is mapped.) */
        for (int i = 0; i < 24 && fp > 0x10000 && fp < 0x0000ffffffffffffUL && !(fp & 7); i++) {
            unsigned long nfp = *(unsigned long *) fp;
            unsigned long nlr = *(unsigned long *) (fp + 8);
            if (nlr <= 0x10000)
                break;
            sig_write(fd, "  ", 2);
            az_hex64(b, nlr);
            sig_write(fd, b, 18);
            sig_write(fd, "\n", 1);
            if (nfp <= fp)
                break; /* chain must ascend */
            fp = nfp;
        }
        /* Module bases: without them the backtrace is unresolvable, since every address
         * is ASLR-relative and names nothing once the process is gone.
         *
         * Copied from /proc/self/maps with raw read/write only (no stdio, no malloc)
         * because we are inside a fault handler. Filtered to our own .so's plus libc,
         * so it stays a few lines. */
        {
            int mf = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
            if (mf >= 0) {
                sig_write(fd, "modules:\n", 9);
                char mb[4096];
                ssize_t got;
                char line[512];
                int ll = 0;
                while ((got = read(mf, mb, sizeof mb)) > 0) {
                    for (ssize_t i = 0; i < got; i++) {
                        if (mb[i] != '\n') {
                            if (ll < (int) sizeof line - 1)
                                line[ll++] = mb[i];
                            continue;
                        }
                        line[ll] = 0;
                        /* keep only executable mappings of things we care about */
                        int keep = 0;
                        /* `k + 8 <= ll`, not `<`: these paths end the line, so the
                         * last valid match has k + 8 == ll. With `<` the azmod.so
                         * line would be dropped from its own crash dump. */
                        for (int k = 0; k + 8 <= ll; k++)
                            if (!memcmp(line + k, "azmod.so", 8) ||
                                !memcmp(line + k, "azstem.s", 8) || !memcmp(line + k, "libc-", 5)) {
                                keep = 1;
                                break;
                            }
                        if (keep) {
                            int isx = 0;
                            for (int k = 0; k + 4 < ll; k++)
                                if (line[k] == 'r' && line[k + 2] == 'x') {
                                    isx = 1;
                                    break;
                                }
                            if (isx) {
                                sig_write(fd, "  ", 2);
                                sig_write(fd, line, (size_t) ll);
                                sig_write(fd, "\n", 1);
                            }
                        }
                        ll = 0;
                    }
                }
                close(mf);
            }
        }
        close(fd);
    }
    /* hand back to EP147's handler, or die the way we would have */
    if (g_prev_segv.sa_flags & SA_SIGINFO) {
        if (g_prev_segv.sa_sigaction)
            g_prev_segv.sa_sigaction(sig, si, uc);
        return;
    }
    if (g_prev_segv.sa_handler && g_prev_segv.sa_handler != SIG_DFL &&
        g_prev_segv.sa_handler != SIG_IGN) {
        g_prev_segv.sa_handler(sig);
        return;
    }
    {
        struct sigaction d;
        memset(&d, 0, sizeof d);
        d.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &d, NULL);
        raise(SIGSEGV);
    }
}
static int g_cr_fd = -1;
/* One SIGSEGV observer at a time. The crash reporter already records a superset of this
 * handler's output, and installing both would make each the other's "previous" action
 * (this handler saves the reporter, then the reporter's re-arm saves this handler). A fault
 * would then run reporter -> this -> reporter -> ... as plain nested calls (SIGSEGV is
 * blocked inside a handler) until the stack runs out, never reaching EP147's handler or the
 * default action. So this installs only when the reporter is not armed, and never records
 * itself as its own previous action. */
static void az_install_segv(void) {
    if (g_cr_fd >= 0)
        return;
    struct sigaction sa, prev;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = az_segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &prev) == 0 && prev.sa_sigaction != az_segv_handler)
        g_prev_segv = prev;
}

/* ==== QR-code cloud login: a stock EP147 crash, guarded =====
 * A few seconds after "ERR at ccpf::CloudLoginHandler::onLinkMemberCollectionUpdated ...
 * linkMember", EP147 can SIGSEGV (1.30: 0x1a79f30) with a null deref on the message thread
 * inside gui::source_select::DevicelessLoginWidgetController's timer callback (1.30:
 * 0x1a79ea0; vtables at 0x2a4e668..). The callback hides some children, then, unless
 * [this+0x98] == 1, takes a shared_ptr from [this+0x80]->vfunc(0x18) and calls
 * ->vfunc(0x20) on it without a null check. The pointer is null after the cloud link
 * changes, e.g. disconnecting after a QR-code login.
 * The guard asks the same getter first and, only when the original is about to take that
 * unchecked path with a null, skips this one tick; every other tick runs the original
 * untouched. The getter's shared_ptr is returned through x8 (C++ non-trivial return), so
 * the call uses a >16-byte struct to get x8, and the reference it hands back is released
 * with EP147's own _Sp_counted_base::_M_release (0x6742d8).
 * 2.00: the same function is at 0x1bdf2f8 (crash at 0x1bdf388), identical except that every
 * field moved +0x10 (this+0x90 getter object, this+0xa8 state). */
#define QRLOGIN_TICK EP_1a79ea0
#define SP_M_RELEASE EP_6742d8
static void (*g_orig_qrtick)(void *);
struct qr_sp {
    void *p, *c, *pad;
}; /* > 16 bytes: returned via x8 */
static void qrtick_hook(void *self) {
    uintptr_t s = (uintptr_t) self, m = 0, vt = 0, fn = 0, st = 0;
    /* 2.00 moved both fields by +0x10. The timer's own `this` on the message thread, and the
     * member EP147's tick is about to call through: plain loads; the vtable slot is EP147's
     * image (img_q). */
    if (s && !(s & 7) && own_q(s + ep_off(0x98, 0xa8), &st) && (int) (st & 0xffffffff) != 1 &&
        own_q(s + ep_off(0x80, 0x90), &m)) {
        int null = !m;
        if (m && !(m & 7) && own_q(m, &vt) && vt && img_q(vt + 0x18, &fn) && fn) {
            struct qr_sp r = ((struct qr_sp (*)(void *)) fn)((void *) m);
            null = !r.p;
            if (r.c)
                ((void (*)(void *)) SP_M_RELEASE)(r.c);
        }
        if (null) {
            static int logs = 5;
            if (logs-- > 0) {
                FILE *f = azlog_open("/tmp/azmod-qrlogin.log", "a");
                if (f) {
                    fprintf(f, "QR login tick skipped: no session object (EP147 would have crashed "
                               "here)\n");
                    fclose(f);
                }
            }
            return;
        }
    }
    g_orig_qrtick(self);
}
static void install_qrlogin_guard(void) {
    /* only on the function this was read from: its first two words */
    uintptr_t w = 0;
    int ok = img_q(QRLOGIN_TICK, &w) && w == 0x52800001a9b37bfdULL;
    int rc =
        ok ? azm_install_hook(QRLOGIN_TICK, (void *) qrtick_hook, (void **) &g_orig_qrtick) : 0;
    /* say which: a refused guard would otherwise look exactly like a working one */
    FILE *f = azlog_open("/tmp/azmod-qrlogin.log", "a");
    if (f) {
        fprintf(f, "QR login guard at 0x%lx: %s (first words %016lx, hook rc=%d)\n",
                (unsigned long) QRLOGIN_TICK, ok ? "installed" : "REFUSED, code does not match",
                (unsigned long) w, rc);
        fclose(f);
    }
}
/* ==== Crash reporter ==========================================
 * EP147 runs with ulimit -c 0 (no core file), so this is the only record of a crash. On
 * SIGSEGV / SIGBUS / SIGABRT / SIGILL / SIGFPE it appends one block to /tmp/azmod-crash.log:
 * signal, fault address, thread name, pc/lr/sp/fp and x0..x28, the frame-pointer chain
 * (EP147 keeps frame records), then the maps lines of EP147 / azmod / azstem so every pc
 * resolves to module+offset. Async-signal-safe only (write, process_vm_readv for the frames,
 * open/read of maps, no stdio, no malloc). Then it chains to the previous
 * action (see cr_chain), so the crash itself is unchanged.
 *
 * Size: /tmp is RAM and the file outlives every EP147 restart, so it is capped. At start
 * a file without CR_RECORD_ROOM left below CR_LOG_MAX moves to azmod-crash.log.1
 * (azlog_rotate), so this process's first records always fit; while running, the bytes
 * written are counted and a record that would pass the cap is cut off with one marker
 * line. A crash loop therefore never holds more than about 2 x CR_LOG_MAX, and the
 * newest crash is always the one kept. */
#include <ucontext.h>
#define CR_LOG_MAX AZLOG_CAP_BYTES
#define CR_RECORD_ROOM (32L * 1024) /* one record is ~4 KB (registers, frames, maps lines) */
static long g_cr_bytes; /* the log's size: at open, then every byte written */
static struct sigaction g_cr_old[32];
static void cr_put(const char *s) {
    static const char full[] = "\n=== crash log full: later records dropped until the next start ===\n";
    size_t n = 0;
    while (s[n])
        n++;
    if (g_cr_fd < 0)
        return;
    long at = __atomic_fetch_add(&g_cr_bytes, (long) n, __ATOMIC_RELAXED);
    if (at + (long) n <= CR_LOG_MAX - (long) sizeof full) {
        (void) !write(g_cr_fd, s, n);
    } else if (at <= CR_LOG_MAX - (long) sizeof full) { /* the first write past the cap: say so */
        (void) !write(g_cr_fd, full, sizeof full - 1);
    }
}
static void cr_hex(unsigned long v) {
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int d = (int) ((v >> ((15 - i) * 4)) & 15);
        b[2 + i] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[18] = 0;
    cr_put(b);
}
static void cr_dec(long v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long) -v : (unsigned long) v;
    do {
        b[--i] = (char) ('0' + u % 10);
        u /= 10;
    } while (u && i > 1);
    if (neg)
        b[--i] = '-';
    cr_put(b + i);
}
static int cr_read8(unsigned long a, unsigned long *v) { /* a crashed thread's frame (ep_mem.h) */
    uintptr_t q = 0;
    int ok = crash_peek8((uintptr_t) a, &q);
    *v = (unsigned long) q;
    return ok;
}
static void cr_chain(int sig, siginfo_t *si, void *ctx);
/* The threads currently inside cr_handler, one slot each (0 = free). Re-entry is judged per
 * thread: with a single owner, a thread that faulted while another thread was inside could
 * not see its own re-entry and would chain back and forth with a handler that calls its
 * predecessor (us) until its stack ran out. Lock-free and async-signal-safe. */
#define CR_INSIDE_MAX 16
static volatile long g_cr_inside[CR_INSIDE_MAX];
static int cr_inside_find(long tid) {
    for (int i = 0; i < CR_INSIDE_MAX; i++)
        if (__atomic_load_n(&g_cr_inside[i], __ATOMIC_ACQUIRE) == tid)
            return i;
    return -1;
}
static int cr_inside_claim(long tid) {
    for (int i = 0; i < CR_INSIDE_MAX; i++) {
        long none = 0;
        if (__atomic_compare_exchange_n(&g_cr_inside[i], &none, tid, 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return i;
    }
    return -1;
}
static void cr_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *) ctx;
    long tid = (long) syscall(SYS_gettid);
    /* Re-entered on this thread while still inside this handler: the chained-to handler
     * called back into us. Chaining again would recurse without bound, so fall to the
     * default action: a fault re-executes after the unwind and kills the process, a sent or
     * raised signal is re-raised once the outer handler returns. */
    if (cr_inside_find(tid) >= 0) {
        struct sigaction d;
        memset(&d, 0, sizeof d);
        d.sa_handler = SIG_DFL;
        sigemptyset(&d.sa_mask);
        sigaction(sig, &d, 0);
        if (sig == SIGABRT || si->si_code <= 0)
            raise(sig);
        return;
    }
    /* More concurrent faults than slots: record nothing, end the process the default way
     * (an unclaimed thread could not detect its own re-entry). */
    int slot = cr_inside_claim(tid);
    if (slot < 0) {
        struct sigaction d;
        memset(&d, 0, sizeof d);
        d.sa_handler = SIG_DFL;
        sigemptyset(&d.sa_mask);
        sigaction(sig, &d, 0);
        if (sig == SIGABRT || si->si_code <= 0)
            raise(sig);
        return;
    }
    /* a fault inside a community mod: name it, so the next start suspends only that mod */
    const char *blamed =
        cm_blame_fault((uintptr_t) uc->uc_mcontext.pc, (uintptr_t) uc->uc_mcontext.regs[30],
                       (uintptr_t) uc->uc_mcontext.regs[29]);
    /* One record per faulting pc. EP147's own handler (which we chain to) returns, so a
     * real fault re-executes and re-enters here ~180 times a second until EP147 gives up,
     * which would fill RAM-backed /tmp in seconds. The first record has everything; the
     * repeats only chain. */
    {
        static volatile unsigned long last_pc;
        static volatile int last_sig;
        if (uc->uc_mcontext.pc == last_pc && sig == last_sig) {
            cr_chain(sig, si, ctx);
            __atomic_store_n(&g_cr_inside[slot], 0L, __ATOMIC_RELEASE);
            return;
        }
        last_pc = uc->uc_mcontext.pc;
        last_sig = sig;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    cr_put("\n=== azmod crash: signal ");
    cr_dec(sig);
    cr_put(" at ");
    cr_dec((long) ts.tv_sec);
    cr_put(" fault ");
    cr_hex((unsigned long) si->si_addr);
    cr_put(" tid ");
    cr_dec((long) syscall(SYS_gettid));
    {
        char p[64] = "/proc/self/task/", *q = p + 16;
        long t = (long) syscall(SYS_gettid);
        char d[16];
        int i = 15;
        d[i] = 0;
        do {
            d[--i] = (char) ('0' + t % 10);
            t /= 10;
        } while (t && i);
        for (char *x = d + i; *x; x++)
            *q++ = *x;
        const char *cm = "/comm";
        while (*cm)
            *q++ = *cm++;
        *q = 0;
        int f = open(p, O_RDONLY | O_CLOEXEC);
        if (f >= 0) {
            char nm[32];
            ssize_t r = read(f, nm, sizeof nm - 1);
            close(f);
            if (r > 0) {
                nm[r] = 0;
                for (ssize_t k = 0; k < r; k++)
                    if (nm[k] == '\n')
                        nm[k] = 0;
                cr_put(" \"");
                cr_put(nm);
                cr_put("\"");
            }
        }
    }
    unsigned long pc = uc->uc_mcontext.pc, sp = uc->uc_mcontext.sp, fp = uc->uc_mcontext.regs[29],
                  lr = uc->uc_mcontext.regs[30];
    if (blamed) { /* the mod's own .so is in the maps below, so its pc maps to a file offset */
        cr_put("\ncommunity mod: ");
        cr_put(blamed);
    }
    cr_put("\npc ");
    cr_hex(pc);
    cr_put(" lr ");
    cr_hex(lr);
    cr_put(" sp ");
    cr_hex(sp);
    cr_put(" fp ");
    cr_hex(fp);
    for (int r = 0; r < 29; r++) {
        if (!(r % 4))
            cr_put("\n");
        cr_put(" x");
        cr_dec(r);
        cr_put("=");
        cr_hex(uc->uc_mcontext.regs[r]);
    }
    cr_put("\nframes:");
    for (int i = 0; i < 40 && fp; i++) { /* {prev fp, return address} records */
        unsigned long nfp = 0, ra = 0;
        if (!cr_read8(fp, &nfp) || !cr_read8(fp + 8, &ra))
            break;
        cr_put("\n  ");
        cr_hex(ra);
        if (nfp <= fp)
            break;
        fp = nfp;
    }
    cr_put("\nmaps:\n");
    int mf = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (mf >= 0) { /* only our modules', community mods' and EP147's own lines */
        char buf[4096], line[512];
        int ln = 0;
        ssize_t r;
        while ((r = read(mf, buf, sizeof buf)) > 0)
            for (ssize_t k = 0; k < r; k++) {
                if (ln < (int) sizeof line - 1)
                    line[ln++] = buf[k];
                if (buf[k] != '\n')
                    continue;
                line[ln] = 0;
                int keep = 0;
                for (int j = 0; line[j] && !keep; j++)
                    if (line[j] == '/' &&
                        (!strncmp(line + j, "/EP147", 6) || !strncmp(line + j, "/azmod", 6) ||
                         !strncmp(line + j, "/azstem", 7) || !strncmp(line + j, "/community/", 11)))
                        keep = 1;
                if (keep)
                    cr_put(line);
                ln = 0;
            }
        close(mf);
    }
    cr_put("=== end\n");
    cr_chain(sig, si, ctx);
    __atomic_store_n(&g_cr_inside[slot], 0L, __ATOMIC_RELEASE);
}
static void cr_chain(int sig, siginfo_t *si, void *ctx) {
    /* hand over to whoever had it: EP147 installs its own handlers for ILL/ABRT/BUS/FPE/SEGV
     * after start-up (SigCgt), so cr_rearm puts us back in front and we chain to it */
    struct sigaction *o = &g_cr_old[sig];
    if ((o->sa_flags & SA_SIGINFO) && o->sa_sigaction) {
        o->sa_sigaction(sig, si, ctx);
        return;
    }
    if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN && o->sa_handler) {
        o->sa_handler(sig);
        return;
    }
    sigaction(sig, o, 0); /* default: die, as before */
    if (sig == SIGABRT || si->si_code <= 0)
        raise(sig); /* raised / sent: re-raise; a real fault re-faults on return */
}
/* Runs on the cr_watch thread. For each signal whose handler EP147 replaced, keep EP147's
 * as the one we chain to and put ours in front again. */
static void cr_rearm(void) {
    if (g_cr_fd < 0)
        return;
    int sigs[5] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
    for (int i = 0; i < 5; i++) {
        struct sigaction cur;
        memset(&cur, 0, sizeof cur);
        if (sigaction(sigs[i], 0, &cur) || cur.sa_sigaction == cr_handler)
            continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = cr_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        struct sigaction prev;
        if (sigaction(sigs[i], &sa, &prev) == 0)
            g_cr_old[sigs[i]] = prev; /* chain to whoever took it */
    }
}
static void *cr_watch(void *a) { /* the first minute: EP147 installs its handlers late */
    (void) a;
    for (int i = 0; i < 30; i++) {
        sleep(2);
        cr_rearm();
    }
    return 0;
}
static void install_crash_reporter(void) {
    azlog_rotate("/tmp/azmod-crash.log", CR_LOG_MAX - CR_RECORD_ROOM);
    g_cr_fd = open("/tmp/azmod-crash.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); /* logcap-ok: rotated above, cr_put caps */
    if (g_cr_fd < 0)
        return;
    {
        struct stat st;
        g_cr_bytes = fstat(g_cr_fd, &st) == 0 ? (long) st.st_size : 0;
    }
    int sigs[5] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
    for (int i = 0; i < 5; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = cr_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, &g_cr_old[sigs[i]]);
    }
}
