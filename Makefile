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
	-nostdlib -fno-builtin -nostartfiles \
	-nodefaultlibs -fno-stack-protector -I$(INCLUDE_DIR) \
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

COBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CSOURCES))
ASMOBJS    := $(patsubst $(SRC_DIR)/%.S,$(OBJ_DIR)/%.o,$(ASMSOURCES))
VENDOR_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(VENDOR_CSOURCES))
OBJS       := $(COBJS) $(ASMOBJS) $(VENDOR_OBJS) $(LUA_OBJS) $(LUA_ASM_OBJ)
DEPS       := $(COBJS:.o=.d) $(VENDOR_OBJS:.o=.d) $(LUA_OBJS:.o=.d)

KERNEL := kernel.bin

# Every C source/header that clang-format should manage (kernel + userland).
# include/limine.h is vendored from the Limine project and kept verbatim, so it
# is excluded from our formatting rules.
FORMAT_FILES := $(filter-out $(INCLUDE_DIR)/limine.h,$(wildcard \
	$(SRC_DIR)/*.c $(INCLUDE_DIR)/*.h $(BUILD_DIR)/user/*.c \
	$(BUILD_DIR)/lab/*.c))

# QEMU drives both `make run` and `make test`. make run opens a GTK window;
# override the backend if you prefer, e.g. `make run QEMU_DISPLAY=curses`.
QEMU         ?= qemu-system-x86_64
QEMU_DISPLAY ?= gtk
QEMU_SMP     ?= 4   # number of cores QEMU exposes (drives the SMP bring-up)
export QEMU QEMU_SMP

.PHONY: all run test clean format lint help

all: $(KERNEL) boot.img

# --- Kernel -----------------------------------------------------------------

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# GNU-assembler sources (.S): assembled by gcc (runs cpp + as).
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) -c -o $@ $<

# Vendored third-party C (flanterm, printf): compiled with our kernel flags but
# without our warning gauntlet (kept verbatim). The %-stem includes the subdir.
$(OBJ_DIR)/flanterm/%.o: $(SRC_DIR)/flanterm/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

$(OBJ_DIR)/printf/%.o: $(SRC_DIR)/printf/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

# Embedded Lua: Lua include path (klibc stubs win for <string.h> etc.), no warns.
$(OBJ_DIR)/lua/%.o: $(SRC_DIR)/lua/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(LUA_INC) $(CPPFLAGS) -c -o $@ $<

$(OBJ_DIR)/lua/%.o: $(SRC_DIR)/lua/%.S | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -c -o $@ $<

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

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
LAB_DIR    := $(BUILD_DIR)/lab
LAB_SRCS   := $(wildcard $(LAB_DIR)/*.c)
LAB_ELVES  := $(patsubst $(LAB_DIR)/%.c,$(LAB_DIR)/%.elf,$(LAB_SRCS))
LAB_CFLAGS := -O2 -std=c11 -ffreestanding -nostdlib -fno-pic -fno-pie \
	-mno-red-zone -mno-mmx -mno-3dnow -fno-stack-protector \
	-Wall -Wextra -I$(INCLUDE_DIR)

$(LAB_DIR)/%.elf: $(LAB_DIR)/%.c $(INCLUDE_DIR)/lab.h
	$(CC) $(LAB_CFLAGS) -c -o $(@:.elf=.o) $<
	$(LD) -melf_x86_64 -e bench -Ttext 0x400000 -o $@ $(@:.elf=.o)

# Everything shipped to the image as a Limine module.
MODULES := $(SCRIPTS) $(LOGO) $(LAB_ELVES)

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
DISK_FILES  := $(shell find $(DISK_DIR) -type f 2>/dev/null)
DISK_IMG    := disk.img
DISK_BLOCKS ?= 8192   # 1 KiB blocks -> 8 MiB image

$(DISK_IMG): $(DISK_FILES)
	mke2fs -q -F -t ext2 -b 1024 -O ^resize_inode,^dir_index,^ext_attr \
		-d $(DISK_DIR) $@ $(DISK_BLOCKS)

# QEMU args attaching the data disk as the primary IDE slave (bus ide.0 unit 1);
# shared by `run` and the smoke tests.
DISK_QEMU := -drive file=$(DISK_IMG),format=raw,if=none,id=juampidisk \
	-device ide-hd,drive=juampidisk,bus=ide.0,unit=1

# Boot the OS in QEMU under OVMF. Limine loads the kernel straight into 64-bit
# long mode. OVMF vars must be writable, so we boot from a private copy.
run: boot.img $(DISK_IMG)
	cp "$(OVMF_FD)" .ovmf.fd && chmod +w .ovmf.fd
	$(QEMU) -bios .ovmf.fd -drive file=boot.img,format=raw -m 512 \
		-smp $(QEMU_SMP) -accel kvm -accel tcg \
		$(DISK_QEMU) \
		-display $(QEMU_DISPLAY) -serial stdio -no-reboot

# Boot the Limine image headless and drive the shell over both input paths:
# serial (boot-smoke) and the PS/2 keyboard via QMP send-key (kbd-smoke).
test: boot.img $(DISK_IMG)
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/kbd-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" DISK="$(DISK_IMG)" \
		INPUT='run("hello.lua")' MARKER=HELLO_FROM_EXT2 tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" \
		INPUT='lab.run("hello.elf")' MARKER=LAB_OK tests/boot-smoke.sh

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
		$(LAB_DIR)/*.o $(LAB_DIR)/*.elf

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
