/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file input_memory.h
 * @brief Channel Input Select Memory at start: the saved inputs, and when to apply one.
 *
 * With Utility > Channel Input Select Memory on, EP147 keeps each mixer channel's input
 * (DECK, LINE, PHONO, Bluetooth) in SETTINGS.DAT and applies it to its mixer channels while
 * it starts. That apply depends on the order of EP147's start-up work: when the start runs a
 * little differently, the channels keep the DECK they are built with, and EP147 then saves
 * DECK over the DJ's choice. azmod reads the saved values before EP147 starts (the file is
 * still the DJ's then) and, once the channels exist, sets any channel still at DECK to its
 * saved input with EP147's own setter, the one Utility > Mixer uses.
 *
 * SETTINGS.DAT entries: the name, a NUL, then 0x01, a payload length, and the payload, whose
 * first byte is its type. An integer is type 0x01 with length 5: the type byte and a 32-bit
 * little-endian value. Other types (strings) carry other lengths and are never read here.
 *
 * Pure functions over untrusted bytes: every read is bounds-checked, and anything malformed
 * leaves the value unknown (fuzzed by tools/fuzz inputmem).
 */
#ifndef AZMOD_INPUT_MEMORY_H
#define AZMOD_INPUT_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IM_CHANNELS 4
#define IM_DECK 0
#define IM_MAX_INPUT 3     /* DECK, LINE, PHONO, Bluetooth */
#define IM_SETTLE_MS 2000  /* EP147's own apply gets this long first */
#define IM_WINDOW_MS 20000 /* after this, the channel's input is the DJ's to change */

struct im_saved {
    int memory;              /* 1 on, 0 off, -1 unknown */
    int input[IM_CHANNELS];  /* 0..IM_MAX_INPUT, or -1 unknown */
};

/* The integer of entry `name`, or -1: the name must start the file or follow a 0 byte (every
 * entry ends in one: an integer's high byte, a string's terminator), end in a NUL, and carry
 * an integer payload. A name found twice is ambiguous and also gives -1. */
static inline long im_int(const uint8_t *b, size_t n, const char *name) {
    size_t L = strlen(name);
    long found = -1;
    int hits = 0;
    if (!b || L == 0 || n < L + 8)
        return -1;
    for (size_t i = 0; i + L + 8 <= n; i++) {
        if (memcmp(b + i, name, L) != 0 || b[i + L] != 0)
            continue;
        if (i > 0 && b[i - 1] != 0)
            continue;
        const uint8_t *e = b + i + L + 1;
        if (e[0] != 0x01 || e[1] != 5 || e[2] != 0x01)
            return -1;
        uint32_t v = (uint32_t) e[3] | (uint32_t) e[4] << 8 | (uint32_t) e[5] << 16 |
                     (uint32_t) e[6] << 24;
        if (++hits > 1)
            return -1;
        found = v > 0x7fffffffu ? -1 : (long) v;
    }
    return found;
}

/* The saved Memory switch and the four inputs, from the file's bytes. */
static inline void im_parse(const uint8_t *b, size_t n, struct im_saved *s) {
    static const char *const names[IM_CHANNELS] = {"InputSelect0", "InputSelect1",
                                                   "InputSelect2", "InputSelect3"};
    long m = im_int(b, n, "InputSelectMemory");
    s->memory = m == 0 || m == 1 ? (int) m : -1;
    for (int i = 0; i < IM_CHANNELS; i++) {
        long v = im_int(b, n, names[i]);
        s->input[i] = v >= 0 && v <= IM_MAX_INPUT ? (int) v : -1;
    }
}

enum {
    IM_WAIT,
    IM_SELECT,
    IM_DONE
};

/* What to do with one channel, `ms` after it was first seen with input `cur`:
 * IM_SELECT = set it to the saved input now; IM_DONE = never touch it again; IM_WAIT. A
 * channel that already shows its saved input, or anything other than the DECK it is built
 * with (EP147 applied it, or the DJ chose), is left alone for good. */
static inline int im_decide(const struct im_saved *s, int ch, int cur, long ms) {
    if (!s || ch < 0 || ch >= IM_CHANNELS || s->memory != 1)
        return IM_DONE;
    int want = s->input[ch];
    if (want <= IM_DECK || cur == want || cur != IM_DECK || ms > IM_WINDOW_MS)
        return IM_DONE;
    return ms >= IM_SETTLE_MS ? IM_SELECT : IM_WAIT;
}
#endif
