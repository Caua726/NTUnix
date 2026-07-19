#!/usr/bin/env bash
# ============================================================================
# vm-setup.sh — define a VM 'ntunix' no libvirt (qemu:///session, sem sudo).
#
#   ./build/vm-setup.sh install [iso]   instala o Windows no disco (APAGA o disco)
#   ./build/vm-setup.sh run             boota do disco, com a arvore do host montada
#   ./build/vm-setup.sh live [iso]      boota a ISO live (WinPE na RAM) — modo antigo
#
# Por que instalado e' melhor que live: o WinPE extrai o boot.wim inteiro pra um
# ramdisk (X:), entao a ISO ocupa RAM enquanto roda. Instalado, o Windows pagina
# do disco e a mesma RAM rende mais.
#
# O SHARE: o QEMU sobe um servidor SMB (samba do host) no SLIRP, em 10.0.2.4.
# O guest monta \\10.0.2.4\qemu e ve a arvore do host AO VIVO — `make` no host,
# `ntctl restart dispd` no guest, sem regerar ISO nem reiniciar a VM.
# Escolhemos SMB e nao virtiofs porque o cliente SMB ja existe no Windows: nao
# ha driver pra instalar, e portanto nao ha muro de assinatura (a mesma razao
# pela qual o canal de debug e' rede e nao serial — ver docs/canal-debug-vm.md).
# ============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CONN="qemu:///session"
NAME="${VM_NAME:-ntunix}"
DISK="$REPO/build/vm/ntunix.qcow2"
DISK_SIZE="${NTUNIX_DISK_SIZE:-40G}"
RAM="${NTUNIX_VM_RAM:-2048}"
VCPUS="${NTUNIX_VM_CPUS:-2}"
SHARE="${NTUNIX_SHARE:-$REPO/out}"
DBGPORT="${NTUNIX_DBG_PORT:-2323}"

MODE="${1:-run}"
ISO="${2:-}"

die()  { printf '\033[31merro:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\033[36m==>\033[0m %s\n' "$*"; }

command -v virt-install >/dev/null || die "falta virt-install"
command -v smbd >/dev/null || die "falta samba (smbd) — o share SMB do QEMU precisa dele"

# O sandbox seccomp do libvirt impede o QEMU de dar fork no smbd — o share falha
# EM SILENCIO (a VM sobe, \\10.0.2.4\qemu simplesmente nao responde). Precisa de
# seccomp_sandbox=0 no qemu.conf da sessao do usuario.
QEMU_CONF="${XDG_CONFIG_HOME:-$HOME/.config}/libvirt/qemu.conf"
if ! grep -qE '^\s*seccomp_sandbox\s*=\s*0' "$QEMU_CONF" 2>/dev/null; then
    mkdir -p "$(dirname "$QEMU_CONF")"
    {
        echo '# NTUnix: o servidor SMB embutido do QEMU precisa dar fork no smbd,'
        echo '# o que o sandbox seccomp bloqueia. Sem isto o share nao sobe.'
        echo 'seccomp_sandbox = 0'
    } >> "$QEMU_CONF"
    step "seccomp_sandbox=0 adicionado em $QEMU_CONF (necessario pro share SMB)"
fi

mkdir -p "$REPO/build/vm"
[ -f "$DISK" ] || { step "criando disco $DISK_SIZE"; qemu-img create -f qcow2 "$DISK" "$DISK_SIZE" >/dev/null; }
[ -d "$SHARE" ] || die "nada pra compartilhar em $SHARE — rode 'make' antes"

# Rede + share num netdev so. O libvirt nao expoe smb= na rede 'user', entao a
# interface inteira vem por qemu:commandline (por isso --network none).
#   smb=<dir>          diretorio servido        -> \\10.0.2.4\qemu
#   guest 10.0.2.15, gateway/host 10.0.2.2, smb 10.0.2.4 (tudo dentro do SLIRP)
QEMU_NET="-netdev user,id=ntu0,smb=$SHARE,smbserver=10.0.2.4 -device e1000e,netdev=ntu0"

# idempotente: destroi e redefine
virsh -c "$CONN" destroy  "$NAME" 2>/dev/null || true
virsh -c "$CONN" undefine --nvram "$NAME" 2>/dev/null || true

common_args=(
    --connect "$CONN"
    --name "$NAME"
    --memory "$RAM" --vcpus "$VCPUS"
    --cpu host-passthrough
    --boot uefi
    --osinfo require=off --os-variant win11
    --graphics spice --video qxl --sound none
    --network none
    --qemu-commandline="$QEMU_NET"
    --noautoconsole
)

case "$MODE" in
install)
    ISO="${ISO:-$REPO/NTUnix.iso}"
    [ -f "$ISO" ] || die "ISO instalavel nao encontrada: $ISO — rode 'make iso' antes"
    step "ATENCAO: o disco $DISK sera APAGADO e reparticionado pelo autounattend"
    step "instalando de $ISO (desatendido; nao interaja com o Setup)"
    virt-install "${common_args[@]}" \
        --disk path="$DISK",bus=virtio,boot.order=2 \
        --disk path="$ISO",device=cdrom,bus=sata,boot.order=1
    echo
    echo "O Setup roda sozinho e reinicia algumas vezes. Acompanhe:"
    echo "    virt-viewer --connect $CONN $NAME"
    echo "Quando cair no desktop do NTUnix, troque pro modo de disco:"
    echo "    ./build/vm-setup.sh run      (nao boota mais da ISO)"
    ;;
run)
    [ "$(qemu-img info --output=json "$DISK" | grep -o '"actual-size": [0-9]*' | grep -o '[0-9]*')" -gt 104857600 ] \
        || die "o disco parece vazio — rode './build/vm-setup.sh install' primeiro"
    step "boot do disco, share SMB de $SHARE"
    virt-install "${common_args[@]}" \
        --disk path="$DISK",bus=virtio,boot.order=1 \
        --import
    ;;
live)
    ISO="${ISO:-$REPO/NTUnix-live.iso}"
    [ -f "$ISO" ] || die "ISO live nao encontrada: $ISO — rode 'make live' antes"
    step "boot da ISO live (WinPE na RAM; usa mais RAM que o modo instalado)"
    virt-install "${common_args[@]}" \
        --disk path="$DISK",bus=virtio,boot.order=2 \
        --disk path="$ISO",device=cdrom,bus=sata,boot.order=1
    ;;
*)
    die "modo desconhecido '$MODE' — use: install | run | live"
    ;;
esac

cat <<EOF

VM '$NAME' definida em $CONN  (${RAM}MB, ${VCPUS} vCPU)
  share:  $SHARE  ->  \\\\10.0.2.4\\qemu   (no guest: net use Z: \\\\10.0.2.4\\qemu)
  debug:  guest conecta em 10.0.2.2:${DBGPORT}; escute com  nc -l ${DBGPORT}
  abrir:  virt-viewer --connect $CONN $NAME
EOF
