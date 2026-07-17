#!/usr/bin/env bash
# test-vm.sh — sobe a ISO do NTUnix numa VM UEFI (QEMU/OVMF) para verificar
# que boota, instala e cai no ntsession sem intervencao.
#
#   ./build/test-vm.sh NTUnix.iso [disco.qcow2]
#
# Nao valida automaticamente (setup do Windows e interativo/demorado); serve
# pra inspecao manual. Precisa de qemu + OVMF (edk2-ovmf).
set -euo pipefail
ISO="${1:?uso: $0 <iso> [disco.qcow2]}"
DISK="${2:-/tmp/ntunix-test.qcow2}"

OVMF=""
for c in /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
         /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$c" ] && { OVMF="$c"; break; }
done
[ -n "$OVMF" ] || { echo "OVMF nao encontrado (pacman -S edk2-ovmf)"; exit 1; }

[ -f "$DISK" ] || qemu-img create -f qcow2 "$DISK" 40G >/dev/null

exec qemu-system-x86_64 \
    -machine q35,accel=kvm -cpu host -smp 4 -m 4096 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive file="$DISK",if=virtio,format=qcow2 \
    -cdrom "$ISO" -boot d \
    -vga std -display gtk
