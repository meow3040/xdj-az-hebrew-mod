Touch FX 0.7.2 - opaque head, Rainbow and trail width test

Adds optional Rainbow: one shared colour cycles smoothly over12.288seconds.
Existing Blue/White/Red/Green indices retained. Blue and tail-only remain defaults.
Restores Slim/Full tail choice with new trail-width key, default Slim.
The head stays39px and fully opaque in its circle interior. Fade starts in
trail history; the soft one-pixel head edge remains antialiased.

The transparent window's alternating-line mask previously affected the head
as well. A private, owner-validated core API now unions only the clipped39px
circle into the cached base mask. Moving/lifting resets the previous circle.
At most39 single-row rectangles, two bounded shape requests per head change;
no bitmap allocation/rebuild on movement or stationary colour cycling.
The fixed core input guard and ordinary SDK watchdog remain unchanged.
No audio library or DSP changes. Other panel behaviour/settings preserved.

Tests: actual core head API and mask extracted into fake X harness; caller/
surface/size validation, solid head, restored background on move/lift,
1000stationary ticks no extra shape work, failclosed guard. Circle geometry
exhaustively checked across the field. SDK premultiplied alpha both widths;
max-alpha tail overlap once per pixel; all30style/colour/width combinations;
LATCH/release/close; geometry, memory, banks, follow mixer, native STEMS labels.
Full ARM64 build and symbol/GLIBC validation. Hardware confirmation pending.

Install bundled core plus both modules together. Previous0.7.0 backed up
before USB staging. Audio/settings unchanged. One user restart; Allow/Reload
both modules if requested. Check opaque head, background during movement,
Rainbow, Slim/Full, and physical/second-finger STEMS controls.
No automatic restart, runtime-cache overwrite, approval edits or publication.
