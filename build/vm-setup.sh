#!/usr/bin/env bash
# ============================================================================
# vm-setup.sh — define/atualiza a VM 'ntunix-live' no libvirt (user session)
# apontando pra NTUnix-live.iso, UEFI, SPICE. NAO precisa de sudo nem do
# libvirtd de sistema: qemu:///session roda como o proprio usuario.
#
#   ./build/vm-setup.sh [caminho/para.iso]
#
# Depois: abra o virt-manager, conecte em "QEMU/KVM User session" e de play
# na VM 'ntunix-live'  (ou:  virt-viewer --connect qemu:///session ntunix-live)
# ============================================================================
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${1:-$REPO/NTUnix-live.iso}"
CONN="qemu:///session"
NAME="ntunix-live"
DISK="$REPO/build/vm/ntunix.qcow2"

[ -f "$ISO" ] || { echo "ISO nao encontrada: $ISO — rode 'make live' antes"; exit 1; }
mkdir -p "$REPO/build/vm"
# disco de 40G (nao usado pelo live, mas pronto pro instalador futuro)
[ -f "$DISK" ] || qemu-img create -f qcow2 "$DISK" 40G >/dev/null

# canal de debug serial (DEV): NTUNIX_DEBUG=1 liga uma serial PCI do guest a um
# socket TCP do host, pra conectar num shell da NTUnix sem depender da rede da VM.
# PCI (nao ISA) porque o WinPE UEFI so enumera a serial via PCI de forma confiavel
# (a ISA nao aparece — sem NTDETECT.COM no boot UEFI). So em dev.
SERIAL_ARGS=()
DBGPORT="${NTUNIX_DBG_PORT:-4555}"
if [ "${NTUNIX_DEBUG:-}" = "1" ]; then
    # addr=0x08: slot PCI livre no bus 0 (0x01/02/03/1f ja usados por video/root-ports/lpc)
    SERIAL_ARGS=(--qemu-commandline="-chardev socket,id=dbgser,host=127.0.0.1,port=${DBGPORT},server=on,wait=off -device pci-serial,chardev=dbgser,addr=0x08")
fi

# idempotente: destrói e redefine a VM
virsh -c "$CONN" destroy  "$NAME" 2>/dev/null || true
virsh -c "$CONN" undefine --nvram "$NAME" 2>/dev/null || true

virt-install \
    --connect "$CONN" \
    --name "$NAME" \
    --memory 2048 --vcpus 2 \
    --cpu host-passthrough \
    --boot uefi \
    --disk path="$DISK",bus=virtio \
    --cdrom "$ISO" \
    --osinfo require=off --os-variant win11 \
    --graphics spice \
    --video qxl \
    --sound none \
    --network user \
    "${SERIAL_ARGS[@]}" \
    --noautoconsole

if [ "${NTUNIX_DEBUG:-}" = "1" ]; then
    echo
    echo "[dev] console serial de debug ligado na COM1 -> TCP 127.0.0.1:${DBGPORT}"
    echo "      quando a VM bootar, conecte com:  nc 127.0.0.1 ${DBGPORT}"
    echo "      (ou: socat -,raw,echo=0 TCP:127.0.0.1:${DBGPORT})"
fi

echo
echo "VM '$NAME' definida em $CONN."
echo "Abra:  virt-manager   (conexao 'QEMU/KVM User session' -> ntunix-live -> play)"
echo "Ou:    virt-viewer --connect $CONN $NAME"
