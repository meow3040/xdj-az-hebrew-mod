/* SPDX-License-Identifier: GPL-3.0-only */
/* browse_fields.h: the fields a Browse track list column can be set to show in Column Edit
 * (browse_fields.c), as pure functions: no EP147, no I/O, so the unit tests and the fuzz harness
 * run them as they are.
 *
 * The fields are the ones EP147's own sub column draws. EP147's sub column list
 * (gui::disp_data::SubCategoryDispList) names each one by a fixed token, the library's menu
 * item name without its markers ("GENRE", "DJ PLAY COUNT"); b2_field_of_token maps a token to a
 * field. Each field has one query on the stick's exportLibrary.db that returns one row for
 * (content_id ?1, title ?2): a number in column 0, a text in column 1, or both (Color: its id
 * and its name). b2_format turns that row into the text EP147 shows for the same field and the
 * sub item kind its painter takes: 0x17 + the color id draws the color's dot before the text,
 * 0 draws the text alone. */
#ifndef BROWSE_FIELDS_H
#define BROWSE_FIELDS_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    B2_NONE = 0, /* EP147's DEFAULT: no field, the column is not shown */
    B2_TRACK,
    B2_ARTIST,
    B2_ALBUM,
    B2_RATING,
    B2_GENRE,
    B2_COMMENTS,
    B2_TIME,
    B2_REMIXER,
    B2_LABEL,
    B2_ORIGART,
    B2_BITRATE,
    B2_COLOR,
    B2_PLAYS,
    B2_ADDED,
    B2_N
};
enum {
    B2F_TEXT,
    B2F_RATING,
    B2F_TIME,
    B2F_BITRATE,
    B2F_COLOR,
    B2F_COUNT
};
#define B2_TEXT_MAX 256      /* a value longer than this is cut, at a character boundary */
#define B2_KIND_COLOR0 0x17  /* EP147's painter: 0x18..0x1f = the dot of color 1..8 */
#define B2_COLOR_MAX 8

static const struct b2_field {
    const char *token; /* EP147's sub column list */
    const char *label; /* the column's header */
    int fmt;
    const char *sql;
} B2_FIELDS[B2_N] = {
    {"DEFAULT", "", B2F_TEXT, 0},
    {"TRACK", "Track", B2F_TEXT, "SELECT NULL, c.title FROM content c WHERE c.content_id = ?1 AND c.title = ?2"},
    {"ARTIST", "Artist", B2F_TEXT,
     "SELECT NULL, a.name FROM content c LEFT JOIN artist a ON a.artist_id = c.artist_id_artist "
     "WHERE c.content_id = ?1 AND c.title = ?2"},
    {"ALBUM", "Album", B2F_TEXT,
     "SELECT NULL, a.name FROM content c LEFT JOIN album a ON a.album_id = c.album_id "
     "WHERE c.content_id = ?1 AND c.title = ?2"},
    {"RATING", "Rating", B2F_RATING, "SELECT c.rating, NULL FROM content c WHERE c.content_id = ?1 AND c.title = ?2"},
    {"GENRE", "Genre", B2F_TEXT,
     "SELECT NULL, g.name FROM content c LEFT JOIN genre g ON g.genre_id = c.genre_id "
     "WHERE c.content_id = ?1 AND c.title = ?2"},
    {"COMMENTS", "Comments", B2F_TEXT,
     "SELECT NULL, c.djComment FROM content c WHERE c.content_id = ?1 AND c.title = ?2"},
    {"TIME", "Time", B2F_TIME, "SELECT c.length, NULL FROM content c WHERE c.content_id = ?1 AND c.title = ?2"},
    {"REMIXER", "Remixer", B2F_TEXT,
     "SELECT NULL, a.name FROM content c LEFT JOIN artist a ON a.artist_id = c.artist_id_remixer "
     "WHERE c.content_id = ?1 AND c.title = ?2"},
    {"LABEL", "Label", B2F_TEXT,
     "SELECT NULL, l.name FROM content c LEFT JOIN label l ON l.label_id = c.label_id "
     "WHERE c.content_id = ?1 AND c.title = ?2"},
    {"ORIGINAL ARTIST", "Original Artist", B2F_TEXT,
     "SELECT NULL, a.name FROM content c LEFT JOIN artist a ON a.artist_id = c.artist_id_originalArtist "
     "WHERE c.content_id = ?1 AND c.title = ?2"},
    {"BITRATE", "Bitrate", B2F_BITRATE, "SELECT c.bitrate, NULL FROM content c WHERE c.content_id = ?1 AND c.title = ?2"},
    {"COLOR", "Color", B2F_COLOR,
     "SELECT c.color_id, k.name FROM content c LEFT JOIN color k ON k.color_id = c.color_id "
     "WHERE c.content_id = ?1 AND c.title = ?2"},
    {"DJ PLAY COUNT", "DJ Play Count", B2F_COUNT,
     "SELECT c.djPlayCount, NULL FROM content c WHERE c.content_id = ?1 AND c.title = ?2"},
    {"DATE ADDED", "Date Added", B2F_TEXT,
     "SELECT NULL, c.dateAdded FROM content c WHERE c.content_id = ?1 AND c.title = ?2"},
};

/* the field a sub column list token names, or B2_NONE (DEFAULT, or one this table lacks) */
static int b2_field_of_token(const char *tok) {
    if (!tok)
        return B2_NONE;
    for (int f = 1; f < B2_N; f++)
        if (!strcmp(tok, B2_FIELDS[f].token))
            return f;
    return B2_NONE;
}

/* The length of the valid UTF-8 sequence at s (at most n bytes), or 0 for an invalid one:
 * shortest form only, no surrogates, nothing past U+10FFFF. */
static size_t b2_utf8_len(const unsigned char *s, size_t n) {
    if (!n)
        return 0;
    unsigned c = s[0];
    size_t len;
    unsigned min, cp;
    if (c < 0x80)
        return 1;
    if ((c & 0xe0) == 0xc0)
        len = 2, min = 0x80, cp = c & 0x1f;
    else if ((c & 0xf0) == 0xe0)
        len = 3, min = 0x800, cp = c & 0x0f;
    else if ((c & 0xf8) == 0xf0)
        len = 4, min = 0x10000, cp = c & 0x07;
    else
        return 0;
    if (len > n)
        return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0;
    return len;
}
/* A library text as EP147 may show it: valid UTF-8 kept, an invalid byte or a control
 * character shown as '?', cut at a character boundary to fit cap (and B2_TEXT_MAX). */
static void b2_clean_text(const char *in, char *out, size_t cap) {
    size_t o = 0, lim = cap < B2_TEXT_MAX ? cap : B2_TEXT_MAX;
    if (!cap)
        return;
    if (in) {
        const unsigned char *s = (const unsigned char *) in;
        size_t n = strlen(in);
        for (size_t i = 0; i < n;) {
            size_t k = b2_utf8_len(s + i, n - i);
            if (!k || (k == 1 && (s[i] < 0x20 || s[i] == 0x7f))) {
                if (o + 1 >= lim)
                    break;
                out[o++] = '?';
                i += k ? k : 1;
                continue;
            }
            if (o + k >= lim)
                break;
            memcpy(out + o, s + i, k);
            o += k;
            i += k;
        }
    }
    out[o] = 0;
}

/* EP147's rating text: 0..5 filled stars of five, separated by spaces */
static const char *const B2_STARS[6] = {
    "\xe2\x98\x86 \xe2\x98\x86 \xe2\x98\x86 \xe2\x98\x86 \xe2\x98\x86",
    "\xe2\x98\x85 \xe2\x98\x86 \xe2\x98\x86 \xe2\x98\x86 \xe2\x98\x86",
    "\xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x86 \xe2\x98\x86 \xe2\x98\x86",
    "\xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x86 \xe2\x98\x86",
    "\xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x86",
    "\xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x85 \xe2\x98\x85",
};

/* One query row (has_num: column 0 was not NULL; num its value; text column 1 or NULL) as
 * the text EP147 shows for the field and the sub item kind. Returns 0 for a field with no
 * column (B2_NONE or out of range): nothing to show. */
static int b2_format(int field, int has_num, long long num, const char *text, char *out, size_t cap,
                     int *kind) {
    *kind = 0;
    if (cap)
        out[0] = 0;
    if (field <= B2_NONE || field >= B2_N || cap < 16)
        return 0;
    switch (B2_FIELDS[field].fmt) {
    case B2F_RATING: {
        long long r = has_num ? num : 0;
        snprintf(out, cap, "%s", B2_STARS[r < 0 ? 0 : r > 5 ? 5
                                                            : r]);
        break;
    }
    case B2F_TIME:
        if (has_num && num >= 0 && num < 100LL * 3600)
            snprintf(out, cap, "%02u:%02u", (unsigned) (num / 60), (unsigned) (num % 60));
        break;
    case B2F_BITRATE:
        if (has_num && num > 0 && num < 100000)
            snprintf(out, cap, "%ukbps", (unsigned) num);
        else
            snprintf(out, cap, "VBR");
        break;
    case B2F_COUNT:
        snprintf(out, cap, "%lld", has_num && num > 0 && num < 1000000000LL ? num : 0LL);
        break;
    case B2F_COLOR:
        if (has_num && num >= 1 && num <= B2_COLOR_MAX) {
            *kind = B2_KIND_COLOR0 + (int) num;
            b2_clean_text(text, out, cap);
        }
        break;
    default:
        b2_clean_text(text, out, cap);
        break;
    }
    return 1;
}

/* The saved fields (settings/azmod-colfields): one "<column id> <TOKEN>" line per column that
 * shows a field, at most B2_FILE_MAX bytes. ids 1..B2_MAXID; a token is one of B2_FIELDS' other
 * than DEFAULT and TRACK. Anything else, or an id given twice, rejects the whole file: a damaged
 * file never sets a column. Returns the number of columns set, or -1 with every one B2_NONE. */
#define B2_MAXID 10
#define B2_FILE_MAX 512
static int b2_fields_parse(const char *s, size_t n, unsigned char field[B2_MAXID + 1]) {
    int count = 0;
    size_t i = 0;
    for (int k = 0; k <= B2_MAXID; k++)
        field[k] = B2_NONE;
    if (n > B2_FILE_MAX)
        goto bad;
    while (i < n) {
        if (s[i] == '\n' || s[i] == '\r') {
            i++;
            continue;
        }
        int id = 0, dig = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && dig < 3) {
            id = id * 10 + (s[i++] - '0');
            dig++;
        }
        if (!dig || i >= n || s[i] != ' ')
            goto bad;
        i++;
        char tok[32];
        size_t t = 0;
        while (i < n && s[i] != '\n' && s[i] != '\r') {
            if (t + 1 >= sizeof tok)
                goto bad;
            tok[t++] = s[i++];
        }
        tok[t] = 0;
        int f = b2_field_of_token(tok);
        if (id < 1 || id > B2_MAXID || f == B2_NONE || f == B2_TRACK || field[id] != B2_NONE)
            goto bad;
        field[id] = (unsigned char) f;
        count++;
    }
    return count;
bad:
    for (int k = 0; k <= B2_MAXID; k++)
        field[k] = B2_NONE;
    return -1;
}
/* The file for field[]: its length, or -1 when cap is too small. */
static int b2_fields_format(const unsigned char field[B2_MAXID + 1], char *buf, size_t cap) {
    size_t len = 0;
    if (!cap)
        return -1;
    buf[0] = 0;
    for (int id = 1; id <= B2_MAXID; id++) {
        int f = field[id];
        if (f <= B2_TRACK || f >= B2_N)
            continue;
        int k = snprintf(buf + len, cap - len, "%d %s\n", id, B2_FIELDS[f].token);
        if (k < 0 || (size_t) k >= cap - len)
            return -1;
        len += (size_t) k;
    }
    return (int) len;
}
#endif
