/* SPDX-License-Identifier: GPL-3.0-only */
/* ov_clip.h: the part of a window's rectangle that lies in the overlay's buffer.
 *
 * Each overlay window shows its own rectangle of the full-screen buffer, and a window that is not
 * in use is parked off screen (wanted_rect), so its rectangle can lie partly or wholly outside the
 * buffer. XPutImage clips such a request to the image itself; XShmPutImage does not, and the
 * server refuses it (BadValue). ov_clip is XPutImage's clipping: the source rectangle cut to the
 * buffer, the destination moved by what was cut from the left and the top. */
#ifndef AZMOD_OV_CLIP_H
#define AZMOD_OV_CLIP_H

struct ov_part {
    int sx, sy; /* in the buffer */
    int dx, dy; /* in the window */
    int w, h;
};
/* the window rectangle (x, y, w, h) of a W x H buffer; 0 = nothing of it is in the buffer */
static int ov_clip(int x, int y, int w, int h, int W, int H, struct ov_part *p) {
    if (w <= 0 || h <= 0 || W <= 0 || H <= 0)
        return 0;
    long x0 = x, y0 = y, x1 = (long) x + w, y1 = (long) y + h; /* no int overflow at the edges */
    long cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0, cx1 = x1 > W ? W : x1, cy1 = y1 > H ? H : y1;
    if (cx0 >= cx1 || cy0 >= cy1)
        return 0;
    p->sx = (int) cx0;
    p->sy = (int) cy0;
    p->dx = (int) (cx0 - x0);
    p->dy = (int) (cy0 - y0);
    p->w = (int) (cx1 - cx0);
    p->h = (int) (cy1 - cy0);
    return 1;
}
#endif
