/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file bluetooth.c
 * @brief Bluetooth auto-connect, remembered channel inputs and UART speed recovery
 *
 * Stock never connects to the paired device by itself. The sink hook connects to the paired device when the stack comes up
 * (AZF_BT_AUTO_CONNECT), and bt_mods_tick switches the channel the DJ last set to
 * Bluetooth back to it on connect and to DECK on disconnect (AZF_BT_REMEMBER_CHANNEL).
 * bt_uart_recover repairs the controller's UART speed after EP147 was stopped with
 * Bluetooth on. im_tick applies Channel Input Select Memory's saved inputs when EP147's own
 * start-up apply missed them (input_memory.h). Every action is EP147's own routine.
 * Diagnostics: /tmp/azmod-bt.log.
 *
 * Entry points: install_bt_mods, bt_mods_tick, bt_uart_recover.
 */
#include "input_memory.h"
/* EP147 runs the sink role of its own stack (libanwbt*):
 *   - sink general handler FUN_0211c0e8(sink, ctx, code, p4): 0x93 = SET_LOCAL_NAME_CFM,
 *     the last step of every stack start (boot, reload, Bluetooth switched on). The paired
 *     device's address is at sink+0x88 (0 = none; cleared when the pairing is removed).
 *     Stock does nothing on 0x93 in this role (the source role auto-connects there).
 *   - AnWBT_A2DP_Connect(&addr) connects out: rc 0, then A2DP event 0x300 (CONNECT_CFM,
 *     p4[0] = 0 on success), which EP147's A2DP handler FUN_0211c918 only logs, leaving
 *     the icon gray although audio plays. Its "connected" routine FUN_0211c840(a2dp)
 *     (sets +0xc9 = 1, tells the listeners, saves the pairing) is what a phone-made
 *     connection runs; we run it for ours, once, if +0xc9 is still 0.
 *   - +0xc9 of that object is EP147's connected flag (FUN_0211c840 sets it, the
 *     disconnected routine FUN_0211bed8 clears it), so connect/disconnect are read off
 *     its edges, whoever made them.
 *   - mixer channels: usecase::mixer::Channel (vt 0x026cf590), built by FUN_00a77a20 with
 *     the channel index (0..3) -> stored at +0x70; input at +0x6c (0 = DECK, 3 =
 *     Bluetooth). Channel::select(ch, &src) FUN_00a77770 is what Utility > Mixer calls,
 *     on the message thread, so the hooks only queue edges and bt_mods_tick selects.
 *     A channel is used only while its vtable and +0x70 still check out. */
#define BT_SNK_EV_FN EP_211c0e8
#define BT_A2DP_EV_FN EP_211c918
#define BT_CONNECTED_FN EP_211c840
#define MIX_CH_CTOR_FN EP_a77a20
#define MIX_CH_SELECT EP_a77770
#define MIX_CH_VT EP_26cf590
#define MIX_IN_DECK 0
#define MIX_IN_BT 3
static _Atomic uintptr_t g_mixch[4];
static _Atomic int g_bt_connected = -1; /* EP147's +0xc9 as last seen; -1 = not seen yet */
static _Atomic int g_bt_conn_pending;   /* our AnWBT_A2DP_Connect is in flight */
static _Atomic int g_bt_edge;           /* 1 = connected, 2 = disconnected: for bt_mods_tick */
static void bt_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-bt.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
static int bt_on(int mod) {
    return az_on(mod) && az_active(mod);
}
/* The two event hooks read only their own arguments, inside EP147's call: plain loads. */
static void (*g_o_bt_snk)(long, long, unsigned, int *);
static void bt_snk_hook(long sink, long ctx, unsigned code, int *p4) {
    g_o_bt_snk(sink, ctx, code, p4);
    if ((code & 0xffff) != 0x93 || !bt_on(AZF_BT_AUTO_CONNECT))
        return;
    uintptr_t a = 0;
    if (!own_q((uintptr_t) sink + 0x88, &a) || !(a & 0xffffffffffffULL)) {
        bt_log("auto connect: no paired device\n");
        return;
    }
    if (__atomic_load_n(&g_bt_connected, __ATOMIC_RELAXED) == 1) {
        bt_log("auto connect: already connected\n");
        return;
    }
    static int (*conn)(void *);
    if (!conn)
        *(void **) &conn = dlsym(RTLD_DEFAULT, "AnWBT_A2DP_Connect");
    if (!conn) {
        bt_log("auto connect: AnWBT_A2DP_Connect not found\n");
        return;
    }
    __atomic_store_n(&g_bt_conn_pending, 1, __ATOMIC_RELEASE);
    int rc = conn((void *) (sink + 0x88));
    if (rc != 0)
        __atomic_store_n(&g_bt_conn_pending, 0, __ATOMIC_RELEASE);
    bt_log("auto connect: AnWBT_A2DP_Connect(paired device) rc=%d\n", rc);
}
static void (*g_o_bt_a2dp)(long, long, unsigned, unsigned *);
static void bt_a2dp_hook(long a2dp, long ctx, unsigned code, unsigned *p4) {
    g_o_bt_a2dp(a2dp, ctx, code, p4);
    uintptr_t w = 0;
    if ((code & 0xffff) == 0x300 && __atomic_exchange_n(&g_bt_conn_pending, 0, __ATOMIC_ACQ_REL)) {
        uintptr_t st = 0;
        int ok = p4 && own_q((uintptr_t) p4, &st) && (st & 0xffffffff) == 0;
        if (ok && own_q((uintptr_t) a2dp + 0xc8, &w) && !((w >> 8) & 0xff)) {
            ((void (*)(long)) BT_CONNECTED_FN)(
                a2dp); /* EP147's own "connected", as for a phone-made link */
            bt_log("auto connect: connected (EP147's connected routine run)\n");
        } else
            bt_log("auto connect: confirm status=%lu, not connected\n",
                   (unsigned long) (st & 0xffffffff));
    }
    if (!own_q((uintptr_t) a2dp + 0xc8, &w))
        return;
    int now = (int) ((w >> 8) & 0xff) != 0;
    int was = __atomic_exchange_n(&g_bt_connected, now, __ATOMIC_ACQ_REL);
    if (was != now && was >= 0) {
        __atomic_store_n(&g_bt_edge, now ? 1 : 2, __ATOMIC_RELEASE);
        bt_log("bluetooth %s\n", now ? "connected" : "disconnected");
    } else if (was < 0 && now) {
        __atomic_store_n(&g_bt_edge, 1, __ATOMIC_RELEASE);
        bt_log("bluetooth connected\n");
    }
}
/* Channel lifetime. usecase::mixer::Channel's two destructors (vtable slots 0 and 1; each
 * carries the whole body, and nothing calls them except through the vtable) are inline-hooked:
 * mixch_forget unpublishes the channel and waits for a reader inside mixch_hold/mixch_drop
 * (a few loads and EP147's input setter, never I/O) before the memory goes back. A channel
 * loaded between hold and drop is therefore live. Without both hooks nothing is recorded and
 * Remember Selected Channel does nothing. */
static int g_mixch_dtor_ok;
static _Atomic int g_mixch_readers;
static void (*g_o_mixch_d1)(long);
static void (*g_o_mixch_d0)(long);
static void mixch_forget(long self) {
    for (int i = 0; i < 4; i++) {
        uintptr_t e = (uintptr_t) self;
        __atomic_compare_exchange_n(&g_mixch[i], &e, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
    }
    if (!on_msg_thread()) /* readers are message-thread only: else a reader is our own caller */
        ep_wait_readers(&g_mixch_readers);
}
static void mixch_d1_hook(long self) {
    mixch_forget(self);
    g_o_mixch_d1(self);
}
static void mixch_d0_hook(long self) {
    mixch_forget(self);
    g_o_mixch_d0(self);
}
static void mixch_hold(void) {
    __atomic_fetch_add(&g_mixch_readers, 1, __ATOMIC_SEQ_CST);
}
/* mixch_get answers only on the message thread (mixch_forget relies on it). */
static void mixch_drop(void) {
    __atomic_fetch_sub(&g_mixch_readers, 1, __ATOMIC_RELEASE);
}
static void (*g_o_mixch_ctor)(long, long, int *, long, long);
static void mixch_ctor_hook(long self, long p2, int *idx, long p4, long p5) {
    g_o_mixch_ctor(self, p2, idx, p4, p5);
    int i = idx ? *idx : -1;
    if (g_mixch_dtor_ok && i >= 0 && i < 4) /* published after the ctor returned */
        __atomic_store_n(&g_mixch[i], (uintptr_t) self, __ATOMIC_SEQ_CST);
}
/* Between mixch_hold and mixch_drop only: this channel, or 0; its vtable and index re-proven. */
static uintptr_t mixch_get(int i) {
    if (!on_msg_thread())
        return 0;
    uintptr_t c = __atomic_load_n(&g_mixch[i], __ATOMIC_SEQ_CST), vt = 0, w = 0;
    if (!c || (c & 7) || !own_q(c, &vt) || vt != MIX_CH_VT || !own_q(c + 0x70, &w) ||
        (int) (w & 0xffffffff) != i)
        return 0;
    return c;
}
static int mixch_input(uintptr_t c) {
    uintptr_t w = 0;
    return own_q(c + 0x68, &w) ? (int) ((w >> 32) & 0xff) : -1;
}
static void mixch_select(uintptr_t c, int src) {
    uint32_t v = (uint32_t) src;
    ((void (*)(long, void *)) MIX_CH_SELECT)((long) c, &v);
}
/* "Remember Selected Channel": the channel the DJ last set to Bluetooth in Utility >
 * Mixer (CH3 = index 2, CH4 = 3), kept in settings/azmod-btchannel ("3" / "4"; absent =
 * none). Watched here by its input byte, so it is EP147's own selection that counts; a
 * change we make ourselves updates the watch copy and is never read back as the DJ's. */
#define BT_CHAN_PATH "/home/root/settings/azmod-btchannel"
static int bt_chan_load(void) {
    FILE *f = fopen(BT_CHAN_PATH, "re");
    int n = 0;
    if (!f)
        return -1;
    if (fscanf(f, "%d", &n) != 1)
        n = 0;
    fclose(f);
    return n == 3 ? 2 : n == 4 ? 3
                               : -1;
}
static void bt_chan_save(int idx) {
    if (idx < 0) {
        unlink(BT_CHAN_PATH);
        return;
    }
    char line[16];
    int n = snprintf(line, sizeof line, "%d\n", idx + 1);
    settings_write(BT_CHAN_PATH, line, (size_t) n, 1);
}
/* Channel Input Select Memory at start (input_memory.h). g_im is written once, by
 * install_bt_mods in the constructor (before EP147 starts any thread that could read it),
 * and read only by im_tick on the message thread afterwards. */
#define IM_SETTINGS_PATH "/home/root/settings/SETTINGS.DAT"
#define IM_OFF_PATH "/home/root/settings/azmod-inputmemory.off"
#define IM_FILE_MAX 65536
static struct im_saved g_im = {-1, {-1, -1, -1, -1}};
static void im_load(void) { /* constructor: the file is still the one EP147 saved last */
    if (access(IM_OFF_PATH, F_OK) == 0)
        return;
    int fd = open(IM_SETTINGS_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    static uint8_t buf[IM_FILE_MAX];
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, sizeof buf - n);
        if (r <= 0)
            break;
        n += (size_t) r;
        if (n == sizeof buf)
            break;
    }
    close(fd);
    if (n == sizeof buf) /* larger than any settings file: not read */
        return;
    im_parse(buf, n, &g_im);
    bt_log("input memory: memory=%d saved inputs %d %d %d %d\n", g_im.memory, g_im.input[0],
           g_im.input[1], g_im.input[2], g_im.input[3]);
}
static void im_tick(void) { /* message thread */
    static int done[IM_CHANNELS], all_done;
    static long long first[IM_CHANNELS];
    if (all_done)
        return;
    if (g_im.memory != 1) {
        all_done = 1;
        return;
    }
    long long now = drag_now_ms();
    int pending = 0;
    mixch_hold();
    for (int i = 0; i < IM_CHANNELS; i++) {
        if (done[i])
            continue;
        pending = 1;
        uintptr_t c = mixch_get(i);
        if (!c)
            continue;
        if (!first[i])
            first[i] = now;
        int cur = mixch_input(c);
        int d = im_decide(&g_im, i, cur, (long) (now - first[i]));
        if (d == IM_SELECT) {
            mixch_select(c, g_im.input[i]);
            bt_log("input memory: CH%d was DECK at start, saved input %d applied (now %d)\n",
                   i + 1, g_im.input[i], mixch_input(c));
        }
        if (d != IM_WAIT)
            done[i] = 1;
    }
    mixch_drop();
    if (!pending)
        all_done = 1;
}
static void bt_mods_tick(void) { /* message thread */
    im_tick();
    static int remembered = -2, last_on = -1, seen[4] = {-1, -1, -1, -1};
    if (remembered == -2)
        remembered = bt_chan_load();
    int on = bt_on(AZF_BT_REMEMBER_CHANNEL);
    int edge = __atomic_exchange_n(&g_bt_edge, 0, __ATOMIC_ACQ_REL);
    int conn = __atomic_load_n(&g_bt_connected, __ATOMIC_RELAXED) == 1;
    int in[4] = {-1, -1, -1, -1};
    mixch_hold();
    for (int i = 2; i < 4; i++) {
        uintptr_t c = mixch_get(i);
        in[i] = c ? mixch_input(c) : -2; /* -2: channel not known */
    }
    mixch_drop();
    for (int i = 2; i < 4; i++) { /* the DJ's own changes on CH3 / CH4 */
        if (in[i] == -2)
            continue;
        int v = in[i];
        if (seen[i] >= 0 && v != seen[i] && on) {
            if (v == MIX_IN_BT && remembered != i) {
                remembered = i;
                bt_chan_save(i);
                bt_log("input: CH%d remembered for Bluetooth\n", i + 1);
            } else if (seen[i] == MIX_IN_BT && v != MIX_IN_BT && remembered == i) {
                remembered = -1;
                bt_chan_save(-1);
                bt_log("input: CH%d taken off Bluetooth by hand, forgotten\n", i + 1);
            }
        }
        seen[i] = v;
    }
    if (on && conn && last_on == 0 && !edge)
        edge = 1; /* switched on while connected */
    last_on = on;
    if (!on || remembered < 0 || !edge)
        return;
    int act = 0; /* 1 set Bluetooth, 2 set DECK */
    mixch_hold();
    uintptr_t c = mixch_get(remembered);
    if (c && edge == 1 && mixch_input(c) != MIX_IN_BT) {
        mixch_select(c, MIX_IN_BT);
        seen[remembered] = mixch_input(c);
        act = 1;
    } else if (c && edge == 2 && mixch_input(c) == MIX_IN_BT) {
        mixch_select(c, MIX_IN_DECK);
        seen[remembered] = mixch_input(c);
        act = 2;
    }
    mixch_drop(); /* logging only after the channel is let go */
    if (!c)
        bt_log("input: CH%d not known, left alone\n", remembered + 1);
    else if (act)
        bt_log("input: CH%d -> %s (now %d)\n", remembered + 1, act == 1 ? "Bluetooth" : "DECK",
               seen[remembered]);
}
static void install_bt_mods(void) {
    im_load();
    int a = azm_install_hook(BT_SNK_EV_FN, (void *) bt_snk_hook, (void **) &g_o_bt_snk);
    int b = azm_install_hook(BT_A2DP_EV_FN, (void *) bt_a2dp_hook, (void **) &g_o_bt_a2dp);
    /* the channel destructors first, from the vtable (RTTI-checked at build time): a channel
     * is recorded only while its destruction is tracked */
    uintptr_t d1 = 0, d0 = 0;
    int k1 = -1, k0 = -1;
    if (img_q(MIX_CH_VT, &d1) && img_q(MIX_CH_VT + 8, &d0) && d1 && d0 && d1 != d0) {
        k1 = azm_install_hook(d1, (void *) mixch_d1_hook, (void **) &g_o_mixch_d1);
        k0 = k1 == 1 ? azm_install_hook(d0, (void *) mixch_d0_hook, (void **) &g_o_mixch_d0) : -1;
    }
    g_mixch_dtor_ok = k1 == 1 && k0 == 1;
    int c = azm_install_hook(MIX_CH_CTOR_FN, (void *) mixch_ctor_hook, (void **) &g_o_mixch_ctor);
    bt_log("bluetooth mods: hooks sink=%d a2dp=%d channel=%d channel-dtors=%d/%d\n", a, b, c, k1,
           k0);
}

/* UART speed recovery. EP147's Bluetooth stack (libanwbt*, NXP/Marvell libbt-vendor)
 * opens /dev/ttyS0 at 115200 and switches the controller to 3,000,000 baud; switching
 * Bluetooth off restores 115200, but stopping EP147 with Bluetooth on does not (an
 * on-deck update, the loader's reload, a crash). The next EP147 then talks 115200 to a
 * controller at 3M: no address, the switch wedged, no pairing. The vendor speed command
 * (0xFC09) back to 115200 fixes it. So, before EP147 opens the UART: ask at 115200 (the
 * normal case); only if silent ask at 3M, and if it answers there set 115200 and verify.
 * No controller, or no answer at either speed, is left exactly as found.
 * Kill switch: settings/azmod-btreset.off. */
static int bt_hci(int fd, speed_t sp, const unsigned char *cmd, int n, unsigned char op_lo,
                  unsigned char op_hi) {
    struct termios t;
    if (tcgetattr(fd, &t) != 0)
        return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD | CRTSCTS;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    if (tcsetattr(fd, TCSANOW, &t) != 0)
        return -1;
    tcflush(fd, TCIOFLUSH);
    if (write(fd, cmd, (size_t) n) != n)
        return -1;
    unsigned char b[64];
    int got = 0;
    for (int ms = 0; ms < 300 && got < (int) sizeof b; ms += 10) {
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 10) > 0) {
            ssize_t r = read(fd, b + got, sizeof b - (size_t) got);
            if (r > 0)
                got += (int) r;
        }
        for (int i = 0; i + 5 < got; i++) /* Command Complete for this opcode */
            if (b[i] == 0x04 && b[i + 1] == 0x0e && b[i + 4] == op_lo && b[i + 5] == op_hi)
                return 1;
    }
    return 0;
}
static void bt_uart_recover(void) {
    if (access("/home/root/settings/azmod-btreset.off", F_OK) == 0)
        return;
    int fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    static const unsigned char rst[] = {0x01, 0x03, 0x0c, 0x00}; /* HCI_Reset */
    static const unsigned char baud[] = {0x01, 0x09, 0xfc, 0x04,
                                         0x00, 0xc2, 0x01, 0x00}; /* vendor 0xFC09: 115200 */
    const char *what;
    if (bt_hci(fd, B115200, rst, 4, 0x03, 0x0c) == 1)
        what = "at 115200 (stock state): nothing to do";
    else if (bt_hci(fd, B3000000, rst, 4, 0x03, 0x0c) != 1)
        what = "no answer at 115200 or 3000000: left alone";
    else if (bt_hci(fd, B3000000, baud, 8, 0x09, 0xfc) != 1)
        what = "at 3000000, speed change NOT acknowledged";
    else {
        /* The acknowledgement comes at the old speed and the controller switches
         * after it. Asking at 115200 immediately lands mid-switch and can leave the
         * controller dead at every speed until a power-cycle, so wait, then retry. */
        int ok = 0;
        for (int k = 0; k < 4 && !ok; k++) {
            usleep(k ? 150000 : 100000);
            ok = bt_hci(fd, B115200, rst, 4, 0x03, 0x0c) == 1;
        }
        what = ok ? "was at 3000000 (EP147 stopped with Bluetooth on): reset to 115200"
                  : "speed change acknowledged, but no answer at 115200 after 4 tries";
    }
    close(fd);
    FILE *f = azlog_open("/tmp/azmod-bt.log", "a");
    if (f) {
        fprintf(f, "[%ld] bluetooth uart %s\n", (long) time(NULL), what);
        fclose(f);
    }
}
