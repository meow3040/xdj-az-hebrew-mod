/* SPDX-License-Identifier: GPL-3.0-only */
/* probe_menu.c: who reads EP147's browse MENU model, and what it holds.
 * Opt-in: /home/root/settings/azmod-menuprobe.on -> /tmp/azmod-menu.log.
 * #included by azmod.c.
 *
 * gui::disp_data::BrowseMenu (vt 0x29c5028, 0x48 bytes, at BrowseDispData+0xa0):
 *   +0x18 s8   menu type (-1 = none)
 *   +0x28 u16* item ids,  +0x38 int count
 * slot 3 (0x1972270) returns 16 bytes from +0x18, slot 4 (0x1974858) the item
 * list by value, both through x8 (sret), so they are hooked with an asm shim
 * that preserves every argument register and only observes `this`.
 * The id -> label switch 0x1bc5668(s8 id, sret String) has no static caller;
 * the same shim shape names who calls it.
 *
 * Development builds only: it walks frame pointers and reads the menu model from any thread
 * that calls it, with no lifetime guarantee, so it needs the dev-only fault-free reader. */
static void install_menu_probe(void) {}
