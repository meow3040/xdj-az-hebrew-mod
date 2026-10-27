/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file key_pane.c
 * @brief Camelot Keys on the deck screen: the key of a track shown beside its waveform and in
 * the track information pane.
 *
 * Those two key displays write a key EP147 holds as a pitch class and a major/minor flag with
 * EP147's classic formatter ("Am"), unless the deck's own key display mode is one of its other
 * two notations, which they build themselves. With Extras > Camelot Keys on, the formatter's
 * answer for a valid key is the Camelot text instead, by EP147's own Camelot rule
 * (camelot_text.h), built in the result with EP147's own string constructor. An invalid key, and
 * the row off, go to the original unchanged. The browse lists are streaming_key.c's.
 *
 * Kill switch: the same settings/azmod-keycamelot.off (read at start-up; no hook then).
 *
 * Entry points: install_key_pane.
 */
/* EP147, the classic key formatter FUN_02224f60 (v2.00 0x2392400), the same code on both:
 *   juce::String f(const uint8_t *key), the result through x8 (the C++ indirect result):
 *   the prologue keeps x0 (key) in x21 and x8 (result) in x23, reads no other argument register,
 *   and returns x23 in x0. key[0] pitch class (signed; a minor key's is its relative major's),
 *   key[1] nonzero = major, key[2] a spelling preference (callers pass 100), key[3] 0 = invalid
 *   (then the text is a fixed constant). Its two callers are the deck key displays (v1.30
 *   0x1d72720 and 0x1d77148, v2.00 0x1eddad8 and 0x1ee23f8), which pass a key built on their
 *   stack from the track's information (+0xa4 pitch, +0xa5 major), for every track kind.
 *   The displays' own Camelot mode (their mode 1) is the rule camelot_text.h mirrors.
 *   juce::String::String(const char *) FUN_02333ce0 builds a string in place: the formatter
 *   itself builds its invalid-key text with it, into the result. */
#include "camelot_text.h"
#define KP_FORMAT EP_2224f60
#define KP_STRING_FROM_CSTR EP_2333ce0
#ifndef KP_OFF_PATH
#define KP_OFF_PATH "/home/root/settings/azmod-keycamelot.off"
#endif
#define KP_STRING_DTOR EP_22fc600 /* juce::String::~String */
#define KP_LOG_CALLS 8 /* the first calls are logged with their registers */

#ifdef SK_HAVE_HOOK
/* Every assembly symbol here is global and hidden: a local label's address taken from C can
 * resolve elsewhere in position-independent code. (streaming_key.c has the per-format pieces.) */
#if defined(__APPLE__)
#define KP_FN(n) ".globl _" #n "\n.private_extern _" #n "\n_" #n ":\n"
#define KP_B(n) "    b _" #n "\n"
#else
#define KP_FN(n) ".globl " #n "\n.hidden " #n "\n.type " #n ", %function\n" #n ":\n"
#define KP_B(n) "    b " #n "\n"
#endif
__attribute__((visibility("hidden"))) void *g_kp_orig;
static int g_kp_calls; /* __atomic only */

/* Calls the formatter as EP147 does: fn(key) with the result written to *out; returns out. */
__attribute__((visibility("hidden"))) void *kp_call(void *fn, void *out, const uint8_t *key);
__asm__(".text\n"
        ".p2align 2\n" KP_FN(kp_call) "    stp x29, x30, [sp, #-16]!\n"
                                      "    mov x29, sp\n"
                                      "    mov x9, x0\n"
                                      "    mov x8, x1\n"
                                      "    mov x0, x2\n"
                                      "    blr x9\n"
                                      "    ldp x29, x30, [sp], #16\n"
                                      "    ret\n" SK_END(kp_call));

static void kp_selftest(void);
static int g_kp_tested; /* __atomic only */
/* The C side of the hook: out is the caller's x8, key its x0. Returns out, as the original does. */
__attribute__((visibility("hidden"))) void *kp_format_c(void *out, const uint8_t *key) {
    if (!__atomic_exchange_n(&g_kp_tested, 1, __ATOMIC_ACQ_REL))
        kp_selftest(); /* on the deck display's own thread, before its first key */
    int n = __atomic_add_fetch(&g_kp_calls, 1, __ATOMIC_RELAXED);
    if (n <= KP_LOG_CALLS)
        sk_log("deck key %d: x0 (key) %p x8 (result) %p key %d %d %d %d\n", n, (const void *) key, out,
               key ? (signed char) key[0] : 0, key ? key[1] : 0, key ? key[2] : 0, key ? key[3] : 0);
    if (out && key && key[3] && az_value(AZF_CAMELOT_KEYS)) {
        char t[4];
        ct_camelot((signed char) key[0], key[1] != 0, t);
        ((void (*)(void *, const char *)) KP_STRING_FROM_CSTR)(out, t);
        return out;
    }
    return kp_call(g_kp_orig, out, key);
}

/* Once, from the first display call: the formatter at its own address (so through this hook)
 * with each key a caller can pass, any track kind's included (each pitch class 0..11 major and
 * minor, an invalid key, and pitch classes outside 0..11), each result logged and destroyed.
 * Only with logs on. */
static void kp_selftest(void) {
    FILE *probe = azlog_open("/tmp/azmod-keycamelot.log", "a"); /* only with logs on */
    if (!probe)
        return;
    fclose(probe);
    int row = az_value(AZF_CAMELOT_KEYS) != 0, ok = 0, n = 0;
    for (int i = 0; i < 24 + 4; i++) {
        uint8_t key[4] = {(uint8_t) (i / 2), (uint8_t) (i & 1), 100, 1};
        if (i >= 24) /* invalid; pitch -1, 12 and 127 */
            key[0] = (uint8_t) (i == 24 ? 3 : i == 25 ? -1
                                          : i == 26   ? 12
                                                      : 127),
            key[3] = i != 24;
        uintptr_t str = 0;
        char got[16] = "", want[4] = "";
        kp_call((void *) KP_FORMAT, &str, key);
        if (str)
            own_str(str, got, sizeof got);
        ((void (*)(uintptr_t *)) KP_STRING_DTOR)(&str);
        if (key[3])
            ct_camelot((signed char) key[0], key[1], want);
        n++;
        ok += row && key[3] ? !strcmp(got, want) : strcmp(got, want) != 0 || !key[3];
        sk_log("deck key self-test: pitch %d %s %s -> \"%s\"\n", (signed char) key[0], key[1] ? "major" : "minor",
               key[3] ? "valid" : "invalid", got);
    }
    sk_log("deck key self-test, row %s: %d/%d as expected\n", row ? "on" : "off", ok, n);
}

/* EP147's entry: x0 = key, x8 = result. kp_format_c(out, key) returns out in x0. */
__attribute__((visibility("hidden"))) void kp_format_hook(void);
__asm__(".text\n"
        ".p2align 2\n" KP_FN(kp_format_hook) "    mov x1, x0\n"
                                             "    mov x0, x8\n" KP_B(kp_format_c) SK_END(kp_format_hook));

static void install_key_pane(void) {
    if (access(KP_OFF_PATH, F_OK) == 0)
        return;
    int rc = azm_install_hook(KP_FORMAT, (void *) kp_format_hook, &g_kp_orig);
    sk_log("key notation (deck screen): hook rc=%d\n", rc);
}
#else  /* not AArch64: a host building the tests, with no hook to install */
static void install_key_pane(void) {}
#endif
