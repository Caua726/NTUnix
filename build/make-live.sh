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

# --- 1. extrair a ISO ------------------------------------------------------
# O install.wim so entra quando a midia tambem serve de INSTALADOR: ele e' a
# fonte que o ntstrap aplica no disco. Sem ele a ISO fica pequena, mas so boota
# live.  NTUNIX_INSTALLER=1  ativa.
rm -rf "$IMG"; mkdir -p "$IMG"
if [ "${NTUNIX_INSTALLER:-}" = 1 ]; then
    step "extraindo $SRC_ISO (COM install.wim — midia instalavel)"
    7z x -y -o"$IMG" "$SRC_ISO" >/dev/null
    if [ -f "$IMG/sources/install.esd" ] && [ ! -f "$IMG/sources/install.wim" ]; then
        step "normalizando install.esd -> install.wim"
        n=$(wimlib-imagex info "$IMG/sources/install.esd" | awk '/^Image Count:/{print $NF}')
        for i in $(seq 1 "$n"); do
            wimlib-imagex export "$IMG/sources/install.esd" "$i" \
                "$IMG/sources/install.wim" --compress=LZX >/dev/null
        done
        rm -f "$IMG/sources/install.esd"
    fi
    [ -f "$IMG/sources/install.wim" ] || die "sources/install.wim ausente na ISO"

    # --- enxugar a imagem ANTES de ela ir pra midia ------------------------
    # Duas reducoes, nesta ordem:
    #   1. exportar SO a edicao escolhida (a ISO da MS traz 6);
    #   2. apagar o peso morto listado em strip.list, dentro do wim.
    # Fazer isso aqui, e nao depois de aplicar, e' o que faz a instalacao
    # NASCER pequena: o dism nunca chega a expandir os GB que iriam fora.
    IW="$IMG/sources/install.wim"
    ed_name() { wimlib-imagex info "$IW" "$1" \
        | sed -n 's/^Name:[[:space:]]*//p' | head -1 | sed 's/[[:space:]]*$//'; }
    IDXS="$(wimlib-imagex info "$IW" | awk '/^Index:/{print $NF}')"
    KEEP=""
    if [ -n "${NTUNIX_EDITION:-}" ]; then
        for i in $IDXS; do [ "$(ed_name "$i")" = "$NTUNIX_EDITION" ] && KEEP="$i"; done
        [ -n "$KEEP" ] || die "edicao '$NTUNIX_EDITION' nao existe nesta ISO"
    else
        for i in $IDXS; do case "$(ed_name "$i")" in *" Pro") KEEP="$i"; break ;; esac; done
        [ -n "$KEEP" ] || KEEP="$(echo $IDXS | awk '{print $1}')"
    fi
    step "edicao base: $(ed_name "$KEEP") (indice $KEEP de $(echo $IDXS | wc -w))"

    before=$(du -m "$IW" | cut -f1)
    wimlib-imagex export "$IW" "$KEEP" "$IW.one" --boot --compress=LZX >/dev/null
    mv "$IW.one" "$IW"

    # --- os TRES perfis, como tres imagens no MESMO wim ------------------
    # O WIM armazena por hash: como debug e' subconjunto de leve, que e' de
    # normal, as duas menores quase nao custam espaco. Uma ISO so' (~2G) instala
    # qualquer um dos tres. Aplicar-e-depois-apagar nao serviria: exigiria os
    # 13G da imagem cheia em disco antes de encolher, e o disco alvo tem 10G.
    #   1 normal ~4,8G · 2 leve ~3,0G · 3 debug ~670MB   (ver docs/perfis.md)
    BASE="$IW.base"; mv "$IW" "$BASE"
    PELIST="$WORK/winpe-files.tsv"
    wimlib-imagex dir "$WIM" 1 --detailed 2>/dev/null \
        | python3 "$REPO/build/dumpwim.py" > "$PELIST"

    idx=0
    for perfil in normal leve debug; do
        idx=$((idx+1))
        step "perfil $idx/3: $perfil"
        wimlib-imagex export "$BASE" 1 "$IW" \
            --image-name "NTUnix $perfil" --compress=LZX >/dev/null
        CMDS="$WORK/perfil-$perfil.txt"
        wimlib-imagex dir "$IW" "$idx" --detailed 2>/dev/null \
            | python3 "$REPO/build/mkprofile.py" "$perfil" "$PELIST" > "$CMDS"
        step "  $(grep -c . "$CMDS") remocoes"
        wimlib-imagex update "$IW" "$idx" < "$CMDS" >/dev/null
    done
    rm -f "$BASE"
    wimlib-imagex info "$IW" | grep -E '^(Index|Name):' || true

    # o delete nao encolhe o wim sozinho: os recursos so somem no rebuild
    step "recomprimindo o wim (solid)"
    wimlib-imagex optimize "$IW" --solid --recompress >/dev/null 2>&1 \
        || wimlib-imagex optimize "$IW" --recompress >/dev/null
    after=$(du -m "$IW" | cut -f1)
    step "install.wim: ${before}MB -> ${after}MB"
else
    step "extraindo $SRC_ISO (sem install.wim — so live)"
    7z x -y -o"$IMG" "$SRC_ISO" -x'!sources/install.wim' -x'!sources/install.esd' >/dev/null
fi
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

# ferramentas do instalador, em \NTUnix\install dentro do ambiente live
step "injetando o ntstrap (instalador)"
mkdir -p "$STAGE/install"
NTUNIX_PW="${NTUNIX_PASSWORD:-ntunix}"
if [ "$NTUNIX_PW" = ntunix ]; then
    step "AVISO seguranca: senha padrao 'ntunix' — defina NTUNIX_PASSWORD=<senha>"
fi
cp "$REPO/build/ntstrap.cmd"     "$STAGE/install/"
cp "$REPO/build/strip.list"      "$STAGE/install/"
cp "$REPO/build/xcopy-skip.txt"  "$STAGE/install/"
sed "s|@NTUNIX_PW@|$NTUNIX_PW|g" "$REPO/build/unattend-oobe.xml" \
    > "$STAGE/install/unattend-oobe.xml"
# atalho no PATH da sessao: digitar 'ntstrap' no terminal do NTUnix funciona
cp "$REPO/build/ntstrap.cmd" "$STAGE/system/bin/" 
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
if [ "${NTUNIX_INSTALLER:-}" = 1 ]; then
    echo "midia INSTALAVEL: boota no NTUnix; la dentro, num terminal:"
    echo "    cmd /c ntstrap.cmd        (particiona, aplica a imagem, torna bootavel)"
else
    echo "midia so LIVE (sem install.wim). Para instalavel: NTUNIX_INSTALLER=1"
fi
echo "teste:  ./build/test-vm.sh \"$OUT_ISO\""
