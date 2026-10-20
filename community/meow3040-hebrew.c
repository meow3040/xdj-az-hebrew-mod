/* SPDX-License-Identifier: GPL-3.0-only */
/* meow3040-hebrew: the Hebrew mod as a Community Mod (AZ-MODS 1.2.3 and newer, EP147 2.00, the
 * build firmware 2.00 and 2.01 share). The same three parts as the built-in edition
 * (azmods/files/src/azmod/hebrew.c, which explains them at length), through the core's mod API:
 *
 * Letters. JUCE loads each character the first time it draws it, through EP147's PLT stubs for
 * FT_Get_Char_Index, FT_Load_Glyph and FT_Get_Kerning (EP147 links libfreetype.so.6). Those
 * stubs are 16 bytes of EP147's own code each, so they are claimed with claim_hook: a Hebrew
 * character the face has no glyph for gets the index HEB_GI_BASE + its code point, and loading
 * it fills the face's glyph slot with that character's outline from heb_glyphs.h (DejaVu Sans
 * Bold), scaled to the face's units per em. The deck's font files are not touched.
 *
 * Right to Left. GlyphArrangement::addCurtailedLineOfText gets lines holding right-to-left text
 * in visual order (bidi_logic.h), as a temporary juce::String; a right-to-left line too wide
 * for its box is cut on the left, so the beginning of the name stays in view.
 *
 * Hebrew Keyboard. The search keyboard opens on a Hebrew page (heb_keys.h): 123 switches it to
 * English, ABC on the numbers page back to Hebrew. While the core names a playlist on this
 * keyboard (NEW PLAYLIST, RENAME), it stays stock: the core's naming bar draws Latin only.
 *
 * Rows: Hebrew (the letters), with Right to Left and Hebrew Keyboard under it; all on by
 * default and read live. Every hook passes through while its row is off. Hooks are claimed
 * after EP147 started, so a Hebrew name JUCE drew before the mod loaded keeps its boxes until
 * the next Reload. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "azmod-mod.h"
#include "bidi_logic.h"
#include "heb_glyphs.h"
#include "heb_keys.h"

#define ID "meow3040-hebrew"
#define FW_200 "ep147-9e9454e357389ece" /* EP147 of firmware 2.00 and 2.01 */

static const az_core_v1 *C;
static int g_row_hebrew = -1, g_row_rtl = -1, g_row_kbd = -1;

static int row_on(int handle) {
    return handle >= 0 && C->row_value(handle) > 0;
}

/* ---- reading EP147's objects: one aligned load, the address plausible (the caller knows the
 * object is alive: a hook's argument, or an object reached from it during EP147's call) */
static int rd_q(uintptr_t a, uintptr_t *out) {
    if (a < 0x10000 || a > 0x0000ffffffffffffUL || (a & 7))
        return 0;
    *out = *(const volatile uintptr_t *) a;
    return 1;
}
static int rd_i32(uintptr_t a, int *out) {
    uintptr_t q = 0;
    if (!rd_q(a & ~(uintptr_t) 7, &q))
        return 0;
    *out = (int) (uint32_t) (q >> ((a & 4) ? 32 : 0));
    return 1;
}
static int rd_str(uintptr_t p, char *out, int n) {
    out[0] = 0;
    if (p < 0x10000 || p > 0x0000ffffffffffffUL || n < 1)
        return 0;
    const volatile unsigned char *s = (const volatile unsigned char *) p;
    for (int i = 0; i < n - 1; i++) {
        if (!(out[i] = (char) s[i]))
            return 1;
    }
    out[n - 1] = 0;
    return 1;
}

/* ---- EP147 2.00 */
#define JSTR_FROM_CSTR 0x24a1180u  /* juce::String::String(const char *) */
#define JSTR_DTOR 0x2469aa0u       /* juce::String::~String */
#define FT_PLT_CHAR_INDEX 0x427b80u /* EP147's PLT stubs into libfreetype */
#define FT_PLT_KERNING 0x427c60u
#define FT_PLT_LOAD_GLYPH 0x427d90u
#define LINE_FN 0x2554ee0u        /* juce::GlyphArrangement::addCurtailedLineOfText */
#define GLYPHPOS_FN 0x25548b0u    /* juce::Font::getGlyphPositions */
#define HK_CTRL_CTOR 0x1dc17d8u   /* SearchSoftwareKeyboardWidgetController(this, widget) */
#define HK_CTRL_DTOR 0x1dc6428u
#define HK_CTRL_RESTRICT 0x1dc1880u
#define HK_CTRL_CONTROL 0x1dc6e58u
#define HK_KEY_PAINT 0x19cc308u    /* image key paint(this, Graphics &, over, down) */
#define HK_SEND 0x1dc71d0u         /* EventSender::send(this, const String &, int) */
#define HK_DRAW_IMAGE_AT 0x251da80u /* juce::Graphics::drawImageAt */
#define HK_IS_ENABLED 0x25ab8c0u   /* juce::Component::isEnabled */
#define HK_REPAINT 0x25f5a10u      /* juce::Component::repaint */
#define HK_IMAGE_FROM_MEMORY 0x255ac60u /* juce::ImageCache::getFromMemory (returns via x8) */
#define HK_CTRL_VT 0x2fd4318u      /* gui::SearchSoftwareKeyboardWidgetController */
#define HK_WIDGET_VT 0x2fd44b8u    /* gui::SoftwareKeyboardWidget */
#define HK_GROUP_VT 0x2f797a8u     /* gui::LowercaseKeyWidget */
static const struct {
    uintptr_t at;
    uint64_t first8; /* the instructions the hooks were written against */
} CODE[] = {
    {FT_PLT_CHAR_INDEX, 0xf9416211d001daf0ULL}, {FT_PLT_KERNING, 0xf9419a11d001daf0ULL},
    {FT_PLT_LOAD_GLYPH, 0xf941e611d001daf0ULL}, {LINE_FN, 0x12001c63a9b27bfdULL},
    {GLYPHPOS_FN, 0x910003fda9bd7bfdULL},       {HK_CTRL_CTOR, 0x910003fda9be7bfdULL},
    {HK_CTRL_DTOR, 0x9101c001a9be7bfdULL},      {HK_CTRL_RESTRICT, 0x910003fda9bc7bfdULL},
    {HK_CTRL_CONTROL, 0x910003fda9be7bfdULL},   {HK_KEY_PAINT, 0x910003fda9bc7bfdULL},
    {HK_SEND, 0xd0ffffe3a9ba7bfdULL},           {HK_DRAW_IMAGE_AT, 0x1e2200601e220041ULL},
    {HK_IS_ENABLED, 0x3720004139430401ULL},
};
static int code_is(uintptr_t at) {
    uintptr_t q = 0;
    for (size_t i = 0; i < sizeof CODE / sizeof CODE[0]; i++)
        if (CODE[i].at == at)
            return rd_q(at, &q) && q == CODE[i].first8;
    return 0;
}

/* ==== letters: FreeType, through EP147's PLT ============================================ */
#define HEB_GI_BASE 0x7FFF0000u /* + code point: a Hebrew glyph the face lacks */
static FT_UInt (*o_char_index)(FT_Face, FT_ULong);
static FT_Error (*o_load_glyph)(FT_Face, FT_UInt, FT_Int32);
static FT_Error (*o_kerning)(FT_Face, FT_UInt, FT_UInt, FT_UInt, FT_Vector *);
static FT_Error (*p_render_glyph)(FT_GlyphSlot, FT_Render_Mode);
static int g_letters_hooked; /* all three: a Hebrew index is handed out only when it can load */

static const struct heb_glyph *heb_glyph_of(unsigned long cp) {
    int lo = 0, hi = (int) (sizeof HEB_GLYPHS / sizeof HEB_GLYPHS[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < HEB_GLYPHS[mid].cp)
            hi = mid - 1;
        else if (cp > HEB_GLYPHS[mid].cp)
            lo = mid + 1;
        else
            return &HEB_GLYPHS[mid];
    }
    return 0;
}
static int heb_face_ok(FT_Face face) {
    return face && FT_IS_SCALABLE(face) && face->units_per_EM > 0 && face->glyph && face->charmap &&
           face->charmap->encoding == FT_ENCODING_UNICODE;
}
static FT_UInt hook_char_index(FT_Face face, FT_ULong charcode) {
    FT_UInt gi = o_char_index(face, charcode);
    if (gi || !__atomic_load_n(&g_letters_hooked, __ATOMIC_ACQUIRE) || !row_on(g_row_hebrew) ||
        !heb_glyph_of(charcode) || !heb_face_ok(face))
        return gi;
    return HEB_GI_BASE + (FT_UInt) charcode;
}

static __thread struct {
    FT_Vector *pts;
    char *tags;
    short *ends;
    int cap;
} t_ol; /* the outline handed to JUCE: read right after the load, on the loading thread */

static FT_Pos heb_scale(long v, long num, long den) { /* v * num / den, rounded */
    long long p = (long long) v * num;
    return (FT_Pos) (p >= 0 ? (p + den / 2) / den : -((-p + den / 2) / den));
}
/* The slot after FreeType loaded glyph 0 into it: its outline and metrics become g's, in font
 * units (FT_LOAD_NO_SCALE, JUCE's way) or 26.6 pixels at the face's size. 0 = out of memory. */
static int heb_fill_slot(FT_Face face, const struct heb_glyph *g, FT_Int32 flags) {
    FT_GlyphSlot s = face->glyph;
    if (!t_ol.cap || g->npts > t_ol.cap) {
        int cap = g->npts < 256 ? 256 : g->npts;
        FT_Vector *p = realloc(t_ol.pts, (size_t) cap * sizeof *p);
        if (!p)
            return 0;
        t_ol.pts = p;
        char *t = realloc(t_ol.tags, (size_t) cap);
        if (!t)
            return 0;
        t_ol.tags = t;
        short *e = realloc(t_ol.ends, (size_t) cap * sizeof *e);
        if (!e)
            return 0;
        t_ol.ends = e;
        t_ol.cap = cap;
    }
    long num = face->units_per_EM, den = HEB_DONOR_UPM;
    int unscaled = (flags & FT_LOAD_NO_SCALE) || !face->size;
    long sx = unscaled ? 0x10000 : face->size->metrics.x_scale;
    long sy = unscaled ? 0x10000 : face->size->metrics.y_scale;
#define HEB_X(v) (unscaled ? heb_scale((v), num, den) : heb_scale(heb_scale((v), num, den), sx, 0x10000))
#define HEB_Y(v) (unscaled ? heb_scale((v), num, den) : heb_scale(heb_scale((v), num, den), sy, 0x10000))
    for (int i = 0; i < g->npts; i++) {
        t_ol.pts[i].x = HEB_X(HEB_PTS[g->pt0 + i][0]);
        t_ol.pts[i].y = HEB_Y(HEB_PTS[g->pt0 + i][1]);
        t_ol.tags[i] = (char) (HEB_TAGS[g->pt0 + i] ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CONIC);
    }
    for (int c = 0; c < g->ncont; c++)
        t_ol.ends[c] = (short) HEB_ENDS[g->ct0 + c];
    s->format = FT_GLYPH_FORMAT_OUTLINE;
    s->outline.n_points = (short) g->npts;
    s->outline.n_contours = (short) g->ncont;
    s->outline.points = t_ol.pts;
    s->outline.tags = t_ol.tags;
    s->outline.contours = t_ol.ends;
    s->outline.flags = 0;
    FT_Pos adv = HEB_X(g->adv);
    s->metrics.width = HEB_X(g->xmax) - HEB_X(g->xmin);
    s->metrics.height = HEB_Y(g->ymax) - HEB_Y(g->ymin);
    s->metrics.horiBearingX = HEB_X(g->xmin);
    s->metrics.horiBearingY = HEB_Y(g->ymax);
    s->metrics.horiAdvance = adv;
    s->metrics.vertBearingX = -s->metrics.width / 2;
    s->metrics.vertBearingY = 0;
    s->metrics.vertAdvance = unscaled ? face->ascender - face->descender
                                      : heb_scale(face->ascender - face->descender, sy, 0x10000);
    s->advance.x = adv;
    s->advance.y = 0;
    s->linearHoriAdvance = unscaled ? adv : adv << 10; /* 26.6 -> 16.16 */
    s->linearVertAdvance = s->metrics.vertAdvance;
    s->lsb_delta = s->rsb_delta = 0;
#undef HEB_X
#undef HEB_Y
    return 1;
}
static FT_Error hook_load_glyph(FT_Face face, FT_UInt glyph_index, FT_Int32 load_flags) {
    const struct heb_glyph *g = glyph_index >= HEB_GI_BASE ? heb_glyph_of(glyph_index - HEB_GI_BASE) : 0;
    /* an index we handed out (no face has that many glyphs): ours, whatever the row says now */
    if (!g || !heb_face_ok(face) || (FT_Long) glyph_index < face->num_glyphs)
        return o_load_glyph(face, glyph_index, load_flags);
    FT_Error err = o_load_glyph(face, 0, load_flags & ~FT_LOAD_RENDER); /* the slot, set up */
    if (err)
        return err;
    if (!heb_fill_slot(face, g, load_flags))
        return FT_Err_Out_Of_Memory;
    if (load_flags & FT_LOAD_RENDER)
        return p_render_glyph ? p_render_glyph(face->glyph, FT_LOAD_TARGET_MODE(load_flags))
                              : FT_Err_Unimplemented_Feature;
    return 0;
}
static FT_Error hook_kerning(FT_Face face, FT_UInt left, FT_UInt right, FT_UInt mode, FT_Vector *k) {
    if (left >= HEB_GI_BASE || right >= HEB_GI_BASE) {
        if (k)
            k->x = k->y = 0; /* the donor's letters carry no pairs with the face's */
        return 0;
    }
    return o_kerning(face, left, right, mode, k);
}

/* ==== juce::String holding UTF-8 ========================================================= */
/* EP147's String(const char *) reads ASCII: it is asked for as many spaces, which it allocates
 * and owns, and the UTF-8 bytes go over them once the result is proven a fresh copy. */
static int heb_jstr(uintptr_t *out, const char *utf8, size_t n) {
    char local[512], *sp = n < sizeof local ? local : malloc(n + 1);
    *out = 0;
    if (!n || !sp)
        return 0;
    memset(sp, ' ', n);
    sp[n] = 0;
    ((void (*)(uintptr_t *, const char *)) JSTR_FROM_CSTR)(out, sp);
    int ok = *out && !memcmp((const void *) *out, sp, n + 1);
    if (sp != local)
        free(sp);
    if (!ok) {
        if (*out)
            ((void (*)(uintptr_t *)) JSTR_DTOR)(out);
        *out = 0;
        return 0;
    }
    memcpy((void *) *out, utf8, n + 1);
    return 1;
}
static void heb_jstr_free(uintptr_t *s) {
    ((void (*)(uintptr_t *)) JSTR_DTOR)(s);
}

/* ==== right to left ====================================================================== */
#define HEB_MAX_BYTES 16384
#define HEB_STACK_CPS 256
struct heb_juce_array { /* juce::Array<T>: elements (freed with free()), numAllocated, numUsed */
    void *data;
    int allocated, pad0;
    int used, pad1;
};
typedef void (*heb_line_fn)(void *, const void *, const void *, float, float, float, bool);
static heb_line_fn o_line;
static __thread int t_nest;

static int heb_has_rtl(const char *s) {
    for (const unsigned char *p = (const unsigned char *) s; *p; p++)
        if ((*p >= 0xD6 && *p <= 0xDF) || (*p == 0xEF && p[1] >= 0xAC && p[1] <= 0xBB))
            return 1;
    return 0;
}
static int heb_utf8_decode(const char *s, size_t len, uint32_t *out) {
    const unsigned char *p = (const unsigned char *) s, *end = p + len;
    int n = 0;
    while (p < end) {
        uint32_t c = *p++, min = 0;
        int more = 0;
        if (c >= 0xC2 && c <= 0xDF)
            c &= 0x1F, more = 1, min = 0x80;
        else if (c >= 0xE0 && c <= 0xEF)
            c &= 0x0F, more = 2, min = 0x800;
        else if (c >= 0xF0 && c <= 0xF4)
            c &= 0x07, more = 3, min = 0x10000;
        else if (c >= 0x80)
            return -1;
        if (end - p < more)
            return -1;
        while (more--) {
            if ((*p & 0xC0) != 0x80)
                return -1;
            c = (c << 6) | (*p++ & 0x3F);
        }
        if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) || c == '\n' || c == '\r' ||
            c == 0x2028 || c == 0x2029 || n == BIDI_MAX)
            return -1;
        if (out)
            out[n] = c;
        n++;
    }
    return n;
}
static size_t heb_utf8_encode(const uint32_t *cp, int n, char *out) {
    unsigned char *p = (unsigned char *) out;
    for (int i = 0; i < n; i++) {
        uint32_t c = cp[i];
        if (c < 0x80)
            *p++ = (unsigned char) c;
        else if (c < 0x800) {
            *p++ = (unsigned char) (0xC0 | c >> 6);
            *p++ = (unsigned char) (0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            *p++ = (unsigned char) (0xE0 | c >> 12);
            *p++ = (unsigned char) (0x80 | (c >> 6 & 0x3F));
            *p++ = (unsigned char) (0x80 | (c & 0x3F));
        } else {
            *p++ = (unsigned char) (0xF0 | c >> 18);
            *p++ = (unsigned char) (0x80 | (c >> 12 & 0x3F));
            *p++ = (unsigned char) (0x80 | (c >> 6 & 0x3F));
            *p++ = (unsigned char) (0x80 | (c & 0x3F));
        }
    }
    *p = 0;
    return (size_t) (p - (unsigned char *) out);
}
/* A right-to-left line wider than w: the first visual index kept, *dots = "..." first. */
static int heb_fit_rtl(const void *font, const char *u8, size_t nbytes, int m, float w, bool ell,
                       int *dots) {
    *dots = 0;
    uintptr_t js = 0;
    if (!heb_jstr(&js, u8, nbytes))
        return 0;
    struct heb_juce_array glyphs = {0}, xs = {0};
    ((void (*)(const void *, const uintptr_t *, struct heb_juce_array *, struct heb_juce_array *))(
        uintptr_t) GLYPHPOS_FN)(font, &js, &glyphs, &xs);
    heb_jstr_free(&js);
    free(glyphs.data);
    const float *x = xs.data;
    int from = 0;
    if (x && xs.used == 3 + m + 1 && x[3 + m] - x[3] > w + 1.0f) {
        *dots = ell && m > 3;
        float room = w + 1.0f - (*dots ? x[3] : 0.0f);
        while (from < m && x[3 + m] - x[3 + from] > room)
            from++;
    }
    free(xs.data);
    return from;
}
static void heb_line_rtl(void *ga, const void *font, const void *text, const char *s, float x,
                         float y, float w, bool ell) {
    size_t len = strnlen(s, HEB_MAX_BYTES + 1);
    int n = len <= HEB_MAX_BYTES ? heb_utf8_decode(s, len, 0) : -1;
    struct {
        uint32_t in[HEB_STACK_CPS], out[HEB_STACK_CPS];
        int kept[HEB_STACK_CPS], pairs[HEB_STACK_CPS + 2];
        signed char lv[HEB_STACK_CPS];
        unsigned char orig[HEB_STACK_CPS], type[HEB_STACK_CPS];
        char u8[3 + 4 * HEB_STACK_CPS + 1];
    } st;
    void *heap = 0;
    uint32_t *in = st.in, *out = st.out;
    int *kept = st.kept, *pairs = st.pairs;
    signed char *lv = st.lv;
    unsigned char *orig = st.orig, *type = st.type;
    char *u8 = st.u8;
    if (n <= 0) {
        o_line(ga, font, text, x, y, w, ell);
        return;
    }
    if (n > HEB_STACK_CPS) {
        heap = malloc((size_t) n * 27 + 64);
        if (!heap) {
            o_line(ga, font, text, x, y, w, ell);
            return;
        }
        char *b = heap;
        in = (uint32_t *) b, b += (size_t) n * 4;
        out = (uint32_t *) b, b += (size_t) n * 4;
        kept = (int *) b, b += (size_t) n * 4;
        pairs = (int *) b, b += ((size_t) n + 2) * 4;
        lv = (signed char *) b, b += n;
        orig = (unsigned char *) b, b += n;
        type = (unsigned char *) b, b += n;
        u8 = b;
    }
    heb_utf8_decode(s, len, in);
    struct bidi_scratch ws = {orig, type, kept, pairs};
    int rtl = 0, m = bidi_visual(in, n, 2, out, &rtl, lv, &ws);
    if (m <= 0 || (m == n && !memcmp(in, out, (size_t) n * sizeof *in))) {
        free(heap);
        o_line(ga, font, text, x, y, w, ell);
        return;
    }
    memcpy(u8, "...", 3);
    size_t nb = heb_utf8_encode(out, m, u8 + 3);
    const char *draw = u8 + 3;
    size_t dn = nb;
    float dw = w;
    bool dell = ell;
    if (rtl && w < 1.0e9f) {
        int dots = 0, from = heb_fit_rtl(font, u8, nb + 3, m, w, ell, &dots);
        if (from > 0 || dots) {
            size_t off = (size_t) heb_utf8_encode(out, from, u8 + 3);
            heb_utf8_encode(out + from, m - from, u8 + 3 + off);
            if (dots) {
                memmove(u8 + 3, u8 + 3 + off, nb - off + 1);
                draw = u8, dn = 3 + nb - off;
            } else
                draw = u8 + 3 + off, dn = nb - off;
            dw = w + 4.0f;
            dell = false;
        }
    }
    uintptr_t js = 0;
    if (!heb_jstr(&js, draw, dn)) {
        free(heap);
        o_line(ga, font, text, x, y, w, ell);
        return;
    }
    free(heap);
    o_line(ga, font, &js, x, y, dw, dell);
    heb_jstr_free(&js);
}
static void hook_line(void *ga, const void *font, const void *text, float x, float y, float w,
                      bool ell) {
    uintptr_t sp = 0;
    if (t_nest || !row_on(g_row_rtl) || !rd_q((uintptr_t) text, &sp) || sp < 0x10000 ||
        !heb_has_rtl((const char *) sp)) {
        o_line(ga, font, text, x, y, w, ell);
        return;
    }
    t_nest++;
    heb_line_rtl(ga, font, text, (const char *) sp, x, y, w, ell);
    t_nest--;
}

/* ==== the Hebrew keyboard ================================================================ */
#define HK_CTRL_WIDGET 0x28
#define HK_CTRL_SENDER 0x50
#define HK_WIDGET_GROUP 0x188
#define HK_KEY_NAME 0x08
#define HK_KEY_IMG_X 0x1cc
#define HK_KEY_IMG_Y 0x1d0
#define HK_VT_SETVISIBLE 0x60
static const unsigned HK_LAYOUTS[4] = {0x128, 0x140, 0x158, 0x170};
enum { HK_HEBREW, HK_ENGLISH, HK_NUMBERS };
#define HK_ART_SHIFT 26
#define HK_ART_EN 27
#define HK_ART_HE 28

/* juce::ImageCache::getFromMemory(data, size) returns its juce::Image through x8 */
__asm__(".text\n.align 2\n.globl heb_ifm_call\n.hidden heb_ifm_call\n.type heb_ifm_call,%function\n"
        "heb_ifm_call:\nmov x16, x3\nmov x8, x0\nmov x0, x1\nmov w1, w2\nbr x16\n"
        ".size heb_ifm_call, .-heb_ifm_call\n");
void heb_ifm_call(uintptr_t *out, const void *data, int size, uintptr_t target);

static void (*o_ctor)(void *, void *);
static void (*o_dtor)(void *);
static void (*o_restrict)(void *, int);
static void (*o_control)(void *, const void *);
static void (*o_paint)(void *, void *, bool, bool);
static void (*o_send)(void *, const void *, int);
static int g_kbd_hooked;
static int (*p_naming)(void); /* the core's azmod_naming_playlist, when it has one */
/* message thread only (every keyboard hook runs there) */
static uintptr_t g_ctrl, g_widget;
static int g_mode, g_shift_shown;
static uintptr_t g_img[HEB_KEYS_N * 3];

static int hk_active(void) {
    return g_kbd_hooked && row_on(g_row_kbd) && !(p_naming && p_naming());
}
static int hk_alive(void) {
    uintptr_t vt = 0, w = 0, wvt = 0;
    return g_ctrl && g_widget && rd_q(g_ctrl, &vt) && vt == HK_CTRL_VT &&
           rd_q(g_ctrl + HK_CTRL_WIDGET, &w) && w == g_widget && rd_q(w, &wvt) && wvt == HK_WIDGET_VT;
}
static int hk_is_search_key(uintptr_t key) {
    if (!key || !hk_alive())
        return 0;
    for (int j = 0; j < 4; j++) {
        uintptr_t v = 0;
        int n = 0;
        if (!rd_q(g_widget + HK_LAYOUTS[j], &v) || !rd_i32(g_widget + HK_LAYOUTS[j] + 16, &n) || n < 0 ||
            n > 96 || (n && !v))
            return 0;
        for (int i = 0; i < n; i++) {
            uintptr_t k = 0;
            if (rd_q(v + 8 * (uintptr_t) i, &k) && k == key)
                return 1;
        }
    }
    return 0;
}
static int hk_name(uintptr_t comp, char *buf, int n) {
    uintptr_t p = 0;
    return rd_q(comp + HK_KEY_NAME, &p) && rd_str(p, buf, n);
}
static int hk_letter(const char *name) {
    const char *q = name[0] && !name[1] ? strchr(HEB_KEY_QWERTY, name[0]) : 0;
    return q ? (int) (q - HEB_KEY_QWERTY) : -1;
}
static uintptr_t hk_image(int art, int state) {
    int i = art * 3 + state;
    if (i < 0 || i >= HEB_KEYS_N * 3)
        return 0;
    if (!g_img[i])
        heb_ifm_call(&g_img[i], HEB_KEY_PNG[i].png, HEB_KEY_PNG[i].len, HK_IMAGE_FROM_MEMORY);
    return g_img[i];
}
static void hk_sync_shift(void) {
    if (!hk_alive())
        return;
    uintptr_t group = 0, gvt = 0, key = 0, kvt = 0, setvis = 0;
    char name[16];
    if (!rd_q(g_widget + HK_WIDGET_GROUP, &group) || !rd_q(group, &gvt) || gvt != HK_GROUP_VT ||
        !rd_q(group + HK_WIDGET_GROUP, &key) || !hk_is_search_key(key) || !hk_name(key, name, sizeof name) ||
        strcmp(name, "toUppercase") || !rd_q(key, &kvt) || !rd_q(kvt + HK_VT_SETVISIBLE, &setvis))
        return;
    int want = hk_active() && g_mode == HK_HEBREW;
    if (want || g_shift_shown) {
        ((void (*)(uintptr_t, bool)) setvis)(key, want);
        g_shift_shown = want;
    }
}
static void hk_repaint(void) {
    if (!hk_alive())
        return;
    hk_sync_shift();
    ((void (*)(uintptr_t))(uintptr_t) HK_REPAINT)(g_widget);
}
static int hk_type(int letter, int delay) {
    uint32_t cp = HEB_KEY_CP[letter];
    char u8[4] = {(char) (0xC0 | cp >> 6), (char) (0x80 | (cp & 0x3F)), 0, 0};
    uintptr_t js = 0;
    if (!heb_jstr(&js, u8, 2))
        return 0;
    o_send((void *) (g_ctrl + HK_CTRL_SENDER), &js, delay);
    heb_jstr_free(&js);
    return 1;
}
static void hook_ctor(void *self, void *widget) {
    o_ctor(self, widget);
    if (g_ctrl && g_ctrl != (uintptr_t) self && hk_alive())
        return; /* a second search controller: left stock */
    g_ctrl = (uintptr_t) self;
    g_widget = (uintptr_t) widget;
    g_mode = HK_HEBREW;
    g_shift_shown = 0;
    if (!hk_alive()) {
        g_ctrl = g_widget = 0;
        return;
    }
    hk_sync_shift();
}
static void hook_dtor(void *self) {
    if ((uintptr_t) self == g_ctrl) {
        g_ctrl = g_widget = 0;
        g_mode = HK_HEBREW;
        g_shift_shown = 0;
    }
    o_dtor(self);
}
static void hook_restrict(void *self, int v) {
    o_restrict(self, v);
    if ((uintptr_t) self == g_ctrl)
        hk_sync_shift();
}
static void hook_paint(void *key, void *g, bool over, bool down) {
    int art = -1;
    char name[16];
    if (hk_active() && hk_is_search_key((uintptr_t) key) && hk_name((uintptr_t) key, name, sizeof name)) {
        if (g_mode == HK_HEBREW)
            art = !strcmp(name, "toUppercase") ? HK_ART_SHIFT : !strcmp(name, "toNumber") ? HK_ART_EN : hk_letter(name);
        else if (g_mode == HK_NUMBERS && (!strcmp(name, "toAlphabet") || !strcmp(name, "toLowercase")))
            art = HK_ART_HE;
    }
    int x = 0, y = 0;
    if (art >= 0 && rd_i32((uintptr_t) key + HK_KEY_IMG_X, &x) && rd_i32((uintptr_t) key + HK_KEY_IMG_Y, &y)) {
        int state = !((bool (*)(void *))(uintptr_t) HK_IS_ENABLED)(key) ? 2 : down ? 1 : 0;
        uintptr_t img = hk_image(art, state);
        if (img) {
            ((void (*)(void *, const uintptr_t *, int, int, bool))(uintptr_t) HK_DRAW_IMAGE_AT)(g, &img, x, y, false);
            return;
        }
    }
    o_paint(key, g, over, down);
}
static void hook_send(void *sender, const void *text, int delay) {
    uintptr_t p = 0;
    char k[4];
    int letter = -1;
    if (hk_active() && g_mode == HK_HEBREW && hk_alive() && (uintptr_t) sender == g_ctrl + HK_CTRL_SENDER &&
        rd_q((uintptr_t) text, &p) && rd_str(p, k, sizeof k))
        letter = hk_letter(k);
    if (letter >= 0 && hk_image(letter, 0) && hk_type(letter, delay))
        return;
    o_send(sender, text, delay);
}
static void hook_control(void *self, const void *name) {
    uintptr_t p = 0;
    char nm[16] = "";
    if (!hk_active() || (uintptr_t) self != g_ctrl || !hk_alive() || !rd_q((uintptr_t) name, &p) ||
        !rd_str(p, nm, sizeof nm)) {
        o_control(self, name);
        return;
    }
    if (g_mode == HK_HEBREW && !strcmp(nm, "toUppercase")) {
        if (hk_image(HK_ART_SHIFT, 0))
            hk_type(HK_ART_SHIFT, 0); /* final pe */
        return;
    }
    if (!strcmp(nm, "toNumber")) {
        if (g_mode == HK_HEBREW) {
            g_mode = HK_ENGLISH;
            hk_repaint();
            return;
        }
        g_mode = HK_NUMBERS;
    } else if (!strcmp(nm, "toAlphabet") || !strcmp(nm, "toLowercase"))
        g_mode = HK_HEBREW;
    o_control(self, name);
    hk_repaint();
}

/* ==== the mod ============================================================================ */
static const az_row_v1 ROWS[] = {
    {sizeof(az_row_v1), "hebrew", "Hebrew", AZ_ROW_TOGGLE, AZ_ROW_DEFAULT_ON, NULL,
     "Hebrew track, playlist and folder names on the deck screen.\nNames the deck already drew "
     "as boxes change after a Reload.",
     NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL},
    {sizeof(az_row_v1), "rtl", "Right to Left", AZ_ROW_TOGGLE, AZ_ROW_DEFAULT_ON, "hebrew",
     "Hebrew reads right to left, with English words and numbers\nin their own order. A long "
     "Hebrew name is cut on the left.",
     NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL},
    {sizeof(az_row_v1), "keyboard", "Hebrew Keyboard", AZ_ROW_TOGGLE, AZ_ROW_DEFAULT_ON, "hebrew",
     "The search keyboard opens in Hebrew. 123 switches it to\nEnglish, and ABC on the numbers "
     "page back to Hebrew.",
     NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL},
};

static int claim(uintptr_t at, void *fn, void *orig) {
    return code_is(at) && C->claim_hook(ID, at, fn, (void **) orig) == 0;
}

static int init(const az_core_v1 *core) {
    C = core;
    if (!AZ_HAS(core, register_row) || !AZ_HAS(core, row_value))
        return 2; /* the rows came with core 1.2.3 */
    g_row_hebrew = core->register_row(ID, &ROWS[0]);
    g_row_rtl = core->register_row(ID, &ROWS[1]);
    g_row_kbd = core->register_row(ID, &ROWS[2]);
    if (g_row_hebrew < 0)
        return 3;
    *(void **) &p_render_glyph = dlsym(RTLD_DEFAULT, "FT_Render_Glyph");
    *(void **) &p_naming = dlsym(RTLD_DEFAULT, "azmod_naming_playlist");
    /* hooks last: they cannot be undone while EP147 runs. Each part needs all of its hooks;
     * a part with a refused claim stays stock (its other hooks pass through). */
    int letters = claim(FT_PLT_LOAD_GLYPH, (void *) hook_load_glyph, &o_load_glyph) &&
                  claim(FT_PLT_KERNING, (void *) hook_kerning, &o_kerning) &&
                  claim(FT_PLT_CHAR_INDEX, (void *) hook_char_index, &o_char_index);
    __atomic_store_n(&g_letters_hooked, letters, __ATOMIC_RELEASE);
    int rtl = code_is(GLYPHPOS_FN) && claim(LINE_FN, (void *) hook_line, &o_line);
    int kbd = code_is(HK_DRAW_IMAGE_AT) && code_is(HK_IS_ENABLED) &&
              claim(HK_CTRL_CTOR, (void *) hook_ctor, &o_ctor) &&
              claim(HK_CTRL_DTOR, (void *) hook_dtor, &o_dtor) &&
              claim(HK_CTRL_RESTRICT, (void *) hook_restrict, &o_restrict) &&
              claim(HK_CTRL_CONTROL, (void *) hook_control, &o_control) &&
              claim(HK_KEY_PAINT, (void *) hook_paint, &o_paint) && claim(HK_SEND, (void *) hook_send, &o_send);
    __atomic_store_n(&g_kbd_hooked, kbd, __ATOMIC_RELEASE);
    core->log(ID, "letters %s, right to left %s, keyboard %s (naming check %s)", letters ? "on" : "OFF",
              rtl ? "on" : "OFF", kbd ? "on" : "OFF", p_naming ? "on" : "unavailable");
    return 0;
}

static const char *const FW_TAGS[] = {FW_200, NULL};
static const az_mod_v1 MOD = {AZ_MOD_ABI, sizeof(az_mod_v1), ID, FW_TAGS, init, NULL, NULL};
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) {
    return &MOD;
}
