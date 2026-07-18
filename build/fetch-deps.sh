#!/usr/bin/env bash
# fetch-deps.sh — baixa as fontes externas do build (musl, busybox) pra um lugar
# PERSISTENTE (build/deps por padrao), NAO /tmp (tmpfs, some no reboot).
# Idempotente: no-op se ja extraido (sentinela .ntu-dep-ok). Roda automatico
# antes de `make musl-nt` / `make busybox-nt`. Override do destino: NTU_DEPS=/path.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"        # raiz do repo
DEPS="${NTU_DEPS:-$HERE/build/deps}"
mkdir -p "$DEPS"

fetch() {   # nome url sha256 dir-destino flag-tar
    local name="$1" url="$2" want="$3" dir="$4" tarflag="$5"
    local target="$DEPS/$dir"
    if [ -e "$target/.ntu-dep-ok" ]; then
        return 0                                 # ja presente
    fi
    echo "deps: baixando $name ($url)"
    local tgz; tgz="$(mktemp "$DEPS/.dl.XXXXXX")"
    curl -fL --retry 3 --connect-timeout 20 -o "$tgz" "$url"
    local got; got="$(sha256sum "$tgz" | cut -d' ' -f1)"
    if [ -n "$want" ] && [ "$want" != "$got" ]; then
        echo "deps: ERRO sha256 de $name nao confere" >&2
        echo "      esperado $want" >&2
        echo "      obtido   $got"  >&2
        rm -f "$tgz"; exit 1
    fi
    [ -z "$want" ] && echo "deps: (fixe o sha256 de $name -> $got)"
    rm -rf "$target.tmp"; mkdir -p "$target.tmp"
    tar "$tarflag" "$tgz" -C "$target.tmp" --strip-components=1
    rm -rf "$target"; mv "$target.tmp" "$target"; rm -f "$tgz"
    touch "$target/.ntu-dep-ok"
    echo "deps: $name pronto -> $target"
}

fetch musl \
    "https://musl.libc.org/releases/musl-1.2.6.tar.gz" \
    "d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a" \
    musl-1.2.6 xzf

fetch busybox \
    "https://busybox.net/downloads/busybox-1.37.0.tar.bz2" \
    "3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4" \
    busybox-1.37.0 xjf

echo "deps: OK -> $DEPS"
