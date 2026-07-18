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
ASM           = nasm
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
	-mno-mmx -mno-sse -mno-sse2 -mno-3dnow \
	-mno-red-zone -mcmodel=kernel -fno-pic -fno-pie
# The kernel never enables SSE (CR4.OSFXSR) nor saves XMM state on a context
# switch, so GCC must not auto-vectorize with SIMD — those instructions would
# #UD (or corrupt state). -mno-red-zone is mandatory for kernel code: the SysV
# red zone is unsafe once interrupts reuse the stack. This is the standard
# freestanding 64-bit kernel flag set.

# Generate per-object .d dependency files so header edits trigger rebuilds.
CPPFLAGS  := -MMD -MP
NASMFLAGS := -i$(INCLUDE_DIR)/ -felf64
LINKSCRIPT := $(BUILD_DIR)/linker.ld
# -z noexecstack silences the "missing .note.GNU-stack" warning from the NASM
# objects; an executable-stack note is meaningless for a freestanding kernel.
LDFLAGS   := -melf_x86_64 -z noexecstack -z max-page-size=0x1000 -T $(LINKSCRIPT)

# Sources and (out-of-tree) objects. Kernel sources live in a flat src/ dir;
# src/flanterm/ is the vendored flanterm terminal emulator (kept verbatim).
CSOURCES   := $(wildcard $(SRC_DIR)/*.c)
ASMSOURCES := $(wildcard $(SRC_DIR)/*.asm)
FLANTERM_CSOURCES := $(SRC_DIR)/flanterm/flanterm.c \
	$(SRC_DIR)/flanterm/flanterm_backends/fb.c
COBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CSOURCES))
ASMOBJS    := $(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(ASMSOURCES))
FLANTERM_OBJS := $(patsubst $(SRC_DIR)/flanterm/%.c,$(OBJ_DIR)/flanterm/%.o,$(FLANTERM_CSOURCES))
OBJS       := $(COBJS) $(ASMOBJS) $(FLANTERM_OBJS)
DEPS       := $(COBJS:.o=.d) $(FLANTERM_OBJS:.o=.d)

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

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.asm | $(OBJ_DIR)
	$(ASM) $(NASMFLAGS) -o $@ $<

# Vendored flanterm: compiled with our kernel flags but without our warning
# gauntlet (it is third-party code kept verbatim).
$(OBJ_DIR)/flanterm/%.o: $(SRC_DIR)/flanterm/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w $(CPPFLAGS) -c -o $@ $<

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# --- Userland + boot image --------------------------------------------------

# OVMF UEFI firmware, resolved from nixpkgs at recipe time (only `run`/`test`
# need it, so we don't pay for it on every make invocation). Override to point
# at a local firmware file if you are not using Nix.
OVMF_FD ?= $$(nix build --no-link --print-out-paths nixpkgs\#OVMF.fd)/FV/OVMF.fd

# A minimal freestanding 64-bit userland program, loaded as a Limine module and
# run by the kernel. Statically linked, non-PIE, at a low user VA.
USER_ELF := $(BUILD_DIR)/user/hello.elf
$(USER_ELF): $(BUILD_DIR)/user/hello.c
	$(CC) -ffreestanding -nostdlib -static -fno-pic -no-pie -mno-red-zone \
		-fno-stack-protector -mno-sse -mno-mmx \
		-Wl,-Ttext=0x400000 -Wl,-e_start -o $@ $<

# Pack the kernel and the userland module into a bootable UEFI image with Limine
# (sudo-free, mtools).
boot.img: $(KERNEL) $(USER_ELF) $(BUILD_DIR)/limine.conf $(BUILD_DIR)/mkboot.sh
	bash $(BUILD_DIR)/mkboot.sh $(KERNEL) $@ $(USER_ELF)

# Boot the OS in QEMU under OVMF. Limine loads the kernel straight into 64-bit
# long mode. OVMF vars must be writable, so we boot from a private copy.
run: boot.img
	cp "$(OVMF_FD)" .ovmf.fd && chmod +w .ovmf.fd
	$(QEMU) -bios .ovmf.fd -drive file=boot.img,format=raw -m 512 \
		-display $(QEMU_DISPLAY) -serial stdio -no-reboot

# Boot the Limine image headless and assert the kernel completed its boot
# self-tests through to userland (greps the serial log for the marker).
test: boot.img
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/boot-smoke.sh

# --- Formatting / linting ---------------------------------------------------

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

lint:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# --- Housekeeping -----------------------------------------------------------

clean:
	rm -rf $(OBJ_DIR) $(KERNEL) boot.img .ovmf.fd $(USER_ELF)

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
