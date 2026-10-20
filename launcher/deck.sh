#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# shellcheck shell=busybox
# deck.sh MODE SHA256: the Hebrew launcher's step on the deck, as root. The launcher copies it
# to /tmp over SSH and starts it with systemd-run: the loader's SSH hook restarts dropbear on
# every pass, which would end a script run inside the SSH session. Progress lines ("@@ ...")
# go to /tmp/az-hebrew-launcher.log, which the launcher reads over SSH.
#
#   apply    let this deck load builds the release key did not sign (the developer override,
#            BUILDING.md), have the loader stage the stick's azmod.so (it must be SHA256),
#            turn the Hebrew row on in azmod.conf while EP147 is stopped (docs/USB-STICK.md:
#            edit it only then), start EP147, and wait for the Hebrew hooks in the hook table
#   restore  have the loader stage the stick's azmod.so (the official one, SHA256), start
#            EP147 again and remove the developer override
#
# AZH_* override the deck paths for the tests only.
set -u
MODE=${1:-} WANT=${2:-}
S=${AZH_SETTINGS:-/home/root/settings}
HOOKS=${AZH_HOOKTABLE:-/tmp/azmod-hooktable}
REL=${AZH_RELEASE:-/etc/az-mods-release}
LOG=${AZH_LOG:-/tmp/az-hebrew-launcher.log}
WAIT=${AZH_WAIT:-1}
exec >> "$LOG" 2>&1
say() { echo "@@ $*"; }
fail() { say error "$*"; exit 1; }

case "$MODE" in apply | restore) ;; *) fail "usage" ;; esac
echo "$WANT" | grep -qE '^[0-9a-f]{64}$' || fail "usage"
[ -d "$S" ] && [ -f "$REL" ] || fail "not-azmods"
say info "firmware $(tr -d '\r\n' < "$REL")"

staged() { sha256sum "$S/az-usbload/azmod.so" 2>/dev/null | cut -d' ' -f1; }

[ "$MODE" = apply ] && { : > "$S/azmods-unsigned.on" || fail "settings-readonly"; }
say step stage
systemctl restart az-usbload.service
i=0
while [ "$(staged)" != "$WANT" ]; do
    i=$((i + 1))
    [ $i -ge 90 ] && fail "stage-timeout"
    sleep "$WAIT"
done

say step restart
systemctl stop EP147.service
CONF=skip
C=$S/azmod.conf
if [ "$MODE" = apply ] && [ -f "$C" ] &&
    grep -q '^[[:space:]]*version[[:space:]]*=[[:space:]]*1[[:space:]]*$' "$C"; then
    if grep -q '^[[:space:]]*hebrew[[:space:]]*=' "$C"; then
        sed 's/^[[:space:]]*hebrew[[:space:]]*=.*$/hebrew = on/' "$C" > "$C.az-hebrew"
    else
        { cat "$C"; [ -n "$(tail -c 1 "$C")" ] && echo; echo 'hebrew = on'; } > "$C.az-hebrew"
    fi && sync && mv -f "$C.az-hebrew" "$C" && sync && CONF=on
    rm -f "$C.az-hebrew"
fi
[ "$MODE" = restore ] && rm -f "$S/azmods-unsigned.on"
rm -f "$HOOKS"
systemctl start EP147.service
[ "$MODE" = restore ] && { say done restored; exit 0; }

say step verify
i=0
while ! grep -qE '(island|16byte) .* heb_line_hook$' "$HOOKS" 2>/dev/null; do
    i=$((i + 1))
    [ $i -ge 75 ] && { say done nohooks "$CONF"; exit 0; }
    sleep "$WAIT"
done
sleep "$WAIT"
KBD=$(grep -cE '(island|16byte) .* hk_[a-z]+_hook$' "$HOOKS")
say done ok "$CONF" "$KBD"
