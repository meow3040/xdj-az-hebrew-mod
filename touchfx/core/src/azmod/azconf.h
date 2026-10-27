/* SPDX-License-Identifier: GPL-3.0-only */
/* azmod.conf: the Mod Menu's settings as one readable, versioned file (azconf.c).
 *
 *   # AZ-MODS settings
 *   version = 1
 *   <key> = <value>
 *
 * - `#` comment lines and blank lines are kept on rewrite. Comments are whole lines only:
 *   a `#` after a value is part of the value.
 * - Whitespace around `=` and at both ends of a line is ignored. Keys match [a-z0-9._:-]+
 *   (`:` is reserved for registered community-mod keys, `<modid>:<key>`). A value may be
 *   empty; it never starts or ends with whitespace.
 * - Duplicate keys: the last one wins, and a rewrite keeps only the winner (in its place).
 * - Unknown keys are kept verbatim on rewrite: a rollback to an older conf-aware build keeps
 *   a newer build's settings.
 * - No valid `version = 1` line: the file is invalid (treated as absent). `version = 2` and
 *   up: a newer build's file, which this build never overwrites (the caller decides).
 * - A line longer than 511 bytes, or with control bytes, or that is not UTF-8, or that is
 *   neither a comment nor `key = value`, is ignored, logged, and dropped on rewrite.
 * - A file over 64 KB is invalid.
 *
 * Threads: start-up and settings changes only (the overlay / message thread). These calls
 * allocate and do file I/O: never call them from the bus thread or an audio hook.
 * A struct azconf is not locked: one thread owns it. */
#ifndef AZCONF_H
#define AZCONF_H

/* Shared by azmod's own translation units only: hidden, so none of these lands in EP147's
 * symbol namespace (menu.h does the same). */
#pragma GCC visibility push(hidden)

struct azconf; /* opaque; ordered lines, comments kept */

/* A fresh conf holds the header comment and `version = 1`, nothing else. NULL on ENOMEM. */
struct azconf *azconf_new(void);
void azconf_free(struct azconf *c);

/* 0 = loaded valid v1; -1 = absent; -2 = invalid/garbage; -3 = a newer version (left alone).
 * On any nonzero return c is reset to the azconf_new() content. A load that dropped lines
 * (junk, duplicates) leaves c dirty, so the next save heals the file. */
int azconf_load(struct azconf *c, const char *path);

/* "version" reads back as the file's version ("1"). */
const char *azconf_get(const struct azconf *c, const char *key);        /* NULL if absent */
int azconf_get_bool(const struct azconf *c, const char *key, int def); /* on/off/1/0/true/false
                                                                          (any case); else def */
long azconf_get_int(const struct azconf *c, const char *key, long def, long min,
                    long max); /* decimal; non-numeric or out of [min, max] -> def */

/* 0 ok, -1 bad key/value (bad key, "version", control bytes, not UTF-8, leading/trailing
 * whitespace, or a line over 511 bytes). An existing key keeps its place in the file; a new
 * one is appended. Setting the value a key already has changes nothing (not dirty). */
int azconf_set(struct azconf *c, const char *key, const char *val);
int azconf_set_bool(struct azconf *c, const char *key, int on); /* writes on / off */
int azconf_set_int(struct azconf *c, const char *key, long v);

/* Atomic + durable: <path>.new, fsync, rename, fsync of the directory (azmod.c
 * settings_write). 0 ok and c is clean (its dirty flag cleared); on failure <path> is
 * untouched and no .new is left. */
int azconf_save(struct azconf *c, const char *path);
int azconf_dirty(const struct azconf *c); /* changed since load/save */

/* The i-th `key = value` entry in file order (comments skipped): 1 and *key / *val set, or 0
 * past the last. A set or an unset moves the entries after it: restart the walk after one. */
int azconf_entry(const struct azconf *c, int i, const char **key, const char **val);
/* Remove a key (0), or -1 when it is absent. */
int azconf_unset(struct azconf *c, const char *key);

/* ---- one-time migration from 1.2.2's files (azmod.cfg, azmod-theme, azmod-vinyl.v,
 * azmod-btchannel, and the azmod-plreorder.on / -rowcache.on / -touchaccum.on flags).
 * The module holds the rules (load_config's, azmod.c); the caller passes the slot numbers. */

/* A radio set of azmod.cfg slots (the drum-roll banks, slots 7-10): one key whose value is
 * the 0-based position in slots[] of the live slot. Resolved as the build resolves it at run
 * time (mod_menu_overlay.c): the first slot that is on wins; none on = position 0. The slots
 * of a radio set get no key of their own: migration skips them whatever slot_key says. */
struct azconf_radio {
    const char *key;  /* e.g. "drumroll.bank" */
    const int *slots; /* azmod.cfg slot numbers, in order */
    int n;
}; /* at most 32 sets */

/* What azmod_state_init + load_config apply around azmod.cfg line 1. A slot number of -1,
 * or one outside [0, n), turns its rule off. */
struct azconf_legacy_defaults {
    const int *default_on; /* on when the file does not set them (REMOTESTEMS, STEMSMARK) */
    int n_default_on;
    const int *force_on; /* after the parse, whatever the file says (DRUMSYNC, release) */
    int n_force_on;
    const int *force_off; /* likewise (the retired slots 12, 23, 26) */
    int n_force_off;
    int slot_fps, slot_overlays; /* FPS on in a file no wider than OVERLAYS -> OVERLAYS on */
    int slot_pledit;             /* azmod-plreorder.on, file no wider than PLEDIT -> on */
    int slot_remotestems;        /* file no wider than REMOTESTEMS -> on */
    int slot_ui60ui;    /* azmod-rowcache.on or -touchaccum.on, no wider than UI60UI -> on */
    int palette_count;  /* AZMOD_PAD_PALETTE_COUNT (28): a valid index is 0..count-1 */
    const char *const *themes; /* the words azmod-theme may hold (menu.c C_THEME_KEY) */
    int n_themes;
    const struct azconf_radio *radio;
    int n_radio;
    const int *int_slots; /* slots whose key holds a number, written "0"/"1" (a 2-way CHOICE's
                             index, as azmod.c conf_capture saves it), not on/off */
    int n_int_slots;
};

/* slot_key[i] = the conf key for azmod.cfg line-1 char i (NULL = a retired/unknown slot,
 * skipped), n = number of slots the current build knows (1..64). Fills c (reset first) and
 * returns 0, or -1 when there is nothing to migrate (no azmod.cfg). Never modifies or
 * deletes the old files. The keys written: every slot key (on/off), each radio key,
 * padcolor.on, padcolor.0..3 and padcolor2.0..3 (native or the palette index; line 2's four
 * pad modes), pinned (slot keys, comma-separated, a pinned radio slot as its set's key; only
 * when some are pinned), and theme / vinyl.brake / bluetooth.channel
 * when their files hold a valid value. Not saved: the caller saves. */
int azconf_migrate_legacy(struct azconf *c, const char *settings_dir, const char *const *slot_key,
                          int n, const struct azconf_legacy_defaults *d);

/* Diagnostics: NULL (the default) = silent. */
extern void (*azconf_log)(const char *fmt, ...);

#pragma GCC visibility pop
#endif
