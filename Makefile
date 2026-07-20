# NTUnix — build cruzado Linux → Windows (mingw-w64).
# `make` monta a árvore staged em out/. Para rodar, use a VM: `make vm`.

CC     := x86_64-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -static -D_WIN32_WINNT=0x0601
# fontes externas: baixadas pra build/deps (persistente, NAO /tmp) por `make deps`
MUSL_SRC ?= $(CURDIR)/build/deps/musl-1.2.6
BUSYBOX_SRC ?= $(CURDIR)/build/deps/busybox-1.37.0

# VM libvirt que `make live` reinicia (override: VM_NAME=... VIRSH="...")
VIRSH   ?= virsh -c qemu:///session
VM_NAME ?= ntunix-live

# ISO do Windows (base do WinPE): local padrao build/deps/windows.iso; nao e'
# obrigatorio passar WIN_ISO — so ponha a ISO ali (ou passe WIN_ISO=/caminho.iso).
WIN_ISO ?= $(CURDIR)/build/deps/windows.iso

# Nome da ISO de saida. Sao DUAS midias diferentes e nao podem colidir:
#   make live -> NTUnix-live.iso  (so live, sem install.wim, pequena)
#   make iso  -> NTUnix.iso       (live + install.wim + ntstrap, instalavel)
# OUT_ISO sobrescreve ambas quando definido na linha de comando.
LIVE_ISO ?= $(if $(OUT_ISO),$(OUT_ISO),$(CURDIR)/NTUnix-live.iso)
INST_ISO ?= $(if $(OUT_ISO),$(OUT_ISO),$(CURDIR)/NTUnix.iso)

OUT := out
BIN := $(OUT)/system/bin

COMMON := src/common/ntupath.c src/common/ntuini.c src/common/ntuutil.c
INITD  := src/initd/initd.c src/initd/service.c src/initd/pipesrv.c
HDRS   := src/common/ntu.h src/initd/initd.h

# dispd (compositor/display server) + ntwm (window manager). GUI nativo.
# term_conpty.c e present_dxgi.c precisam de headers 0x0A00 -> TU isolada em out/obj.
DISPD_SRC := src/dispd/dispd.c src/dispd/compositor.c src/dispd/present_gdi.c \
             src/dispd/vt.c src/dispd/term.c src/dispd/term_scrape.c \
             src/dispd/term_pty.c src/dispd/foreign.c \
             src/dispd/input.c src/dispd/wmproto.c src/dispd/appsrv.c \
             src/dispd/dbgterm.c
DISPD_HDR := src/dispd/dispd.h src/dispd/present.h src/dispd/term.h \
             src/common/ntu.h src/common/ntuwm.h
NTWM_SRC  := src/ntwm/ntwm.c src/ntwm/proto.c src/ntwm/layout.c \
             src/ntwm/config.c src/ntwm/actions.c src/ntwm/ipc.c \
             src/ntwm/selftest.c

BINS := $(BIN)/initd.exe $(BIN)/ntctl.exe $(BIN)/logd.exe $(BIN)/demod.exe \
        $(BIN)/ntsession.exe $(BIN)/dispd.exe $(BIN)/ntwm.exe $(BIN)/ntclock.exe \
        $(BIN)/ntwmctl.exe $(BIN)/ntbar.exe $(BIN)/ntdbgcon.exe

all: $(BINS) stage-files

$(BIN)/initd.exe: $(INITD) $(COMMON) $(HDRS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(INITD) $(COMMON)

$(BIN)/ntctl.exe: src/ntctl/ntctl.c $(COMMON) src/common/ntu.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/ntctl/ntctl.c $(COMMON)

$(BIN)/logd.exe: src/logd/logd.c $(COMMON) src/common/ntu.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/logd/logd.c $(COMMON)

$(BIN)/demod.exe: src/demod/demod.c $(COMMON) src/common/ntu.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/demod/demod.c $(COMMON)

# shell da sessao: sem console proprio (-mwindows)
$(BIN)/ntsession.exe: src/ntsession/ntsession.c $(COMMON) src/common/ntu.h | $(BIN)
	$(CC) $(CFLAGS) -mwindows -o $@ src/ntsession/ntsession.c $(COMMON)

# console de debug via REDE (DEV): reverse shell TCP guest->host (10.0.2.2:2323),
# relay socket<->pipes<->busybox. Precisa de Winsock (-lws2_32).
$(BIN)/ntdbgcon.exe: src/ntdbgcon/ntdbgcon.c | $(BIN)
	$(CC) $(CFLAGS) -mwindows -o $@ src/ntdbgcon/ntdbgcon.c -lws2_32

# ConPTY: headers 0x0A00, mas as 3 funcoes vem via GetProcAddress (fallback WinPE).
$(OUT)/obj:
	mkdir -p $(OUT)/obj

$(OUT)/obj/term_conpty.o: src/dispd/term_conpty.c src/dispd/term.h | $(OUT)/obj
	$(CC) $(filter-out -D_WIN32_WINNT=0x0601,$(CFLAGS)) -D_WIN32_WINNT=0x0A00 -c -o $@ $<

# DXGI flip-model (M3): headers 0x0A00, D3D11CreateDevice via GetProcAddress
# (sem -ld3d11: carrega so em runtime). IIDs vem de -ldxguid (dado estatico).
$(OUT)/obj/present_dxgi.o: src/dispd/present_dxgi.c src/dispd/present.h | $(OUT)/obj
	$(CC) $(filter-out -D_WIN32_WINNT=0x0601,$(CFLAGS)) -D_WIN32_WINNT=0x0A00 -c -o $@ $<

# libvterm (third_party, MIT): engine VT do vim/neovim. C portavel; compilado
# como objetos com -w (nao poluir com warnings de terceiros). vt.c usa a API.
LIBVTERM_DIR := third_party/libvterm
LIBVTERM_INC := -I$(LIBVTERM_DIR)/include -I$(LIBVTERM_DIR)/src
LIBVTERM_SRC := $(wildcard $(LIBVTERM_DIR)/src/*.c)
LIBVTERM_OBJ := $(patsubst $(LIBVTERM_DIR)/src/%.c,$(OUT)/obj/vterm_%.o,$(LIBVTERM_SRC))

$(OUT)/obj/vterm_%.o: $(LIBVTERM_DIR)/src/%.c | $(OUT)/obj
	$(CC) -O2 -static -D_WIN32_WINNT=0x0601 $(LIBVTERM_INC) -w -c -o $@ $<

# compositor: GUI nativo. term_conpty/present_dxgi ja compilados a 0x0A00.
$(BIN)/dispd.exe: $(DISPD_SRC) $(OUT)/obj/term_conpty.o $(OUT)/obj/present_dxgi.o $(LIBVTERM_OBJ) $(COMMON) $(DISPD_HDR) | $(BIN)
	$(CC) $(CFLAGS) $(LIBVTERM_INC) -mwindows -o $@ $(DISPD_SRC) $(OUT)/obj/term_conpty.o $(OUT)/obj/present_dxgi.o $(LIBVTERM_OBJ) $(COMMON) -lgdi32 -luser32 -ldxguid -lws2_32

# window manager: so pipe + logica, sem libs extras.
$(BIN)/ntwm.exe: $(NTWM_SRC) $(COMMON) src/common/ntu.h src/common/ntuwm.h src/ntwm/ntwm.h | $(BIN)
	$(CC) $(CFLAGS) -mwindows -o $@ $(NTWM_SRC) $(COMMON)

# app demo (relogio): cliente da fronteira apps<->dispd; surface compartilhada.
$(BIN)/ntclock.exe: src/apps/ntclock/ntclock.c src/common/ntu.h | $(BIN)
	$(CC) $(CFLAGS) -mwindows -o $@ src/apps/ntclock/ntclock.c -lgdi32 -luser32

$(BIN)/ntwmctl.exe: src/apps/ntwmctl/ntwmctl.c src/common/ntu.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/apps/ntwmctl/ntwmctl.c

$(BIN)/ntbar.exe: src/apps/ntbar/ntbar.c src/common/ntu.h \
                  src/common/ntuapp.h src/common/ntuwm.h | $(BIN)
	$(CC) $(CFLAGS) -mwindows -o $@ src/apps/ntbar/ntbar.c -lgdi32 -luser32

$(BIN):
	mkdir -p $(BIN) $(OUT)/etc/units/enabled $(OUT)/var/log $(OUT)/run

stage-files: | $(BIN)
	cp etc/units/*.service $(OUT)/etc/units/
	cp etc/passwd etc/group etc/mtab etc/hosts $(OUT)/etc/
	mkdir -p $(OUT)/etc/ntwm
	cp etc/ntwm/ntwm.conf $(OUT)/etc/ntwm/
	mkdir -p $(OUT)/proc
	cp proc/mounts $(OUT)/proc/
	touch $(OUT)/etc/units/enabled/logd $(OUT)/etc/units/enabled/demod
	touch $(OUT)/etc/units/enabled/dispd $(OUT)/etc/units/enabled/ntwm
	touch $(OUT)/etc/units/enabled/ntbar
	@# o canal de debug agora e' o dbgterm DENTRO do dispd (terminal compartilhado,
	@# gated por NTUNIX_DEBUG no proprio dispd) -> o ntdbgcon separado nao e' mais
	@# habilitado (conflitaria na mesma porta 2323).
	rm -f $(OUT)/etc/units/enabled/ntdbgcon
	@# marcador de dev lido pelo dispd em runtime (o env NTUNIX_DEBUG e' so de build)
	@if [ "$(NTUNIX_DEBUG)" = "1" ]; then \
	    touch $(OUT)/etc/ntunix-debug; \
	    echo "  [dev] terminal de debug COMPARTILHADO (dbgterm no dispd) ativo"; \
	else \
	    rm -f $(OUT)/etc/ntunix-debug; \
	fi

# ISO LIVE: FAZ TUDO — deps + libc + busybox + tools + ISO — e reinicia a VM.
# ISO do Windows: build/deps/windows.iso por padrao (ou WIN_ISO=/caminho.iso).
#   make live                       (usa build/deps/windows.iso, reinicia a VM)
#   make live WIN_ISO=/x.iso NO_BOOT=1   (ISO custom, sem reiniciar a VM)
live:
	@test -f "$(WIN_ISO)" || { echo "ISO do Windows nao encontrada: $(WIN_ISO)"; \
		echo "  ponha a ISO em build/deps/windows.iso ou passe WIN_ISO=/caminho.iso"; exit 1; }
	$(MAKE) busybox-nt          # deps + musl-nt + busybox (+ copia busybox.exe)
	$(MAKE) all                 # dispd/ntwm/initd/... + stage
	./build/make-live.sh "$(WIN_ISO)" "$(LIVE_ISO)"
	@if [ -z "$(NO_BOOT)" ] && command -v $(firstword $(VIRSH)) >/dev/null 2>&1; then \
		echo ">> reiniciando a VM $(VM_NAME)"; \
		$(VIRSH) destroy $(VM_NAME) 2>/dev/null || true; \
		sleep 2; \
		$(VIRSH) start $(VM_NAME); \
	else \
		echo ">> boot da VM pulado (NO_BOOT setado ou virsh ausente)"; \
	fi

# ---- console serial de debug (SO DEV) ----
# Um comando: builda com o ntdbgcon ligado, redefine a VM com a COM1 num socket
# TCP do host, e boota. Depois conecte:  nc 127.0.0.1 4555  -> shell da NTUnix,
# pra rodar comandos/ler logs sem print/loop. Nunca use isto em build de producao.
debug-live:
	$(MAKE) live NTUNIX_DEBUG=1 NO_BOOT=1
	NTUNIX_DEBUG=1 ./build/vm-setup.sh live "$(LIVE_ISO)"
	@command -v $(firstword $(VIRSH)) >/dev/null 2>&1 && $(VIRSH) start $(VM_NAME) || true
	@echo ">> canal serial de debug pronto:  nc 127.0.0.1 $${NTUNIX_DBG_PORT:-4555}"

# ISO INSTALAVEL: a mesma midia live, mais o install.wim, mais o ntstrap.
# Boota no NTUnix; de la' voce roda `ntstrap` e ele instala no disco — particiona,
# aplica a imagem com DISM, configura os hives offline e chama o bcdboot. O Setup
# da Microsoft nao participa: e' por isso que nao ha tela de idioma, chave nem OOBE
# decidindo nada. Mesma relacao que a ISO do Arch tem com o pacstrap.
# A ISO traz os TRES perfis como imagens do mesmo wim (o conteudo comum e'
# armazenado uma vez so'), e o ntstrap pergunta qual instalar:
#   normal  ~4,8G instalado    .NET, 32 bits e drivers completos
#   leve    ~3,0G instalado    sem .NET nem 32 bits
#   debug    ~670MB            so' VM: base minima + navegador
#   make iso [WIN_ISO=/caminho/Win11.iso] [OUT_ISO=NTUnix.iso]
iso:
	@test -f "$(WIN_ISO)" || { echo "ISO do Windows nao encontrada: $(WIN_ISO)"; \
		echo "  ponha a ISO em build/deps/windows.iso ou passe WIN_ISO=/caminho.iso"; exit 1; }
	$(MAKE) busybox-nt
	$(MAKE) all
	NTUNIX_INSTALLER=1 ./build/make-live.sh "$(WIN_ISO)" "$(INST_ISO)"

# ---- VM de desenvolvimento (Windows INSTALADO + arvore do host montada) ----
# Fluxo novo, que substitui o ciclo "regera ISO de 830M e reboota":
#
#   make vm-install     UMA VEZ: instala o Windows no disco (APAGA build/vm/*.qcow2)
#   make vm             boota do disco, com out/ do host visivel via SMB
#   make                 <- daqui pra frente: recompila no host...
#   (no guest) ntctl restart dispd   ... e o guest ja ve o binario novo
#
# Instalado gasta menos RAM que a ISO live: o WinPE extrai o boot.wim inteiro
# pra um ramdisk, entao a imagem ocupa memoria enquanto roda.
vm-install:
	$(MAKE) iso
	./build/vm-setup.sh install "$(INST_ISO)"

vm: all
	./build/vm-setup.sh run

# modo antigo: ISO live (WinPE na RAM). Mantido pra testar a midia em si.
vm-live: all
	./build/vm-setup.sh live

# valida a base de build sem precisar de uma ISO (lint + dry-run)
check-build: all
	./test/build-check.sh

# baixa as fontes externas (musl, busybox) pra build/deps — persistente, NAO /tmp.
# Idempotente: no-op se ja presentes. Roda automatico antes dos builds que usam.
deps:
	./build/fetch-deps.sh

# libc LP64 própria, sem UCRT. A fonte da musl e a do BusyBox são externas.
musl-nt: deps
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" all

# compila a bateria da libc e valida que nada importa UCRT/MSVCRT (objdump).
# A EXECUCAO e' na VM — ver 'make vm'.
musl-nt-check: deps
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" test

busybox-nt: deps
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" \
		BUSYBOX_SRC="$(BUSYBOX_SRC)" busybox
	mkdir -p $(BIN)
	cp musl-nt/build/busybox.exe $(BIN)/busybox.exe

busybox-nt-check: deps
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" \
		BUSYBOX_SRC="$(BUSYBOX_SRC)" busybox-test

clean:
	rm -rf $(OUT) build/work

.PHONY: all clean stage-files live iso debug-live check-build deps musl-nt \
	vm-install vm vm-live \
	musl-nt-check busybox-nt busybox-nt-check
