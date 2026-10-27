/* SPDX-License-Identifier: GPL-3.0-only */
/* azmod.conf reader / writer and the one-time migration from azmod.cfg. Format and
 * threading rules: azconf.h. Start-up and settings changes only, never a hot path. */
#include "azconf.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define AZCONF_LINE_MAX 511       /* bytes, without the line end */
#define AZCONF_FILE_MAX (64 * 1024)
/* The cap for load and save alike, plus one byte for a final newline, so a file
 * accepted without one can be written back with it. */
static int over_cap(const char *buf, size_t len) {
    return len > (size_t) AZCONF_FILE_MAX + (len && buf[len - 1] == '\n');
}
#define AZCONF_HEADER "# AZ-MODS settings"

void (*azconf_log)(const char *fmt, ...) = NULL; /* silent unless the caller sets it */
#define LOG(...)                     \
    do {                             \
        if (azconf_log)              \
            azconf_log(__VA_ARGS__); \
    } while (0)

/* One line of the file. raw is what is written back; key/val are set for an entry,
 * NULL for a comment or blank line. */
struct azline {
    char *raw, *key, *val;
};
struct azconf {
    struct azline *l;
    int n, cap;
    int dirty;
};

/* ---- lines */
static char *dupn(const char *s, size_t n) {
    /* n is the length of text already in memory, so n + 1 cannot wrap to a 0-byte malloc */
    char *p = malloc(n + 1); // NOLINT(clang-analyzer-optin.portability.UnixAPI): see above
    if (p) {
        memcpy(p, s, n);
        p[n] = 0; // NOLINT(clang-analyzer-unix.Malloc): see above
    }
    return p;
}
static void line_free(struct azline *l) {
    free(l->raw);
    free(l->key);
    free(l->val);
}
static void conf_clear(struct azconf *c) {
    for (int i = 0; i < c->n; i++)
        line_free(&c->l[i]);
    c->n = 0;
}
/* Takes ownership of raw/key/val (freed on failure). */
static int conf_push(struct azconf *c, char *raw, char *key, char *val) {
    if (!raw || (key && !val) || (!key && val))
        goto fail;
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        struct azline *l = realloc(c->l, (size_t) cap * sizeof *l);
        if (!l)
            goto fail;
        c->l = l;
        c->cap = cap;
    }
    c->l[c->n++] = (struct azline) {raw, key, val};
    return 0;
fail:
    free(raw);
    free(key);
    free(val);
    return -1;
}
static void conf_remove(struct azconf *c, int i) {
    line_free(&c->l[i]);
    memmove(&c->l[i], &c->l[i + 1], (size_t) (c->n - i - 1) * sizeof c->l[0]);
    c->n--;
}
static int conf_find(const struct azconf *c, const char *key) {
    for (int i = 0; i < c->n; i++)
        if (c->l[i].key && !strcmp(c->l[i].key, key))
            return i;
    return -1;
}
static char *entry_raw(const char *key, const char *val) {
    size_t n = strlen(key) + strlen(val) + 3;
    char *p = malloc(n + 1);
    if (p)
        snprintf(p, n + 1, "%s = %s", key, val);
    return p;
}
static int conf_reset(struct azconf *c) {
    conf_clear(c);
    c->dirty = 0;
    if (conf_push(c, strdup(AZCONF_HEADER), NULL, NULL) != 0)
        return -1;
    return conf_push(c, strdup("version = 1"), strdup("version"), strdup("1"));
}

/* ---- validation */
static int key_ok(const char *k, size_t n) {
    if (n == 0 || n > AZCONF_LINE_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = k[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
              ch == ':' || ch == '-'))
            return 0;
    }
    return 1;
}
/* No control bytes (tab allowed) and well-formed UTF-8: no overlongs, surrogates or code
 * points past U+10FFFF. */
static int text_ok(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned ch = s[i];
        if (ch < 0x80) {
            if ((ch < 0x20 && ch != '\t') || ch == 0x7f)
                return 0;
            i++;
            continue;
        }
        int len = ch >= 0xc2 && ch <= 0xdf ? 2 : ch >= 0xe0 && ch <= 0xef ? 3
                                             : ch >= 0xf0 && ch <= 0xf4   ? 4
                                                                          : 0;
        if (!len || i + (size_t) len > n)
            return 0;
        unsigned c1 = s[i + 1];
        if ((ch == 0xe0 && c1 < 0xa0) || (ch == 0xed && c1 > 0x9f) || (ch == 0xf0 && c1 < 0x90) ||
            (ch == 0xf4 && c1 > 0x8f))
            return 0;
        for (int k = 1; k < len; k++)
            if ((s[i + k] & 0xc0) != 0x80)
                return 0;
        i += (size_t) len;
    }
    return 1;
}
static int is_ws(char ch) {
    return ch == ' ' || ch == '\t';
}
static int val_ok(const char *v) {
    size_t n = strlen(v);
    return n <= AZCONF_LINE_MAX && text_ok((const unsigned char *) v, n) &&
           !(n && (is_ws(v[0]) || is_ws(v[n - 1])));
}

/* ---- load */
/* One line (line end already stripped). 0 = kept, 1 = ignored (logged). */
static int parse_line(struct azconf *c, const char *p, size_t len, int lineno) {
    if (len > AZCONF_LINE_MAX) {
        LOG("azconf: line %d: longer than %d bytes, ignored\n", lineno, AZCONF_LINE_MAX);
        return 1;
    }
    if (!text_ok((const unsigned char *) p, len)) {
        LOG("azconf: line %d: control bytes or not UTF-8, ignored\n", lineno);
        return 1;
    }
    size_t a = 0;
    while (a < len && is_ws(p[a]))
        a++;
    if (a == len || p[a] == '#') /* blank or comment: kept verbatim */
        return conf_push(c, dupn(p, len), NULL, NULL) ? 1 : 0;
    const char *eq = memchr(p, '=', len);
    if (!eq) {
        LOG("azconf: line %d: not `key = value`, ignored\n", lineno);
        return 1;
    }
    size_t ke = (size_t) (eq - p), vs = ke + 1, ve = len;
    while (ke > a && is_ws(p[ke - 1]))
        ke--;
    while (vs < ve && is_ws(p[vs]))
        vs++;
    while (ve > vs && is_ws(p[ve - 1]))
        ve--;
    if (!key_ok(p + a, ke - a)) {
        LOG("azconf: line %d: bad key, ignored\n", lineno);
        return 1;
    }
    char *key = dupn(p + a, ke - a);
    int dup = key ? conf_find(c, key) : -1;
    if (dup >= 0) /* the last one wins, in its own place */
        conf_remove(c, dup);
    if (conf_push(c, dupn(p, len), key, dupn(p + vs, ve - vs)) != 0)
        return 1;
    if (dup >= 0) {
        LOG("azconf: line %d: duplicate key %s, the last one wins\n", lineno, c->l[c->n - 1].key);
        return 1;
    }
    return 0;
}
/* The file's version: 1, >= 2, or 0 when there is no valid version line. */
static long conf_version(const struct azconf *c) {
    const char *v = azconf_get(c, "version");
    if (!v || !*v)
        return 0;
    for (const char *q = v; *q; q++)
        if (*q < '0' || *q > '9')
            return 0;
    if (strlen(v) > 6)
        return 99999; /* absurd, but plainly not ours: never overwrite it */
    return atol(v);
}
int azconf_load(struct azconf *c, const char *path) {
    FILE *f = fopen(path, "re");
    if (!f) {
        int absent = errno == ENOENT;
        conf_reset(c);
        if (!absent)
            LOG("azconf: %s: cannot open (%s)\n", path, strerror(errno));
        return absent ? -1 : -2;
    }
    char *buf = malloc(AZCONF_FILE_MAX + 2);
    size_t len = buf ? fread(buf, 1, AZCONF_FILE_MAX + 2, f) : 0;
    int err = !buf || ferror(f);
    fclose(f);
    if (err || over_cap(buf, len)) {
        LOG("azconf: %s: %s\n", path, err ? "read failed" : "over 64 KB, invalid");
        free(buf);
        conf_reset(c);
        return -2;
    }
    conf_clear(c);
    int dropped = 0, lineno = 0;
    size_t off = 0;
    if (len >= 3 && !memcmp(buf, "\xef\xbb\xbf", 3)) { /* a BOM from an editor */
        off = 3;
        dropped = 1;
    }
    while (off < len) {
        const char *nl = memchr(buf + off, '\n', len - off);
        size_t e = nl ? (size_t) (nl - buf) : len, l = e - off;
        if (l && buf[off + l - 1] == '\r') { /* CRLF: written back as LF */
            l--;
            dropped = 1;
        }
        dropped |= parse_line(c, buf + off, l, ++lineno);
        off = e + 1;
    }
    free(buf);
    long v = conf_version(c);
    if (v != 1) {
        LOG("azconf: %s: %s\n", path, v ? "a newer version, left alone" : "no valid version line");
        conf_reset(c);
        return v ? -3 : -2;
    }
    c->dirty = dropped;
    return 0;
}

/* ---- get / set */
const char *azconf_get(const struct azconf *c, const char *key) {
    int i = c && key ? conf_find(c, key) : -1;
    return i >= 0 ? c->l[i].val : NULL;
}
int azconf_get_bool(const struct azconf *c, const char *key, int def) {
    const char *v = azconf_get(c, key);
    if (!v)
        return def;
    if (!strcasecmp(v, "on") || !strcmp(v, "1") || !strcasecmp(v, "true"))
        return 1;
    if (!strcasecmp(v, "off") || !strcmp(v, "0") || !strcasecmp(v, "false"))
        return 0;
    LOG("azconf: %s = %s: not on/off, default used\n", key, v);
    return def;
}
long azconf_get_int(const struct azconf *c, const char *key, long def, long min, long max) {
    const char *v = azconf_get(c, key);
    if (!v)
        return def;
    char *end;
    errno = 0;
    long x = strtol(v, &end, 10);
    if (!*v || *end || errno || x < min || x > max || is_ws(*v)) {
        LOG("azconf: %s = %s: not a number in %ld..%ld, default used\n", key, v, min, max);
        return def;
    }
    return x;
}
int azconf_set(struct azconf *c, const char *key, const char *val) {
    if (!c || !key || !val || !key_ok(key, strlen(key)) || !strcmp(key, "version") ||
        !val_ok(val) || strlen(key) + strlen(val) + 3 > AZCONF_LINE_MAX)
        return -1;
    int i = conf_find(c, key);
    if (i >= 0 && !strcmp(c->l[i].val, val))
        return 0;
    char *raw = entry_raw(key, val), *v = strdup(val);
    if (!raw || !v) {
        free(raw);
        free(v);
        return -1;
    }
    if (i >= 0) {
        free(c->l[i].raw);
        free(c->l[i].val);
        c->l[i].raw = raw;
        c->l[i].val = v;
    } else if (conf_push(c, raw, strdup(key), v) != 0) {
        return -1;
    }
    c->dirty = 1;
    return 0;
}
int azconf_set_bool(struct azconf *c, const char *key, int on) {
    return azconf_set(c, key, on ? "on" : "off");
}
int azconf_set_int(struct azconf *c, const char *key, long v) {
    char b[24];
    snprintf(b, sizeof b, "%ld", v);
    return azconf_set(c, key, b);
}
int azconf_dirty(const struct azconf *c) {
    return c ? c->dirty : 0;
}

/* ---- save: azmod.c settings_write's approach, durable */
static int write_whole(const char *path, const char *buf, size_t len) {
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int) sizeof tmp)
        return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        off += (size_t) w;
    }
    int ok = off == len && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
    }
    return 0;
}
int azconf_save(struct azconf *c, const char *path) {
    if (!c || !path)
        return -1;
    size_t len = 0;
    for (int i = 0; i < c->n; i++)
        len += strlen(c->l[i].raw) + 1;
    char *buf = malloc(len + 1), *p = buf;
    if (!buf)
        return -1;
    for (int i = 0; i < c->n; i++) {
        size_t n = strlen(c->l[i].raw);
        memcpy(p, c->l[i].raw, n);
        p[n] = '\n';
        p += n + 1;
    }
    if (over_cap(buf, len)) { /* load would refuse it */
        LOG("azconf: %s: over 64 KB, not saved\n", path);
        free(buf);
        return -1;
    }
    int rc = write_whole(path, buf, len);
    free(buf);
    if (rc != 0) {
        LOG("azconf: %s: save failed (%s), the old file is untouched\n", path, strerror(errno));
        return -1;
    }
    c->dirty = 0;
    return 0;
}

struct azconf *azconf_new(void) {
    struct azconf *c = calloc(1, sizeof *c);
    if (c && conf_reset(c) != 0) {
        azconf_free(c);
        return NULL;
    }
    return c;
}
void azconf_free(struct azconf *c) {
    if (!c)
        return;
    conf_clear(c);
    free(c->l);
    free(c);
}

/* ---- migration from 1.2.2 */
static int slot_ok(int slot, int n) {
    return slot >= 0 && slot < n;
}
static int flag_exists(const char *dir, const char *name) {
    char p[512];
    if (snprintf(p, sizeof p, "%s/%s", dir, name) >= (int) sizeof p)
        return 0;
    return access(p, F_OK) == 0;
}
/* The first word of a small settings file (fscanf "%31s", as its readers do); 0 = none. */
static int read_word(const char *dir, const char *name, char *out, size_t cap) {
    char p[512];
    if (snprintf(p, sizeof p, "%s/%s", dir, name) >= (int) sizeof p)
        return 0;
    FILE *f = fopen(p, "re");
    if (!f)
        return 0;
    char b[64] = {0};
    size_t len = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[len] = 0;
    char *s = b;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    size_t n = strcspn(s, " \t\r\n");
    if (!n || n >= cap || n > 31) {
        LOG("azconf: migrate: %s is empty or unreadable, skipped\n", name);
        return 0;
    }
    memcpy(out, s, n);
    out[n] = 0;
    return 1;
}
/* azmod.cfg line 1: its width when it validates, else 0. A copy of azmod.c cfg_line1_width:
 * a run of '0'/'1' at least AZMOD_CFG_MIN_WIDTH (13) long, ended by end of line. */
#define AZCONF_CFG_MIN_WIDTH 13
/* Line 2 has four pad modes (AZMOD_PADMODE_COUNT): its layout, so fixed here. */
#define AZCONF_PADMODES 4
static int cfg_width(const char *buf, int size) {
    int n = 0;
    while (n < size - 1 && (buf[n] == '0' || buf[n] == '1'))
        n++;
    if (n >= AZCONF_CFG_MIN_WIDTH && (buf[n] == '\0' || buf[n] == '\n' || buf[n] == '\r'))
        return n;
    return 0;
}
/* The settings load_config derives from azmod.cfg. */
struct legacy {
    char on[64];
    int color[AZCONF_PADMODES], color2[AZCONF_PADMODES], colors_on;
    unsigned long long pins;
};
/* load_config's reads, in its order and with its buffer sizes (fgets of AZMOD_MOD_COUNT + 66,
 * then 64, then 32), so a malformed file splits into lines exactly as it does there. */
static void legacy_read(FILE *f, struct legacy *L, int n, const char *dir,
                        const struct azconf_legacy_defaults *d) {
    char buf[64 + 66] = {0};
    int line1_ok = 0;
    if (fgets(buf, n + 66, f)) {
        int w = cfg_width(buf, n + 66);
        if (w) {
            line1_ok = 1;
            int k = w < n ? w : n;
            for (int i = 0; i < k; i++)
                L->on[i] = buf[i] == '1';
            if (slot_ok(d->slot_overlays, n) && slot_ok(d->slot_fps, n) && w <= d->slot_overlays &&
                L->on[d->slot_fps])
                L->on[d->slot_overlays] = 1;
            if (slot_ok(d->slot_pledit, n) && w <= d->slot_pledit &&
                flag_exists(dir, "azmod-plreorder.on"))
                L->on[d->slot_pledit] = 1;
            if (slot_ok(d->slot_remotestems, n) && w <= d->slot_remotestems)
                L->on[d->slot_remotestems] = 1;
            if (slot_ok(d->slot_ui60ui, n) && w <= d->slot_ui60ui &&
                (flag_exists(dir, "azmod-rowcache.on") || flag_exists(dir, "azmod-touchaccum.on")))
                L->on[d->slot_ui60ui] = 1;
        } else {
            LOG("azconf: migrate: azmod.cfg line 1 is corrupt: defaults, lines 2-3 ignored\n");
        }
    }
    if (!line1_ok)
        return;
    char cbuf[64];
    if (fgets(cbuf, sizeof cbuf, f)) { /* line 2: c0 c1 c2 c3 master c2_0..c2_3 */
        int v[AZCONF_PADMODES], w[AZCONF_PADMODES], master = 1;
        for (int j = 0; j < AZCONF_PADMODES; j++)
            v[j] = w[j] = -1;
        int got = sscanf(cbuf, "%d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &master,
                         &w[0], &w[1], &w[2], &w[3]);
        for (int j = 0; j < AZCONF_PADMODES; j++) {
            L->color[j] = (v[j] >= 0 && v[j] < d->palette_count) ? v[j] : -1;
            L->color2[j] = (got >= 6 + j && w[j] >= 0 && w[j] < d->palette_count) ? w[j] : -1;
        }
        L->colors_on = got >= 5 ? master != 0 : 1;
    }
    char pbuf[32];
    if (fgets(pbuf, sizeof pbuf, f)) { /* line 3: the pin mask */
        unsigned long long pm = 0;
        if (sscanf(pbuf, "%llu", &pm) == 1)
            L->pins = pm & (n >= 64 ? ~0ull : (1ull << n) - 1);
    }
}
/* The radio set holding slot, or -1. */
static int radio_of(const struct azconf_legacy_defaults *d, int slot) {
    for (int r = 0; r < d->n_radio; r++)
        for (int k = 0; d->radio[r].slots && k < d->radio[r].n; k++)
            if (d->radio[r].slots[k] == slot)
                return r;
    return -1;
}
static void set_logged(struct azconf *c, const char *key, const char *val) {
    if (azconf_set(c, key, val) != 0)
        LOG("azconf: migrate: %s = %s refused (bad key or value), skipped\n",
            key ? key : "(no key)", val);
}
/* azmod-theme / azmod-vinyl.v / azmod-btchannel: a valid value becomes its key; anything
 * else is skipped (the default applies) and logged. */
static void migrate_small_files(struct azconf *c, const char *dir,
                                const struct azconf_legacy_defaults *d) {
    char w[32];
    if (read_word(dir, "azmod-theme", w, sizeof w)) {
        int ok = 0;
        for (int k = 0; k < d->n_themes; k++)
            ok |= d->themes[k] && !strcmp(w, d->themes[k]);
        if (ok)
            set_logged(c, "theme", w);
        else
            LOG("azconf: migrate: azmod-theme \"%s\" is not a theme, skipped\n", w);
    }
    if (read_word(dir, "azmod-vinyl.v", w, sizeof w)) {
        /* digits and one '.', 0.00..1.00: what the slider writes ("%.2f") */
        char *end;
        int dots = 0, digits = 0, ok = 1;
        for (const char *q = w; *q; q++) {
            dots += *q == '.';
            digits += *q >= '0' && *q <= '9';
            ok &= (*q >= '0' && *q <= '9') || *q == '.';
        }
        double v = strtod(w, &end);
        if (ok && dots <= 1 && digits && !*end && v >= 0.0 && v <= 1.0)
            set_logged(c, "vinyl.brake", w);
        else
            LOG("azconf: migrate: azmod-vinyl.v \"%s\" is not 0.00..1.00, skipped\n", w);
    }
    if (read_word(dir, "azmod-btchannel", w, sizeof w)) {
        if (!strcmp(w, "3") || !strcmp(w, "4"))
            set_logged(c, "bluetooth.channel", w);
        else
            LOG("azconf: migrate: azmod-btchannel \"%s\" is not 3 or 4, skipped\n", w);
    }
}
int azconf_migrate_legacy(struct azconf *c, const char *settings_dir, const char *const *slot_key,
                          int n, const struct azconf_legacy_defaults *d) {
    if (!c || !settings_dir || !slot_key || !d || n < 1 || n > 64 || d->n_radio < 0 ||
        d->n_radio > 32 || (d->n_radio && !d->radio)) {
        LOG("azconf: migrate: bad arguments\n");
        return -1;
    }
    char p[512];
    if (snprintf(p, sizeof p, "%s/azmod.cfg", settings_dir) >= (int) sizeof p)
        return -1;
    FILE *f = fopen(p, "re");
    if (!f)
        return -1;
    struct legacy L;
    memset(&L, 0, sizeof L);
    for (int j = 0; j < AZCONF_PADMODES; j++)
        L.color[j] = L.color2[j] = -1;
    L.colors_on = 1;
    for (int i = 0; i < d->n_default_on; i++) /* azmod_state_init */
        if (slot_ok(d->default_on[i], n))
            L.on[d->default_on[i]] = 1;
    for (int i = 0; i < d->n_force_on; i++)
        if (slot_ok(d->force_on[i], n))
            L.on[d->force_on[i]] = 1;
    legacy_read(f, &L, n, settings_dir, d);
    fclose(f);
    for (int i = 0; i < d->n_force_on; i++) /* re-pinned after the parse, as load_config does */
        if (slot_ok(d->force_on[i], n))
            L.on[d->force_on[i]] = 1;
    for (int i = 0; i < d->n_force_off; i++)
        if (slot_ok(d->force_off[i], n))
            L.on[d->force_off[i]] = 0;

    conf_reset(c);
    for (int i = 0; i < n; i++)
        if (slot_key[i] && radio_of(d, i) < 0) {
            int num = 0;
            for (int k = 0; d->int_slots && k < d->n_int_slots && !num; k++)
                num = d->int_slots[k] == i;
            set_logged(c, slot_key[i], num ? (L.on[i] ? "1" : "0") : (L.on[i] ? "on" : "off"));
        }
    for (int r = 0; r < d->n_radio; r++) { /* the first slot on wins; none = position 0 */
        int pos = 0;
        for (int k = 0; d->radio[r].slots && k < d->radio[r].n; k++)
            if (slot_ok(d->radio[r].slots[k], n) && L.on[d->radio[r].slots[k]]) {
                pos = k;
                break;
            }
        char b[16];
        snprintf(b, sizeof b, "%d", pos);
        set_logged(c, d->radio[r].key, b);
    }
    set_logged(c, "padcolor.on", L.colors_on ? "on" : "off");
    for (int pass = 0; pass < 2; pass++)
        for (int j = 0; j < AZCONF_PADMODES; j++) {
            char k[32], v[16];
            int x = pass ? L.color2[j] : L.color[j];
            snprintf(k, sizeof k, "%s.%d", pass ? "padcolor2" : "padcolor", j);
            if (x < 0)
                snprintf(v, sizeof v, "native");
            else
                snprintf(v, sizeof v, "%d", x);
            set_logged(c, k, v);
        }
    if (L.pins) {
        char list[AZCONF_LINE_MAX + 1];
        size_t len = 0;
        unsigned radio_pinned = 0;
        list[0] = 0;
        for (int i = 0; i < n; i++) {
            if (!(L.pins >> i & 1))
                continue;
            /* a pinned radio slot pins its set's key (once); a retired slot's pin is dropped */
            int r = radio_of(d, i);
            const char *k = r >= 0 ? d->radio[r].key : slot_key[i];
            if (!k) {
                LOG("azconf: migrate: pin on retired slot %d dropped\n", i);
                continue;
            }
            if (r >= 0 && radio_pinned >> r & 1)
                continue;
            if (r >= 0)
                radio_pinned |= 1u << r;
            int w = snprintf(list + len, sizeof list - len, "%s%s", len ? "," : "", k);
            if (w < 0 || (size_t) w >= sizeof list - len)
                break;
            len += (size_t) w;
        }
        if (len)
            set_logged(c, "pinned", list);
    }
    migrate_small_files(c, settings_dir, d);
    c->dirty = 1;
    return 0;
}

int azconf_entry(const struct azconf *c, int i, const char **key, const char **val) {
    for (int k = 0; c && k < c->n; k++)
        if (c->l[k].key && strcmp(c->l[k].key, "version") && i-- == 0) {
            *key = c->l[k].key;
            *val = c->l[k].val;
            return 1;
        }
    return 0;
}
int azconf_unset(struct azconf *c, const char *key) {
    int i = c && key && strcmp(key, "version") ? conf_find(c, key) : -1;
    if (i < 0)
        return -1;
    conf_remove(c, i);
    c->dirty = 1;
    return 0;
}
