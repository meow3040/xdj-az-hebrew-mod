/* SPDX-License-Identifier: GPL-3.0-only */
/* azmod TTF text: overlay labels in EP147's own typefaces, antialiased, instead of the
 * built-in 5x7 bitmap font, so our UI does not read as foreign.
 *
 * The faces ship in /usr/share/fonts/ttf and EP147 already links libfreetype.so.6, so
 * this adds no runtime dependency. Small labels use light (vertical-only) hinting:
 * full hinting snaps stems to whole pixels and makes 9-11px text look blocky. The
 * rootfs has only a DemiBold Latin face, so heavier weights are synthesized.
 * `track` adds per-glyph letter-spacing; EP147's captions sit about +1px wider than
 * the face's natural advances.
 *
 * Kept out of menu.c so that file stays a pure, syscall-free compositor. Every entry
 * point returns 0 when the font cannot be opened, and menu.c falls back to the bitmap font. */
#include "menu.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

/* Two faces, both shipped in the rootfs and mapped by EP147 itself:
 *  [0] UCGothicLatin-DB: matches EP147's pad-mode captions; used by the stems strip
 *      and pills (azmod_ttf_center). AZMOD_TTF overrides.
 *  [1] TsukuGoCustomPro-B: the face of EP147's SOURCE list and right-pane text;
 *      used by the Mod Menu (azmod_ttf_menu). AZMOD_MENU_TTF overrides.
 * Separate slots so changing the menu's face cannot restyle the stems text. */
struct ttf_face {
    FT_Face f;
    int state;
    int px;
    const char *path;
    const char *env;
};
static struct ttf_face g_faces[2] = {
    {.path = "/usr/share/fonts/ttf/PioneerDJ-UCGothicLatin-DB.ttf", .env = "AZMOD_TTF"},
    {.path = "/usr/share/fonts/ttf/PDJ-TsukuGoCustomPro-B.otf", .env = "AZMOD_MENU_TTF"},
};
static FT_Library g_lib;
static int g_lib_state; /* 0 untried, 1 ready, -1 unavailable */

static int face_ready(struct ttf_face *fc) {
    if (fc->state)
        return fc->state > 0;
    fc->state = -1;
    if (!g_lib_state)
        g_lib_state = FT_Init_FreeType(&g_lib) ? -1 : 1;
    if (g_lib_state < 0)
        return 0;
    const char *path = getenv(fc->env);
    if (!path)
        path = fc->path;
    if (FT_New_Face(g_lib, path, 0, &fc->f))
        return 0;
    fc->state = 1;
    return 1;
}

/* Blend an 8-bit coverage value of color `c` over the pixel at (x,y). */
static void blend_px(const azmod_fb *fb, int x, int y, uint32_t c, unsigned a) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h || !a)
        return;
    uint32_t *p = (uint32_t *) (fb->px + (long) y * fb->stride + (long) x * 4);
    c = azm_ink(azm_theme(c)); /* Day Mode: grays flipped, colored text inked (menu_theme.h) */
    uint32_t d = *p;
    unsigned ia = 255u - a;
    unsigned r = (((c >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255u;
    unsigned g = (((c >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia) / 255u;
    unsigned b = ((c & 0xFF) * a + (d & 0xFF) * ia) / 255u;
    *p = (r << 16) | (g << 8) | b;
}

/* Load and render one glyph into face->glyph. Returns 0 on a load failure (the
 * caller skips the char). `bold` is a flag word: bit 0 = synthetic bold, bit 1 =
 * no hinting. The light hinter grid-fits each glyph independently, which shifts
 * neighbors by a pixel in large display text; unhinted glyphs keep their true
 * proportions and one shared baseline. Small labels keep the hinter. */
static int load_glyph(FT_Face g_face, unsigned char ch, int bold) {
    FT_Int32 fl = (bold & 2) ? (FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT)
                             : (FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT);
    if (FT_Load_Char(g_face, ch, fl))
        return 0;
    if (bold & 1)
        FT_GlyphSlot_Embolden(g_face->glyph);
    return FT_Render_Glyph(g_face->glyph, FT_RENDER_MODE_NORMAL) == 0;
}

/* Glyph cache: each (face, px, bold, char) is rasterized once and kept as a tight
 * alpha bitmap. The Mod Menu paints ~40 strings per knob detent, and rasterizing
 * each one twice (measure, then draw) made scrolling lag. Open addressing; at 3/4
 * full the table is dropped and refilled (the working set is a few hundred glyphs).
 * One mutex covers the cache and the shared FT_Face, held for a whole draw. */
#define GC_N 2048
struct gc_ent {
    unsigned key;
    short w, h, left, top, adv, ok;
    unsigned char *buf;
};
static struct gc_ent g_gc[GC_N];
static int g_gc_used;
static pthread_mutex_t g_gc_mx = PTHREAD_MUTEX_INITIALIZER;
static void gc_flush(void) {
    for (int i = 0; i < GC_N; i++) {
        free(g_gc[i].buf);
        g_gc[i].buf = 0;
        g_gc[i].key = 0;
    }
    g_gc_used = 0;
}
static const struct gc_ent *gc_get(struct ttf_face *fc, int px, int bold, unsigned char ch) {
    unsigned key = ((unsigned) (fc - g_faces) & 1u) << 31 | ((unsigned) bold & 3u) << 29 |
                   ((unsigned) px & 0x1FFFu) << 8 | ch; /* px >= 1, so never 0 */
    unsigned h = (key * 2654435761u) & (GC_N - 1);
    for (int n = 0; n < GC_N; n++, h = (h + 1) & (GC_N - 1)) {
        if (g_gc[h].key == key)
            return &g_gc[h];
        if (!g_gc[h].key)
            break;
    }
    if (g_gc_used >= GC_N * 3 / 4) {
        gc_flush();
        h = (key * 2654435761u) & (GC_N - 1);
    }
    while (g_gc[h].key)
        h = (h + 1) & (GC_N - 1);
    struct gc_ent *e = &g_gc[h];
    memset(e, 0, sizeof *e);
    e->key = key;
    g_gc_used++;
    FT_Face f = fc->f;
    if (px != fc->px) {
        if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt) px))
            return e;
        fc->px = px;
    }
    if (!load_glyph(f, ch, bold))
        return e;
    FT_GlyphSlot gs = f->glyph;
    e->w = (short) gs->bitmap.width;
    e->h = (short) gs->bitmap.rows;
    e->left = (short) gs->bitmap_left;
    e->top = (short) gs->bitmap_top;
    e->adv = (short) (gs->advance.x >> 6);
    e->ok = 1;
    if (e->w > 0 && e->h > 0) {
        e->buf = malloc((size_t) e->w * e->h);
        if (!e->buf) {
            e->w = e->h = 0;
            return e;
        }
        for (int r = 0; r < e->h; r++)
            memcpy(e->buf + (size_t) r * e->w,
                   gs->bitmap.buffer + (size_t) r * (unsigned) gs->bitmap.pitch, (size_t) e->w);
    }
    return e;
}

/* ttf_draw with a color switch: glyphs from byte `split` on are drawn in `c2`
 * (split < 0: one color). One string means one measurement and one baseline; two
 * strings would each center on their own cap height and could sit a pixel apart. */
static int ttf_draw2(struct ttf_face *fc, const azmod_fb *fb, int x, int y, int w, int h,
                     const char *s, int px, int track, int bold, uint32_t c, int split,
                     uint32_t c2);
static int ttf_draw(struct ttf_face *fc, const azmod_fb *fb, int x, int y, int w, int h,
                    const char *s, int px, int track, int bold, uint32_t c) {
    return ttf_draw2(fc, fb, x, y, w, h, s, px, track, bold, c, -1, c);
}
static int ttf_draw2(struct ttf_face *fc, const azmod_fb *fb, int x, int y, int w, int h,
                     const char *s, int px, int track, int bold, uint32_t c, int split,
                     uint32_t c2) {
    if (px < 1 || !face_ready(fc))
        return 0;
    pthread_mutex_lock(&g_gc_mx);
    /* Measure advance and cap height (max bitmap_top) to center on the ink: the
     * font's line box includes descender and leading and would sit the label low. */
    int adv = 0, cap = 0;
    for (const char *p = s; *p; p++) {
        const struct gc_ent *g = gc_get(fc, px, bold, (unsigned char) *p);
        if (!g->ok)
            continue;
        adv += g->adv + track;
        if (g->top > cap)
            cap = g->top;
    }
    if (!adv) {
        pthread_mutex_unlock(&g_gc_mx);
        return 0;
    }

    int pen = w > 0 ? x + (w - adv) / 2 : x; /* w <= 0: left-align at x (menu labels) */
    int base = y + (h + cap) / 2;
    for (const char *p = s; *p; p++) {
        uint32_t pc = (split >= 0 && p - s >= split) ? c2 : c;
        const struct gc_ent *g = gc_get(fc, px, bold, (unsigned char) *p);
        if (!g->ok)
            continue;
        for (int row = 0; row < g->h; row++)
            for (int col = 0; col < g->w; col++)
                blend_px(fb, pen + g->left + col, base - g->top + row, pc,
                         g->buf[(size_t) row * g->w + col]);
        pen += g->adv + track;
    }
    pthread_mutex_unlock(&g_gc_mx);
    return 1;
}

int azmod_ttf_center(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px,
                     int track, int bold, uint32_t c) {
    return ttf_draw(&g_faces[0], fb, x, y, w, h, s, px, track, bold, c);
}
int azmod_ttf_menu(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px, int track,
                   int bold, uint32_t c) {
    return ttf_draw(&g_faces[1], fb, x, y, w, h, s, px, track, bold, c);
}
int azmod_ttf_menu2(const azmod_fb *fb, int x, int y, int w, int h, const char *s, int px,
                    int track, int bold, uint32_t c, int split, uint32_t c2) {
    return ttf_draw2(&g_faces[1], fb, x, y, w, h, s, px, track, bold, c, split, c2);
}

/* Advance width of the first n bytes of s in the menu face, measured as ttf_draw2
 * measures (with no tracking), so a line wrapped with it fits when drawn by
 * azmod_ttf_menu at track 0. */
int azmod_ttf_menu_width(const char *s, int n, int px, int bold) {
    struct ttf_face *fc = &g_faces[1];
    if (px < 1 || !face_ready(fc))
        return 0;
    pthread_mutex_lock(&g_gc_mx);
    int adv = 0;
    for (int i = 0; i < n && s[i]; i++) {
        const struct gc_ent *g = gc_get(fc, px, bold, (unsigned char) s[i]);
        if (g->ok)
            adv += g->adv;
    }
    pthread_mutex_unlock(&g_gc_mx);
    return adv;
}
