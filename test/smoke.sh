#!/usr/bin/env bash
# Smoke test do initd/ntctl sob Wine.
# Sobe o initd com a árvore staged em out/, exercita o ciclo completo
# (list/status/logs/stop/start/restart-on-kill/shutdown) e confere saídas.
set -u
cd "$(dirname "$0")/.."

export WINEDEBUG=${WINEDEBUG:--all}
PASS=0; FAIL=0

check() { # check <descricao> <comando...>
    local desc=$1; shift
    if "$@" >/dev/null 2>&1; then
        echo "ok   - $desc"; PASS=$((PASS+1))
    else
        echo "FAIL - $desc"; FAIL=$((FAIL+1))
    fi
}

ntctl() { wine out/system/bin/ntctl.exe "$@" 2>/dev/null; }

echo "== subindo initd =="
wine out/system/bin/initd.exe > out/initd-console.log 2>&1 &
INITD_PID=$!

# espera o pipe ficar de pé
up=0
for _ in $(seq 1 30); do
    if ntctl ping >/dev/null; then up=1; break; fi
    sleep 0.5
done
if [ "$up" != 1 ]; then
    echo "FAIL - initd nao respondeu ao ping"; kill $INITD_PID 2>/dev/null
    exit 1
fi
echo "ok   - initd de pe"

check "ping" ntctl ping
check "list mostra logd rodando"  bash -c "ntctl_out=\$(wine out/system/bin/ntctl.exe list 2>/dev/null); echo \"\$ntctl_out\" | grep -q 'logd.*running'"
check "list mostra demod rodando" bash -c "wine out/system/bin/ntctl.exe list 2>/dev/null | grep -q 'demod.*running'"
check "status demod" ntctl status demod

echo "== esperando heartbeats =="
sleep 7
check "logs demod tem heartbeat" bash -c "wine out/system/bin/ntctl.exe logs demod 2>/dev/null | grep -q heartbeat"
check "logd recebeu via pipe"    bash -c "grep -q 'demod: heartbeat' out/var/log/system.log"

echo "== stop / start =="
check "stop demod" ntctl stop demod
check "demod parado" bash -c "wine out/system/bin/ntctl.exe list 2>/dev/null | grep -q 'demod.*stopped'"
check "start demod" ntctl start demod
check "demod rodando de novo" bash -c "wine out/system/bin/ntctl.exe list 2>/dev/null | grep -q 'demod.*running'"

echo "== restart automatico (Restart=always) =="
DEMOD_PID=$(wine out/system/bin/ntctl.exe status demod 2>/dev/null | awk '/PID:/{print $2}' | tr -d '\r')
if [ -n "${DEMOD_PID:-}" ]; then
    # mata o processo por fora; initd deve reerguer em ~1s
    wine taskkill /F /PID "$DEMOD_PID" >/dev/null 2>&1
    sleep 4
    check "demod ressuscitou" bash -c "wine out/system/bin/ntctl.exe list 2>/dev/null | grep -q 'demod.*running'"
    NEW_PID=$(wine out/system/bin/ntctl.exe status demod 2>/dev/null | awk '/PID:/{print $2}' | tr -d '\r')
    check "com pid novo" test "$NEW_PID" != "$DEMOD_PID"
else
    echo "SKIP - nao consegui extrair pid do demod"
fi

echo "== enable/disable =="
check "disable demod" ntctl disable demod
check "marcador removido" bash -c "test ! -e out/etc/units/enabled/demod"
check "enable demod" ntctl enable demod
check "marcador criado" bash -c "test -e out/etc/units/enabled/demod"

echo "== shutdown =="
check "shutdown" ntctl shutdown
sleep 2
check "initd saiu" bash -c "! kill -0 $INITD_PID 2>/dev/null"
check "ping falha apos shutdown" bash -c "! wine out/system/bin/ntctl.exe ping >/dev/null 2>&1"

kill $INITD_PID 2>/dev/null
wait $INITD_PID 2>/dev/null

echo
echo "== resultado: $PASS ok, $FAIL falha(s) =="
exit $((FAIL > 0 ? 1 : 0))
