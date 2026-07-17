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

BINS := $(BIN)/initd.exe $(BIN)/ntctl.exe $(BIN)/logd.exe $(BIN)/demod.exe \
        $(BIN)/ntsession.exe

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

$(BIN):
	mkdir -p $(BIN) $(OUT)/etc/units/enabled $(OUT)/var/log $(OUT)/run

stage-files: | $(BIN)
	cp etc/units/*.service $(OUT)/etc/units/
	cp etc/passwd etc/group etc/mtab etc/hosts $(OUT)/etc/
	mkdir -p $(OUT)/proc
	cp proc/mounts $(OUT)/proc/
	touch $(OUT)/etc/units/enabled/logd $(OUT)/etc/units/enabled/demod

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
