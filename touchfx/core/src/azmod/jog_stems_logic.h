/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file jog_stems_logic.h
 * @brief Stem state on the jog display: which deck shows it, the page the jog is told to show,
 *        and the pad table that carries the state
 *
 * The jog display is drawn by the deck's own controller from records EP147 sends it. Two of
 * them carry this feature (jog_display.c hooks both, per deck):
 *   - the pad-mode byte of gui::sub_display::PlayerInfo: +0xd4 = the user's "Pad Info Display"
 *     setting is On, +0xd5 = the pad mode shown. While a deck's pads are its stem toggles and
 *     the setting is On, +0xd5 is set to 0: the jog shows its HOT CUE pad page, which draws
 *     one tile per pad from the pad table below, beside the waveform preview.
 *   - the pad table (record 0x40, deck_ucom::TrackInfoQueue slot +0x58): eight entries, one per
 *     pad, each set or empty, with a color from the controller's 64 hot cue colors. The
 *     controller applies a new table whole, as one small record, so a stem toggle changes only
 *     that pad's tile.
 * While the deck shows stems the table is built from the stem state: pads 1-4 = VOCAL, INST,
 * BASS, DRUMS, set in the stem's color while the stem is on and empty while it is off; pads 5-8
 * = solo, set in white on the soloed stem. When it stops, EP147's own table is sent again.
 *
 * One published word per deck carries the decision: JOG_ON | the stem mask, or 0, made from
 * that deck's state only, so an unknown deck is never shown another deck's stems.
 *
 * The waveform preview strip (below) shows the same state as four blocks.
 *
 * Nothing here touches EP147, allocates or calls the system; tools/unit-tests/jogstems_test.c
 * builds this header natively.
 */
#ifndef AZMOD_JOG_STEMS_LOGIC_H
#define AZMOD_JOG_STEMS_LOGIC_H

#include <stdint.h>
#include <string.h>

#define JOG_ON 0x10u /* the deck shows stems on its jog; low four bits = the stem mask */
#define JOG_PADS 8

/* The word a deck publishes. engaged: Stems on and the deck's track has stems. page: the
 * deck's pads are its stem toggles. mask: bit i = stem i on (0 vocal, 1 inst, 2 bass, 3 drums). */
static inline unsigned jog_word(int engaged, int page, unsigned mask) {
    return (engaged && page) ? (JOG_ON | (mask & 0xfu)) : 0u;
}

/* The pad-mode record of one deck, right after EP147 filled it. With the user's Pad Info
 * Display On (d4 == 1) and stems to show, the jog is asked for its HOT CUE pad page (d5 = 0),
 * whose tiles come from the pad table. Returns the word the table hooks act on: the deck's word
 * when the record was changed, else 0. With the setting Off the jog shows the artwork the user
 * picked, and nothing is changed. */
static inline unsigned jog_padinfo_override(const unsigned char *d4, unsigned char *d5,
                                            unsigned word) {
    if (!(word & JOG_ON) || *d4 != 1)
        return 0u;
    *d5 = 0;
    return word;
}

/* One pad of the table as EP147 hands it to the queue: an element of a std::vector, 0x18
 * bytes, of which the encoder reads pos, is_loop, set and color. On the wire each pad is
 * {color 1..64, 0 empty / 1 hot cue / 2 loop, position}. */
struct jog_cue {
    int64_t pos;     /* +0x00: INT64_MAX = no position */
    int64_t pos_end; /* +0x08 */
    uint32_t argb;   /* +0x10: juce::Colour */
    uint8_t is_loop; /* +0x14 */
    uint8_t set;     /* +0x15: 0 = an empty pad */
    uint8_t color;   /* +0x16: the controller's hot cue color, 1..64 */
    uint8_t pad_;
};
struct jog_cue_vec { /* std::vector<jog_cue> */
    struct jog_cue *begin, *end, *cap;
};
/* The controller's hot cue colors nearest the stem tile strokes (N = its g_img_pad_hotcue_N:
 * 21 green, 42 red, 50 magenta, 4 blue), and 64 = white for a solo. */
static const unsigned char JOG_CUE_COLOR[4] = {21, 42, 50, 4};
static const uint32_t JOG_CUE_ARGB[4] = {0xff00ff00u, 0xffff0000u, 0xffff00b8u, 0xff0054ffu};
#define JOG_CUE_WHITE 64

/* The table for a word (JOG_ON | mask): pads 1-4 the stems, pads 5-8 the solo. A stem is soloed
 * when it is the only one on, the same rule as the pad LEDs. */
static inline void jog_cues_build(unsigned word, struct jog_cue out[JOG_PADS]) {
    memset(out, 0, sizeof(struct jog_cue) * JOG_PADS);
    unsigned mask = word & 0xfu;
    int solo = (mask && !(mask & (mask - 1))) ? __builtin_ctz(mask) : -1;
    for (int p = 0; p < JOG_PADS; p++) {
        int s = p & 3;
        out[p].pos = out[p].pos_end = INT64_MAX;
        if (p < 4 ? (int) ((mask >> s) & 1u) : s == solo) {
            out[p].set = 1;
            out[p].color = p < 4 ? JOG_CUE_COLOR[s] : JOG_CUE_WHITE;
            out[p].argb = p < 4 ? JOG_CUE_ARGB[s] : 0xffffffffu;
        }
    }
}

/* After EP147's hot cue update for a deck: what the jog's table should become. sent: what was
 * last sent (0 = EP147's own, else the word ours was built from). Returns JC_OURS (send ours),
 * JC_EP147 (send EP147's own again) or JC_NONE, and records it. */
enum {
    JC_NONE = 0,
    JC_OURS,
    JC_EP147
};
static inline int jog_cue_step(unsigned *sent, unsigned active) {
    if (active & JOG_ON) {
        if (*sent == active)
            return JC_NONE;
        *sent = active;
        return JC_OURS;
    }
    if (!*sent)
        return JC_NONE;
    *sent = 0;
    return JC_EP147;
}

/* ---- Jog messages in the deck frames EP147 sends (timing diagnostics) --------------------
 * One message slot per deck frame: present 0x4c (1), sequence u16 0x4e (a repeated frame
 * repeats it), display 0x50 (0x10 left, 0x20 right), id 0x51, piece u16 0x52 (from 1),
 * pieces u16 0x54, 58 data bytes at 0x56. Artwork = id 0x33; its first piece starts with the
 * JPEG's length (3 bytes, little endian). */
#define JOG_MSG_ART 0x33
#define JOG_MSG_PREV 0x2c /* the waveform preview strip */
#define JOG_MSG_END (0x56 + 58)
struct jog_msg {
    unsigned display, id, seq, index, total, len;
};
static inline int jog_msg_parse(const unsigned char *f, size_t n, struct jog_msg *m) {
    if (n < JOG_MSG_END || f[0x4c] != 1 || (f[0x50] != 0x10 && f[0x50] != 0x20))
        return 0;
    m->display = f[0x50];
    m->id = f[0x51];
    m->seq = (unsigned) f[0x4e] | (unsigned) f[0x4f] << 8;
    m->index = (unsigned) f[0x52] | (unsigned) f[0x53] << 8;
    m->total = (unsigned) f[0x54] | (unsigned) f[0x55] << 8;
    m->len = m->index == 1
                 ? ((unsigned) f[0x56] | (unsigned) f[0x57] << 8 | (unsigned) f[0x58] << 16)
                 : 0;
    return 1;
}
/* One display's artwork transfer as it goes out. Feed every parsed message with the time;
 * returns 1 when an artwork's last piece went out, with first, last, pieces, gaps and len
 * describing it. A repeated frame (same sequence) counts once. */
struct jog_xfer {
    int have_seq;
    unsigned seq, pieces, gaps, len, total, next, id;
    int64_t first, last;
};
static inline int jog_xfer_feed(struct jog_xfer *x, const struct jog_msg *m, int64_t now) {
    if (x->have_seq && m->seq == x->seq)
        return 0;
    x->have_seq = 1;
    x->seq = m->seq;
    if ((m->id != JOG_MSG_ART && m->id != JOG_MSG_PREV) || !m->index)
        return 0;
    if (m->index == 1) {
        x->id = m->id;
        x->first = now;
        x->pieces = 0;
        x->gaps = 0;
        x->len = m->len;
        x->total = m->total;
        x->next = 1;
    } else if (!x->next || m->id != x->id) {
        return 0; /* joined mid-transfer, or another record: wait for a first piece */
    }
    if (m->index != x->next)
        x->gaps++;
    x->pieces++;
    x->next = m->index + 1;
    if (m->index != m->total)
        return 0;
    x->last = now;
    x->next = 0;
    return 1;
}

/* ---- The waveform preview strip -------------------------------------------------------
 * EP147 sends a deck's jog its waveform preview through the deck's TrackInfoQueue (slot +0x20,
 * record 0x2c) as a vector of 5-byte columns {R, G, B, height, highlight}, heights 0..128 for
 * the strip's 40 rows; a 1200-column vector goes out as every second column. The controller
 * draws each column as one bar from the bottom in its color, decodes a new strip off screen and
 * swaps it in whole, so a new strip never blanks the jog.
 * While a deck shows stems its strip is four blocks in pad order (VOCAL, INST, BASS, DRUMS):
 * a stem that is on is a tall block in the stem tile's stroke color, one that is off a short
 * block at 30 % of it. The column count and the flags stay EP147's. */
struct jog_prev_col {
    uint8_t r, g, b, h, hl;
};
#define JOG_PREV_MAX 1200 /* EP147's own limit for the vector */
#define JOG_PREV_ON_H 128
#define JOG_PREV_OFF_H 40
/* Block layout on a 600-column strip: 15 + 4 x 132 + 3 x 14 + 15. */
#define JOG_PREV_M 15
#define JOG_PREV_W 132
#define JOG_PREV_G 14
static const unsigned char JOG_PREV_RGB[4][3] = {
    {31, 206, 31},  /* vocal */
    {235, 44, 44},  /* inst */
    {235, 47, 188}, /* bass */
    {58, 88, 235},  /* drums */
};
/* The block (0..3) column x of an n-column strip falls in, or -1 for a margin or a gap. */
static inline int jog_prev_block(int x, int n) {
    if (n <= 0 || x < 0 || x >= n)
        return -1;
    int c = (int) ((long) x * 600 / n) - JOG_PREV_M;
    if (c < 0)
        return -1;
    int b = c / (JOG_PREV_W + JOG_PREV_G);
    if (b > 3 || c % (JOG_PREV_W + JOG_PREV_G) >= JOG_PREV_W)
        return -1;
    return b;
}
/* How many columns our strip has, for EP147's count n: the controller draws 600 entries, and
 * EP147 sends a 1200-column vector as every second column but still sends 7 bytes for each of
 * the 1200 (half of the record zeros, 145 pieces); 600 columns give the same 600 entries in half
 * the record (73 pieces). A shorter strip from EP147 keeps its own count. */
#define JOG_PREV_SENT 600
static inline int jog_prev_ncols(int n) {
    return n > JOG_PREV_SENT ? JOG_PREV_SENT : n;
}
/* Fills n (1..JOG_PREV_MAX) columns for the word (JOG_ON | mask). Returns n, or 0 for a bad n. */
static inline int jog_prev_build(unsigned word, struct jog_prev_col *out, int n) {
    if (n <= 0 || n > JOG_PREV_MAX)
        return 0;
    for (int x = 0; x < n; x++) {
        int b = jog_prev_block(x, n);
        struct jog_prev_col c = {0, 0, 0, 0, 0};
        if (b >= 0) {
            int on = (int) ((word >> b) & 1u);
            for (int k = 0; k < 3; k++) {
                unsigned v = JOG_PREV_RGB[b][k];
                (&c.r)[k] = (uint8_t) (on ? v : v * 77u / 255u);
            }
            c.h = (uint8_t) (on ? JOG_PREV_ON_H : JOG_PREV_OFF_H);
        }
        out[x] = c;
    }
    return n;
}

#endif
