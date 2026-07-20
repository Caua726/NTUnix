#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WINE=${WINE:-wine}

make -C "$ROOT" out/system/bin/ntwm.exe out/system/bin/dispd.exe \
    out/system/bin/ntbar.exe out/system/bin/ntwmctl.exe stage-files

WINROOT=$(printf '%s' "$ROOT/out" | sed 's|/|\\|g')
NTUNIX_ROOT="Z:$WINROOT" WINEDEBUG=-all "$WINE" \
    "$ROOT/out/system/bin/ntwm.exe" --check-config
WINEDEBUG=-all "$WINE" "$ROOT/out/system/bin/ntwm.exe" --selftest
DISPD_GEOMETRY_SELFTEST=1 WINEDEBUG=-all "$WINE" \
    "$ROOT/out/system/bin/dispd.exe"
WINEDEBUG=-all "$WINE" "$ROOT/out/system/bin/ntbar.exe" --selftest

TMP=${TMPDIR:-/tmp}/ntunix-config-check-$$
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP/etc/ntwm"
printf '%s\n' '[general]' 'layout=nao-existe' > "$TMP/etc/ntwm/ntwm.conf"
WINTMP=$(printf '%s' "$TMP" | sed 's|/|\\|g')
if NTUNIX_ROOT="Z:$WINTMP" WINEDEBUG=-all "$WINE" \
    "$ROOT/out/system/bin/ntwm.exe" --check-config; then
    echo "desktop-check: parser aceitou configuracao invalida" >&2
    exit 1
fi

echo "desktop-check: ok"
