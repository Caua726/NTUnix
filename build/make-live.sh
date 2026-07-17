#!/usr/bin/env bash
# ============================================================================
# make-live.sh — constroi a ISO LIVE do NTUnix (arch-be-like): boota direto
# no user space NTUnix rodando na RAM, SEM instalar, SEM o setup do Windows.
#
#   ./build/make-live.sh <windows-oficial.iso> [saida.iso]
#
# Base = o WinPE (indice 1 do boot.wim) da ISO oficial que o usuario traz.
# NAO redistribui bits da Microsoft (o usuario fornece a ISO). O que faz:
#   1. extrai a ISO de origem, DESCARTANDO install.wim (nao instalamos);
#   2. do boot.wim, joga fora o indice 2 (Windows Setup — o instalador);
#   3. injeta a arvore do NTUnix em X:\NTUnix dentro do WinPE;
#   4. troca o shell do WinPE por winpeshl.ini → wpeinit + ntsession;
#   5. remove a experiencia de instalacao (setup.exe) da midia;
#   6. reempacota uma ISO hibrida bootavel (BIOS + UEFI).
#
# Deps: wimlib-imagex, xorriso, 7z. Roda em Linux, sem Windows.
# ============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC_ISO="${1:-}"
OUT_ISO="${2:-$REPO/NTUnix-live.iso}"
WORK="${NTUNIX_WORK:-$REPO/build/work}"
IMG="$WORK/live-iso"
WIM="$IMG/sources/boot.wim"

die()  { printf '\033[31merro:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\033[36m==>\033[0m %s\n' "$*"; }

[ -n "$SRC_ISO" ] || die "uso: $0 <windows-oficial.iso> [saida.iso]"
[ -f "$SRC_ISO" ] || die "ISO de origem nao encontrada: $SRC_ISO"
[ -d "$REPO/out/system/bin" ] || die "rode 'make' antes (out/ ausente)"
for t in wimlib-imagex xorriso 7z; do
    command -v "$t" >/dev/null || die "falta '$t'"
done

# --- 1. extrair a ISO, pulando o install.wim (7GB, nao usado no live) ------
step "extraindo $SRC_ISO (sem install.wim)"
rm -rf "$IMG"; mkdir -p "$IMG"
7z x -y -o"$IMG" "$SRC_ISO" -x'!sources/install.wim' -x'!sources/install.esd' >/dev/null
chmod -R u+w "$IMG"
[ -f "$WIM" ] || die "sources/boot.wim ausente na ISO"

# --- 2. boot.wim: descartar o indice 2 (Windows Setup) ---------------------
nidx=$(wimlib-imagex info "$WIM" | awk '/^Image Count:/{print $NF}')
if [ "$nidx" -ge 2 ]; then
    step "removendo o indice 2 do boot.wim (Windows Setup / instalador)"
    # exporta so o WinPE (indice 1) pra um wim novo, ja marcado como bootavel
    wimlib-imagex export "$WIM" 1 "$WIM.new" --boot --compress=LZX >/dev/null
    mv "$WIM.new" "$WIM"
fi
step "boot.wim agora: $(wimlib-imagex info "$WIM" | awk -F': +' '/^Name:/{print $2}')"

# --- 3+4. injetar NTUnix e trocar o shell do WinPE -------------------------
STAGE="$WORK/live-stage"; rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -a "$REPO/out/." "$STAGE/"
step "injetando \\NTUnix e winpeshl.ini no WinPE"
wimlib-imagex update "$WIM" 1 --command "add '$STAGE' '\\NTUnix'" >/dev/null
wimlib-imagex update "$WIM" 1 \
    --command "add '$REPO/build/winpeshl.ini' '\\Windows\\System32\\winpeshl.ini'" >/dev/null

# --- 5. remover a experiencia de instalacao da midia -----------------------
step "removendo setup.exe e autorun da midia (nao instalamos)"
rm -f "$IMG/setup.exe" "$IMG/sources/setup.exe" "$IMG/autorun.inf" 2>/dev/null || true

# --- 6. reempacotar ISO hibrida bootavel -----------------------------------
BIOS="boot/etfsboot.com"
EFI=""
for c in efi/microsoft/boot/efisys_noprompt.bin efi/microsoft/boot/efisys.bin; do
    [ -f "$IMG/$c" ] && { EFI="$c"; break; }
done
[ -f "$IMG/$BIOS" ] || die "boot/etfsboot.com ausente"
[ -n "$EFI" ]       || die "efisys*.bin ausente"

step "gerando $OUT_ISO (BIOS=$BIOS EFI=$EFI)"
xorriso -as mkisofs \
    -iso-level 4 -rock \
    -volid "NTUNIX_LIVE" \
    -disable-deep-relocation -untranslated-filenames \
    -b "$BIOS" -no-emul-boot -boot-load-size 8 -hide boot.catalog \
    -eltorito-alt-boot -eltorito-platform efi \
    -b "$EFI" -no-emul-boot \
    -o "$OUT_ISO" "$IMG"

printf '\033[32mpronto:\033[0m %s (%s)\n' "$OUT_ISO" "$(du -h "$OUT_ISO" | cut -f1)"
echo "teste:  ./build/test-vm.sh \"$OUT_ISO\"   (boota direto no ntsession, sem instalar)"
