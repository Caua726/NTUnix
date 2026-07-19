# Canal de debug da VM — terminal compartilhado (`dbgterm`)

**Só em build de dev (`NTUNIX_DEBUG=1`).** Permite dirigir a VM da NTUnix de fora,
com o desenvolvedor (ou o assistente) mandando comandos que **aparecem digitando e
executando num terminal visível do desktop da VM**, ao vivo. É o mesmo terminal,
mesmo shell, mesmo tty — não um shell separado.

## Por que rede (e não serial)

Tentamos serial primeiro; **não funciona no WinPE UEFI**:

| Tentativa | Falha |
|---|---|
| ISA serial (COM1) | Não enumera sob UEFI — sem `NTDETECT.COM` no boot UEFI. |
| PCI serial (`pci-serial` 1b36:0002) + `drvload` | `exit 3` — `drvload` fraco com INF MultiFunction. |
| idem + `pnputil /add-driver /install` | **`0xE000022F`** (`SPAPI_E_NO_CATALOG_FOR_OEM_INF`) — o `qemupciserial.inf` cru não é assinado; WinPE x64 UEFI recusa. |
| idem + `dism /online /Add-Driver /ForceUnsigned` | `0x80040002` — `/Add-Driver` só existe pra imagem **offline** (precisa de DISM/Windows, não temos no build Linux). |

**A rede ganha** porque o driver da NIC **e1000e é inbox (assinado pela Microsoft)** —
o WinPE carrega sozinho no boot, sem o muro de assinatura. Validado: `ipconfig`
mostra `10.0.2.15`, gateway `10.0.2.2`.

## Arquitetura

```
host                                   guest (VM, WinPE + NTUnix)
────                                   ──────────────────────────
nc -l 2323  ◄───── TCP (SLIRP) ─────►  dispd/dbgterm reverse-connect 10.0.2.2:2323
  │  (10.0.2.2 = gateway do SLIRP,       │
  │   encaminha guest→host; o SLIRP      ├─ recv → term_input() → terminal VISÍVEL
  │   bloqueia host→guest, por isso       │        (aparece digitando + roda na tela)
  │   é REVERSE, guest→host)              └─ vt_feed → dbgterm_tap() → send() de volta
                                                   (a saída do terminal volta pro host)
```

- **`src/dispd/dbgterm.c`** — dentro do `dispd`. Em dev, `dbgterm_start()`:
  1. abre um terminal visível (`spawn_terminal`), guarda em `g_dbgwin`;
  2. sobe uma thread que faz reverse-connect em `10.0.2.2:2323` (retry a cada 2 s);
  3. **input:** `recv` do socket → `term_input(g_dbgwin->term, ...)` → injeta como se
     digitado (o `\n` vira `\r` pra casar com o teclado, que o pty converte por ICRNL);
  4. **output:** `dbgterm_tap()` é chamado em `vt_feed()` (thread leitora do terminal)
     e faz `send()` do stream VT de volta pro socket.
- O socket fica 100% no `dispd` (mingw/Winsock, `-lws2_32`); o busybox só vê o pty
  normal do terminal — não depende de Winsock-como-fd na musl-nt.

## Gate (dev only)

O env `NTUNIX_DEBUG` é de **build**, não existe no runtime da VM. Então o gate é um
**arquivo marcador**: `stage-files` cria `/etc/ntunix-debug` quando `NTUNIX_DEBUG=1`,
e `dbgterm_start()` checa `GetFileAttributes` desse arquivo. Sem o marcador (produção),
`dbgterm_start()` retorna 0 e o `dispd` abre o terminal normal. Em dev, abre **só** o
terminal de debug. O canal **não tem autenticação** e o listener do host fica em
`127.0.0.1` — nunca habilitar em produção.

## Como usar

```sh
# 1. builda com o canal + redefine a VM (e1000e) + boota:
make debug-live

# 2. no host, escuta (o guest conecta assim que o dispd sobe):
nc -l 2323

# 3. digite comandos — aparecem no terminal de debug DA TELA DA VM e rodam lá;
#    a saída volta pro seu nc.
```

Para dirigir por script (fifo de entrada + arquivo de saída):

```sh
mkfifo /tmp/ncin; : > /tmp/ncout
( exec 9<>/tmp/ncin; nc -l 2323 <&9 >> /tmp/ncout )   # em background
echo 'ls' > /tmp/ncin          # injeta um comando
tail -f /tmp/ncout             # acompanha a saída (= o que aparece na tela da VM)
```

## Config da VM (`build/vm-setup.sh`, com `NTUNIX_DEBUG=1`)

- `--network user,model=e1000e` (driver inbox assinado; sobe no boot).
- **Sem** `hostfwd` / `qemu:commandline`: a conexão é reversa (guest→`10.0.2.2`), então
  o SLIRP encaminha sem precisar de forward.

## Limitação conhecida / bug aberto

Ao dirigir o terminal real (tty do desktop) pelo canal, apareceu um bug que o shell
por pipe mascarava: **`ls` sai vazio com exit code 5**. Não é o multi-coluna — `ls -1`
e `ls -la` também saem vazios.

Medido em 2026-07-19, pelo canal, com o dispd em execução:

| Comando | Resultado |
|---|---|
| `ls`, `ls -1`, `ls -la` | vazio, rc=5 |
| `ls /mnt/x/NTUnix` | **vazio também** — com caminho explícito não resolve |
| `echo /system/bin/*.exe` | **lista os 10 binários** |
| `pwd` | `/mnt/x/NTUnix` |
| `cd` + TAB | completa os diretórios (`etc/ obj/ proc/ run/ system/ var/`) |
| `uname -a`, `id`, `cat`, `df -h` | funcionam |

O glob do ash percorre o mesmo diretório e acerta, e o `cd` completa — então
`opendir`/`getcwd` **não** são a causa (a suspeita anterior). O defeito está no
caminho do applet `ls` do BusyBox. Próximo passo: comparar o que o `ls` faz a mais
que o glob — provavelmente o `stat`/`lstat` por entrada, ou o buffer de `getdents`.

> Este é justamente o valor do terminal compartilhado: ele **reflete o sistema real**
> (mesmo tty, mesmo shell) e não esconde bugs específicos de tty como um pipe faria.
