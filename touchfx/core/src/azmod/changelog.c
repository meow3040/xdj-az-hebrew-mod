/* SPDX-License-Identifier: GPL-3.0-only */
/* changelog.c: the changelog box on the Update Mods screen: layout, drawing, scrolling.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* ---- changelog box (Updates screen, under the button) -----------------------
 * One scrolling area in the file's own order (the MODS section, then the APP
 * section). The wrap is computed once per (text generation, width); the view
 * scrolls by pixels, clipped to the box, with a thin position bar. */
#define C_CL_BG 0x001C1C1Cu  /* the box: a shade under the pane's scrim */
#define C_CL_NEW 0x0022E022u /* the VOCAL stem green, like "Up to date" */
#define C_CL_FIX 0x00FF8C00u /* the menu's orange */
#define C_CL_IMP 0x0040A8FFu
#define C_CL_CHG 0x00B0B0B0u
#define C_CL_DATE 0x00A8A8A8u /* the version's date: thin light-gray subtext */
#define CL_MAXL 3000
static void round_rect(const azmod_fb *fb, int x, int y, int w, int h, int r, int t, uint32_t fill,
                       uint32_t outline); /* below */
typedef struct {
    int off, len;
    char kind, mods;
} cl_line; /* kind: '#' section, 'v' version, '+!^~' entry, 'c' continuation, 'p' plain, ' ' gap;
              mods: inside the MODS section */
static struct {
    unsigned gen;
    int w, px, n, vis, lh;
    bool valid;
    cl_line l[CL_MAXL];
} g_cl;
static struct {
    bool on;
    int y0, s0;
} g_cld; /* a touch drag in progress */

static void changelog_rect(const azmod_fb *fb, int *x, int *y, int *w, int *h) {
    upd_geo g;
    upd_band(fb, &g);
    int rowh = menu_rowh_list(fb), pad = fb->w / 40;
    int cw = g_docked ? dock_w(fb) : menu_content_w(fb);
    int ph = g_docked ? dock_h(fb) : fb->h;
    *x = menu_bx(fb) + pad;
    *w = cw - 2 * pad;
    *y = g.py + g.ph + fb->h * 10 / 800; /* under the (reserved) progress row */
    *h = menu_by(fb) + ph - *y - rowh / 4;
    if (*h < 0)
        *h = 0;
}
/* changelog text size: the menu's own description lines (17/800, at least 10 px) */
static int cl_px(const azmod_fb *fb) {
    int px = fb->h * 17 / 800;
    return px < 10 ? 10 : px;
}
static int cl_lh(const azmod_fb *fb) {
    return cl_px(fb) * 3 / 2;
}
static int cl_textw(const char *s, int n, int px) {
    int w = azmod_ttf_menu_width ? azmod_ttf_menu_width(s, n, px, 2) : 0;
    return w > 0 ? w : n * px / 2;
}
static char g_cl_in_mods; /* cl_layout: the section being laid out is "MODS" */
static void cl_push(int off, int len, char kind) {
    if (g_cl.n < CL_MAXL) {
        g_cl.l[g_cl.n].off = off;
        g_cl.l[g_cl.n].len = len;
        g_cl.l[g_cl.n].kind = kind;
        g_cl.l[g_cl.n].mods = g_cl_in_mods;
        g_cl.n++;
    }
}
/* Greedy word wrap of t[off, off+len) into lines no wider than `width`. */
static void cl_wrap(const char *t, int off, int len, int width, int px, char kind) {
    int start = off, end = off + len;
    while (start < end) {
        while (start < end && t[start] == ' ')
            start++;
        if (start >= end)
            break;
        int best = -1, i = start;
        while (i < end) {
            int j = i;
            while (j < end && t[j] != ' ')
                j++;
            if (cl_textw(t + start, j - start, px) > width)
                break;
            best = j;
            i = j;
            while (i < end && t[i] == ' ')
                i++;
        }
        if (best < 0) { /* one word wider than the box: cut it */
            int k = start + 1;
            while (k < end && cl_textw(t + start, k - start + 1, px) <= width)
                k++;
            best = k;
        }
        cl_push(start, best - start, kind);
        kind = 'c';
        start = best;
    }
}
static void cl_layout(const azmod_state *s, int wtext, int wfull, int px) {
    const char *t = s->changelog;
    int n = s->changelog_len;
    g_cl.n = 0;
    g_cl_in_mods = 0;
    for (int i = 0; i < n;) {
        int e = i;
        while (e < n && t[e] != '\n')
            e++;
        int ln = e - i;
        if (ln > 0 && t[i + ln - 1] == '\r')
            ln--;
        const char *L = t + i;
        if (ln >= 3 && L[0] == '#' && L[1] == '#' && L[2] == ' ') {
            if (g_cl.n && g_cl.l[g_cl.n - 1].kind != '#')
                cl_push(i, 0, ' ');
            cl_push(i + 3, ln - 3, 'v');
        } else if (ln >= 2 && L[0] == '#' && L[1] == ' ') {
            g_cl_in_mods = (ln - 2 == 4 && !memcmp(L + 2, "MODS", 4));
            if (g_cl.n) {
                cl_push(i, 0, ' ');
                cl_push(i, 0, ' ');
            }
            cl_push(i + 2, ln - 2, '#');
        } else if (ln >= 2 && L[1] == ' ' &&
                   (L[0] == '+' || L[0] == '!' || L[0] == '^' || L[0] == '~')) {
            cl_wrap(t, i + 2, ln - 2, wtext, px, L[0]);
        } else if (ln > 0) {
            cl_wrap(t, i, ln, wfull, px, 'p');
        }
        i = e + 1;
    }
}
static void cl_text(const azmod_fb *fb, int x, int y, int h, const char *s, int px, int bold,
                    uint32_t c) {
    if (azmod_ttf_menu && azmod_ttf_menu(fb, x, y, 0, h, s, px, 0, bold, c))
        return;
    draw_text(fb, x, y + (h - 7) / 2, s, 1, c); /* no font: the 5x7 bitmap */
}
/* Scroll limits in pixels: g_cl.vis is the box's inner height in px and g_cl.lh
 * the line height, both refreshed by every draw. */
static int cl_maxpx(void) {
    int m = g_cl.valid ? g_cl.n * g_cl.lh + g_cl.px / 2 - g_cl.vis
                       : 0; /* + the top pad (px/4) and as much under the last line */
    return m < 0 ? 0 : m;
}
static void cl_clamp(azmod_state *s) {
    int m = cl_maxpx();
    if (s->changelog_scroll > m)
        s->changelog_scroll = m;
    if (s->changelog_scroll < 0)
        s->changelog_scroll = 0;
    if (s->changelog_pos > m)
        s->changelog_pos = m;
    if (s->changelog_pos < 0)
        s->changelog_pos = 0;
}
/* One easing step toward the target (called from azmod_tick every overlay tick):
 * a third of the remaining distance, at least 1 px, so a knob click glides two
 * lines in ~150 ms instead of jumping. Returns true while still moving. */
static bool cl_animate(azmod_state *s) {
    int d = s->changelog_scroll - s->changelog_pos;
    if (!d)
        return false;
    int step = d / 3;
    if (!step)
        step = d > 0 ? 1 : -1;
    s->changelog_pos += step;
    return true;
}
/* yyyy-mm-dd -> mm-dd-yyyy (US-style display); anything else is shown as it is. */
static void cl_date(const char *in, char *out, size_t n) {
    if (strlen(in) >= 10 && in[4] == '-' && in[7] == '-')
        snprintf(out, n, "%.2s-%.2s-%.4s", in + 5, in + 8, in);
    else
        snprintf(out, n, "%s", in);
}
static void draw_changelog(azmod_state *s, const azmod_fb *fb) {
    int x, y, w, h;
    changelog_rect(fb, &x, &y, &w, &h);
    int px = cl_px(fb), lh = cl_lh(fb), tagpx = px * 4 / 5;
    if (h < lh)
        return;
    int sbw = fb->w / 320 + 3, lx = x + px / 2;
    int bulw = px * 3 / 4; /* the bullet's column */
    int tagw = cl_textw("IMPROVED", 8, tagpx) + px;
    int wtext = w - (lx - x) - bulw - tagw - sbw - px / 2, wfull = w - (lx - x) - sbw - px / 2;
    fill_rect(fb, x, y, w, h, C_CL_BG);
    if (!g_cl.valid || g_cl.gen != s->changelog_gen || g_cl.w != w || g_cl.px != px) {
        cl_layout(s, wtext, wfull, px);
        g_cl.gen = s->changelog_gen;
        g_cl.w = w;
        g_cl.px = px;
        g_cl.valid = true;
    }
    g_cl.vis = h;
    g_cl.lh = lh;
    cl_clamp(s);
    if (g_cl.n == 0) {
        cl_text(fb, lx, y + px / 4, lh, "No changelog on this USB yet.", px, 2, C_TEXT_DIM);
        return;
    }
    /* A view of the framebuffer that is the box: every pixel write clips to a
     * framebuffer's own bounds, so lines half-scrolled out are cut at the box
     * edges for free, which is what lets the scroll move by pixels. */
    azmod_fb box = *fb;
    box.px = fb->px + (long) y * fb->stride + (long) x * 4;
    box.w = w - sbw - 2;
    box.h = h;
    int bx0 = lx - x, pad = px / 4;
    int first = (s->changelog_pos - pad) / lh;
    if (first < 0)
        first = 0;
    char buf[512];
    for (int li = first; li < g_cl.n; li++) {
        int ly = pad + li * lh - s->changelog_pos;
        if (ly >= h)
            break;
        const cl_line *L = &g_cl.l[li];
        int ln = L->len < (int) sizeof buf - 2 ? L->len : (int) sizeof buf - 2;
        memcpy(buf, s->changelog + L->off, (size_t) ln);
        buf[ln] = 0;
        switch (L->kind) {
        case '#': { /* section title, orange, as written: "MODS", "AZ-MODS Desktop" */
            cl_text(&box, bx0, ly, lh, buf, px, 1 | 2, C_HI);
            fill_rect(&box, bx0, ly + lh - 2, box.w - bx0 - px / 2, 1, C_CHECK_BOX);
            break;
        }
        case 'v': { /* "v1.0.16" bold white, then the date as thin light-gray subtext */
            char ver[128], date[64] = "", dd[64];
            const char *sp = strstr(buf, "  ");
            if (sp) {
                snprintf(ver, sizeof ver, "v%.*s", (int) (sp - buf), buf);
                while (*sp == ' ')
                    sp++;
                snprintf(date, sizeof date, "%s", sp);
            } else {
                snprintf(ver, sizeof ver, "v%s", buf);
            }
            cl_text(&box, bx0, ly, lh, ver, px, 1 | 2, C_TEXT);
            int tx = bx0 + cl_textw(ver, (int) strlen(ver), px) + px;
            if (date[0]) {
                cl_date(date, dd, sizeof dd);
                cl_text(&box, tx, ly, lh, dd, px * 4 / 5, 2, C_CL_DATE);
                tx += cl_textw(dd, (int) strlen(dd), px * 4 / 5) + px;
            }
            /* Tag the installed version and the newest one an update
             * would bring. MODS section only: the deck cannot know the desktop app's
             * version. ver+1 skips the "v". Unknown versions get no tag, never a guess. */
            if (L->mods) {
                const char *v = ver + 1, *tag = NULL;
                uint32_t tc = 0;
                if (s->mods_local[0] && !strcmp(v, s->mods_local)) {
                    tag = "INSTALLED";
                    tc = C_CL_NEW;
                } else if (s->mods_remote[0] && strcmp(s->mods_remote, s->mods_local) &&
                           !strcmp(v, s->mods_remote)) {
                    tag = "UPDATE AVAILABLE";
                    tc = C_CL_FIX;
                }
                if (tag)
                    cl_text(&box, tx, ly, lh, tag, tagpx, 1 | 2, tc);
            }
            break;
        }
        case '+':
        case '!':
        case '^':
        case '~': {
            const char *tag = L->kind == '+'   ? "NEW"
                              : L->kind == '!' ? "FIXED"
                              : L->kind == '^' ? "IMPROVED"
                                               : "CHANGED";
            uint32_t tc = L->kind == '+'   ? C_CL_NEW
                          : L->kind == '!' ? C_CL_FIX
                          : L->kind == '^' ? C_CL_IMP
                                           : C_CL_CHG;
            /* TAG | bullet | text (the bullet belongs to the text, not the
             * tag); wrapped lines align with the text, after the bullet */
            cl_text(&box, bx0, ly, lh, tag, tagpx, 1 | 2, tc);
            {
                int d = px / 3 < 4 ? 4 : px / 3; /* a round bullet, drawn (the font is 8-bit) */
                round_rect(&box, bx0 + tagw, ly + (lh - d) / 2, d, d, d / 2, 0, C_CL_DATE,
                           C_CL_DATE);
            }
            cl_text(&box, bx0 + tagw + bulw, ly, lh, buf, px, 2, C_TEXT);
            break;
        }
        case 'c':
            cl_text(&box, bx0 + tagw + bulw, ly, lh, buf, px, 2, C_TEXT);
            break;
        case 'p':
            cl_text(&box, bx0, ly, lh, buf, px, 2, C_TEXT_DIM);
            break;
        default:
            break;
        }
    }
    int total = g_cl.n * lh + pad;
    if (total > h) { /* position bar */
        int tx = x + w - sbw - 2, th = h * h / total;
        if (th < lh)
            th = lh;
        int maxs = cl_maxpx();
        int ty = y + (h - th) * s->changelog_pos / (maxs > 0 ? maxs : 1);
        fill_rect(fb, tx, y, sbw, h, C_ROW_BG);
        fill_rect(fb, tx, ty, sbw, th, C_CHECK_BOX);
    }
}
static bool changelog_hit(const azmod_fb *fb, int tx, int ty) {
    int x, y, w, h;
    changelog_rect(fb, &x, &y, &w, &h);
    return in_rect(tx, ty, x, y, w, h);
}
bool azmod_changelog_drag(azmod_state *s, const azmod_fb *fb, int y, int phase) {
    if (s->mode != AZ_UPDATE) {
        g_cld.on = false;
        return false;
    }
    if (phase == 0) {
        int bx, by, bw, bh;
        changelog_rect(fb, &bx, &by, &bw, &bh);
        if (y < by || y >= by + bh)
            return false;
        g_cld.on = true;
        g_cld.y0 = y;
        g_cld.s0 = s->changelog_scroll;
        return true;
    }
    if (!g_cld.on)
        return false;
    if (phase == 1) { /* whole-line steps under the finger (a redraw only per line) */
        int lh = cl_lh(fb);
        int ns = g_cld.s0 + (g_cld.y0 - y) / lh * lh;
        if (ns == s->changelog_scroll)
            return false;
        s->changelog_scroll = ns;
        cl_clamp(s);
        s->changelog_pos = s->changelog_scroll;
        return true;
    }
    g_cld.on = false;
    return true;
}
