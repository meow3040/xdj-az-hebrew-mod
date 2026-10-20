# The Community Mods edition

The Hebrew mod as an AZ-MODS [Community Mod](https://github.com/Kyle-Hosman/az-mods-sdk):
`meow3040-hebrew`, a library the official AZ-MODS core (1.2.3 or newer) loads from the USB
stick. It needs no build of AZ-MODS of its own and no developer override, and it stays
installed when AZ-MODS updates (as long as the deck's firmware is 2.00 or 2.01, the build its
hooks are made for).

**Install on the deck:** Mod Menu > Community Mods > Repositories > Add, type
`meow3040/xdj-az-hebrew-mod`, then Browse > Hebrew > Install, and Reload. The desktop app's
**Add repository** works the same way.

It has the same three parts as the built-in edition (`../azmods/files/src/azmod/hebrew.c`),
through the core's mod API (`include/azmod-mod.h`, the SDK's header):

| Part | Hooks (`claim_hook`, EP147 2.00) |
|---|---|
| Letters | EP147's PLT stubs for `FT_Get_Char_Index`, `FT_Load_Glyph` and `FT_Get_Kerning`: every call JUCE makes into FreeType passes through them |
| Right to Left | `juce::GlyphArrangement::addCurtailedLineOfText` |
| Hebrew Keyboard | the search keyboard's controller (constructor, destructor, restriction, named keys), the image key painter and the key sender |

Its rows: **Hebrew** (the letters), with **Right to Left** and **Hebrew Keyboard** under it.
A part whose claims the core refuses stays stock; the mod's log says which.

## Build and release

```sh
launcher/build-azmod.sh --community . community/out <download dir>    # in WSL or on Linux
<sdk>/tools/azmod-pack validate community
<sdk>/tools/azmod-pack package community --github meow3040/xdj-az-hebrew-mod --tag meow3040-hebrew-<version>
<sdk>/tools/azmod-pack repo add azmods-repo.json community/dist/meow3040-hebrew-<version>.entry.json
```

Then a GitHub release under that tag with the `.azmod` and its icon, and the repository file
committed on `main`. `build-azmod.sh --community` uses the same pinned toolchain as the
launcher's build, with the flags of the SDK's template, and keeps the library only when it
needs no glibc newer than 2.27, exports only `az_mod_v1_entry` and links only what a deck
allows.
