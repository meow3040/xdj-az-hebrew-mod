/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file font_size.c
 * @brief The Browse screen's font size survives a restart.
 *
 * The Browse font size button cycles gui::FontManager's own size (setFontSizeSetting), which
 * EP147 never saves, so every start shows the default size again. (EP147 also has an
 * application_setting::FontSize; nothing on screen reads it.) The hook on the Browse
 * buttons' handler saves the size to settings/azmod-fontsize after the font size button has
 * set it. The FontManager the
 * Browse screen uses is the one EP147 registers by name at start-up; right after that one is
 * constructed, before anything has subscribed to it or read it, the saved size is set on it
 * with the same setter, so the Browse screen is built at that size. The Browse font size
 * selector keeps its own index, set only by a press; after a Browse function bar is built,
 * the selector is set from that FontManager so it shows the size in use.
 *
 * Always on: it restores what the DJ last chose, as stock does for its other settings.
 * Kill switch: settings/azmod-fontsize.off.
 *
 * Entry points: install_font_size.
 */
/* EP147, gui::FontManager:
 *   - ctor FUN_01f4ac90(this); the registered instance is the one built by the start-up
 *     registration (FUN_020110a0), so its ctor returns to that call site;
 *   - setFontSizeSetting(this, FontSize_t) FUN_01f4a3f0: refuses a size >= 3, stores it at
 *     +0x44 (stlr) and calls each listener (array +0x48, count +0x58) with it;
 *   - getFontSizeSetting(this) FUN_01f4a590 = load of +0x44;
 *   - the Browse function buttons' handler FUN_019ab418(view, button): button 1 is the font
 *     size; it reads the size from the FontManager held at view+0x1540, takes the next one
 *     and sets it with setFontSizeSetting. setFontSizeSetting itself cannot be hooked (a
 *     PC-relative branch in its first four instructions), so the save hooks this handler;
 *   - gui::status_area::browse::StatusAreaWidgetController (the view's +0x14f0): ctor
 *     FUN_01c71030(this, ...) builds the Browse function bar and its font size selector (at
 *     its default); FUN_01c70fd0(this, size) is what the handler calls to move the selector
 *     after a press. */
#define FS_CTOR EP_1f4ac90
#define FS_SET EP_1f4a3f0
#define FS_GET EP_1f4a590
#define FS_VT EP_2dccc70     /* gui::FontManager's vtable */
#define FS_BUTTONS EP_19ab418
#define FS_VIEW_FM 0x1540    /* the view's FontManager */
#define FS_BUTTON_SIZE 1
#define FS_BAR_CTOR EP_1c71030
#define FS_BAR_VT EP_2c52ff0 /* StatusAreaWidgetController's vtable */
#define FS_BAR_SELECT EP_1c70fd0
#define FS_REG_RET EP_201281c /* the return address of the registration's ctor call */
#define FS_N 3
#ifndef FS_PATH
#define FS_PATH "/home/root/settings/azmod-fontsize"
#endif
static int g_fs_saved = -1;    /* the size in the file, -1 none (__atomic only) */
static uintptr_t g_fs_fm;      /* the registered FontManager, vtable-checked at every use (__atomic only) */
static void fs_log(const char *fmt, ...) {
    FILE *f = azlog_open("/tmp/azmod-fontsize.log", "a");
    if (!f)
        return;
    fprintf(f, "[%ld] ", (long) time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
#ifndef FS_OFF_PATH
#define FS_OFF_PATH "/home/root/settings/azmod-fontsize.off"
#endif
static int g_fs_off = -1; /* the kill switch, read once (__atomic only) */
static int fs_on(void) {
    int off = __atomic_load_n(&g_fs_off, __ATOMIC_ACQUIRE);
    if (off < 0) {
        off = access(FS_OFF_PATH, F_OK) == 0;
        __atomic_store_n(&g_fs_off, off, __ATOMIC_RELEASE);
    }
    return !off;
}
static int fs_load_file(void) {
    FILE *f = fopen(FS_PATH, "re");
    int n = -1;
    if (!f)
        return -1;
    if (fscanf(f, "%d", &n) != 1)
        n = -1;
    fclose(f);
    return n >= 0 && n < FS_N ? n : -1;
}
/* Save a size the DJ set; once per new size. */
static void fs_save(int size) {
    if (size < 0 || size >= FS_N || !fs_on())
        return;
    if (__atomic_exchange_n(&g_fs_saved, size, __ATOMIC_ACQ_REL) == size)
        return;
    char line[8];
    int n = snprintf(line, sizeof line, "%d\n", size);
    settings_write(FS_PATH, line, (size_t) n, 1);
    fs_log("saved %d\n", size);
}
/* fm if it is a live gui::FontManager (aligned, readable, its vtable), else 0 */
static uintptr_t fs_fm_ok(uintptr_t fm) {
    uintptr_t vt = 0;
    return fm && !(fm & 7) && own_q(fm, &vt) && vt == FS_VT ? fm : 0;
}
/* The view's FontManager, or 0. */
static uintptr_t fs_view_fm(long view) {
    uintptr_t fm = 0;
    if (!view || (view & 7) || !own_q((uintptr_t) view + FS_VIEW_FM, &fm))
        return 0;
    return fs_fm_ok(fm);
}
static void (*g_o_fs_buttons)(long, int);
static void fs_buttons_hook(long view, int button) {
    g_o_fs_buttons(view, button);
    if (button != FS_BUTTON_SIZE)
        return;
    uintptr_t fm = fs_view_fm(view);
    if (fm)
        fs_save(((int (*)(long)) FS_GET)((long) fm));
}
/* The ctor's caller decides: only the registered FontManager gets the saved size, through
 * EP147's own setter (it is not a button press, so nothing is written back). */
static void fs_ctor_apply(long fm, uintptr_t ret) {
    if (!FS_REG_RET || ret != FS_REG_RET)
        return;
    int v = fs_on() ? fs_load_file() : -1;
    __atomic_store_n(&g_fs_saved, v, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fs_fm, (uintptr_t) fm, __ATOMIC_RELEASE);
    if (v >= 0)
        ((void (*)(long, int)) FS_SET)(fm, v);
    fs_log("registered FontManager %lx: saved size %d%s\n", (unsigned long) fm, v, v >= 0 ? ", applied" : "");
}
static void (*g_o_fs_ctor)(long);
static void fs_ctor_hook(long fm) {
    uintptr_t ret = (uintptr_t) __builtin_return_address(0);
    g_o_fs_ctor(fm);
    fs_ctor_apply(fm, ret);
}
/* A status area controller just built: its font size selector shows the size the
 * FontManager holds. */
static void fs_bar_sync(long bar) {
    uintptr_t vt = 0, fm = fs_fm_ok(__atomic_load_n(&g_fs_fm, __ATOMIC_ACQUIRE));
    if (!fm || !fs_on() || !bar || (bar & 7) || !own_q((uintptr_t) bar, &vt) || vt != FS_BAR_VT)
        return;
    int size = ((int (*)(long)) FS_GET)((long) fm);
    if (size >= 0 && size < FS_N)
        ((void (*)(long, int)) FS_BAR_SELECT)(bar, size);
}
/* the ctor's arguments are forwarded whole (x0-x3), whatever it takes */
static void (*g_o_fs_bar)(long, long, long, long);
static void fs_bar_hook(long bar, long a1, long a2, long a3) {
    g_o_fs_bar(bar, a1, a2, a3);
    fs_bar_sync(bar);
}
static void install_font_size(void) {
    /* each half alone is harmless: saving without restoring, or restoring a saved size */
    int a = azm_install_hook(FS_BUTTONS, (void *) fs_buttons_hook, (void **) &g_o_fs_buttons);
    int b = azm_install_hook(FS_CTOR, (void *) fs_ctor_hook, (void **) &g_o_fs_ctor);
    int c = azm_install_hook(FS_BAR_CTOR, (void *) fs_bar_hook, (void **) &g_o_fs_bar);
    fs_log("font size: hooks buttons=%d ctor=%d bar=%d\n", a, b, c);
}
