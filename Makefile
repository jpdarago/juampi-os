# ---------------------------------------------------------------------------
# juampi-os top-level build (x86-64, booted by Limine)
#
# Common targets:
#   make            build the kernel and the bootable UEFI image (boot.img)
#   make kernel.bin build just the kernel binary
#   make run        boot the image in QEMU under OVMF
#   make test       headless boot-smoke test (tests/boot-smoke.sh)
#   make format     reformat all C sources/headers in place with clang-format
#   make lint       check formatting without modifying files (used by CI)
#   make clean      remove all build artifacts
#   make help       list the available targets
# ---------------------------------------------------------------------------

# Toolchain. The build host is x86-64, so the host GCC emits freestanding
# 64-bit kernel code directly. Set CROSS to a cross prefix (e.g. x86_64-elf-)
# to build with a cross toolchain instead, as needed on macOS.
CROSS        ?=
CC            = $(CROSS)gcc
LD            = $(CROSS)ld
AR            = $(CROSS)ar
CLANG_FORMAT  = clang-format
# Native compiler for build-time host tools (e.g. the logo generator). Never
# the cross prefix: these run on the build host, not the target.
HOSTCC       ?= cc

# Directories.
SRC_DIR     := src
INCLUDE_DIR := include
OBJ_DIR     := obj
BUILD_DIR   := build

# Compiler / assembler / linker flags.
CFLAGS := -O2 -std=c11 -Werror -Wall -Wextra \
	-Wno-unused-parameter -Wno-override-init \
	-Wno-address-of-packed-member \
	-Wunreachable-code -Wshadow -Wcast-qual \
	-Wformat=2 -Wwrite-strings -Wstrict-prototypes \
	-Wredundant-decls -Wnested-externs -Wmissing-include-dirs \
	-Wjump-misses-init -Wlogical-op \
	-nostdlib -fno-builtin -nostartfiles -fno-strict-aliasing \
	-nodefaultlibs -fno-stack-protector -I$(INCLUDE_DIR) \
	-I$(SRC_DIR)/bearssl/inc \
	-mno-mmx -mno-3dnow \
	-mno-red-zone -mcmodel=kernel -fno-pic -fno-pie \
	-fno-omit-frame-pointer \
	-DPRINTF_ALIAS_STANDARD_FUNCTION_NAMES_HARD=1
# -fno-omit-frame-pointer keeps a walkable rbp chain for panic backtraces.
# The last flag makes the vendored printf provide the standard names
# (printf/snprintf/vsnprintf/...) as real symbols; the kernel supplies putchar_.
# SSE/SSE2 are enabled (for double arithmetic, e.g. Lua): the entry stub turns
# on CR4.OSFXSR before any C runs, and the context switch saves FP/SSE state
# with fxsave/fxrstor. MMX/3DNow stay off (legacy, unused). -mno-red-zone is
# mandatory for kernel code: the SysV red zone is unsafe once interrupts reuse
# the stack.

# Generate per-object .d dependency files so header edits trigger rebuilds.
CPPFLAGS  := -MMD -MP
# Assembly (.S, GNU assembler via gcc): the files carry their own .note.GNU-stack
# so no exec-stack markers are needed here.
LINKSCRIPT := $(BUILD_DIR)/linker.ld
LDFLAGS   := -melf_x86_64 -z max-page-size=0x1000 -T $(LINKSCRIPT)

# Sources and (out-of-tree) objects. Kernel sources live in a flat src/ dir;
# src/flanterm/ is the vendored flanterm terminal emulator (kept verbatim).
# Assembly is GNU-assembler .S (assembled by gcc); there is no NASM dependency.
CSOURCES   := $(wildcard $(SRC_DIR)/*.c)
ASMSOURCES := $(wildcard $(SRC_DIR)/*.S)
# Vendored third-party C (flanterm terminal, printf), compiled verbatim.
VENDOR_CSOURCES := $(SRC_DIR)/flanterm/flanterm.c \
	$(SRC_DIR)/flanterm/flanterm_backends/fb.c \
	$(SRC_DIR)/printf/printf.c
# Embedded Lua (src/lua/): the vendored Lua 5.4 core+libs, the freestanding libc
# shim it runs on, and the kernel glue — all built with the Lua include path
# (klibc stubs first, then the Lua headers) and warnings off.
LUA_CSOURCES := $(wildcard $(SRC_DIR)/lua/*.c)
LUA_OBJS     := $(patsubst $(SRC_DIR)/lua/%.c,$(OBJ_DIR)/lua/%.o,$(LUA_CSOURCES))
LUA_ASM_OBJ  := $(OBJ_DIR)/lua/klibc_setjmp.o
LUA_INC      := -I$(SRC_DIR)/lua/klibc -Iinclude/lua

# Shell syntax highlighter (src/highlight/): a Ragel-generated Lua scanner over a
# gperf perfect-hash keyword table. Both .c files are generated then committed,
# so a normal build compiles them like any other source (no ragel/gperf needed);
# `make highlight-gen` regenerates them from the .rl/.gperf. Compiled verbatim
# (-w) with the klibc <string.h> on the include path — the gperf lookup calls
# memcmp — since generated code does not pass the kernel warning gauntlet.
HL_DIR   := $(SRC_DIR)/highlight
HL_OBJS  := $(OBJ_DIR)/highlight/highlight.o $(OBJ_DIR)/highlight/lua_keywords.o

# Vendored picohttpparser (src/http/): the HTTP response parser used by src/http.c.
# Compiled verbatim (-w) with the klibc <string.h>/<assert.h> on the include path,
# like the other vendored code; the SSE4.2 fast path is #ifdef'd off in our build.
HTTP_OBJS := $(OBJ_DIR)/http/picohttpparser.o

# Vendored microui (src/microui/): the immediate-mode UI used by src/ui.c.
# Compiled verbatim (-w) with the klibc <stdio.h>/<stdlib.h>/<string.h> on the
# include path (it needs sprintf/qsort/memcpy, all provided by the shim).
MICROUI_OBJS := $(OBJ_DIR)/microui/microui.o

# Vendored stb_truetype (src/stb/): the font rasteriser behind src/ttf.c. Built
# verbatim (-w); every libc dependency is routed through STBTT_* macros to
# kernel facilities in stb_truetype_impl.c, so it needs no klibc include.
STB_OBJS := $(OBJ_DIR)/stb/stb_truetype_impl.o

# Vendored BearSSL (src/bearssl/): the TLS library for HTTPS. Its whole src/ tree
# compiles freestanding (no malloc, no OS deps); built verbatim (-w) with its own
# includes + the klibc <string.h>. sysrng.c (the /dev/urandom seeder) was dropped
# — entropy is injected from RDRAND. Objects mirror the source tree under obj/.
BEARSSL_DIR  := $(SRC_DIR)/bearssl
BEARSSL_SRCS := $(shell find $(BEARSSL_DIR)/src -name '*.c' 2>/dev/null)
BEARSSL_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(BEARSSL_SRCS))
BEARSSL_INC  := -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src -I$(SRC_DIR)/lua/klibc

COBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CSOURCES))
ASMOBJS    := $(patsubst $(SRC_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASMSOURCES))
# Vendored uACPI (src/uacpi/, pinned 6.0.0): the full ACPI stack — table access
# + AML interpreter + namespace. Built with its own builtin string helpers,
# verbatim (-w). uACPI includes as <uacpi/...>, which CFLAGS' -Iinclude resolves.
UACPI_SRCS := $(wildcard $(SRC_DIR)/uacpi/*.c)
UACPI_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(UACPI_SRCS))
UACPI_DEF  := -DUACPI_USE_BUILTIN_STRING

# Vendored newlib subset (src/newlib/, curated from the linker map of the hosted
# demos) — the libc for hosted C programs (build/hosted/), NOT the kernel. Built
# per-file with the host GCC into a static archive that hosted programs link
# against, with our crt0 + int-0x80 libgloss stubs. See docs/hosted-libc.md.
# _mallocr.c is #included by mallocr.c/freer.c/... — not compiled on its own.
NEWLIB_DIR  := $(SRC_DIR)/newlib
NEWLIB_SRCS := $(filter-out $(NEWLIB_DIR)/stdlib/_mallocr.c,\
	$(shell find $(NEWLIB_DIR) -name '*.c' 2>/dev/null))
NEWLIB_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(NEWLIB_SRCS))
HOSTED_DIR  := $(BUILD_DIR)/hosted
HOSTED_LIB  := $(HOSTED_DIR)/libnewlib.a
# Hosted-userland flags: newlib headers only (no host libc, no kernel headers),
# default code model (programs load low at 0x400000). _LIBC disables newlib's
# fortify/ssp wrappers when compiling libc itself; MISSING_SYSCALL_NAMES makes
# the reent layer call our bare-named syscalls; HAVE_MMAP=0 -> sbrk-only malloc.
HOSTED_CFLAGS := -O2 -std=gnu11 -ffreestanding -nostdinc \
	-isystem $(shell $(CC) -print-file-name=include) -I$(NEWLIB_DIR)/include \
	-I$(NEWLIB_DIR)/libm -I$(HOSTED_DIR) \
	-w -fno-pic -fno-pie -fno-stack-protector -fno-builtin -mno-red-zone \
	-DMISSING_SYSCALL_NAMES -DHAVE_MMAP=0 -D_LIBC
LIBGCC       := $(shell $(CC) -print-libgcc-file-name)
# Hosted demo programs, shipped as Limine modules (run("chello.elf")).
HOSTED_PROGS := chello filetest gfxdemo
HOSTED_ELVES := $(patsubst %,$(HOSTED_DIR)/%.elf,$(HOSTED_PROGS))

VENDOR_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(VENDOR_CSOURCES))
OBJS       := $(COBJS) $(ASMOBJS) $(VENDOR_OBJS) $(LUA_OBJS) $(LUA_ASM_OBJ) $(HL_OBJS) $(HTTP_OBJS) $(MICROUI_OBJS) $(STB_OBJS) $(BEARSSL_OBJS) $(UACPI_OBJS)
DEPS       := $(COBJS:.o=.d) $(VENDOR_OBJS:.o=.d) $(LUA_OBJS:.o=.d) $(HL_OBJS:.o=.d) $(HTTP_OBJS:.o=.d) $(MICROUI_OBJS:.o=.d) $(STB_OBJS:.o=.d) $(BEARSSL_OBJS:.o=.d) $(UACPI_OBJS:.o=.d)

KERNEL := kernel.bin

# Every C source/header that clang-format should manage (kernel + userland).
# include/limine.h is vendored from the Limine project and kept verbatim, so it
# is excluded from our formatting rules.
FORMAT_FILES := $(filter-out $(INCLUDE_DIR)/limine.h,$(wildcard \
	$(SRC_DIR)/*.c $(INCLUDE_DIR)/*.h $(BUILD_DIR)/lab/*.c))

# QEMU drives both `make run` and `make test`. make run opens a GTK window;
# override the backend if you prefer, e.g. `make run QEMU_DISPLAY=curses`.
QEMU         ?= qemu-system-x86_64
# Machine realism: q35 (PCIe chipset, the same shape as the XPS target) and,
# when KVM is available, the host CPU model — the dev box IS the XPS, so
# `-cpu host` exposes the target's real CPUID/MSR surface, including the PMU.
# Without /dev/kvm (CI) fall back to TCG, where `-cpu host` is unavailable.
KVM_OK := $(shell test -w /dev/kvm 2>/dev/null && echo 1)
ifeq ($(KVM_OK),1)
QEMU_MACHINE := -machine q35 -accel kvm -cpu host
else
QEMU_MACHINE := -machine q35 -accel tcg
endif
QEMU_DISPLAY ?= gtk
# Host audio backend for `make run`'s AC'97 device (a QEMU -audiodev driver;
# see `qemu-system-x86_64 -audiodev help`). Override for your system, e.g.
# `make run QEMU_AUDIO=pipewire` / alsa / sdl; `none` keeps audio.* working but
# silent.
QEMU_AUDIO   ?= pa
# Number of cores QEMU exposes (drives the SMP bring-up). Keep the value on its
# own line: a trailing inline comment would leave whitespace in the value, which
# breaks the quoted `-smp "$QEMU_SMP"` in tests/boot-smoke.sh.
QEMU_SMP     ?= 4
export QEMU QEMU_SMP

.PHONY: all run test clean format lint help

all: $(KERNEL) boot.img

# --- Kernel -----------------------------------------------------------------

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Flag stamps: make only compares file mtimes, so editing a rule's compile
# flags (say, adding a -D) leaves stale objects behind — the exact failure mode
# that once shipped a silent Doom (a pre-FEATURE_SOUND i_sound.o). Each stamp
# file holds its rule family's full command prefix and is rewritten only when
# that changes; objects depend on their family's stamp, so a flags edit
# rebuilds exactly that family and an unchanged one rebuilds nothing.
.PHONY: FORCE
FORCE:
$(OBJ_DIR)/.flags.kernel: FORCE | $(OBJ_DIR)
	@printf '%s\n' '$(CC) $(CFLAGS) $(CPPFLAGS)' | cmp -s - $@ 2>/dev/null || \
		printf '%s\n' '$(CC) $(CFLAGS) $(CPPFLAGS)' > $@
$(OBJ_DIR)/.flags.hosted: FORCE | $(OBJ_DIR)
	@printf '%s\n' '$(CC) $(HOSTED_CFLAGS)' | cmp -s - $@ 2>/dev/null || \
		printf '%s\n' '$(CC) $(HOSTED_CFLAGS)' > $@
$(OBJ_DIR)/.flags.doom: FORCE | $(OBJ_DIR)
	@printf '%s\n' '$(CC) $(HOSTED_CFLAGS) -I$(DOOM_DIR) -DFEATURE_SOUND' | \
		cmp -s - $@ 2>/dev/null || \
		printf '%s\n' '$(CC) $(HOSTED_CFLAGS) -I$(DOOM_DIR) -DFEATURE_SOUND' > $@
$(OBJ_DIR)/.flags.lab: FORCE | $(OBJ_DIR)
	@printf '%s\n' '$(CC) $(LAB_CFLAGS)' | cmp -s - $@ 2>/dev/null || \
		printf '%s\n' '$(CC) $(LAB_CFLAGS)' > $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(OBJ_DIR)/.flags.kernel | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# GNU-assembler sources (.S): assembled by gcc (runs cpp + as).
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S $(OBJ_DIR)/.flags.kernel | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) -c -o $@ $<

# Vendored third-party C (flanterm, printf): compiled with our kernel flags but
# without our warning gauntlet (kept verbatim). The %-stem includes the subdir.
$(OBJ_DIR)/flanterm/%.o: $(SRC_DIR)/flanterm/%.c $(OBJ_DIR)/.flags.kernel \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

# Vendored newlib: host GCC, hosted flags (more specific than the generic obj
# rule, so it wins for src/newlib/**). These objects go into libnewlib.a for
# hosted programs, never into the kernel.
$(OBJ_DIR)/newlib/%.o: $(SRC_DIR)/newlib/%.c $(OBJ_DIR)/.flags.hosted \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOSTED_CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(HOSTED_LIB): $(NEWLIB_OBJS)
	$(AR) rcs $@ $^

# crt0 + libgloss stubs, compiled against the newlib headers.
$(OBJ_DIR)/hosted/crt0.o: $(HOSTED_DIR)/crt0.S $(OBJ_DIR)/.flags.hosted \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOSTED_CFLAGS) $(CPPFLAGS) -c -o $@ $<
$(OBJ_DIR)/hosted/syscalls.o: $(HOSTED_DIR)/syscalls.c $(OBJ_DIR)/.flags.hosted \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOSTED_CFLAGS) $(CPPFLAGS) -c -o $@ $<

# A hosted program: compile against newlib, then link static at 0x400000 with
# our crt0 + stubs + the vendored libc + the compiler's libgcc (entry _start).
$(HOSTED_DIR)/%.elf: $(HOSTED_DIR)/%.c $(OBJ_DIR)/hosted/crt0.o \
		$(OBJ_DIR)/hosted/syscalls.o $(HOSTED_LIB) $(OBJ_DIR)/.flags.hosted
	$(CC) $(HOSTED_CFLAGS) $(CPPFLAGS) -c -o $(OBJ_DIR)/hosted/$*.o $<
	$(CC) -nostdlib -static -no-pie -Wl,-Ttext-segment=0x400000 \
		$(OBJ_DIR)/hosted/crt0.o $(OBJ_DIR)/hosted/$*.o \
		$(OBJ_DIR)/hosted/syscalls.o $(HOSTED_LIB) $(LIBGCC) -o $@

$(OBJ_DIR)/printf/%.o: $(SRC_DIR)/printf/%.c $(OBJ_DIR)/.flags.kernel \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

# Generated highlighter (ragel/gperf output): kernel flags, warnings off, with
# the klibc <string.h> ahead of the gcc one (the gperf lookup calls memcmp).
$(OBJ_DIR)/highlight/%.o: $(HL_DIR)/%.c $(OBJ_DIR)/.flags.kernel | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -I$(SRC_DIR)/lua/klibc $(CPPFLAGS) -c -o $@ $<

# Vendored picohttpparser: same treatment (verbatim, klibc headers).
$(OBJ_DIR)/http/%.o: $(SRC_DIR)/http/%.c $(OBJ_DIR)/.flags.kernel | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -I$(SRC_DIR)/lua/klibc $(CPPFLAGS) -c -o $@ $<

# Vendored microui: same treatment (verbatim, klibc headers for sprintf/qsort).
$(OBJ_DIR)/microui/%.o: $(SRC_DIR)/microui/%.c $(OBJ_DIR)/.flags.kernel \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -I$(SRC_DIR)/lua/klibc $(CPPFLAGS) -c -o $@ $<

# Vendored stb_truetype: verbatim (-w). No klibc include — the STBTT_* macros in
# stb_truetype_impl.c route every dependency to kernel headers already on
# $(CFLAGS)'s -Iinclude path.
$(OBJ_DIR)/stb/%.o: $(SRC_DIR)/stb/%.c $(OBJ_DIR)/.flags.kernel \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

# Vendored BearSSL: verbatim (-w) with its own includes; objects mirror the tree.
$(OBJ_DIR)/bearssl/%.o: $(SRC_DIR)/bearssl/%.c $(OBJ_DIR)/.flags.kernel \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(BEARSSL_INC) $(CPPFLAGS) -c -o $@ $<

# Vendored uACPI: verbatim (-w), barebones tables-only mode + builtin strings.
$(OBJ_DIR)/uacpi/%.o: $(SRC_DIR)/uacpi/%.c $(OBJ_DIR)/.flags.kernel \
		| $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(UACPI_DEF) $(CPPFLAGS) -c -o $@ $<

# Generated TLS trust-anchor table: -w (its initialisers cast away const). An
# explicit target so it overrides the generic src/%.o gauntlet rule.
$(OBJ_DIR)/tls_trust_anchors.o: $(SRC_DIR)/tls_trust_anchors.c \
		$(OBJ_DIR)/.flags.kernel | $(OBJ_DIR)
	$(CC) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

# Embedded Lua: Lua include path (klibc stubs win for <string.h> etc.), no warns.
$(OBJ_DIR)/lua/%.o: $(SRC_DIR)/lua/%.c $(OBJ_DIR)/.flags.kernel | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(LUA_INC) $(CPPFLAGS) -c -o $@ $<

$(OBJ_DIR)/lua/%.o: $(SRC_DIR)/lua/%.S $(OBJ_DIR)/.flags.kernel | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -c -o $@ $<

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Regenerate the committed highlighter sources from the Ragel grammar and gperf
# keyword list. Not part of the normal build (the .c files are committed, like
# build/scripts/logo.qoi): run it after editing highlight.rl / lua_keywords.gperf.
# The tools live in the devenv; guarded so a checkout without them still builds.
RAGEL ?= ragel
GPERF ?= gperf
.PHONY: highlight-gen
highlight-gen:
	@if command -v $(RAGEL) >/dev/null 2>&1; then \
		echo "RAGEL  $(HL_DIR)/highlight.c"; \
		$(RAGEL) -C -o $(HL_DIR)/highlight.c $(HL_DIR)/highlight.rl; \
	else echo "ragel not found; keeping committed $(HL_DIR)/highlight.c"; fi
	@if command -v $(GPERF) >/dev/null 2>&1; then \
		echo "GPERF  $(HL_DIR)/lua_keywords.c"; \
		$(GPERF) --output-file=$(HL_DIR)/lua_keywords.c $(HL_DIR)/lua_keywords.gperf; \
	else echo "gperf not found; keeping committed $(HL_DIR)/lua_keywords.c"; fi

# --- Userland + boot image --------------------------------------------------

# OVMF UEFI firmware, resolved from nixpkgs at recipe time (only `run`/`test`
# need it, so we don't pay for it on every make invocation). Override to point
# at a local firmware file if you are not using Nix.
OVMF_FD ?= $$(nix build --no-link --print-out-paths nixpkgs\#OVMF.fd)/FV/OVMF.fd

# Lua scripts shipped as Limine modules (init.lua runs at startup; the rest are
# available via run("name.lua") in the shell).
SCRIPTS := $(wildcard $(BUILD_DIR)/scripts/*.lua)

# Boot logo: build/scripts/logo.qoi is a checked-in QOI image (decoded by the
# kernel's src/qoi.c and blitted by fb.image(); see init.lua / logo.lua). It is
# a static asset, so the normal build needs no image tooling. Regenerate it from
# the source PNG with `make logo` (needs ImageMagick): that resizes the art,
# flood-fills the (connected) background to transparent so the logo composes
# over the console instead of showing as a square, and encodes the RGBA to QOI
# with the reference codec via the png2qoi host tool.
LOGO       := $(BUILD_DIR)/scripts/logo.qoi
LOGO_SRC   := $(BUILD_DIR)/assets/logo.png
LOGO_SIZE  ?= 256
# Last pixel index (LOGO_SIZE-1), used as the corner flood-fill seeds.
LOGO_MAX   := $(shell expr $(LOGO_SIZE) - 1)
PNG2QOI    := $(BUILD_DIR)/tools/png2qoi
MAGICK     ?= magick

$(PNG2QOI): $(BUILD_DIR)/tools/png2qoi.c $(BUILD_DIR)/tools/qoi.h
	$(HOSTCC) -O2 -I$(BUILD_DIR)/tools -o $@ $<

.PHONY: logo
logo: $(PNG2QOI) $(LOGO_SRC) | $(OBJ_DIR)
	$(MAGICK) $(LOGO_SRC) -resize $(LOGO_SIZE)x$(LOGO_SIZE)\! -alpha set \
		-fuzz 22% -fill none \
		-draw "alpha 0,0 floodfill" \
		-draw "alpha $(LOGO_MAX),0 floodfill" \
		-draw "alpha 0,$(LOGO_MAX) floodfill" \
		-draw "alpha $(LOGO_MAX),$(LOGO_MAX) floodfill" \
		-depth 8 RGBA:$(OBJ_DIR)/logo.rgba
	$(PNG2QOI) $(LOGO_SIZE) $(LOGO_SIZE) $(OBJ_DIR)/logo.rgba $(LOGO)

# --- Lab: native benchmark binaries ----------------------------------------
# C programs in build/lab/, compiled freestanding and linked static at a fixed
# VA, are loaded and called directly in ring 0 by the `lab` Lua library (see
# include/lab.h): a "sterile lab" for benchmarking algorithm implementations.
# -mno-red-zone is mandatory — they run in ring 0, where an interrupt reuses the
# stack. Entry symbol is `bench`; they include <lab.h> for the ABI.
# -fcf-protection=none stops gcc emitting a .note.gnu.property (CET) section,
# which on some toolchains gets an LMA overlapping .text under -Ttext (CI).
LAB_DIR    := $(BUILD_DIR)/lab
# raytracer.c is a native lab program shipped on the ext2 disk instead of as a
# Limine module (see the data-disk section), so it is excluded here — modules
# resolve before the disk in run(), and would shadow the on-disk copy.
LAB_SRCS   := $(filter-out $(LAB_DIR)/raytracer.c,$(wildcard $(LAB_DIR)/*.c))
LAB_ELVES  := $(patsubst $(LAB_DIR)/%.c,$(LAB_DIR)/%.elf,$(LAB_SRCS))
LAB_CFLAGS := -O2 -std=c11 -ffreestanding -nostdlib -fno-pic -fno-pie \
	-mno-red-zone -mno-mmx -mno-3dnow -fno-stack-protector \
	-fcf-protection=none -Wall -Wextra -I$(INCLUDE_DIR)

$(LAB_DIR)/%.elf: $(LAB_DIR)/%.c $(INCLUDE_DIR)/lab.h $(OBJ_DIR)/.flags.lab
	$(CC) $(LAB_CFLAGS) -c -o $(@:.elf=.o) $<
	$(LD) -melf_x86_64 -e bench -Ttext 0x400000 -o $@ $(@:.elf=.o)

# Build all lab binaries (a gcc/ld-only "userland" target for CI).
.PHONY: lab
lab: $(LAB_ELVES)

# QOA audio assets shipped as Limine modules (decoded by src/qoa.c, played via
# audio.play("name.qoa")); always available regardless of which disk is mounted.
SNDASSETS := $(wildcard $(BUILD_DIR)/scripts/*.qoa)

# Everything shipped to the image as a Limine module.
MODULES := $(SCRIPTS) $(LOGO) $(SNDASSETS) $(LAB_ELVES) $(HOSTED_ELVES)

# Pack the kernel and the modules (scripts + logo) into a bootable UEFI image
# with Limine (sudo-free, mtools).
boot.img: $(KERNEL) $(MODULES) $(BUILD_DIR)/limine.conf $(BUILD_DIR)/mkboot.sh
	bash $(BUILD_DIR)/mkboot.sh $(KERNEL) $@ $(MODULES)

# --- Data disk (ext2) -------------------------------------------------------
# A second, non-boot disk carrying an ext2 filesystem, attached to QEMU as the
# primary IDE slave; the kernel's ata.c + ext2.c read it, backing the `disk`
# and `fs` Lua libraries and run()-from-disk. Built from build/disk/ with
# mke2fs -d (no root needed). The feature set is deliberately conservative
# (1 KiB blocks; no resize_inode/dir_index/ext_attr) — exactly what the
# read-only reader in src/ext2.c supports. Drop files into build/disk/ to ship
# them on the disk.
DISK_DIR    := $(BUILD_DIR)/disk
# The raytracer is compiled straight into the disk tree (a native ring-0 lab
# program; see build/lab/raytracer.c). It is excluded from the plain file list so
# it is always the freshly built binary, and added as an explicit prerequisite.
DISK_RAYTRACER := $(DISK_DIR)/raytracer.elf
DISK_FILES  := $(filter-out $(DISK_RAYTRACER),$(shell find $(DISK_DIR) -type f 2>/dev/null))
DISK_IMG    := disk.img
DISK_BLOCKS ?= 16384  # 1 KiB blocks -> 16 MiB image (fits the Doom WAD)

# Build the native raytracer into the disk tree with the lab flags/ABI (entry
# `bench`, linked at 0x400000), so run("raytracer.elf") loads it from ext2.
$(DISK_RAYTRACER): $(LAB_DIR)/raytracer.c $(INCLUDE_DIR)/lab.h
	$(CC) $(LAB_CFLAGS) -c -o $(@:.elf=.o) $<
	$(LD) -melf_x86_64 -e bench -Ttext 0x400000 -o $@ $(@:.elf=.o)
	rm -f $(@:.elf=.o)

# A scalable font on the data disk for the TrueType rasteriser (src/ttf.c) and
# its boot self-test. Copied deterministically from the committed source asset
# (assets/fonts/) so a fresh clone / CI always has it — DejaVu Sans Mono is
# freely redistributable (assets/fonts/DejaVuSansMono-LICENSE.txt). The disk copy
# is generated (gitignored); the source in assets/ is what's version-controlled.
FONT_SRC  := assets/fonts/DejaVuSansMono.ttf
DISK_FONT := $(DISK_DIR)/font.ttf
$(DISK_FONT): $(FONT_SRC) | $(DISK_DIR)
	cp $< $@

$(DISK_IMG): $(DISK_FILES) $(DISK_RAYTRACER) $(DISK_FONT)
	mke2fs -q -F -t ext2 -b 1024 -O ^resize_inode,^dir_index,^ext_attr \
		-d $(DISK_DIR) $@ $(DISK_BLOCKS)

# --- Doom (doomgeneric), a hosted program (see docs/hosted-libc.md) ----------
# The engine sources are vendored under build/hosted/doom/ (GPLv2 — see the
# license note in that dir); our frontend is doomgeneric_juampi.c. doom.elf and
# the WAD live on the ext2 disk (not the boot image); build with `make doom`,
# then boot with the data disk and run("doom.elf"). `make doom-src` re-fetches
# the upstream engine (only needed to update the vendored copy).
DOOM_DIR  := $(HOSTED_DIR)/doom
DOOM_SRCS := $(wildcard $(DOOM_DIR)/*.c)
DOOM_OBJS := $(patsubst $(HOSTED_DIR)/%.c,$(OBJ_DIR)/hosted/%.o,$(DOOM_SRCS))
DOOM_WAD_URL ?= https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad

$(OBJ_DIR)/hosted/doom/%.o: $(DOOM_DIR)/%.c $(OBJ_DIR)/.flags.doom | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOSTED_CFLAGS) -I$(DOOM_DIR) -DFEATURE_SOUND $(CPPFLAGS) \
		-c -o $@ $<

$(DISK_DIR)/doom.elf: $(DOOM_OBJS) $(OBJ_DIR)/hosted/crt0.o \
		$(OBJ_DIR)/hosted/syscalls.o $(HOSTED_LIB)
	$(CC) -nostdlib -static -no-pie -Wl,-Ttext-segment=0x400000 \
		$(OBJ_DIR)/hosted/crt0.o $(DOOM_OBJS) \
		$(OBJ_DIR)/hosted/syscalls.o $(HOSTED_LIB) $(LIBGCC) -o $@

# Fetch + prune the doomgeneric engine sources (keeps our frontend).
doom-src:
	bash $(HOSTED_DIR)/fetch-doom.sh

# Fetch the shareware WAD onto the ext2 disk tree (not committed).
doom-wad: | $(DISK_DIR)
	@test -f $(DISK_DIR)/doom1.wad || \
		curl -fL -o $(DISK_DIR)/doom1.wad "$(DOOM_WAD_URL)"
	@ls -l $(DISK_DIR)/doom1.wad

# Build Doom onto the data disk (fetch the WAD, link the vendored engine, rebuild
# the ext2 image).
doom: doom-wad
	$(MAKE) $(DISK_DIR)/doom.elf
	$(MAKE) $(DISK_IMG)
	@echo 'Doom ready. Boot with the data disk, then run("doom.elf")'

.PHONY: doom doom-src doom-wad

# QEMU args attaching the data disk for `make run`. NVMe (not IDE): it's the
# faster path (DMA + MSI-X completions vs ATA PIO) and matches the real target
# (the XPS is NVMe-only), so `run("doom.elf")` etc. load from ext2-on-NVMe. The
# ext2 mount prefers NVMe > USB > ATA. (The smoke tests attach an IDE disk of
# their own, so ata.c keeps its coverage.)
DISK_QEMU := -drive file=$(DISK_IMG),format=raw,if=none,id=juampidisk \
	-device nvme,serial=juampidisk,drive=juampidisk

# Boot the OS in QEMU under OVMF. Limine loads the kernel straight into 64-bit
# long mode. OVMF vars must be writable, so we boot from a private copy.
run: boot.img $(DISK_IMG)
	cp "$(OVMF_FD)" .ovmf.fd && chmod +w .ovmf.fd
	$(QEMU) -bios .ovmf.fd \
		-drive file=boot.img,format=raw,if=none,id=jboot \
		-device ide-hd,drive=jboot,bootindex=0 -m 512 \
		-smp $(QEMU_SMP) $(QEMU_MACHINE) \
		$(DISK_QEMU) \
		-nic user,model=e1000 \
		-audiodev $(QEMU_AUDIO),id=snd -device AC97,audiodev=snd \
		-display $(QEMU_DISPLAY) -serial stdio -no-reboot

# Boot the Limine image headless and drive the shell over both input paths:
# serial (boot-smoke) and the PS/2 keyboard via QMP send-key (kbd-smoke).
test: boot.img $(DISK_IMG)
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/kbd-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" DISK="$(DISK_IMG)" DISK_IF=ide \
		QEMU_FLAGS="-machine pc -accel kvm -accel tcg" \
		INPUT='run("hello.lua")' MARKER=HELLO_FROM_EXT2 tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" \
		INPUT='run("hello.elf")' MARKER=LAB_OK tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/net-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" NIC=1 \
		INPUT='net.ping("10.0.2.2"); local d,c=net.rxirqs(); assert(d and c>0); print("NICIRQ_OK")' \
		MARKER=NICIRQ_OK tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/udp-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/tcp-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/prt-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/hda-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/perf-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" \
		INPUT='assert(k.time()>0 and k.date().year>=2026); assert(type((input.mouse()))=="number"); fb.text(0,0,"x"); k.sleep(1); print("API_OK")' \
		MARKER=API_OK tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" \
		INPUT='run("chello.elf")' MARKER=HOSTED_OK tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" DISK="$(DISK_IMG)" \
		INPUT='run("filetest.elf")' MARKER=FILEIO_OK tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" \
		INPUT='run("parallel.lua")' MARKER=PARALLEL_OK tests/boot-smoke.sh

# --- Formatting / linting ---------------------------------------------------

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

lint:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# --- Housekeeping -----------------------------------------------------------

# Note: build/scripts/logo.qoi is a checked-in asset, not a build artifact, so
# clean leaves it in place (regenerate it with `make logo`).
clean:
	rm -rf $(OBJ_DIR) $(KERNEL) boot.img disk.img .ovmf.fd $(PNG2QOI) \
		$(LAB_DIR)/*.o $(LAB_DIR)/*.elf $(HOSTED_LIB) $(HOSTED_ELVES)

help:
	@echo "Targets: all kernel.bin run test format lint clean"
	@echo "  make            build the kernel and bootable image (boot.img)"
	@echo "  make kernel.bin build just the kernel"
	@echo "  make run        boot the image in QEMU under OVMF"
	@echo "  make test       headless boot-smoke test"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make lint       check formatting (CI); does not modify files"
	@echo "  make clean      remove build artifacts"

-include $(DEPS)
# Hosted-side dep files (newlib, libgloss stubs, hosted programs, Doom): these
# compiles now emit -MMD too, so header edits (juampi.h, juampi_abi.h, the
# doomgeneric headers) rebuild their dependents.
-include $(NEWLIB_OBJS:.o=.d) $(DOOM_OBJS:.o=.d) \
	$(wildcard $(OBJ_DIR)/hosted/*.d)
