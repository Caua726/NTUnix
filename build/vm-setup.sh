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

# canal de debug via REDE (DEV): a NIC e1000e (--network user, driver inbox do WinPE)
# ja da 10.0.2.15 ao guest, com o host em 10.0.2.2 (gateway do SLIRP). O ntdbgcon
# faz reverse-connect pra 10.0.2.2:2323, entao NAO precisa de hostfwd nem de nenhum
# qemu:commandline — o SLIRP encaminha guest->10.0.2.2 pro host. So em dev.
DBGPORT="${NTUNIX_DBG_PORT:-2323}"

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
    --network user,model=e1000e \
    --noautoconsole

if [ "${NTUNIX_DEBUG:-}" = "1" ]; then
    echo
    echo "[dev] canal de debug via rede (reverse shell). Quando a VM bootar, escute no host:"
    echo "      nc -lv 127.0.0.1 ${DBGPORT}"
    echo "      o guest conecta em 10.0.2.2:${DBGPORT} e cai num shell da NTUnix."
fi

echo
echo "VM '$NAME' definida em $CONN."
echo "Abra:  virt-manager   (conexao 'QEMU/KVM User session' -> ntunix-live -> play)"
echo "Ou:    virt-viewer --connect $CONN $NAME"
