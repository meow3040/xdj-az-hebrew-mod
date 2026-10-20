#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# build-azmod.sh: build AZ-MODS' azmod.so with the Hebrew mod, without Docker.
#
#   build-azmod.sh MOD OFFICIAL_AZMOD_SO OUT_DIR DOWNLOAD_DIR
#
# MOD is this repository. The AZ-MODS source comes from the repository and commit that
# MOD/azmods/BASE names (fetched with git, so the commit hash proves the content); the mod's
# own files (MOD/azmods/files) go in at their places and MOD/azmods/hebrew.patch changes the
# AZ-MODS files they hook into. A patch that does not apply stops the build.
#
# Runs on any x86-64 Linux (the launcher runs it in WSL). The official build runs in Ubuntu 18.04's
# arm64 image (GCC 7.5, glibc 2.27); this uses the closest standalone cross toolchain, Bootlin's
# aarch64 glibc stable 2018.11 (GCC 7.3, glibc 2.27), with Ubuntu 18.04's own arm64 X11 and
# FreeType packages, and the compile and strip lines of src/Makefile's azmod target. Every
# download is pinned by SHA-256 and kept in DOWNLOAD_DIR; the toolchain is unpacked once into
# ${XDG_CACHE_HOME:-~/.cache}/az-hebrew-launcher. The tree itself is not written to: the build
# is put together in a temporary folder, with the firmware-derived headers filled in from
# OFFICIAL_AZMOD_SO (tools/extract-assets.py, which also proves that file is the release this
# source matches).
#
# The result is checked before it is kept: it needs no glibc newer than 2.27 (the deck has 2.29)
# and exports exactly the tree's tests/unit/exports-azmod.txt. Progress lines for the launcher start with
# "@@ ". Exit 0: OUT_DIR/azmod.so written.
#
#   build-azmod.sh --community MOD OUT_DIR DOWNLOAD_DIR
#
# builds the Community Mods edition instead (MOD/community, see its README) with the same
# toolchain and the flags of the AZ-MODS SDK's template, into OUT_DIR/<id>.so. It is kept only
# when it needs no glibc newer than 2.27, exports only az_mod_v1_entry and links only what a
# deck lets a mod link.
set -uo pipefail
MODE=azmod
if [ "${1:-}" = --community ]; then
    MODE=community
    MOD=$2 OUT=$3 DL=$4 OFFICIAL=
else
    MOD=$1 OFFICIAL=$2 OUT=$3 DL=$4
fi
step() { echo "@@ $*"; }
die() { echo "@@ error $*"; exit 1; }

TC=aarch64--glibc--stable-2018.11-1
TC_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/$TC.tar.bz2
TC_SHA=abae0522480b9f37ff6cee4249e147e7cb78e1997cc6f76dba7e0fb8ec04221d
PORTS=https://ports.ubuntu.com/pool/main
DEBS=(
    "libx/libx11/libx11-6_1.6.4-3ubuntu0.4_arm64.deb d595379abec774b0632041cf11dbf2594517f28e2b55c74cf1b9c7a65d85f568"
    "libx/libx11/libx11-dev_1.6.4-3ubuntu0.4_arm64.deb 4603112a9be89d7f33c701949917e5e8bea822781fc2e2c90ee7e413ba1b3f89"
    "libx/libxext/libxext6_1.3.3-1_arm64.deb e1fb4dc4abae8f1efc335b124745302ee0de3369c838bd209594a5acc4e9a0f5"
    "libx/libxext/libxext-dev_1.3.3-1_arm64.deb 18eb7c2293628cb1f0ca8140697f3fdfdf9c621063da35bc40a715cac169257c"
    "f/freetype/libfreetype6_2.8.1-2ubuntu2.2_arm64.deb 4dbb49b4dda1dbf2b7f01281641679339a86b5537bae61c0c3dfe2dcae49ffcd"
    "f/freetype/libfreetype6-dev_2.8.1-2ubuntu2.2_arm64.deb a49608fb0ad4b6d41696c788cced32cb0c2631e2cc0c23aa0b95f20911087c35"
    "x/xorgproto/x11proto-dev_2018.4-4_all.deb 1797fb12c67d037fb530e7203bf38a2c94e1e239bf654432506181c9d43bda01"
)

for t in curl python3 dpkg-deb sha256sum git tar; do
    command -v $t >/dev/null || die "the Linux side has no $t"
done
BASE=$MOD/azmods/BASE
[ -f "$BASE" ] && [ -f "$MOD/azmods/hebrew.patch" ] && [ -d "$MOD/azmods/files" ] || die "not the Hebrew mod repository: $MOD"
[ "$MODE" = community ] || [ -f "$OFFICIAL" ] || die "no official azmod.so at $OFFICIAL"
SRC_REPO=$(sed -n 's/^repo=//p' "$BASE") SRC_COMMIT=$(sed -n 's/^commit=//p' "$BASE")
echo "$SRC_COMMIT" | grep -qE '^[0-9a-f]{40}$' && [ -n "$SRC_REPO" ] || die "azmods/BASE names no repository and commit"
mkdir -p "$OUT" "$DL" || die "cannot create $OUT or $DL"

fetch() { # url sha256 file
    if [ -f "$3" ] && [ "$(sha256sum "$3" | cut -d' ' -f1)" = "$2" ]; then return 0; fi
    curl -sSfL --retry 3 -o "$3.part" "$1" || { rm -f "$3.part"; die "download failed: $1"; }
    [ "$(sha256sum "$3.part" | cut -d' ' -f1)" = "$2" ] || { rm -f "$3.part"; die "checksum mismatch: $1"; }
    mv -f "$3.part" "$3"
}

step 1 "downloading the toolchain (once, about 70 MB)"
fetch "$TC_URL" $TC_SHA "$DL/$TC.tar.bz2"
for d in "${DEBS[@]}"; do
    set -- $d
    fetch "$PORTS/$1" "$2" "$DL/${1##*/}"
done

CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/az-hebrew-launcher/toolchain
STAMP=$CACHE/ready-$TC_SHA
if [ ! -f "$STAMP" ]; then
    step 2 "unpacking the toolchain (once)"
    rm -rf "$CACHE" && mkdir -p "$CACHE/sysroot" || die "cannot create $CACHE"
    python3 - "$DL/$TC.tar.bz2" "$CACHE" <<'PY' || die "the toolchain did not unpack"
import sys, tarfile
t = tarfile.open(sys.argv[1])
if hasattr(tarfile, "tar_filter"):
    t.extractall(sys.argv[2], filter="tar")
else:
    t.extractall(sys.argv[2])
PY
    for d in "${DEBS[@]}"; do
        set -- $d
        dpkg-deb -x "$DL/${1##*/}" "$CACHE/sysroot" || die "cannot unpack ${1##*/}"
    done
    touch "$STAMP"
fi
B=$CACHE/$TC/bin
CC=$B/aarch64-linux-gcc
for t in gcc nm objcopy readelf; do
    [ -x "$B/aarch64-linux-$t" ] || die "the toolchain has no aarch64-linux-$t"
done
SR=$CACHE/sysroot/usr
NO_OUTLINE_ATOMICS=$(printf 'int main(void){return 0;}\n' | $CC -x c -mno-outline-atomics -c -o /dev/null - 2>/dev/null && echo -mno-outline-atomics)
# what Ubuntu 18.04's gcc turns on by itself, so the result matches an official build's
UBUNTU_DEFAULTS="-D_FORTIFY_SOURCE=2 -fstack-protector-strong -Wformat -Wformat-security -Wl,-z,relro -Wl,--as-needed"
newest_glibc() { "$B/aarch64-linux-readelf" -V "$1" | grep -oE 'GLIBC_[0-9.]+' | sort -t. -k1,1 -k2,2n -k3,3n -u | tail -1; }
glibc_ok() {
    case "$1" in GLIBC_2.1[0-9] | GLIBC_2.2[0-7] | "") return 0 ;; esac
    return 1
}

if [ "$MODE" = community ]; then
    # the AZ-MODS SDK template's flags (template/Makefile), plus FreeType's headers: the mod
    # reads FreeType's structures but links only what a deck allows a mod to link
    CM=$MOD/community
    ID=$(sed -n 's/^ *"id": *"\([a-z0-9-]*\)".*/\1/p' "$CM/azmod.json")
    [ -n "$ID" ] && [ -f "$CM/$ID.c" ] || die "community/azmod.json names no id with a $ID.c"
    step 3 "compiling the community mod $ID"
    W=$(mktemp -d) || die "no temporary directory"
    trap 'rm -rf "$W"' EXIT
    $CC $UBUNTU_DEFAULTS -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden -fno-omit-frame-pointer \
        -fexceptions -fasynchronous-unwind-tables $NO_OUTLINE_ATOMICS -I"$CM/include" \
        -I"$MOD/azmods/files/src/azmod" -I"$SR/include/freetype2" -idirafter "$SR/include" \
        -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu \
        -o "$W/$ID.so" "$CM/$ID.c" -ldl > "$W/compile.log" 2>&1 || { cat "$W/compile.log"; die "the compiler stopped (above)"; }
    step 5 "checking the result"
    newest=$(newest_glibc "$W/$ID.so")
    glibc_ok "$newest" || die "the mod needs $newest, newer than the deck's glibc allows"
    exports=$("$B/aarch64-linux-nm" -D --defined-only "$W/$ID.so" | awk '{print $3}' |
        grep -vxE '_init|_fini|_edata|_end|__bss_start|__bss_start__|__bss_end__|_bss_end__|__end__' | tr '\n' ' ')
    [ "$exports" = "az_mod_v1_entry " ] || die "the mod exports more than az_mod_v1_entry: $exports"
    for n in $("$B/aarch64-linux-readelf" -d "$W/$ID.so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        case "$n" in libc.so.6 | libm.so.6 | libdl.so.2 | libpthread.so.0 | librt.so.1 | libgcc_s.so.1 | \
            libstdc++.so.6 | ld-linux-aarch64.so.1) ;; *) die "the mod links $n, which a deck refuses" ;; esac
    done
    cp -f "$W/$ID.so" "$OUT/$ID.so.new" && mv -f "$OUT/$ID.so.new" "$OUT/$ID.so" || die "cannot write $OUT/$ID.so"
    step done "$(sha256sum "$OUT/$ID.so" | cut -d' ' -f1) $(stat -c %s "$OUT/$ID.so") $newest"
    exit 0
fi

step 3 "fetching AZ-MODS ${SRC_COMMIT:0:7} and adding the Hebrew mod"
GIT=${XDG_CACHE_HOME:-$HOME/.cache}/az-hebrew-launcher/azmods.git
[ -d "$GIT" ] || git init -q --bare "$GIT" || die "cannot create $GIT"
git -C "$GIT" cat-file -e "$SRC_COMMIT^{commit}" 2>/dev/null ||
    git -C "$GIT" fetch -q --depth 1 "$SRC_REPO" "$SRC_COMMIT" || die "cannot fetch AZ-MODS $SRC_COMMIT from $SRC_REPO"
W=$(mktemp -d) || die "no temporary directory"
trap 'rm -rf "$W"' EXIT
REPO=$W/tree
mkdir -p "$REPO" && git -C "$GIT" archive "$SRC_COMMIT" | tar -x -C "$REPO" || die "cannot unpack AZ-MODS"
cp -r "$MOD/azmods/files/." "$REPO/" || die "cannot add the mod's files"
(cd "$REPO" && git apply --binary "$MOD/azmods/hebrew.patch") > "$W/patch.log" 2>&1 ||
    { cat "$W/patch.log"; die "azmods/hebrew.patch does not apply to AZ-MODS ${SRC_COMMIT:0:7}"; }

# The compile lines below are src/Makefile's. If those lines change, so must this script.
grep -qxF 'AZMOD_SRCS := azmod/azmod.c azmod/menu.c azmod/text.c azmod/azconf.c' "$REPO/src/Makefile" &&
    grep -qF '$(CC) $(AZMOD_CFLAGS) -o $@.full $(AZMOD_SRCS) -ldl -lpthread -lX11 -lXext -lfreetype -lm' "$REPO/src/Makefile" &&
    grep -qF 'AZMOD_CFLAGS := $(AZ_UNWIND) -O2 $(AZ_WARN) -fPIC -shared -static-libgcc -Wl,--no-undefined $(NO_OUTLINE_ATOMICS) -Iazmod $(AZMOD_FT_CFLAGS)' "$REPO/src/Makefile" ||
    die "src/Makefile's azmod lines changed: update launcher/build-azmod.sh to match"
python3 "$REPO/tools/extract-assets.py" "$OFFICIAL" > "$W/assets.log" 2>&1 ||
    { cat "$W/assets.log"; die "the official azmod.so is not the release this source is (tools/extract-assets.py)"; }

step 4 "compiling azmod.so (about a minute)"
cd "$REPO/src" || die "no src"
mkdir -p out
AZMOD_CFLAGS="-fexceptions -fasynchronous-unwind-tables -O2 -Wall -Wextra -Werror -fPIC -shared -static-libgcc -Wl,--no-undefined $NO_OUTLINE_ATOMICS -Iazmod -I$SR/include/freetype2 -DAZMOD_RELEASE"
$CC $UBUNTU_DEFAULTS $AZMOD_CFLAGS -idirafter "$SR/include" -L"$SR/lib/aarch64-linux-gnu" -o out/azmod.so.full \
    azmod/azmod.c azmod/menu.c azmod/text.c azmod/azconf.c -ldl -lpthread -lX11 -lXext -lfreetype -lm \
    > "$W/compile.log" 2>&1
rc=$?
grep -v 'needed by .*libX11.so.*not found\|try using -rpath or -rpath-link' "$W/compile.log"
[ $rc = 0 ] || die "the compiler stopped (above)"

step 5 "checking the result"
# src/Makefile's release strip: only the asset arrays keep their symbols
"$B/aarch64-linux-nm" --defined-only out/azmod.so.full | awk '$2 ~ /^[rRdD]$/ {print $3}' | sort -u > out/objs
grep -ohE '[A-Za-z_][A-Za-z0-9_]*' azmod/caption_strips.h azmod/fxart.h azmod/fxhdr.h azmod/fxbars.h \
    azmod/menulabels.h azmod/stems4art.h azmod/srcref.h azmod/wavref.h azmod/onelibkey.h azmod/onelibkey_sm.h |
    sort -u | comm -12 - out/objs > out/keep
"$B/aarch64-linux-objcopy" --strip-unneeded --keep-symbols=out/keep out/azmod.so.full out/azmod.so || die "strip failed"
newest=$(newest_glibc out/azmod.so)
glibc_ok "$newest" || die "the build needs $newest, newer than the deck's glibc allows"
"$B/aarch64-linux-nm" -D --defined-only out/azmod.so | awk '{print $3}' | LC_ALL=C sort > out/exports
diff out/exports "$REPO/tests/unit/exports-azmod.txt" > out/exports.diff ||
    { cat out/exports.diff; die "the build exports other symbols than tests/unit/exports-azmod.txt"; }
cp -f out/azmod.so "$OUT/azmod.so.new" && mv -f "$OUT/azmod.so.new" "$OUT/azmod.so" || die "cannot write $OUT/azmod.so"
step done "$(sha256sum "$OUT/azmod.so" | cut -d' ' -f1) $(stat -c %s "$OUT/azmod.so") $newest"
