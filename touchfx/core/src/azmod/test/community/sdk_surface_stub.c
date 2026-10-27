/* SPDX-License-Identifier: GPL-3.0-only */
/* Stand-ins for the drawing services (sdk_surface.c) in the community host's test: that file
 * needs the overlay thread and X11, which this harness does not have. Every service refuses,
 * as it would before the overlay is up. */
static int sdk_view(void) { return AZ_VIEW_OTHER; }
static az_surface *sdk_surface_create(const char *id, const az_surface_v1 *s) {
    (void) id;
    (void) s;
    return NULL;
}
static void sdk_surface_place(az_surface *s, az_rect r) {
    (void) s;
    (void) r;
}
static void sdk_surface_show(az_surface *s, int on) {
    (void) s;
    (void) on;
}
static void sdk_surface_redraw(az_surface *s) { (void) s; }
static int sdk_surface_focus(az_surface *s, int on) {
    (void) s;
    (void) on;
    return 0;
}
static int sdk_xwin_show(const char *id, unsigned long x, az_rect r, int on) {
    (void) id;
    (void) x;
    (void) r;
    (void) on;
    return -1;
}
static int sdk_xwin_find(const char *id, int pid, const char *p, int w, int h, unsigned long *x) {
    (void) id;
    (void) pid;
    (void) p;
    (void) w;
    (void) h;
    (void) x;
    return -1;
}
static int sdk_xwin_geometry(const char *id, unsigned long x, az_rect *o) {
    (void) id;
    (void) x;
    (void) o;
    return -1;
}
static int sdk_draw_image(az_draw *d, int x, int y, const unsigned char *p, int w, int h) {
    (void) d;
    (void) x;
    (void) y;
    (void) p;
    (void) w;
    (void) h;
    return -1;
}
static void sdk_draw_line(az_draw *d, int x0, int y0, int x1, int y1, int w, uint32_t c) {
    (void) d;
    (void) x0;
    (void) y0;
    (void) x1;
    (void) y1;
    (void) w;
    (void) c;
}
static int sdk_pointer_inject(int x, int y, int b, int down) {
    (void) x;
    (void) y;
    (void) b;
    (void) down;
    return -1;
}
static void sdk_pointer_cursor(int v) { (void) v; }
