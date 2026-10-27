/* SPDX-License-Identifier: GPL-3.0-only */
/* ==== community tile icons: EP147's own PNG decoder, on EP147's message thread =============
 * The catalog names a local PNG per mod (/tmp/az-community/icons/, written by the fetcher).
 * libpng is not mapped in EP147, so the decode is JUCE's, the same path menu_popup.c's runtime
 * labels use (mp_image_uncached: MemoryInputStream 0x230d790 -> findImageFormatForStream
 * 0x23b4be0 -> the format's decodeImage, released with 0x23a2e60), and like there it runs on
 * the message thread: from the message tick (calltimers_hook, installed lazily by the first
 * icon request via install_msgtick_hook), one icon per tick, never from a draw. The Mod Menu
 * (overlay thread) only asks for an icon and blits what is ready. While a decode is queued
 * (normally a tick or two) the tile is left empty, so a picture does not flash in over its
 * placeholder; the placeholder is drawn when the icon cannot be read, when no decode runs, or
 * when one has waited CMI_WAIT_MS. Each icon is scaled once to the requested size and kept
 * as premultiplied ARGB.
 * Bounded: 48 slots. When all are taken, the least recently used READY or FAILED one is freed
 * and reused; only the overlay thread does that, and it uses an icon's pixels only right after
 * asking for them, so no pointer outlives its slot (the message thread only touches WANTED slots).
 * Icons come from any repository a DJ browses, so before JUCE sees a byte the file must be a
 * well-formed PNG (cmi_png_ok): every chunk's length and CRC valid, IHDR first and alone, 8-bit
 * RGB / RGBA / palette, no interlace, at most 512 x 512, no unknown critical chunk, nothing after
 * IEND. Ancillary chunks (text, time, background, color profiles...) are dropped from the copy
 * JUCE decodes, so it never parses them.
 * A mod's screenshots (its catalog page) take the same path with their own limits: at most
 * 1280 x 800 and 1 MB, scaled once to fit a box (the aspect kept, never enlarged). They have
 * their own CMS_N slots, so a few large pictures never push the icons out and their memory stays
 * bounded (CMS_N full-screen pictures at most). */
#define CMI_N 48
#define CMS_N 4
#define CMI_MAX (256 * 1024)
#define CMS_MAX (1024 * 1024)
#define CMI_WAIT_MS 1000 /* a queued decode longer than this shows the placeholder meanwhile */
enum {
    CMI_FREE = 0,
    CMI_WANTED,
    CMI_READY,
    CMI_FAILED
};
typedef struct {
    char path[160];
    int sz;     /* an icon: its square's side; a screenshot: the box's width */
    int bh;     /* a screenshot: the box's height (0: an icon) */
    int ow, oh; /* the scaled picture's size, set before READY */
    int st;
    uint32_t *px;
    unsigned used;
    long asked; /* now_ms() of the request (overlay thread only) */
} cmi_slot;
static cmi_slot g_cmi[CMI_N];   /* st: atomics only */
static cmi_slot g_cms[CMS_N];   /* the screenshots' slots, the same rules */
static unsigned g_cmi_clock;   /* overlay thread: bumps on every request (LRU) */
static _Atomic int g_cmi_ok, g_cmi_bad;
/* icons finished so far, decoded or refused: the Community screen redraws when it moves */
unsigned azcm_icons_done(void) {
    return (unsigned) (g_cmi_ok + g_cmi_bad); /* _Atomic: two atomic loads */
}

/* overlay thread: the slot for (path, sz, bh) in a[0..n): ready pixels, or NULL (and ask for them
 * once). *pending: 1 while a decode is queued and younger than CMI_WAIT_MS (draw nothing yet). */
static const uint32_t *cmi_want(cmi_slot *a, int n, const char *path, int sz, int bh, int *pending, cmi_slot **got) {
    /* the decode runs on the message tick: install it the first time a picture is wanted (a deck
     * that never opens Community never gets the hook); once only, shared with the library hooks */
    static int tick = -1;
    if (tick < 0) tick = install_msgtick_hook("Community icons");
    if (!tick) return NULL;
    g_cmi_clock++;
    int freei = -1, lru = -1;
    for (int i = 0; i < n; i++) {
        int st = __atomic_load_n(&a[i].st, __ATOMIC_ACQUIRE);
        if (st == CMI_FREE) {
            if (freei < 0) freei = i;
            continue;
        }
        if (a[i].sz == sz && a[i].bh == bh && !strcmp(a[i].path, path)) {
            a[i].used = g_cmi_clock;
            *pending = st == CMI_WANTED && now_ms() - a[i].asked < CMI_WAIT_MS;
            if (st == CMI_READY) *got = &a[i];
            return st == CMI_READY ? a[i].px : NULL;
        }
        if ((st == CMI_READY || st == CMI_FAILED) && (lru < 0 || a[i].used < a[lru].used)) lru = i;
    }
    if (freei < 0 && lru >= 0) {   /* reuse the least recently used finished slot (overlay thread only) */
        free(a[lru].px);
        a[lru].px = NULL;
        __atomic_store_n(&a[lru].st, CMI_FREE, __ATOMIC_RELEASE);
        freei = lru;
    }
    if (freei < 0) return NULL;   /* every slot is being decoded: the placeholder for now */
    snprintf(a[freei].path, sizeof a[freei].path, "%s", path);
    a[freei].sz = sz;
    a[freei].bh = bh;
    a[freei].used = g_cmi_clock;
    a[freei].asked = now_ms();
    __atomic_store_n(&a[freei].st, CMI_WANTED, __ATOMIC_RELEASE);
    *pending = 1;
    return NULL;
}
/* overlay thread: the scaled icon if ready, else NULL (and ask for it once). *pending: 1 while
 * a decode is queued and younger than CMI_WAIT_MS (draw nothing yet), else 0 */
const uint32_t *azcm_icon(const char *path, int sz, int *pending) {
    *pending = 0;
    if (!path || !path[0] || sz < 8 || sz > 256 || strlen(path) >= sizeof g_cmi[0].path) return NULL;
    cmi_slot *got = NULL;
    return cmi_want(g_cmi, CMI_N, path, sz, 0, pending, &got);
}
/* overlay thread: a screenshot scaled to fit bw x bh (*ow x *oh), as azcm_icon */
const uint32_t *azcm_shot(const char *path, int bw, int bh, int *ow, int *oh, int *pending) {
    *pending = 0;
    *ow = *oh = 0;
    if (!path || !path[0] || bw < 8 || bh < 8 || bw > 1280 || bh > 800 || strlen(path) >= sizeof g_cms[0].path) return NULL;
    cmi_slot *got = NULL;
    const uint32_t *px = cmi_want(g_cms, CMS_N, path, bw, bh, pending, &got);
    if (px && got) {
        *ow = got->ow;
        *oh = got->oh;
    }
    return px;
}
static uint32_t cmi_crc(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static uint32_t cmi_be32(const unsigned char *p) { return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3]; }
static int cmi_png_fit(unsigned char *b, int n, uint32_t maxw, uint32_t maxh);
/* an icon: at most 512 x 512 */
__attribute__((unused)) static int cmi_png_ok(unsigned char *b, int n) { return cmi_png_fit(b, n, 512, 512); }
/* the PNG's length once compacted in place to the chunks a picture needs, or 0 = refused (see the
 * header comment); at most maxw x maxh */
static int cmi_png_fit(unsigned char *b, int n, uint32_t maxw, uint32_t maxh) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n < 8 + 25 + 12 || memcmp(b, sig, 8)) return 0;
    int off = 8, out = 8, seen_ihdr = 0, seen_idat = 0, idat_done = 0, ct = -1;
    while (off + 12 <= n) {
        uint32_t len = cmi_be32(b + off);
        const unsigned char *type = b + off + 4;
        if (len > (uint32_t) (n - off - 12)) return 0;
        if (cmi_crc(type, len + 4) != cmi_be32(type + 4 + len)) return 0;
        for (int k = 0; k < 4; k++)
            if (!((type[k] >= 'A' && type[k] <= 'Z') || (type[k] >= 'a' && type[k] <= 'z'))) return 0;
        const unsigned char *d = type + 4;
        int keep = 1;
        if (!memcmp(type, "IHDR", 4)) {
            if (seen_ihdr || off != 8 || len != 13) return 0;
            uint32_t w = cmi_be32(d), h = cmi_be32(d + 4);
            ct = d[9];
            if (!w || !h || w > maxw || h > maxh || d[8] != 8 || !(ct == 2 || ct == 6 || ct == 3) || d[10] || d[11] || d[12]) return 0;
            seen_ihdr = 1;
        } else if (!seen_ihdr) return 0;
        else if (!memcmp(type, "IDAT", 4)) {
            if (idat_done) return 0;
            seen_idat = 1;
        } else if (!memcmp(type, "IEND", 4)) {
            if (len || !seen_idat || off + 12 != n) return 0;
            memmove(b + out, b + off, 12);
            return out + 12;
        } else {
            if (seen_idat) idat_done = 1;   /* IDAT chunks must be consecutive */
            if (!memcmp(type, "PLTE", 4)) {
                if (ct != 3 || len % 3 || len > 768) return 0;
            } else if (type[0] >= 'A' && type[0] <= 'Z') return 0;   /* an unknown critical chunk */
            else keep = !memcmp(type, "tRNS", 4) || !memcmp(type, "sRGB", 4) || !memcmp(type, "gAMA", 4) ||
                        !memcmp(type, "pHYs", 4) || !memcmp(type, "cHRM", 4);
        }
        if (keep) {
            memmove(b + out, b + off, 12 + len);
            out += 12 + (int) len;
        }
        off += 12 + (int) len;
    }
    return 0;   /* no IEND */
}
static int cmi_read(const char *path, unsigned char **out, int *len, int cap, uint32_t maxw, uint32_t maxh) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 33 || st.st_size > cap) {
        close(fd);
        return 0;
    }
    unsigned char *b = malloc((size_t) st.st_size);
    int n = 0;
    while (b && n < st.st_size) {
        ssize_t r = read(fd, b + n, (size_t) (st.st_size - n));
        if (r <= 0) break;
        n += (int) r;
    }
    close(fd);
    if (!b || n != st.st_size) {
        free(b);
        return 0;
    }
    if (!(n = cmi_png_fit(b, n, maxw, maxh))) {
        free(b);
        return 0;
    }
    *out = b;
    *len = n;
    return 1;
}
/* the size a w x h picture is scaled to: an icon's square, or a screenshot fitted into its box
 * (the aspect kept, never enlarged, at least 1 x 1) */
static void cmi_out_size(const cmi_slot *sl, int w, int h, int *ow, int *oh) {
    if (!sl->bh) {
        *ow = *oh = sl->sz;
        return;
    }
    long x = w, y = h;
    if (x > sl->sz) {
        y = y * sl->sz / x;
        x = sl->sz;
    }
    if (y > sl->bh) {
        x = x * sl->bh / y;
        y = sl->bh;
    }
    *ow = x < 1 ? 1 : (int) x;
    *oh = y < 1 ? 1 : (int) y;
}
/* the last decode's phases, in ms (message thread only; for the icon log line) */
static long g_cmi_t_read, g_cmi_t_dec, g_cmi_t_scale;
/* decode + scale one picture; 1 = ready */
static int cmi_decode(cmi_slot *sl) {
    unsigned char *buf = NULL;
    int len = 0, shot = sl->bh > 0;
    uint32_t maxw = shot ? 1280 : 512, maxh = shot ? 800 : 512;
    long t0 = now_ms();
    g_cmi_t_read = g_cmi_t_dec = g_cmi_t_scale = 0;
    if (!cmi_read(sl->path, &buf, &len, shot ? CMS_MAX : CMI_MAX, maxw, maxh)) return 0;
    long t1 = now_ms();
    uintptr_t img = mp_image_uncached(buf, len), fm = 0, wh = 0, px = 0, st = 0;
    free(buf);   /* the stream was made without a copy, and decodeImage has finished with it */
    long t2 = now_ms();
    g_cmi_t_read = t1 - t0;
    g_cmi_t_dec = t2 - t1;
    int ok = 0;
    /* img: the decoded pixel data we hold a reference to until mp_release below */
    if (img && own_q(img + 0x08, &fm) && own_q(img + 0x10, &wh) && own_q(img + 0x48, &px) && own_q(img + 0x50, &st) && px) {
        int f = (int) (fm >> 32), w = (int) (uint32_t) wh, h = (int) (wh >> 32), ps = (int) (uint32_t) st, ls = (int) (st >> 32);
        if (((f == 2 && ps == 4) || (f == 1 && ps == 3)) && w > 0 && h > 0 && w <= (int) maxw && h <= (int) maxh && ls >= w * ps) {
            int ow, oh;
            cmi_out_size(sl, w, h, &ow, &oh);
            uint32_t *o = malloc((size_t) ow * oh * 4);
            if (o) {   /* box filter: each output pixel averages its source rectangle (premultiplied) */
                const uint8_t *s = (const uint8_t *) px;
                for (int y = 0; y < oh; y++) {
                    int y0 = y * h / oh, y1 = (y + 1) * h / oh;
                    if (y1 <= y0) y1 = y0 + 1;
                    for (int x = 0; x < ow; x++) {
                        int x0 = x * w / ow, x1 = (x + 1) * w / ow;
                        if (x1 <= x0) x1 = x0 + 1;
                        unsigned acc[4] = {0, 0, 0, 0}, n = 0;
                        for (int yy = y0; yy < y1; yy++)
                            for (int xx = x0; xx < x1; xx++) {
                                const uint8_t *p = s + (long) yy * ls + xx * ps;   /* B G R (A) */
                                acc[0] += p[0];
                                acc[1] += p[1];
                                acc[2] += p[2];
                                acc[3] += ps == 4 ? p[3] : 255;
                                n++;
                            }
                        /* NOLINTNEXTLINE(clang-analyzer-core.DivideZero): n >= 1, the loops above run at least once (y1 > y0, x1 > x0) */
                        o[y * ow + x] = (acc[3] / n) << 24 | (acc[2] / n) << 16 | (acc[1] / n) << 8 | (acc[0] / n);
                    }
                }
                sl->px = o;
                sl->ow = ow;
                sl->oh = oh;
                ok = 1;
                g_cmi_t_scale = now_ms() - t2;
            }
        }
    }
    mp_release(img);
    return ok;
}
/* message thread: the first queued slot of a[0..n) decoded, 1 = one was (at most one per tick) */
static int cmi_tick_one(cmi_slot *a, int n) {
    for (int i = 0; i < n; i++) {
        int st = __atomic_load_n(&a[i].st, __ATOMIC_ACQUIRE);
        if (st != CMI_WANTED) continue;
        long waited = now_ms() - a[i].asked;   /* asked: published by the RELEASE store above */
        int ok = cmi_decode(&a[i]);
        __atomic_add_fetch(ok ? &g_cmi_ok : &g_cmi_bad, 1, __ATOMIC_RELAXED);
        if (access("/home/root/settings/azmod-logs.on", F_OK) == 0) {   /* logs on: one line per picture */
            FILE *lf = azlog_open("/tmp/azmod-community-icons.log", "a");
            if (lf) {
                fprintf(lf, "[%ld] %s %s at %dx%d: %s (queued %ld ms, read %ld, decode %ld, scale %ld)\n", now_ms(),
                        a[i].bh ? "screenshot" : "icon", a[i].path, a[i].sz, a[i].bh ? a[i].bh : a[i].sz,
                        ok ? "decoded" : "refused or not decodable: placeholder", waited, g_cmi_t_read, g_cmi_t_dec, g_cmi_t_scale);
                fclose(lf);
            }
        }
        __atomic_store_n(&a[i].st, ok ? CMI_READY : CMI_FAILED, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}
/* message thread (calltimers_hook): at most one decode per tick, icons first */
static void cm_icons_tick(void) {
    if (!cmi_tick_one(g_cmi, CMI_N)) cmi_tick_one(g_cms, CMS_N);
}
