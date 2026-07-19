#!/usr/bin/env bash
# ============================================================================
# make-iso.sh — trata uma ISO oficial do Windows e produz a ISO do NTUnix.
#
#   ./build/make-iso.sh <windows-oficial.iso> [saida.iso]
#
# O que faz, sem NUNCA redistribuir bits da Microsoft (o usuario traz a ISO):
#   1. extrai a ISO de origem para uma arvore de trabalho;
#   2. normaliza install.esd → install.wim (se necessario);
#   3. injeta a arvore do NTUnix (out/) em \NTUnix dentro da imagem;
#   4. remove o user space padrao listado em build/strip.list;
#   5. planta autounattend.xml + SetupComplete.cmd (troca de shell/servicos);
#   6. reempacota uma ISO hibrida bootavel (BIOS + UEFI) com xorriso.
#
# Deps: wimlib-imagex, xorriso, 7z (ou bsdtar). Roda em Linux, sem Windows.
# ============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC_ISO="${1:-}"
OUT_ISO="${2:-$REPO/NTUnix.iso}"
WORK="${NTUNIX_WORK:-$REPO/build/work}"
IMG="$WORK/iso"          # arvore da ISO
WIM="$IMG/sources/install.wim"

die()  { printf '\033[31merro:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\033[36m==>\033[0m %s\n' "$*"; }

[ -n "$SRC_ISO" ] || die "uso: $0 <windows-oficial.iso> [saida.iso]"
[ -f "$SRC_ISO" ] || die "ISO de origem nao encontrada: $SRC_ISO"
[ -d "$REPO/out/system/bin" ] || die "rode 'make' antes (out/ ausente)"
for t in wimlib-imagex xorriso; do
    command -v "$t" >/dev/null || die "falta '$t' (pacman -S wimlib xorriso)"
done
EXTRACT=""
command -v 7z    >/dev/null && EXTRACT=7z
[ -z "$EXTRACT" ] && command -v bsdtar >/dev/null && EXTRACT=bsdtar
[ -n "$EXTRACT" ] || die "falta 7z ou bsdtar para extrair a ISO"

# --- 1. extrair a ISO de origem -------------------------------------------
step "limpando work e extraindo $SRC_ISO"
rm -rf "$WORK"; mkdir -p "$IMG"
if [ "$EXTRACT" = 7z ]; then
    7z x -y -o"$IMG" "$SRC_ISO" >/dev/null
else
    bsdtar -C "$IMG" -xf "$SRC_ISO"
fi
chmod -R u+w "$IMG"
[ -d "$IMG/sources" ] || die "arvore inesperada: sem \\sources (essa ISO tem setup do Windows?)"

# --- 2. install.esd → install.wim -----------------------------------------
if [ ! -f "$WIM" ] && [ -f "$IMG/sources/install.esd" ]; then
    step "convertendo install.esd → install.wim (todas as edicoes)"
    ESD="$IMG/sources/install.esd"
    n=$(wimlib-imagex info "$ESD" | awk '/Image Count:/{print $NF}')
    for i in $(seq 1 "$n"); do
        wimlib-imagex export "$ESD" "$i" "$WIM" --compress=LZX >/dev/null
    done
    rm -f "$ESD"
fi
[ -f "$WIM" ] || die "sem sources/install.wim nem install.esd"

# quais edicoes tratar (padrao: todas)
# ANCORAR o '^': o cabecalho do WIM tem um campo 'Boot Index:' que, sem a
# ancora, entrava na lista como se fosse a edicao 0 — e indice de WIM comeca
# em 1. Passava despercebido porque o loop de delete engole o erro.
EDITIONS="${NTUNIX_EDITIONS:-$(wimlib-imagex info "$WIM" | awk '/^Index:/{print $NF}')}"
step "edicoes a tratar: $(echo $EDITIONS | tr '\n' ' ')"

# --- 3+4+5. por edicao: injetar NTUnix, stripar, plantar hooks ------------
STAGE="$WORK/stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/NTUnix" \
    "$STAGE/Windows/Setup/Scripts" "$STAGE/Windows/Panther/Unattend"
cp -a "$REPO/out/." "$STAGE/NTUnix/"
cp "$REPO/build/SetupComplete.cmd" "$STAGE/Windows/Setup/Scripts/SetupComplete.cmd"

# audit #119: a senha do usuario 'ntunix' vem da variavel de build NTUNIX_PASSWORD
# (nao mais hardcoded no XML). Sem ela, cai no default 'ntunix' com AVISO alto.
NTUNIX_PW="${NTUNIX_PASSWORD:-ntunix}"
if [ "$NTUNIX_PW" = ntunix ]; then
    step "AVISO seguranca: usando senha padrao 'ntunix' — defina NTUNIX_PASSWORD=<senha> no build"
fi
# Nome EXATO da edicao que o Setup vai aplicar. Um nome errado (ou com o padding
# que o wimlib imprime) faz o Setup parar pedindo a edicao na mao.
ed_name() {   # $1 = indice -> nome, sem espaco nas pontas
    wimlib-imagex info "$WIM" "$1" \
        | sed -n 's/^Name:[[:space:]]*//p' | head -1 \
        | sed 's/[[:space:]]*$//'
}
# Padrao: preferir "Pro" (que e' a edicao a que a chave abaixo corresponde);
# so cai na primeira da lista se a imagem nao tiver Pro.
if [ -z "${NTUNIX_EDITION:-}" ]; then
    NTUNIX_ED=""
    for idx in $EDITIONS; do
        n="$(ed_name "$idx")"
        case "$n" in *" Pro") NTUNIX_ED="$n"; break ;; esac
    done
    [ -n "$NTUNIX_ED" ] || NTUNIX_ED="$(ed_name "$(echo $EDITIONS | awk '{print $1}')")"
else
    NTUNIX_ED="$NTUNIX_EDITION"
fi

# O Setup precisa de ALGUMA chave pra nao parar na tela de licenca: com <Key>
# vazio ele erra em vez de pular. Usamos a GVLK (KMS client key) que a propria
# Microsoft publica em
#   learn.microsoft.com/windows-server/get-started/kms-client-activation-keys
# Ela apenas SELECIONA A EDICAO. Nao ativa nada (sem um host KMS na rede nao
# ativa mesmo) e nao substitui licenca: a licenca continua sendo a sua, como o
# resto do projeto assume. Sobrescreva com NTUNIX_KEY=<sua chave>.
#
# A CHAVE TEM QUE CASAR COM A EDICAO, senao o Setup para.
case "$NTUNIX_ED" in
    *"Pro for Workstations") DEFKEY=NRG8B-VKK3Q-CXVCJ-9G2XF-6Q84J ;;
    *"Pro Education")        DEFKEY=6TP4R-GNPTD-KYYHQ-7B7DP-J447Y ;;
    *"Education")            DEFKEY=NW6C2-QMPVW-D7KKK-3GKT6-VCFB2 ;;
    *"Enterprise")           DEFKEY=NPPR9-FWDCX-D2C8J-H872K-2YT43 ;;
    *"Pro")                  DEFKEY=W269N-WFGWX-YVC9B-4J6C9-T83GX ;;
    *)                       DEFKEY="" ;;
esac
NTUNIX_KEY="${NTUNIX_KEY:-$DEFKEY}"
[ -n "$NTUNIX_KEY" ] || die "sem chave GVLK conhecida pra edicao '$NTUNIX_ED' —
  passe NTUNIX_KEY=<chave> ou NTUNIX_EDITION=<outra edicao>.
  edicoes nesta imagem: $(for i in $EDITIONS; do printf '%s; ' "$(ed_name "$i")"; done)"
step "instalacao desatendida: edicao '$NTUNIX_ED' no disco 0 (o disco sera APAGADO)"

UNATTEND="$WORK/unattend.rendered.xml"
sed -e "s|@NTUNIX_PW@|$NTUNIX_PW|g" \
    -e "s|@NTUNIX_EDITION@|$NTUNIX_ED|g" \
    -e "s|@NTUNIX_KEY@|$NTUNIX_KEY|g" \
    "$REPO/build/autounattend.xml" > "$UNATTEND"
cp "$UNATTEND" "$STAGE/Windows/Panther/Unattend/unattend.xml"

# lista de deletes a partir do strip.list
mapfile -t DEL < <(grep -vE '^\s*(#|$)' "$REPO/build/strip.list")

for idx in $EDITIONS; do
    name=$(wimlib-imagex info "$WIM" "$idx" | awk -F': ' '/^Name:/{print $2; exit}')
    step "edicao $idx ($name): removendo user space padrao"
    for path in "${DEL[@]}"; do
        wimlib-imagex update "$WIM" "$idx" --command \
            "delete --force --recursive '$(echo "$path" | tr '/' '\\')'" >/dev/null 2>&1 || true
    done
    step "edicao $idx ($name): injetando NTUnix + hooks"
    # wimlib-imagex update aceita so um --command por invocacao
    wimlib-imagex update "$WIM" "$idx" \
        --command "add '$STAGE/NTUnix' '\\NTUnix'" >/dev/null
    wimlib-imagex update "$WIM" "$idx" \
        --command "add '$STAGE/Windows/Setup/Scripts/SetupComplete.cmd' '\\Windows\\Setup\\Scripts\\SetupComplete.cmd'" >/dev/null
    wimlib-imagex update "$WIM" "$idx" \
        --command "add '$STAGE/Windows/Panther/Unattend/unattend.xml' '\\Windows\\Panther\\Unattend\\unattend.xml'" >/dev/null
done

# autounattend.xml na raiz da ISO (Setup le automaticamente do boot media)
cp "$UNATTEND" "$IMG/autounattend.xml"   # renderizado com a senha do build (#119)

# --- 6. reempacotar ISO hibrida bootavel ----------------------------------
step "localizando arquivos de boot"
BIOS=$(cd "$IMG" && ls boot/etfsboot.com 2>/dev/null || true)
EFI=""
for c in efi/microsoft/boot/efisys_noprompt.bin efi/microsoft/boot/efisys.bin; do
    [ -f "$IMG/$c" ] && { EFI="$c"; break; }
done
[ -n "$BIOS" ] || die "boot/etfsboot.com ausente (ISO de origem incompleta?)"
[ -n "$EFI" ]  || die "efisys*.bin ausente"

LABEL="NTUNIX"
step "gerando $OUT_ISO (BIOS=$BIOS EFI=$EFI)"
xorriso -as mkisofs \
    -iso-level 4 -rock \
    -volid "$LABEL" \
    -disable-deep-relocation -untranslated-filenames \
    -b "$BIOS" -no-emul-boot -boot-load-size 8 -hide boot.catalog \
    -eltorito-alt-boot -eltorito-platform efi \
    -b "$EFI" -no-emul-boot \
    -o "$OUT_ISO" "$IMG"

printf '\033[32mpronto:\033[0m %s (%s)\n' "$OUT_ISO" "$(du -h "$OUT_ISO" | cut -f1)"
echo "teste rapido:  ./build/test-vm.sh \"$OUT_ISO\""
