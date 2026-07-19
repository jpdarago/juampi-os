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
	$(SRC_DIR)/*.c $(INCLUDE_DIR)/*.h $(BUILD_DIR)/user/*.c))

# QEMU drives both `make run` and `make test`. make run opens a GTK window;
# override the backend if you prefer, e.g. `make run QEMU_DISPLAY=curses`.
QEMU         ?= qemu-system-x86_64
QEMU_DISPLAY ?= gtk
export QEMU

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

# Pack the kernel and the Lua scripts into a bootable UEFI image with Limine
# (sudo-free, mtools).
boot.img: $(KERNEL) $(SCRIPTS) $(BUILD_DIR)/limine.conf $(BUILD_DIR)/mkboot.sh
	bash $(BUILD_DIR)/mkboot.sh $(KERNEL) $@ $(SCRIPTS)

# Boot the OS in QEMU under OVMF. Limine loads the kernel straight into 64-bit
# long mode. OVMF vars must be writable, so we boot from a private copy.
run: boot.img
	cp "$(OVMF_FD)" .ovmf.fd && chmod +w .ovmf.fd
	$(QEMU) -bios .ovmf.fd -drive file=boot.img,format=raw -m 512 \
		-display $(QEMU_DISPLAY) -serial stdio -no-reboot

# Boot the Limine image headless and drive the shell over both input paths:
# serial (boot-smoke) and the PS/2 keyboard via QMP send-key (kbd-smoke).
test: boot.img
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/boot-smoke.sh
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/kbd-smoke.sh

# --- Formatting / linting ---------------------------------------------------

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

lint:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# --- Housekeeping -----------------------------------------------------------

clean:
	rm -rf $(OBJ_DIR) $(KERNEL) boot.img .ovmf.fd

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
