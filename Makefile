# NTUnix — build cruzado Linux → Windows (mingw-w64).
# `make` monta a árvore staged em out/ pronta pra rodar (Wine ou Windows).

CC     := x86_64-w64-mingw32-gcc
CFLAGS := -O2 -Wall -Wextra -static -D_WIN32_WINNT=0x0601
MUSL_SRC ?= /tmp/musl-1.2.6
BUSYBOX_SRC ?= /tmp/bbsrc

OUT := out
BIN := $(OUT)/system/bin

COMMON := src/common/ntupath.c src/common/ntuini.c src/common/ntuutil.c
INITD  := src/initd/initd.c src/initd/service.c src/initd/pipesrv.c
HDRS   := src/common/ntu.h src/initd/initd.h

# dispd (compositor/display server) + ntwm (window manager). GUI nativo.
# term_conpty.c e present_dxgi.c precisam de headers 0x0A00 -> TU isolada em out/obj.
DISPD_SRC := src/dispd/dispd.c src/dispd/compositor.c src/dispd/present_gdi.c \
             src/dispd/vt.c src/dispd/term.c src/dispd/term_scrape.c \
             src/dispd/term_pty.c \
             src/dispd/input.c src/dispd/wmproto.c src/dispd/appsrv.c
DISPD_HDR := src/dispd/dispd.h src/dispd/present.h src/dispd/term.h \
             src/common/ntu.h src/common/ntuwm.h
NTWM_SRC  := src/ntwm/ntwm.c src/ntwm/proto.c src/ntwm/layout.c

BINS := $(BIN)/initd.exe $(BIN)/ntctl.exe $(BIN)/logd.exe $(BIN)/demod.exe \
        $(BIN)/ntsession.exe $(BIN)/dispd.exe $(BIN)/ntwm.exe $(BIN)/ntclock.exe

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
	$(CC) $(CFLAGS) $(LIBVTERM_INC) -mwindows -o $@ $(DISPD_SRC) $(OUT)/obj/term_conpty.o $(OUT)/obj/present_dxgi.o $(LIBVTERM_OBJ) $(COMMON) -lgdi32 -luser32 -ldxguid

# window manager: so pipe + logica, sem libs extras.
$(BIN)/ntwm.exe: $(NTWM_SRC) $(COMMON) src/common/ntu.h src/common/ntuwm.h src/ntwm/ntwm.h | $(BIN)
	$(CC) $(CFLAGS) -mwindows -o $@ $(NTWM_SRC) $(COMMON)

# app demo (relogio): cliente da fronteira apps<->dispd; surface compartilhada.
$(BIN)/ntclock.exe: src/apps/ntclock/ntclock.c src/common/ntu.h | $(BIN)
	$(CC) $(CFLAGS) -mwindows -o $@ src/apps/ntclock/ntclock.c -lgdi32 -luser32

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

smoke: all
	./test/smoke.sh

# ISO LIVE (arch-be-like): boota direto no NTUnix, sem instalar. Base = WinPE.
#   make live WIN_ISO=/caminho/Win11.iso [OUT_ISO=NTUnix-live.iso]
live: all
	@test -n "$(WIN_ISO)" || { echo "uso: make live WIN_ISO=/caminho/Windows.iso"; exit 1; }
	./build/make-live.sh "$(WIN_ISO)" $(OUT_ISO)

# ISO instalavel (aplica o Windows completo estilo pacstrap). Fase posterior.
#   make iso WIN_ISO=/caminho/Win11.iso [OUT_ISO=NTUnix.iso]
iso: all
	@test -n "$(WIN_ISO)" || { echo "uso: make iso WIN_ISO=/caminho/Windows.iso"; exit 1; }
	./build/make-iso.sh "$(WIN_ISO)" $(OUT_ISO)

# valida a base de build sem precisar de uma ISO (lint + dry-run)
check-build: all
	./test/build-check.sh

# libc LP64 própria, sem UCRT. A fonte da musl e a do BusyBox são externas.
musl-nt:
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" all

musl-nt-test:
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" test

busybox-nt:
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" \
		BUSYBOX_SRC="$(BUSYBOX_SRC)" busybox
	mkdir -p $(BIN)
	cp musl-nt/build/busybox.exe $(BIN)/busybox.exe

busybox-nt-test:
	$(MAKE) -C musl-nt MUSL_SRC="$(MUSL_SRC)" \
		BUSYBOX_SRC="$(BUSYBOX_SRC)" busybox-test

clean:
	rm -rf $(OUT) build/work

.PHONY: all clean stage-files smoke live iso check-build musl-nt \
	musl-nt-test busybox-nt busybox-nt-test
