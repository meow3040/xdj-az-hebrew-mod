# Publication routes

## Current route: source-build beta

Upload this source tree to mrdjjag/az-touchfx-test. Keep version0.8.0 and its stable
module ID. Make a source-only beta release, linking README, BUILD and INSTALL.
Do not attach the private core binary, the local workspace source ZIP, backups,
settings, database keys, extracted firmware headers or the enclosing working folder.

A standalone .azmod is not a complete installer. Do not create a Browse index entry
that implies it installs the required custom core/audio. Source-build users must
build the custom libraries, verify their exact binding, back up their USB and opt
into upstream's developer installation route themselves.

## A conventional Community Mod release

This requires a supported way to supply the core APIs and audio capabilities that
Touch FX uses. A possible next step is upstream review of these changes; acceptance
is not guaranteed. Once those requirements are available through a documented,
supported installation, package the matching panel, attach package/icon to a GitHub
release, generate azmods-repo.json using azmod-pack, validate its download and then
optionally request listing in the Mod Index.

## Before uploading

Review attribution and source provenance; pick a final short summary and icon;
keep source and binary requirements explicit. Retain beta status until tested
beyond the author's player. No files have been uploaded by this preparation step.
