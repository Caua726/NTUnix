#!/usr/bin/env bash
# build-check.sh — valida a base de build da ISO SEM precisar de uma ISO
# do Windows. Checa presenca de deps, sintaxe dos scripts, XML bem-formado,
# a arvore staged (out/) e simula a fase de strip/inject do make-iso num
# .wim sintetico. Complementa o smoke.sh (que testa o runtime).
set -u
cd "$(dirname "$0")/.."
PASS=0; FAIL=0; SKIP=0
ok()   { echo "ok   - $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL - $1"; FAIL=$((FAIL+1)); }
skip() { echo "skip - $1"; SKIP=$((SKIP+1)); }
have() { command -v "$1" >/dev/null 2>&1; }

echo "== arvore staged (out/) =="
for b in initd ntctl logd demod ntsession; do
    if [ -f "out/system/bin/$b.exe" ]; then ok "binario $b.exe presente"
    else bad "binario $b.exe AUSENTE (rode make)"; fi
done
[ -f out/etc/units/logd.service ] && ok "units staged" || bad "units nao staged"

echo "== ntsession sem console (-mwindows) =="
if have x86_64-w64-mingw32-objdump; then
    subsys=$(x86_64-w64-mingw32-objdump -p out/system/bin/ntsession.exe 2>/dev/null \
             | awk '/^Subsystem/{print $2}')
    # PE GUI subsystem = 2
    [ "$subsys" = "00000002" ] && ok "ntsession e GUI (subsystem 2)" \
        || bad "ntsession nao e GUI (subsystem=$subsys)"
else skip "objdump do mingw ausente"; fi

echo "== sintaxe dos scripts =="
for s in build/make-iso.sh build/test-vm.sh test/smoke.sh test/build-check.sh; do
    bash -n "$s" && ok "bash -n $s" || bad "sintaxe $s"
done

echo "== SetupComplete.cmd sanidade =="
grep -q 'Winlogon' build/SetupComplete.cmd && grep -q 'ntsession.exe' build/SetupComplete.cmd \
    && ok "SetupComplete troca o Shell para ntsession" \
    || bad "SetupComplete nao aponta o Shell para ntsession"

echo "== autounattend.xml bem-formado =="
if have xmllint; then
    xmllint --noout build/autounattend.xml 2>/dev/null \
        && ok "autounattend.xml valido" || bad "autounattend.xml malformado"
else skip "xmllint ausente (pacote libxml2)"; fi

echo "== strip.list nao remove itens perigosos =="
# audit #121: filtra comentarios ANTES de procurar (o `grep -q | grep` antigo nao
# emitia nada no 1o grep, entao o 2o sempre falhava e o check sempre "passava")
if grep -vE '^\s*#' build/strip.list | grep -iqE 'SystemApps|WebView2|Defender'; then
    bad "strip.list contem item perigoso fora de comentario"
else ok "strip.list conservador (sem SystemApps/WebView2/Defender ativos)"; fi

echo "== deps da geracao de ISO =="
# testa que EXECUTA, nao so que o binario existe: um soname mismatch
# (ex. libntfs-3g bumpado) deixa `command -v` passar mas o binario nao roda.
for t in wimlib-imagex xorriso; do
    if ! have "$t"; then bad "dep $t FALTA (pacman -S wimlib xorriso)"
    elif "$t" --version >/dev/null 2>&1; then ok "dep $t executa"
    else bad "dep $t presente mas NAO executa ($("$t" --version 2>&1 | head -1))"; fi
done
have 7z || have bsdtar && ok "extrator de ISO (7z/bsdtar)" || bad "sem 7z nem bsdtar"

echo "== dry-run: strip + inject num .wim sintetico =="
if have wimlib-imagex; then
    W=build/work/selftest; rm -rf "$W"; mkdir -p "$W/src/Windows/System32" \
        "$W/src/Program Files/WindowsApps" "$W/inject"
    echo dummy > "$W/src/Windows/System32/ntdll.dll"
    echo app   > "$W/src/Program Files/WindowsApps/junk.txt"
    cp -a out/. "$W/inject/"
    if wimlib-imagex capture "$W/src" "$W/t.wim" NT --compress=none >/dev/null 2>&1; then
        wimlib-imagex update "$W/t.wim" 1 --command \
            "delete --force --recursive '\\Program Files\\WindowsApps'" >/dev/null 2>&1
        wimlib-imagex update "$W/t.wim" 1 --command \
            "add '$W/inject' '\\NTUnix'" >/dev/null 2>&1
        listing=$(wimlib-imagex dir "$W/t.wim" 1 2>/dev/null)
        echo "$listing" | grep -q '/NTUnix/system/bin/initd.exe' \
            && ok "inject: /NTUnix/system/bin/initd.exe presente na imagem" \
            || bad "inject falhou"
        echo "$listing" | grep -q 'WindowsApps' \
            && bad "strip falhou: WindowsApps ainda na imagem" \
            || ok "strip: WindowsApps removido da imagem"
        rm -rf "$W"
    else
        skip "wimlib capture indisponivel neste ambiente"
        rm -rf "$W"
    fi
else skip "wimlib ausente: pulei o dry-run de imagem"; fi

echo
echo "== build-check: $PASS ok, $FAIL falha(s), $SKIP skip =="
exit $((FAIL > 0 ? 1 : 0))
