/* SPDX-License-Identifier: GPL-3.0-only */
/* menu_input.c: azmod_input (every screen's touch and knob handling) and azmod_tick.
 * Part of the Mod Menu's one translation unit: #included by menu.c, in the order listed there. */
/* Closed: the NEW PLAYLIST field's CANCEL / CREATE, the held banner and the deck-screen strips; taps outside them pass through to EP147. */
static bool input_closed(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched, int enc_delta,
                         bool enc_click) {
    (void) enc_delta;
    (void) enc_click; /* tap-only: the knob does nothing until a menu is open */
    /* The MOD MENU entry is EP147's own SOURCE-list row (the srcrow hooks), so
     * there is no overlay tap region for it here. */
    if (s->nm_visible) { /* NEW PLAYLIST: CANCEL / CREATE on the title bar */
        int bar[4], ca[4], cr[4];
        azmod_naming_rects(fb, bar, ca, cr);
        if (touched && in_rect(touch_x, touch_y, cr[0], cr[1], cr[2], cr[3])) {
            azmod_naming_create();
            return true;
        }
        if (touched && in_rect(touch_x, touch_y, ca[0], ca[1], ca[2], ca[3])) {
            azmod_naming_cancel();
            return true;
        }
        return touched &&
               in_rect(touch_x, touch_y, bar[0], bar[1], bar[2], bar[3]); /* the bar is ours */
    }
    if (s->tb_visible) { /* our held banner: UNDO and/or DONE (tap only, the knob browses) */
        int bx, by, bw, bh;
        if (s->tb_undo) {
            azmod_tb_undo_rect(fb, &bx, &by, &bw, &bh);
            if (touched && in_rect(touch_x, touch_y, bx, by, bw, bh)) {
                azmod_tb_undo();
                return true;
            }
        }
        if (!s->tb_undo || s->tb_done_btn) {
            azmod_tb_done_rect(fb, &bx, &by, &bw, &bh);
            if (touched && in_rect(touch_x, touch_y, bx, by, bw, bh)) {
                azmod_tb_done();
                return true;
            }
        }
        return false;
    }
    /* The stem-toggle strips are live only on the WAVEFORM screen, only while
     * the STEMS mod (master) is enabled, and only for a deck whose loaded
     * track has stems; elsewhere taps pass straight through. */
    if (touched && s->waveform_visible && azmod_stems_engine(s)) {
        for (int d = 0; d < AZMOD_STEM_DECKS; d++) {
            if (!s->stems_available[d] || !s->pad_beatloop[d])
                continue;
            for (int i = 0; i < AZMOD_STEM_COUNT; i++) {
                int x, y, w, h;
                azmod_stem_btn_rect(fb, d, i, &x, &y, &w, &h);
                if (in_rect(touch_x, touch_y, x, y, w, h)) {
                    s->stem_on[d][i] = !s->stem_on[d][i];
                    s->last_action = AZ_ACT_STEMS; /* azmod.c writes per-deck masks */
                    return true;
                }
            }
        }
    }
    return false; /* taps outside the strips pass through to EP147 */
}

/* The Updates screen: one button (click, or a tap on it: update if one is available, else check); the knob turn scrolls the changelog. */
static bool input_updates(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched, int enc_delta,
                          bool enc_click) {
    /* the knob turn scrolls the changelog box (the click is still the button):
     * two whole lines per detent, drawn at once. A pixel glide re-renders the
     * visible text through FreeType every frame, which is too slow on the deck. */
    if (enc_delta) {
        s->changelog_scroll += enc_delta * 2 * cl_lh(fb);
        cl_clamp(s);
        s->changelog_pos = s->changelog_scroll;
        return true;
    }
    if (touched && changelog_hit(fb, touch_x, touch_y)) { /* a drag starts here */
        azmod_changelog_drag(s, fb, touch_y, 0);
        return true;
    }
    if (s->update_busy)
        return true; /* nothing to press while it runs */
    bool hit = enc_click;
    if (touched) {
        int bx, by, bw, bh;
        update_button_rect(fb, &bx, &by, &bw, &bh);
        hit = in_rect(touch_x, touch_y, bx - 8, by - 8, bw + 16, bh + 16);
    }
    if (hit) {
        s->update_want = s->update_avail ? 2 : 1;
        /* busy carries the kind (1 check, 2 update): for a check,
         * "available" is the answer, not a phase; see read_modupdate */
        s->update_busy = s->update_want;
        s->update_pct = 0;
    }
    return true;
}

/* Pad Colors: the master toggle and one row per pad-mode button. */
static bool input_pad_colors(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched, int enc_delta,
                             bool enc_click) {
    /* Enable off: the color rows are locked (grayed, no knob stop, no
     * tap, nothing opens), so the only thing to do is Enable (the
     * colors must be enabled first). */
    int nrows = s->pad_colors_on ? COLORS_ROWS : 1;
    if (!s->pad_colors_on)
        s->menu_sel = COLORS_ROW_MASTER;
    if (enc_delta) {
        s->menu_sel += (enc_delta > 0 ? 1 : -1);
        if (s->menu_sel < 0)
            s->menu_sel = nrows - 1;
        if (s->menu_sel >= nrows)
            s->menu_sel = 0;
    }
    bool hit = false;
    if (touched) {
        for (int i = 0; i < nrows; i++) {
            int x, y, w, h;
            pc_row_rect(fb, i, &x, &y, &w, &h);
            if (in_rect(touch_x, touch_y, x, y, w, h)) {
                s->menu_sel = i;
                hit = true;
                break;
            }
        }
    }
    if (enc_click || hit) { /* a miss-tap navigates nothing, activates nothing */
        if (s->menu_sel == COLORS_ROW_MASTER) {
            s->pad_colors_on = !s->pad_colors_on;
            s->config_dirty = true;
            s->pressed_item = COLORS_ROW_MASTER;
            s->pressed_ticks = AZMOD_FLASH_TICKS;
        } else { /* a button row opens its own swatch grid */
            s->color_sel = colors_row_to_sel(s->menu_sel);
            s->mode = AZ_COLOR_PICK;
            s->menu_sel = colors_cell_of(*pick_slot(s, s->color_sel));
        }
    }
    return true; /* consume everything while the screen is up */
}

/* Overlays, the Clock or Browse View Mods: the switch first (a group has none), then the rows under it. */
static bool input_overlays(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched, int enc_delta,
                           bool enc_click) {
    int ovr[OVERLAYS_MAX], OVERLAYS_ROWS = overlays_rows(sub_head(s->mode), ovr);
    if (enc_delta) {
        s->menu_sel += (enc_delta > 0 ? 1 : -1);
        if (s->menu_sel < 0)
            s->menu_sel = OVERLAYS_ROWS - 1;
        if (s->menu_sel >= OVERLAYS_ROWS)
            s->menu_sel = 0;
    }
    bool hit = false;
    if (touched) {
        for (int i = 0; i < OVERLAYS_ROWS; i++) {
            int x, y, w, h;
            colors_row_rect(fb, i, &x, &y, &w, &h);
            if (in_rect(touch_x, touch_y, x, y, w, h)) {
                s->menu_sel = i;
                hit = true;
                break;
            }
        }
    }
    if ((enc_click || hit) && s->menu_sel >= 0 && s->menu_sel < OVERLAYS_ROWS) {
        /* Every row toggles, the readouts included while the master is off:
         * like Pad Colors, pick them first, then switch them on together. A choice steps to
         * its next value. */
        int m = ovr[s->menu_sel];
        if (m == C_TZ_ROW) { /* the time zone: its list */
            clock_tz_open(s, fb);
            return true;
        }
        if (m == C_SET_ROW) { /* the time: its picker */
            clock_set_open(s);
            return true;
        }
        az_toggle(m); /* a choice steps to its next value */
        s->config_dirty = true;
        s->last_action = m;
        s->pressed_item = s->menu_sel;
        s->pressed_ticks = AZMOD_FLASH_TICKS;
    }
    return true; /* consume everything while the screen is up */
}

/* The swatch grid for one pad-mode button (color_sel). */
static bool input_color_pick(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched, int enc_delta,
                             bool enc_click) {
    if (enc_delta) {
        s->menu_sel += (enc_delta > 0 ? 1 : -1);
        if (s->menu_sel < 0)
            s->menu_sel = AZMOD_PAD_SWATCH_CELLS - 1;
        if (s->menu_sel >= AZMOD_PAD_SWATCH_CELLS)
            s->menu_sel = 0;
    }
    bool hit = false;
    if (touched) {
        for (int k = 0; k < AZMOD_PAD_SWATCH_CELLS; k++) {
            int x, y, w, h;
            colors_swatch_rect(fb, k, &x, &y, &w, &h);
            if (in_rect(touch_x, touch_y, x - 4, y - 4, w + 8, h + 8)) {
                s->menu_sel = k;
                hit = true;
                break;
            }
        }
    }
    if (enc_click || hit) { /* pick -> back to the list, where the chip shows it */
        if (s->color_sel < 0 || s->color_sel >= AZMOD_PADPICK_COUNT)
            s->color_sel = 0;
        *pick_slot(s, s->color_sel) = colors_index_of_cell(s->menu_sel);
        s->config_dirty = true;
        s->mode = AZ_COLORS;
        s->menu_sel = colors_sel_to_row(s->color_sel);
    }
    return true; /* consume everything while the grid is up */
}

/* The main list: the knob steps through the rows in display order, a tap selects, and a click
 * or a hit activates (a toggle, a CHOICE, a sub-screen, or a mod's action). */
static bool input_main_list(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched, int enc_delta,
                            bool enc_click) {
/* AZ_OPEN. Navigate; swallow all input from EP147 underneath. */
    if (enc_delta) {
    /* Step through the display order, not the item id. The bank row is one knob stop
     * (one CHOICE); its buttons are touch-only. */
        int ord[AZI_COUNT], no = menu_order(ord), p = -1;
        for (int q = 0; q < no; q++)
            if (ord[q] == s->menu_sel)
                p = q;
        do {
            if (p < 0)
                p = (enc_delta > 0) ? -1 : no;
            p += (enc_delta > 0 ? 1 : -1);
            if (p < 0)
                p = no - 1; /* wrap within the items */
            if (p >= no)
                p = 0;
            s->menu_sel = ord[p];
        } while (item_hidden(s->menu_sel));
        c_cshow(fb, s->menu_sel); /* the list scrolls to keep the selection in view */
    }
    bool hit = false;
    int hit_bank = -1; /* a tapped bank button */
    if (touched) {
    /* Pin: the header glyph toggles whether the highlighted mod is pinned as a
     * permanent left-column row. Checked before the item rows: it sits in the
     * header, above row 0. Only the boolean mods are pinnable. */
        if (g_docked && s->pinrows_on && az_pinnable(s->menu_sel)) {
            int px, py, pw, ph;
            pin_rect(fb, &px, &py, &pw, &ph);
            if (in_rect(touch_x, touch_y, px, py, pw, ph)) {
                az_pin_toggle(s->menu_sel);
                s->config_dirty = true; /* azmod.c persists line 3 of azmod.cfg */
                return true;
            }
        }
        int vtop, vbot;
        c_cclamp(fb);
        c_cview(fb, &vtop, &vbot);
        for (int i = 0; i < AZI_COUNT && !hit && touch_y >= vtop && touch_y < vbot; i++) {
            if (item_hidden(i))
                continue; /* its rect would overlap the next row */
            int x, y, w, h;
            if (item_is_bank(i)) { /* a bank row: only its buttons are targets */
                for (int b = 0; b < az_n_choices(i); b++) {
                    item_bank_rect(fb, i, b, &x, &y, &w, &h);
                    if (in_rect(touch_x, touch_y, x, y, w, h)) {
                        s->menu_sel = i;
                        hit_bank = b;
                        hit = true;
                        break;
                    }
                }
                continue;
            }
            azmod_item_rect(fb, i, &x, &y, &w, &h);
            if (in_rect(touch_x, touch_y, x, y, w, h)) {
                s->menu_sel = i;
                hit = true;
            }
        }
    }
    bool activate = enc_click || hit; /* a miss-tap navigates nothing, activates nothing */
    if (activate) {
        if (sub_screen(s->menu_sel)) { /* a sub-screen, like Pad Colors */
            s->mode = sub_screen(s->menu_sel);
            s->menu_sel = 0;
        } else if (az_has_value(s->menu_sel)) { /* a mod toggle */
        /* A sub-setting whose parent is off is inert: it is drawn grayed,
         * and tapping it must not silently arm something invisible. */
            if (!az_active(s->menu_sel))
                return true;
            if (item_is_bank(s->menu_sel)) {
            /* Radio: exactly one bank is live. Re-tapping the live bank is
             * a no-op rather than a toggle-off: there is no "no bank".
             * A knob click on the bank row cycles to the next bank instead. */
                int it = s->menu_sel, n = az_n_choices(it);
                az_set(it, hit_bank >= 0 ? hit_bank : (az_get(it) + 1) % n);
                s->config_dirty = true;
                flash_and_activate(s, it);
                return true;
            }
            az_toggle(s->menu_sel);
            s->config_dirty = true; /* azmod.c applies and persists the change */
            flash_and_activate(s, s->menu_sel);
        } else if (s->menu_sel == AZI_SYSINFO) {
            s->mode = AZ_INFO;
            s->last_action = AZI_SYSINFO; /* tells azmod.c to fill info_line[] */
        } else if (s->menu_sel == AZI_COMMUNITY)
            s->mode = AZ_COMMUNITY; /* header only: managed from the full-screen menu */
        else if (s->menu_sel == AZI_UPDATE) {
            s->mode = AZ_UPDATE;
            s->last_action =
                AZI_UPDATE; /* azmod.c fills update_line[] from the status file */
        } else if (s->menu_sel == AZI_PAD_COLORS) {
            s->mode = AZ_COLORS;
            s->menu_sel = 0;
            s->color_sel = 0;
        } else { /* RELOAD (a group or a screen row only flashes) */
            const struct az_row *r = az_row(s->menu_sel);
            if (r && r->d.kind == AZ_ROW_ACTION && s->menu_sel >= AZI_REG0 && s->menu_sel < AZI_ACT0 &&
                r->d.on_activate && az_active(s->menu_sel))
                r->d.on_activate(r->d.user); /* a mod's action */
            flash_and_activate(s, s->menu_sel);
        }
    }
    return true;
}

bool azmod_input(azmod_state *s, const azmod_fb *fb, int touch_x, int touch_y, bool touched,
                 int enc_delta, bool enc_click) {
    /* The knob arrives as a count of detents (azmod.c g_enc_dir). Every handler
     * below takes one step per call, so feed them one detent at a time. */
    if (!touched && (enc_delta > 1 || enc_delta < -1)) {
        int st = enc_delta > 0 ? 1 : -1, n = enc_delta > 0 ? enc_delta : -enc_delta;
        bool r = false;
        for (int k = 0; k < n; k++)
            r |= azmod_input(s, fb, 0, 0, false, st, false);
        if (enc_click)
            r |= azmod_input(s, fb, 0, 0, false, 0, true);
        return r;
    }
    s->last_action = -1;
    s->config_dirty = false;
    if (s->pressed_ticks <= 0)
        s->pressed_item = -1; /* zero-init default: no flash */

    if (s->mode == AZ_CLOSED)
        return input_closed(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);

    if (g_docked && !s->menu_focused) { /* PREVIEW card: a tap opens the menu */
        if (touched)
            s->want_focus = true;
        return true;
    }
    if (g_docked && c_input(s, fb, touch_x, touch_y, touched, enc_delta, enc_click))
        return true;
    if (s->mode == AZ_COMMUNITY)
        return cm_input(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);

    /* Sub-screen header BACK square: a tap does exactly what the physical BACK does
     * one level down (azmod_back -> top menu). azmod.c's tap handler then sets the
     * same cursor-in-menu state the bus thread's depth-2 BACK sets, so the next
     * physical BACK is judged at the right depth. Padded 8px for a finger. */
    if (touched && g_docked &&
        (s->mode == AZ_COLOR_PICK || s->mode == AZ_CLOCK_TZ || s->mode == AZ_CLOCK_SET)) { /* full-screen: the screens one level down */
        int bx, by, bw, bh;
        back_rect(fb, &bx, &by, &bw, &bh);
        if (in_rect(touch_x, touch_y, bx - 8, by - 8, bw + 16, bh + 16)) {
            azmod_back(s);
            return true;
        }
    }
    if (s->mode == AZ_INFO)
        return input_info(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);
    if (s->mode == AZ_COMMUNITY)
        return input_subscreen(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);
    if (s->mode == AZ_UPDATE)
        return input_updates(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);


    if (s->mode == AZ_COLORS)
        return input_pad_colors(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);

    if (sub_mode(s->mode))
        return input_overlays(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);

    if (s->mode == AZ_COLOR_PICK)
        return input_color_pick(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);

    if (s->mode == AZ_CLOCK_TZ)
        return input_clock_tz(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);

    if (s->mode == AZ_CLOCK_SET)
        return input_clock_set(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);

    return input_main_list(s, fb, touch_x, touch_y, touched, enc_delta, enc_click);
}

static bool cm_tick(azmod_state *s);
bool azmod_tick(azmod_state *s) {
    bool glide = c_glide_tick(s); /* the Mod Menu's lists in motion */
    if (s->mode == AZ_UPDATE && cl_animate(s))
        return true; /* changelog easing: redraw */
    if (cm_tick(s))
        return true; /* Community: the fetcher's files */
    if (s->pressed_item < 0)
        return glide;
    if (s->pressed_ticks > 0) {
        s->pressed_ticks--;
        return true;
    }
    s->pressed_item = -1;
    return true; /* one more redraw to clear the flash */
}
