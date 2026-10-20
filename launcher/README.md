# The Hebrew launcher

Double-click `AZ-Hebrew-Launcher.cmd` at the top of this repository (Windows). A window opens
with three steps:

| Step | What it does |
|---|---|
| Build | fetches the AZ-MODS source at the commit `azmods/BASE` names, adds the mod (`azmods/files`, `azmods/hebrew.patch`) and builds `azmod.so` in WSL, without Docker (`build-azmod.sh`) |
| Copy to the USB stick | puts that `azmod.so` over `MODS\azmod.so` on a stick the AZ-MODS app set up, and adds the launcher's SSH key (`MODS\authorized_keys`, `MODS\ssh.enable`) |
| Turn it on at the deck | over SSH: allows this deck to load builds that are not signed (the [developer override](https://github.com/Kyle-Hosman/xdj-az-mods/blob/main/BUILDING.md#run-your-own-build-on-your-deck)), has the loader take the stick's set, turns the Hebrew row on and starts the deck software again (`deck.sh apply`) |

**Back to the official mods** (the window's footer) puts the official `azmod.so` back on the
stick and, when the deck was set up from the launcher, starts the deck with it and removes the
override (`deck.sh restore`).

## What it needs

| | |
|---|---|
| Python 3.8 or newer with the `py` launcher | python.org |
| WSL with Ubuntu (curl, python3, dpkg-deb and git, all there by default) | `wsl --install` in an administrator PowerShell, once |
| The official AZ-MODS set of the version in `azmods/BASE` | the AZ-MODS app keeps it in `%APPDATA%\AZ-MODS\mods\<version>` after it installed that version once |
| The deck on the same network as the computer | for the last step. The first time, the deck asks "Allow remote access (SSH)" for the launcher's key: choose Allow |

## How the build matches the official one

The official build runs in Ubuntu 18.04's arm64 image (AZ-MODS' `docker/azmod-build.Dockerfile`).
The launcher uses the closest standalone cross toolchain instead: Bootlin's aarch64 glibc
stable 2018.11 (GCC 7.3, glibc 2.27), Ubuntu 18.04's own arm64 X11 and FreeType packages, the
compile and strip lines of AZ-MODS' `src/Makefile` and the hardening Ubuntu's GCC turns on by
default. Every download is pinned by SHA-256, and the AZ-MODS source is fetched with git by
commit, so the commit hash proves it. The firmware-derived headers are filled in from the
official `azmod.so` by AZ-MODS' `tools/extract-assets.py`. A build is kept only when it needs
no glibc newer than 2.27 and exports exactly the tree's `tests/unit/exports-azmod.txt`. Built
from unchanged AZ-MODS source, it has the official `azmod.so`'s exports, imports and
libraries, and its `.text` is within 0.1% of the official one's.

`build-azmod.sh` stops when `src/Makefile`'s azmod lines change, or when `hebrew.patch` does
not apply, so a new AZ-MODS version never builds into something half-patched.

## Where things go

| Path | |
|---|---|
| `%LOCALAPPDATA%\AZ-Hebrew-Launcher` | the build, the downloads (about 75 MB), the launcher's SSH key and known hosts, its settings and log |
| `~/.cache/az-hebrew-launcher` in WSL | the unpacked toolchain and the fetched AZ-MODS commits |
| on the deck | `/home/root/settings/azmods-unsigned.on` (the override), `hebrew = on` in `azmod.conf`, `/tmp/az-hebrew-launcher.log` |

The window is a local page on `127.0.0.1` with a fresh token per start; nothing listens
on the network. The launcher closes with its Close button, or ten minutes after its window
is closed.

## Tests

```sh
python3 launcher/test_launcher.py    # on Linux or in WSL
```

They cover the stick (copy, keys, restore), the local server's checks, the glyph paths the
window draws, and `deck.sh` against a simulated deck: the conf edit, the hook check, a stick
that is not in the deck, restore. The mod's own tests (the bidi suite, `tests/unit/bidi_test.c`)
are part of AZ-MODS' unit tests once the mod is applied.
