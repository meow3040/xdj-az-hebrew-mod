/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file streaming_key.c
 * @brief Keys that EP147 writes out from a key id read in Camelot notation.
 *
 * EP147 turns a key id into text with one function, which takes the notation as a flag:
 * Camelot ("8A") or classic ("Am"). The streaming services' track lists and track
 * information (Spotify, and the other services whose replies are built the same way) ask it
 * for the notation their settings object gives, which is classic, so their keys read "Am"
 * beside a rekordbox library whose keys read "8A". The same function names the keys in a
 * library's KEY lists, with the notation the request carries. The track rows of a rekordbox
 * library show the library's own key text and do not pass through it.
 *
 * With Extras > Camelot Keys on, the hook asks that function for Camelot for every valid key
 * id; with it off, the caller's flag passes through as stock. The row is read at every call
 * (one atomic load), so a change applies to the next list EP147 builds. The text is EP147's
 * own table entry, so nothing here formats a key.
 *
 * Off by default. Kill switch: settings/azmod-keycamelot.off (read at start-up; no hook then).
 *
 * Entry points: install_streaming_key.
 */
/* EP147, the key text function FUN_00d34790(const uint8_t *key, bool camelot): returns a
 * std::u16string through x8 (the C++ indirect result). A key id 1..24 (1A, 1B, 2A ... 12B)
 * gives that entry of a 24-entry table, its Camelot text when camelot == 1, else its classic
 * text; id 0 gives "". The callers pass the flag in w1 as a byte. Prologue stp/mov/stp/adrp:
 * hookable. */
#define SK_KEYTEXT EP_d34790
#define SK_IDS 24
#ifndef SK_OFF_PATH
#define SK_OFF_PATH "/home/root/settings/azmod-keycamelot.off"
#endif

static void sk_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-keycamelot.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* The notation flag the original gets: with the row on, Camelot for a key id it has an
 * entry for; the caller's own flag otherwise (row off, id 0, anything out of range). */
__attribute__((visibility("hidden"))) unsigned sk_flag(const uint8_t *key, unsigned flag) {
    if (az_value(AZF_CAMELOT_KEYS) && key && *key >= 1 && *key <= SK_IDS)
        return 1;
    return flag & 0xff;
}

/* The wrappers below are AArch64 assembly (the result string comes back through x8, which a C
 * wrapper could not hand on untouched). Symbol and relocation syntax for ELF (the deck) and for
 * Mach-O (a Mac building the tests). */
#if defined(__aarch64__)
#if defined(__APPLE__)
#define SK_FN(n) ".private_extern _" #n "\n_" #n ":\n"
#define SK_END(n) ""
#define SK_BL(n) "    bl _" #n "\n"
#define SK_LOAD(r, n) "    adrp " r ", _" #n "@PAGE\n    ldr " r ", [" r ", _" #n "@PAGEOFF]\n"
#else
#define SK_FN(n) ".hidden " #n "\n.type " #n ", %function\n" #n ":\n"
#define SK_END(n) ".size " #n ", .-" #n "\n"
#define SK_BL(n) "    bl " #n "\n"
#define SK_LOAD(r, n) "    adrp " r ", " #n "\n    ldr " r ", [" r ", :lo12:" #n "]\n"
#endif
#define SK_HAVE_HOOK 1
#endif

/* Saves x0..x8, asks sk_flag for the flag, then tail-calls the original with x1 swapped and
 * x0 and x8 (the returned string) as the caller set them, so the original returns straight
 * to EP147's caller. */
__attribute__((visibility("hidden"))) void *g_sk_orig;
#ifdef SK_HAVE_HOOK
__attribute__((visibility("hidden"))) void sk_keytext_hook(void);
__asm__(".text\n"
        ".p2align 2\n" SK_FN(sk_keytext_hook) "    stp x29, x30, [sp, #-96]!\n"
                                              "    mov x29, sp\n"
                                              "    stp x0, x1, [sp, #16]\n"
                                              "    stp x2, x3, [sp, #32]\n"
                                              "    stp x4, x5, [sp, #48]\n"
                                              "    stp x6, x7, [sp, #64]\n"
                                              "    str x8, [sp, #80]\n" SK_BL(sk_flag) "    mov x9, x0\n"
                                                                                       "    ldp x0, x1, [sp, #16]\n"
                                                                                       "    ldp x2, x3, [sp, #32]\n"
                                                                                       "    ldp x4, x5, [sp, #48]\n"
                                                                                       "    ldp x6, x7, [sp, #64]\n"
                                                                                       "    ldr x8, [sp, #80]\n"
                                                                                       "    mov x1, x9\n"
                                                                                       "    ldp x29, x30, [sp], #96\n" SK_LOAD("x10", g_sk_orig) "    br x10\n" SK_END(sk_keytext_hook));

/* Calls a key text function as EP147 does: fn(key, flag) with the string written to *out. */
__attribute__((visibility("hidden"))) void sk_call(void *fn, void *out, const uint8_t *key,
                                                   unsigned flag);
__asm__(".text\n"
        ".p2align 2\n" SK_FN(sk_call) "    stp x29, x30, [sp, #-16]!\n"
                                      "    mov x29, sp\n"
                                      "    mov x9, x0\n"
                                      "    mov x8, x1\n"
                                      "    mov x0, x2\n"
                                      "    and w1, w3, #0xff\n"
                                      "    blr x9\n"
                                      "    ldp x29, x30, [sp], #16\n"
                                      "    ret\n" SK_END(sk_call));

/* std::u16string as EP147's libstdc++ lays it out: data, length, then the inline buffer */
struct sk_u16s {
    const uint16_t *p;
    size_t n;
    uint16_t buf[8];
};
/* the text as ASCII into out (at most cap - 1 characters), "?" for anything else */
static void sk_ascii(const struct sk_u16s *s, char *out, size_t cap) {
    size_t i = 0;
    if (s->p == s->buf && s->n < sizeof s->buf / sizeof s->buf[0])
        for (; i < s->n && i + 1 < cap; i++)
            out[i] = s->p[i] < 0x80 ? (char) s->p[i] : '?';
    else if (cap > 1)
        out[i++] = '?'; /* not inline: left alone (the table's texts are all short) */
    out[i] = 0;
}
/* With logs on, writes what every key id gives through EP147's entry (hooked) and through
 * the original with each flag: with the row on, the hooked text must equal the original's
 * Camelot text; with it off, the classic text. */
static void sk_selftest(void) {
    FILE *probe = azlog_open("/tmp/azmod-keycamelot.log", "a");
    if (!probe)
        return;
    fclose(probe);
    int row = az_value(AZF_CAMELOT_KEYS) != 0, same = 0;
    for (unsigned id = 0; id <= SK_IDS; id++) {
        uint8_t key = (uint8_t) id;
        struct sk_u16s h, c, a;
        char th[8], tc[8], ta[8];
        sk_call((void *) SK_KEYTEXT, &h, &key, 0);
        sk_call(g_sk_orig, &c, &key, 0);
        sk_call(g_sk_orig, &a, &key, 1);
        sk_ascii(&h, th, sizeof th);
        sk_ascii(&c, tc, sizeof tc);
        sk_ascii(&a, ta, sizeof ta);
        same += strcmp(th, row ? ta : tc) == 0;
        sk_log("key %2u: shown \"%s\"  classic \"%s\"  camelot \"%s\"\n", id, th, tc, ta);
    }
    sk_log("self-test, row %s: %d/%d ids show their %s text\n", row ? "on" : "off", same,
           SK_IDS + 1, row ? "Camelot" : "classic");
}

/* message thread: the self-test from the first timer tick after the hook is in, and again
 * whenever the row changes */
static int g_sk_tested_row = -1; /* the row value last tested, -1 none (__atomic only) */
static void sk_tick(void) {
    if (!g_sk_orig)
        return;
    int row = az_value(AZF_CAMELOT_KEYS) != 0;
    if (__atomic_exchange_n(&g_sk_tested_row, row, __ATOMIC_ACQ_REL) == row)
        return;
    sk_selftest();
}

static void install_streaming_key(void) {
    if (access(SK_OFF_PATH, F_OK) == 0) {
        sk_log("key notation: kill switch, no hook\n");
        return;
    }
    int rc = azm_install_hook(SK_KEYTEXT, (void *) sk_keytext_hook, &g_sk_orig);
    sk_log("key notation: hook rc=%d\n", rc);
}
#else  /* not AArch64: a host building the tests, with no hook to install */
static void sk_tick(void) {}
static void install_streaming_key(void) {}
#endif
