# musl-nt: LP64 musl 1.2.6 lowered to Win64/COFF plus the NT syscall backend.
MUSL_SRC ?= /tmp/musl-1.2.6
BUILD    ?= build
MINGW_CC ?= x86_64-w64-mingw32-gcc
AR       ?= x86_64-w64-mingw32-ar
RANLIB   ?= x86_64-w64-mingw32-ranlib
OBJDUMP  ?= x86_64-w64-mingw32-objdump
WINE     ?= wine
TIMEOUT  ?= timeout
BUSYBOX_SRC ?= /tmp/bbsrc
LP64_CC  := tools/lp64-coff-cc
CC_REWRITE := $(BUILD)/tools/sysv-coff-rewrite
ROOT     := $(abspath .)

MUSL_BASE := $(wildcard $(MUSL_SRC)/src/*/*.c) \
             $(wildcard $(MUSL_SRC)/src/malloc/mallocng/*.c)
MUSL_SKIP := $(MUSL_SRC)/src/env/__init_tls.c \
             $(MUSL_SRC)/src/thread/__set_thread_area.c \
             $(MUSL_SRC)/src/setjmp/setjmp.c \
             $(MUSL_SRC)/src/setjmp/longjmp.c \
             $(MUSL_SRC)/src/process/posix_spawn.c
MUSL_SRCS := $(filter-out $(MUSL_SKIP),$(MUSL_BASE))
MUSL_OBJS := $(patsubst $(MUSL_SRC)/src/%.c,$(BUILD)/musl/%.o,$(MUSL_SRCS))

NT_SRCS := $(wildcard nt/*.c)
NT_OBJS := $(patsubst nt/%.c,$(BUILD)/nt/%.o,$(NT_SRCS))
OVERRIDE_OBJS := $(BUILD)/override/__init_tls.o \
                 $(BUILD)/override/__set_thread_area.o \
                 $(BUILD)/override/syscall_cp.o \
                 $(BUILD)/override/posix_spawn.o \
                 $(BUILD)/override/setjmp.o
CRT0 := $(BUILD)/crt0.o
LIB  := $(BUILD)/libc-nt.a

NT_CFLAGS := -std=c11 -D_WIN32_WINNT=0x0A00 -O2 -Wall -Wextra -Werror \
             -ffreestanding -fno-builtin -fno-stack-protector -Iinclude

all: $(LIB) $(CRT0)

$(BUILD)/include/bits/alltypes.h: $(MUSL_SRC)/arch/x86_64/bits/alltypes.h.in \
                                  $(MUSL_SRC)/include/alltypes.h.in
	@mkdir -p $(@D)
	sed -f $(MUSL_SRC)/tools/mkalltypes.sed $^ > $@

$(BUILD)/include/bits/syscall.h: $(MUSL_SRC)/arch/x86_64/bits/syscall.h.in
	@mkdir -p $(@D)
	cp $< $@
	sed -n -e 's/__NR_/SYS_/p' $< >> $@

$(BUILD)/include/version.h: $(MUSL_SRC)/VERSION
	@mkdir -p $(@D)
	printf '#define VERSION "%s"\n' "$$(cat $<)" > $@

GENERATED := $(BUILD)/include/bits/alltypes.h $(BUILD)/include/bits/syscall.h \
             $(BUILD)/include/version.h

$(CC_REWRITE): tools/sysv-coff-rewrite.cpp
	@mkdir -p $(@D)
	clang++ -O2 -std=c++17 $$(llvm-config --cxxflags) $< -o $@ \
		$$(llvm-config --ldflags --libs core bitreader bitwriter irreader --system-libs)

$(BUILD)/musl/%.o: $(MUSL_SRC)/src/%.c $(GENERATED) $(LP64_CC) $(CC_REWRITE)
	@mkdir -p $(@D)
	MUSL_SRC=$(MUSL_SRC) SYSV_REWRITE=$(CC_REWRITE) $(LP64_CC) $< $@

$(BUILD)/nt/%.o: nt/%.c include/nt/ntabi.h include/nt/ntpriv.h
	@mkdir -p $(@D)
	$(MINGW_CC) $(NT_CFLAGS) -c $< -o $@

$(BUILD)/override/__init_tls.o: override/__init_tls.c $(GENERATED) $(LP64_CC) $(CC_REWRITE)
	@mkdir -p $(@D)
	MUSL_SRC=$(MUSL_SRC) SYSV_REWRITE=$(CC_REWRITE) $(LP64_CC) $< $@

$(BUILD)/override/__set_thread_area.o: override/__set_thread_area.c
	@mkdir -p $(@D)
	$(MINGW_CC) $(NT_CFLAGS) -c $< -o $@

$(BUILD)/override/syscall_cp.o: override/syscall_cp.c $(GENERATED) $(LP64_CC) $(CC_REWRITE)
	@mkdir -p $(@D)
	MUSL_SRC=$(MUSL_SRC) SYSV_REWRITE=$(CC_REWRITE) $(LP64_CC) $< $@

$(BUILD)/override/posix_spawn.o: override/posix_spawn.c $(GENERATED) $(LP64_CC) $(CC_REWRITE)
	@mkdir -p $(@D)
	MUSL_SRC=$(MUSL_SRC) SYSV_REWRITE=$(CC_REWRITE) $(LP64_CC) $< $@

$(BUILD)/override/setjmp.o: override/setjmp.S
	@mkdir -p $(@D)
	$(MINGW_CC) -c $< -o $@

$(CRT0): crt/crt0.c
	@mkdir -p $(@D)
	$(MINGW_CC) $(NT_CFLAGS) -c $< -o $@

$(LIB): $(MUSL_OBJS) $(NT_OBJS) $(OVERRIDE_OBJS)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(BUILD)/test/%.o: test/%.c $(GENERATED) $(LP64_CC) $(CC_REWRITE)
	@mkdir -p $(@D)
	MUSL_SRC=$(MUSL_SRC) SYSV_REWRITE=$(CC_REWRITE) $(LP64_CC) $< $@

$(BUILD)/test/%.exe: $(BUILD)/test/%.o $(LIB) $(CRT0) ntposix-gcc
	./ntposix-gcc -o $@ $<

$(BUILD)/test/etc/hosts: ../etc/hosts
	@mkdir -p $(@D)
	cp $< $@

hello: $(BUILD)/test/hello.exe
memory: $(BUILD)/test/memory.exe
network: $(BUILD)/test/network.exe $(BUILD)/test/etc/hosts
smoke: $(BUILD)/test/smoke.exe
	cd $(BUILD)/test && env WINEDEBUG=-all $(TIMEOUT) --kill-after=2s 20s \
		$(WINE) smoke.exe

test: $(BUILD)/test/hello.exe $(BUILD)/test/smoke.exe \
      $(BUILD)/test/memory.exe $(BUILD)/test/network.exe \
      $(BUILD)/test/spawn.exe \
      $(BUILD)/test/etc/hosts check-imports
	cd $(BUILD)/test && for executable in hello.exe smoke.exe memory.exe \
		network.exe spawn.exe; do \
		echo "== $$executable =="; \
		env WINEDEBUG=-all $(TIMEOUT) --kill-after=2s 20s \
			$(WINE) "$$executable" || exit $$?; \
	done

check-imports: $(BUILD)/test/hello.exe
	@imports=$$($(OBJDUMP) -p $< 2>/dev/null | sed -n 's/.*DLL Name: //p'); \
	 echo "$$imports"; \
	 ! echo "$$imports" | grep -qiE 'ucrt|msvcrt|api-ms-win-crt'

busybox: all
	@test -d "$(BUSYBOX_SRC)" || { \
		echo "BusyBox não encontrado: $(BUSYBOX_SRC)" >&2; exit 2; }
	$(ROOT)/tools/configure-busybox "$(BUSYBOX_SRC)" \
		$(ROOT)/test/busybox-coreutils.config
	$(MAKE) -C "$(BUSYBOX_SRC)" -j$${JOBS:-8} ARCH=x86_64 \
		CC=$(ROOT)/ntposix-gcc LD=x86_64-w64-mingw32-ld \
		AR=x86_64-w64-mingw32-ar NM=x86_64-w64-mingw32-nm \
		STRIP=x86_64-w64-mingw32-strip
	cp "$(BUSYBOX_SRC)/busybox" "$(BUILD)/busybox.exe"
	cp test/busybox-runtime.cmd "$(BUILD)/busybox-runtime.cmd"
	mkdir -p "$(BUILD)/etc"
	cp ../etc/passwd ../etc/group ../etc/mtab "$(BUILD)/etc/"
	cp ../etc/hosts "$(BUILD)/etc/"
	mkdir -p "$(BUILD)/proc"
	cp ../proc/mounts "$(BUILD)/proc/"

check-busybox-imports: busybox
	@imports=$$($(OBJDUMP) -p $(BUILD)/busybox.exe 2>/dev/null | \
		sed -n 's/.*DLL Name: //p'); \
	 echo "$$imports"; \
	 ! echo "$$imports" | grep -qiE 'ucrt|msvcrt|api-ms-win-crt'

busybox-test: check-busybox-imports
	cd $(BUILD) && env WINEDEBUG=-all $(TIMEOUT) --kill-after=2s 60s \
		$(WINE) cmd /d /c busybox-runtime.cmd busybox.exe

clean:
	rm -rf $(BUILD)

.PHONY: all hello memory network smoke test check-imports busybox \
	check-busybox-imports busybox-test clean

.DELETE_ON_ERROR:
