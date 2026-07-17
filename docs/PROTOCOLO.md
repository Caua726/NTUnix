# Protocolo de controle do initd (v0)

Transporte: named pipe `\\.\pipe\ntunix-initd`, modo byte, **uma requisição
por conexão** — o cliente conecta, escreve o comando, lê a resposta até EOF
e o initd desconecta.

## Requisição

Texto puro, uma linha:

```text
VERBO [arg1 [arg2]]
```

O verbo é case-insensitive. `ntctl` apenas converte `argv` nesse formato.

## Resposta

A primeira linha começa com `OK` ou `ERR`; o corpo (opcional) vem em
seguida. `ntctl` retorna exit code `0` para `OK`, `1` para `ERR`, `2` se o
pipe estiver inacessível.

## Verbos

| Verbo | Args | Efeito |
|---|---|---|
| `PING` | — | identifica versão e raiz do initd |
| `LIST` | — | tabela de serviços (nome, estado, pid, restarts, descrição) |
| `STATUS` | `<svc>` | detalhes de um serviço |
| `START` | `<svc>` | inicia, resolvendo `Requires=` recursivamente |
| `STOP` | `<svc>` | `TerminateJobObject` (mata a árvore inteira); espera até 5 s |
| `RESTART` | `<svc>` | STOP + START |
| `ENABLE` | `<svc>` | cria marcador `/etc/units/enabled/<svc>` (inicia no boot) |
| `DISABLE` | `<svc>` | remove o marcador |
| `LOGS` | `<svc> [n]` | últimas *n* linhas de `/var/log/<svc>.log` (padrão 20, máx 500) |
| `RELOAD` | — | rescaneia `/etc/units` (units de serviços rodando não são recarregadas) |
| `SHUTDOWN` | — | para todos os serviços e encerra o initd |

## Units (subconjunto v0)

```ini
[Unit]
Description=texto livre
Requires=outro-servico          # tambem aceita "outro.service"; After= e alias

[Service]
ExecStart=/system/bin/foo.exe args   # caminho unix-style; split no 1o espaco (sem quoting)
Restart=no | on-failure | always     # throttle: max 5 restarts por janela de 10 s
MemoryMax=64M                        # K/M/G; vira JOB_OBJECT_LIMIT_JOB_MEMORY
WorkingDirectory=/                   # opcional; padrao = raiz NTUnix

[Install]                            # ignorado na v0 (enable = marcador em arquivo)
WantedBy=default.target
```

## Semântica de supervisão

- Cada serviço roda num **Job Object** próprio com `KILL_ON_JOB_CLOSE`:
  se o initd morrer, todos os serviços morrem junto; parar um serviço mata
  todos os processos que ele tenha criado (VISAO.md §11).
- O processo nasce `CREATE_SUSPENDED`, entra no job e só então é retomado —
  nenhum filho escapa do job.
- stdout/stderr de cada serviço vão para `/var/log/<nome>.log` (append).
- Saída inesperada aplica `Restart=` com 1 s de espera; mais de 5 saídas
  numa janela de 10 s marca o serviço como `failed` e desiste.
- A raiz NTUnix é resolvida por `NTUNIX_ROOT` (env) ou, na ausência, subindo
  dois níveis a partir de `<dir do exe>` quando ele termina em `\system\bin`.
  O initd propaga `NTUNIX_ROOT` aos serviços.
