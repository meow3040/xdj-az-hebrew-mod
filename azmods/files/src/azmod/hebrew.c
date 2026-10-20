/* SPDX-License-Identifier: GPL-3.0-only */
/* hebrew.c: the Hebrew mod (Mod Menu > Hebrew), in three parts.
 *
 * Letters (the Hebrew row). EP147's faces (UCGothicLatin, UCGothic_J, TsukuGo) have no Hebrew,
 * so a Hebrew name draws as boxes. JUCE loads a character the first time it draws it, through
 * FreeType, at EP147's one call site (JUCE's FreeTypeTypeface::loadGlyphIfPossible, 2.00
 * 0x25620d8): FT_Get_Char_Index, then FT_Load_Glyph(FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP |
 * FT_LOAD_IGNORE_TRANSFORM | FT_LOAD_NO_HINTING), then it reads face->glyph's outline and
 * horiAdvance, and FT_Get_Kerning for the pairs. azmod interposes those three: a Hebrew
 * character the face has no glyph for gets the index HEB_GI_BASE + its code point (past any
 * face's own glyphs), and loading that index fills the face's glyph slot with the character's
 * outline from heb_glyphs.h (DejaVu Sans Bold), scaled to the face's units per em. The face
 * files are not touched. Build-independent: FreeType is EP147's dynamic library.
 *
 * Right to Left (2.00). JUCE lays every line out left to right in
 * GlyphArrangement::addCurtailedLineOfText (drawText, drawSingleLineText and fitted text all go
 * through it). The hook hands it a line holding right-to-left text in visual order
 * (bidi_logic.h) as a temporary juce::String; the caller's string is not changed. A
 * right-to-left line too wide for its box is cut on the left, with JUCE's "...", so the
 * beginning of the name stays in view (JUCE cuts the right end, which in visual order is where
 * a Hebrew name begins). Multi-line strings and malformed UTF-8 are left as they are.
 *
 * Hebrew Keyboard (2.00). The search keyboard (gui::SearchSoftwareKeyboardWidgetController)
 * opens on a Hebrew page: its letter keys draw Hebrew key art (heb_keys.h) and type Hebrew, the
 * hidden shift key types final pe, 123 turns the page to English (EN) and the numbers page's ABC
 * key turns it back to Hebrew (HE). While Playlist Editing names a playlist on this keyboard
 * (NEW PLAYLIST, RENAME), the keyboard stays stock: azmod's naming bar draws Latin only.
 *
 * The hooks go in at EP147 start, only when the Hebrew row is on then: switching it on takes
 * one Reload (the menu says so), and a deck with Hebrew off runs none of this. Switched off
 * while running, every part passes through at once, except letters JUCE has already loaded:
 * JUCE keeps every glyph it loads until the next Reload.
 * Log (with azmod-logs.on): /tmp/azmod-hebrew.log. */
#include <ft2build.h>
#include FT_FREETYPE_H
#include "bidi_logic.h"
#include "heb_glyphs.h"
#include "heb_keys.h"

static void heb_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-hebrew.log", "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ==== letters: FreeType ================================================================= */
#define HEB_GI_BASE 0x7FFF0000u /* + code point: a Hebrew glyph the face lacks */

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
/* a face the donor's outlines can go into: scalable, Unicode charmap, a glyph slot */
static int heb_face_ok(FT_Face face) {
    return face && FT_IS_SCALABLE(face) && face->units_per_EM > 0 && face->glyph &&
           face->charmap && face->charmap->encoding == FT_ENCODING_UNICODE;
}
static int g_heb_started; /* the Hebrew row was on at EP147 start: the mod is in */
static int heb_letters_on(void) {
    return g_heb_started && !g_azm_inert && az_on(AZF_HEBREW);
}

static FT_UInt (*next_ft_char_index)(FT_Face, FT_ULong);
static FT_Error (*next_ft_load_glyph)(FT_Face, FT_UInt, FT_Int32);
static FT_Error (*next_ft_kerning)(FT_Face, FT_UInt, FT_UInt, FT_UInt, FT_Vector *);

__attribute__((visibility("default"))) FT_UInt FT_Get_Char_Index(FT_Face face, FT_ULong charcode) {
    NEXT(next_ft_char_index, "FT_Get_Char_Index");
    if (!next_ft_char_index)
        return 0;
    FT_UInt gi = next_ft_char_index(face, charcode);
    if (gi || !heb_letters_on() || !heb_glyph_of(charcode) || !heb_face_ok(face))
        return gi;
    return HEB_GI_BASE + (FT_UInt) charcode;
}

/* The outline handed to JUCE: per thread (JUCE reads it right after the load, on the loading
 * thread), grown to the largest glyph loaded. */
static __thread struct {
    FT_Vector *pts;
    char *tags;
    short *ends;
    int cap;
} t_heb_ol;

static FT_Pos heb_scale(long v, long num, long den) { /* v * num / den, rounded */
    long long p = (long long) v * num;
    return (FT_Pos) (p >= 0 ? (p + den / 2) / den : -((-p + den / 2) / den));
}
/* The slot after FreeType loaded glyph 0 into it: its outline and metrics become g's, in font
 * units (FT_LOAD_NO_SCALE) or 26.6 pixels at the face's size. 0 = out of memory. */
static int heb_fill_slot(FT_Face face, const struct heb_glyph *g, FT_Int32 flags) {
    FT_GlyphSlot s = face->glyph;
    if (!t_heb_ol.cap || g->npts > t_heb_ol.cap) { /* a mark (no points) still gets arrays */
        int cap = g->npts < 256 ? 256 : g->npts;
        FT_Vector *p = realloc(t_heb_ol.pts, (size_t) cap * sizeof *p);
        if (!p)
            return 0;
        t_heb_ol.pts = p;
        char *t = realloc(t_heb_ol.tags, (size_t) cap);
        if (!t)
            return 0;
        t_heb_ol.tags = t;
        short *e = realloc(t_heb_ol.ends, (size_t) cap * sizeof *e);
        if (!e)
            return 0;
        t_heb_ol.ends = e;
        t_heb_ol.cap = cap;
    }
    long num = face->units_per_EM, den = HEB_DONOR_UPM;
    int unscaled = (flags & FT_LOAD_NO_SCALE) || !face->size;
    long sx = unscaled ? 0x10000 : face->size->metrics.x_scale;
    long sy = unscaled ? 0x10000 : face->size->metrics.y_scale;
    /* font units of the face, then (scaled) 16.16 -> 26.6 pixels */
#define HEB_X(v) (unscaled ? heb_scale((v), num, den) : heb_scale(heb_scale((v), num, den), sx, 0x10000))
#define HEB_Y(v) (unscaled ? heb_scale((v), num, den) : heb_scale(heb_scale((v), num, den), sy, 0x10000))
    for (int i = 0; i < g->npts; i++) {
        t_heb_ol.pts[i].x = HEB_X(HEB_PTS[g->pt0 + i][0]);
        t_heb_ol.pts[i].y = HEB_Y(HEB_PTS[g->pt0 + i][1]);
        t_heb_ol.tags[i] = (char) (HEB_TAGS[g->pt0 + i] ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CONIC);
    }
    for (int c = 0; c < g->ncont; c++)
        t_heb_ol.ends[c] = (short) HEB_ENDS[g->ct0 + c];
    s->format = FT_GLYPH_FORMAT_OUTLINE;
    s->outline.n_points = (short) g->npts;
    s->outline.n_contours = (short) g->ncont;
    s->outline.points = t_heb_ol.pts;
    s->outline.tags = t_heb_ol.tags;
    s->outline.contours = t_heb_ol.ends;
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

__attribute__((visibility("default"))) FT_Error FT_Load_Glyph(FT_Face face, FT_UInt glyph_index,
                                                              FT_Int32 load_flags) {
    NEXT(next_ft_load_glyph, "FT_Load_Glyph");
    if (!next_ft_load_glyph)
        return FT_Err_Invalid_Argument;
    const struct heb_glyph *g = glyph_index >= HEB_GI_BASE ? heb_glyph_of(glyph_index - HEB_GI_BASE) : 0;
    /* an index we handed out (no face has that many glyphs): ours, whatever the row says now */
    if (!g || !heb_face_ok(face) || (FT_Long) glyph_index < face->num_glyphs)
        return next_ft_load_glyph(face, glyph_index, load_flags);
    FT_Error err = next_ft_load_glyph(face, 0, load_flags & ~FT_LOAD_RENDER); /* the slot, set up */
    if (err)
        return err;
    if (!heb_fill_slot(face, g, load_flags))
        return FT_Err_Out_Of_Memory;
    if (load_flags & FT_LOAD_RENDER)
        return FT_Render_Glyph(face->glyph, FT_LOAD_TARGET_MODE(load_flags));
    return 0;
}

__attribute__((visibility("default"))) FT_Error FT_Get_Kerning(FT_Face face, FT_UInt left,
                                                               FT_UInt right, FT_UInt kern_mode,
                                                               FT_Vector *akerning) {
    NEXT(next_ft_kerning, "FT_Get_Kerning");
    if (left >= HEB_GI_BASE || right >= HEB_GI_BASE || !next_ft_kerning) {
        if (akerning)
            akerning->x = akerning->y = 0; /* the donor's letters carry no pairs with the face's */
        return next_ft_kerning ? 0 : FT_Err_Invalid_Argument;
    }
    return next_ft_kerning(face, left, right, kern_mode, akerning);
}

/* ==== juce::String holding UTF-8 ========================================================= */
/* EP147's juce::String(const char *) (JSTR_FROM_CSTR) reads its argument as ASCII: a byte above
 * 0x7F becomes a character of its own. So it is asked for a string of as many spaces, which it
 * allocates and owns, and the UTF-8 bytes are written over them, once the result is proven to
 * be a fresh copy of those spaces (never JUCE's shared empty string). 0 = not made. */
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

/* ==== right to left: GlyphArrangement::addCurtailedLineOfText (2.00) ==================== */
#define HEB_LINE_FN ((uintptr_t) ep_off(0, 0x2554ee0))     /* juce::GlyphArrangement::addCurtailedLineOfText */
#define HEB_GLYPHPOS_FN ((uintptr_t) ep_off(0, 0x25548b0)) /* juce::Font::getGlyphPositions */
#define HEB_MAX_BYTES 16384
#define HEB_STACK_CPS 256 /* lines up to this many characters are reordered on the stack */
/* juce::Array<T> on 2.00: the elements (HeapBlock, freed with free()), numAllocated, numUsed
 * (addCurtailedLineOfText zeroes +0, +8 and +0x10 of each before getGlyphPositions) */
struct heb_juce_array {
    void *data;
    int allocated, pad0;
    int used, pad1;
};
typedef void (*heb_line_fn)(void *, const void *, const void *, float, float, float, bool);
static heb_line_fn g_heb_orig_line;
static int g_heb_rtl_hooked;
static __thread int t_heb_nest; /* inside our own call of the original: pass through */
static _Atomic unsigned g_heb_lines;

/* a byte that starts a right-to-left character: U+0580-U+07FF (Hebrew, Arabic, Syriac, Thaana,
 * NKo) or U+FB1D-U+FEFF (the Hebrew and Arabic presentation forms) */
static int heb_has_rtl(const char *s) {
    for (const unsigned char *p = (const unsigned char *) s; *p; p++)
        if ((*p >= 0xD6 && *p <= 0xDF) || (*p == 0xEF && p[1] >= 0xAC && p[1] <= 0xBB))
            return 1;
    return 0;
}
/* Strict UTF-8 -> code points; -1 for malformed text, a line break, or more than BIDI_MAX
 * characters. With out == NULL, only counts. */
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
/* A right-to-left line wider than w keeps its visual right end, where it begins, and is cut
 * on the left: the first visual index kept, with *dots = draw "..." first (JUCE's own rule:
 * useEllipsis and more than 3 characters). JUCE's widths, through Font::getGlyphPositions over
 * "..." + the line (u8, which starts with those dots). 0 and *dots = 0: it fits, or it could
 * not be measured. */
static int heb_fit_rtl(const void *font, const char *u8, size_t nbytes, int m, float w, bool ell,
                       int *dots) {
    *dots = 0;
    uintptr_t js = 0;
    if (!heb_jstr(&js, u8, nbytes))
        return 0;
    struct heb_juce_array glyphs = {0}, xs = {0};
    ((void (*)(const void *, const uintptr_t *, struct heb_juce_array *,
               struct heb_juce_array *)) HEB_GLYPHPOS_FN)(font, &js, &glyphs, &xs);
    ((void (*)(uintptr_t *)) JSTR_DTOR)(&js);
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
/* The line, reordered: a temporary juce::String in place of the caller's. Anything that
 * cannot be done leaves the stock call. */
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
        g_heb_orig_line(ga, font, text, x, y, w, ell);
        return;
    }
    if (n > HEB_STACK_CPS) { /* one block: 4+4+4 + 4*2 (pairs) + 1+1+1 + 4 (UTF-8) per character */
        heap = malloc((size_t) n * 27 + 64);
        if (!heap) {
            g_heb_orig_line(ga, font, text, x, y, w, ell);
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
        u8 = b; /* 3 + 4n + 1 */
    }
    heb_utf8_decode(s, len, in);
    struct bidi_scratch ws = {orig, type, kept, pairs};
    int rtl = 0, m = bidi_visual(in, n, 2, out, &rtl, lv, &ws);
    if (m <= 0 || (m == n && !memcmp(in, out, (size_t) n * sizeof *in))) {
        free(heap);
        g_heb_orig_line(ga, font, text, x, y, w, ell);
        return;
    }
    memcpy(u8, "...", 3);
    size_t nb = heb_utf8_encode(out, m, u8 + 3);
    const char *draw = u8 + 3;
    size_t dn = nb;
    float dw = w;
    bool dell = ell;
    if (rtl && w < 1.0e9f) { /* a box to fit (addLineOfText passes 1e10: no limit) */
        int dots = 0, from = heb_fit_rtl(font, u8, nb + 3, m, w, ell, &dots);
        if (from > 0 || dots) {
            size_t off = (size_t) heb_utf8_encode(out, from, u8 + 3); /* the bytes cut */
            heb_utf8_encode(out + from, m - from, u8 + 3 + off);
            if (dots) {
                memmove(u8 + 3, u8 + 3 + off, nb - off + 1);
                draw = u8, dn = 3 + nb - off;
            } else
                draw = u8 + 3 + off, dn = nb - off;
            dw = w + 4.0f; /* measured to fit: JUCE must not cut its first letter on rounding */
            dell = false;
        }
    }
    uintptr_t js = 0;
    if (!heb_jstr(&js, draw, dn)) {
        free(heap);
        g_heb_orig_line(ga, font, text, x, y, w, ell);
        return;
    }
    free(heap);
    g_heb_orig_line(ga, font, &js, x, y, dw, dell);
    ((void (*)(uintptr_t *)) JSTR_DTOR)(&js);
    if (__atomic_fetch_add(&g_heb_lines, 1, __ATOMIC_RELAXED) < 3)
        heb_log("[%ld] rtl: a line reordered (%d characters, %s)\n", (long) time(NULL), n,
                rtl ? "right to left" : "left to right");
}
static void heb_line_hook(void *ga, const void *font, const void *text, float x, float y,
                          float w, bool ell) {
    uintptr_t sp = 0;
    if (t_heb_nest || !az_value(AZF_HEBREW_RTL) || !own_q((uintptr_t) text, &sp) || sp < 0x10000 ||
        !heb_has_rtl((const char *) sp)) {
        g_heb_orig_line(ga, font, text, x, y, w, ell);
        return;
    }
    t_heb_nest++; /* getGlyphPositions and the original below never come back here */
    heb_line_rtl(ga, font, text, (const char *) sp, x, y, w, ell);
    t_heb_nest--;
}

/* ==== the Hebrew keyboard (2.00) ========================================================= */
#define HK_CTRL_CTOR ((uintptr_t) ep_off(0, 0x1dc17d8))     /* SearchSoftwareKeyboardWidgetController(this, widget) */
#define HK_CTRL_DTOR ((uintptr_t) ep_off(0, 0x1dc6428))     /* ~SearchSoftwareKeyboardWidgetController */
#define HK_CTRL_RESTRICT ((uintptr_t) ep_off(0, 0x1dc1880)) /* (this, int): stock hides the shift key here */
#define HK_CTRL_CONTROL ((uintptr_t) ep_off(0, 0x1dc6e58))  /* (this, const String &name): a named key */
#define HK_KEY_PAINT ((uintptr_t) ep_off(0, 0x19cc308))     /* image key paint(this, Graphics &, over, down) */
#define HK_SEND ((uintptr_t) ep_off(0, 0x1dc71d0))          /* EventSender::send(this, const String &, int) */
#define HK_DRAW_IMAGE_AT ((uintptr_t) ep_off(0, 0x251da80)) /* juce::Graphics::drawImageAt(Image &, x, y, bool) */
#define HK_IS_ENABLED ((uintptr_t) ep_off(0, 0x25ab8c0))    /* juce::Component::isEnabled() */
#define HK_CTRL_VT ((uintptr_t) ep_off(0, 0x2fd4318))       /* gui::SearchSoftwareKeyboardWidgetController */
#define HK_GROUP_VT ((uintptr_t) ep_off(0, 0x2f797a8))      /* gui::LowercaseKeyWidget */
#define HK_CTRL_WIDGET 0x28   /* controller: its SoftwareKeyboardWidget (SKW_VT) */
#define HK_CTRL_SENDER 0x50   /* controller: the EventSender the keys type through */
#define HK_WIDGET_GROUP 0x188 /* widget: the lowercase group; group +0x188: its shift key */
#define HK_KEY_NAME 0x08      /* juce::Component::componentName */
#define HK_KEY_IMG_X 0x1cc    /* image key: where its image is drawn */
#define HK_KEY_IMG_Y 0x1d0
#define HK_VT_SETVISIBLE 0x60 /* juce::Component::setVisible(bool) */
/* the widget's four layouts (juce::Array<Key *>), read by EP147 at 0x1dc2a88 */
static const unsigned HK_LAYOUTS[4] = {0x128, 0x140, 0x158, 0x170};
enum {
    HK_HEBREW,
    HK_ENGLISH,
    HK_NUMBERS
};
#define HK_ART_EN 27 /* on the Hebrew page's 123 key */
#define HK_ART_HE 28 /* on the numbers page's ABC key */
#define HK_ART_SHIFT 26

static void (*g_hk_orig_ctor)(void *, void *);
static void (*g_hk_orig_dtor)(void *);
static void (*g_hk_orig_restrict)(void *, int);
static void (*g_hk_orig_control)(void *, const void *);
static void (*g_hk_orig_paint)(void *, void *, bool, bool);
static void (*g_hk_orig_send)(void *, const void *, int);
static int g_heb_kbd_hooked;
/* message thread only (every hook below runs there) */
static uintptr_t g_hk_ctrl, g_hk_widget;
static int g_hk_mode, g_hk_shift_shown;
static uintptr_t g_hk_img[HEB_KEYS_N * 3]; /* decoded key art: juce::Image pixel-data pointers */

static int hk_active(void) {
    return g_heb_kbd_hooked && az_value(AZF_HEBREW_KEYBOARD) && !nm_naming();
}
static int hk_i32(uintptr_t a, int *out) { /* an int of a live object, read as its aligned word */
    uintptr_t q = 0;
    if (!own_q(a & ~(uintptr_t) 7, &q))
        return 0;
    *out = (int) (uint32_t) (q >> ((a & 4) ? 32 : 0));
    return 1;
}
/* the captured search controller and its widget, still what they were */
static int hk_alive(void) {
    uintptr_t vt = 0, w = 0, wvt = 0;
    return g_hk_ctrl && g_hk_widget && own_q(g_hk_ctrl, &vt) && vt == HK_CTRL_VT &&
           own_q(g_hk_ctrl + HK_CTRL_WIDGET, &w) && w == g_hk_widget && own_q(w, &wvt) &&
           wvt == SKW_VT;
}
static int hk_is_search_key(uintptr_t key) {
    if (!key || !hk_alive())
        return 0;
    for (int j = 0; j < 4; j++) {
        uintptr_t v = 0;
        int n = 0;
        if (!own_q(g_hk_widget + HK_LAYOUTS[j], &v) || !hk_i32(g_hk_widget + HK_LAYOUTS[j] + 16, &n) ||
            n < 0 || n > 96 || (n && !v))
            return 0;
        for (int i = 0; i < n; i++) {
            uintptr_t k = 0;
            if (own_q(v + 8 * (uintptr_t) i, &k) && k == key)
                return 1;
        }
    }
    return 0;
}
static int hk_name(uintptr_t comp, char *buf, int n) {
    uintptr_t p = 0;
    return own_q(comp + HK_KEY_NAME, &p) && own_str(p, buf, n);
}
/* the Hebrew letter a stock key name types on the Hebrew page, -1 for none */
static int hk_letter(const char *name) {
    const char *q = name[0] && !name[1] ? strchr(HEB_KEY_QWERTY, name[0]) : 0;
    return q ? (int) (q - HEB_KEY_QWERTY) : -1;
}
static uintptr_t hk_image(int art, int state) { /* decoded once, held for the process */
    int i = art * 3 + state;
    if (i < 0 || i >= HEB_KEYS_N * 3)
        return 0;
    if (!g_hk_img[i])
        mp_image_from_memory(&g_hk_img[i], HEB_KEY_PNG[i].png, HEB_KEY_PNG[i].len);
    return g_hk_img[i];
}
/* the stock shift key, hidden on the search keyboard, is shown on the Hebrew page (final pe) */
static void hk_sync_shift(void) {
    if (!hk_alive())
        return;
    uintptr_t group = 0, gvt = 0, key = 0, kvt = 0, setvis = 0;
    char name[16];
    if (!own_q(g_hk_widget + HK_WIDGET_GROUP, &group) || !own_q(group, &gvt) || gvt != HK_GROUP_VT ||
        !own_q(group + HK_WIDGET_GROUP, &key) || !hk_is_search_key(key) ||
        !hk_name(key, name, sizeof name) || strcmp(name, "toUppercase") || !own_q(key, &kvt) ||
        !img_q(kvt + HK_VT_SETVISIBLE, &setvis))
        return;
    int want = hk_active() && g_hk_mode == HK_HEBREW;
    if (want || g_hk_shift_shown) {
        ((void (*)(uintptr_t, bool)) setvis)(key, want);
        g_hk_shift_shown = want;
    }
}
static void hk_repaint(void) {
    if (!hk_alive())
        return;
    hk_sync_shift();
    ((void (*)(uintptr_t)) JUCE_COMPONENT_REPAINT)(g_hk_widget);
}
/* type `cp` through the controller's sender: 1 = sent */
static int hk_type(int letter, int delay) {
    uint32_t cp = HEB_KEY_CP[letter];
    char u8[4] = {(char) (0xC0 | cp >> 6), (char) (0x80 | (cp & 0x3F)), 0, 0};
    uintptr_t js = 0;
    if (!heb_jstr(&js, u8, 2))
        return 0;
    g_hk_orig_send((void *) (g_hk_ctrl + HK_CTRL_SENDER), &js, delay);
    ((void (*)(uintptr_t *)) JSTR_DTOR)(&js);
    return 1;
}

static void hk_ctor_hook(void *self, void *widget) {
    g_hk_orig_ctor(self, widget);
    if (g_hk_ctrl && g_hk_ctrl != (uintptr_t) self && hk_alive()) {
        heb_log("[%ld] keyboard: a second search controller, left stock\n", (long) time(NULL));
        return;
    }
    g_hk_ctrl = (uintptr_t) self;
    g_hk_widget = (uintptr_t) widget;
    g_hk_mode = HK_HEBREW;
    g_hk_shift_shown = 0;
    if (!hk_alive()) {
        g_hk_ctrl = g_hk_widget = 0;
        heb_log("[%ld] keyboard: unexpected search controller layout, left stock\n", (long) time(NULL));
        return;
    }
    hk_sync_shift();
    heb_log("[%ld] keyboard: search keyboard found, Hebrew page first\n", (long) time(NULL));
}
static void hk_dtor_hook(void *self) {
    if ((uintptr_t) self == g_hk_ctrl) {
        g_hk_ctrl = g_hk_widget = 0;
        g_hk_mode = HK_HEBREW;
        g_hk_shift_shown = 0;
    }
    g_hk_orig_dtor(self);
}
static void hk_restrict_hook(void *self, int v) {
    g_hk_orig_restrict(self, v);
    if ((uintptr_t) self == g_hk_ctrl)
        hk_sync_shift(); /* after stock hid the shift key */
}
static void hk_paint_hook(void *key, void *g, bool over, bool down) {
    int art = -1;
    char name[16];
    if (hk_active() && hk_is_search_key((uintptr_t) key) && hk_name((uintptr_t) key, name, sizeof name)) {
        if (g_hk_mode == HK_HEBREW)
            art = !strcmp(name, "toUppercase") ? HK_ART_SHIFT : !strcmp(name, "toNumber") ? HK_ART_EN
                                                                                          : hk_letter(name);
        else if (g_hk_mode == HK_NUMBERS && (!strcmp(name, "toAlphabet") || !strcmp(name, "toLowercase")))
            art = HK_ART_HE;
    }
    int x = 0, y = 0;
    if (art >= 0 && hk_i32((uintptr_t) key + HK_KEY_IMG_X, &x) && hk_i32((uintptr_t) key + HK_KEY_IMG_Y, &y)) {
        int state = !((bool (*)(void *)) HK_IS_ENABLED)(key) ? 2 : down ? 1
                                                                        : 0;
        uintptr_t img = hk_image(art, state);
        if (img) {
            ((void (*)(void *, const uintptr_t *, int, int, bool)) HK_DRAW_IMAGE_AT)(g, &img, x, y, false);
            return;
        }
    }
    g_hk_orig_paint(key, g, over, down);
}
static void hk_send_hook(void *sender, const void *text, int delay) {
    uintptr_t p = 0;
    char k[4];
    int letter = -1;
    if (hk_active() && g_hk_mode == HK_HEBREW && hk_alive() &&
        (uintptr_t) sender == g_hk_ctrl + HK_CTRL_SENDER && own_q((uintptr_t) text, &p) &&
        own_str(p, k, sizeof k))
        letter = hk_letter(k);
    /* Hebrew only once its key art is up: a key that shows a Latin letter types it */
    if (letter >= 0 && hk_image(letter, 0) && hk_type(letter, delay))
        return;
    g_hk_orig_send(sender, text, delay);
}
static void hk_control_hook(void *self, const void *name) {
    uintptr_t p = 0;
    char nm[16] = "";
    if (!hk_active() || (uintptr_t) self != g_hk_ctrl || !hk_alive() || !own_q((uintptr_t) name, &p) ||
        !own_str(p, nm, sizeof nm)) {
        g_hk_orig_control(self, name);
        return;
    }
    if (g_hk_mode == HK_HEBREW && !strcmp(nm, "toUppercase")) {
        if (hk_image(HK_ART_SHIFT, 0))
            hk_type(HK_ART_SHIFT, 0); /* final pe; the stock shift (uppercase) stays unused */
        return;
    }
    if (!strcmp(nm, "toNumber")) {
        if (g_hk_mode == HK_HEBREW) { /* 123 on the Hebrew page: the same page, in English */
            g_hk_mode = HK_ENGLISH;
            hk_repaint();
            return;
        }
        g_hk_mode = HK_NUMBERS;
    } else if (!strcmp(nm, "toAlphabet") || !strcmp(nm, "toLowercase"))
        g_hk_mode = HK_HEBREW;
    g_hk_orig_control(self, name);
    hk_repaint();
}

/* ==== install (constructor, install_static_hooks) ======================================== */
/* the first instructions at an address, as the hooks expect them (2.00) */
static int heb_code_is(uintptr_t a, uint64_t first8) {
    uintptr_t q = 0;
    return a && img_q(a, &q) && q == first8;
}
static void install_hebrew(void) {
    g_heb_started = az_on(AZF_HEBREW);
    if (!g_heb_started)
        return; /* off at start: nothing of the mod runs until a Reload with it on */
    heb_log("[%ld] Hebrew: letters on (%d glyphs from the donor)\n", (long) time(NULL),
            (int) (sizeof HEB_GLYPHS / sizeof HEB_GLYPHS[0]));
    if (g_ep147_build != EPB_V200) {
        heb_log("[%ld] Hebrew: right to left and the keyboard need EP147 2.00\n", (long) time(NULL));
        return;
    }
    /* right to left: the hook, and the measure it calls */
    int r = -9;
    if (heb_code_is(HEB_LINE_FN, 0x12001c63a9b27bfdULL) &&
        heb_code_is(HEB_GLYPHPOS_FN, 0x910003fda9bd7bfdULL))
        r = azm_install_hook(HEB_LINE_FN, (void *) heb_line_hook, (void **) &g_heb_orig_line);
    g_heb_rtl_hooked = r == 1;
    /* the keyboard: all six or the keyboard stays stock */
    int k[6] = {-9, -9, -9, -9, -9, -9};
    if (heb_code_is(HK_CTRL_CTOR, 0x910003fda9be7bfdULL) &&
        heb_code_is(HK_CTRL_DTOR, 0x9101c001a9be7bfdULL) &&
        heb_code_is(HK_CTRL_RESTRICT, 0x910003fda9bc7bfdULL) &&
        heb_code_is(HK_CTRL_CONTROL, 0x910003fda9be7bfdULL) &&
        heb_code_is(HK_KEY_PAINT, 0x910003fda9bc7bfdULL) &&
        heb_code_is(HK_SEND, 0xd0ffffe3a9ba7bfdULL) &&
        heb_code_is(HK_DRAW_IMAGE_AT, 0x1e2200601e220041ULL) &&
        heb_code_is(HK_IS_ENABLED, 0x3720004139430401ULL)) {
        k[0] = azm_install_hook(HK_CTRL_CTOR, (void *) hk_ctor_hook, (void **) &g_hk_orig_ctor);
        k[1] = azm_install_hook(HK_CTRL_DTOR, (void *) hk_dtor_hook, (void **) &g_hk_orig_dtor);
        k[2] = azm_install_hook(HK_CTRL_RESTRICT, (void *) hk_restrict_hook, (void **) &g_hk_orig_restrict);
        k[3] = azm_install_hook(HK_CTRL_CONTROL, (void *) hk_control_hook, (void **) &g_hk_orig_control);
        k[4] = azm_install_hook(HK_KEY_PAINT, (void *) hk_paint_hook, (void **) &g_hk_orig_paint);
        k[5] = azm_install_hook(HK_SEND, (void *) hk_send_hook, (void **) &g_hk_orig_send);
    }
    g_heb_kbd_hooked = k[0] == 1 && k[1] == 1 && k[2] == 1 && k[3] == 1 && k[4] == 1 && k[5] == 1;
    heb_log("[%ld] Hebrew: right to left %d, keyboard %d/%d/%d/%d/%d/%d\n", (long) time(NULL), r,
            k[0], k[1], k[2], k[3], k[4], k[5]);
}
