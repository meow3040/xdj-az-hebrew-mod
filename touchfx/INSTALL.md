# Installation (custom-build beta)

Do not install this panel alone on the official core. The public distribution
route for the matching custom core/audio must be settled first.

For an authorised test bundle containing all three matching components:
1. Note your firmware and AZ-MODS versions. Back up the entire USB MODS folder,
   plus the player's current AZ-MODS settings, before replacing anything.
2. With the player powered off and the USB connected to your computer, replace
   only MODS/azmod.so and MODS/azstem.so with the matching supplied files.
3. After building and packaging the panel, extract its .azmod ZIP contents into
   MODS/community-mods/mrdjjag-touchfx-test/ (manifest and library directly inside,
   without an extra enclosing directory). Back up an existing module directory
   first and preserve its data/ folder. Do not rename the module ID.
4. Custom core/audio are unsigned. Upstream documents a developer installation
   route over SSH: `touch /home/root/settings/azmods-unsigned.on` as root.
   This player-wide option lets unverified deck scripts and libraries run, not just
   Touch FX. Enable it only if you accept that change and trust everything on the
   USB. Ordinary Community Mods do not require it. We do not enable it automatically.
   See core/UPSTREAM-BUILDING.md and upstream UPDATES-AND-SIGNING.md.
   Do not alter cached libraries or module approval files.
5. Reconnect USB, fully restart the player, and approve Touch FX if prompted.
6. Enable Touch FX. For STEMS, load a track prepared with Kyle's Mod Manager and
   enable STEMS on that deck. Test channel following, effects and layout before use.

Keep all three components from the same bundle. Official updates may replace
custom libraries; compatibility with later updates must be verified separately.
