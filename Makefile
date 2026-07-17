# ---------------------------------------------------------------------------
# juampi-os top-level build
#
# Common targets:
#   make            build the kernel, userland and bootable floppy image
#   make kernel.bin build just the kernel binary (no disk image, no sudo)
#   make image      build the Minix hard-disk image (no sudo needed)
#   make run        build everything and boot it in QEMU
#   make format     reformat all C sources/headers in place with clang-format
#   make lint       check formatting without modifying files (used by CI)
#   make clean      remove all build artifacts
#   make help       list the available targets
# ---------------------------------------------------------------------------

# Toolchain. By default this builds with the host GCC in 32-bit mode (-m32),
# which works on Linux with gcc-multilib. Set CROSS to a cross-compiler prefix
# to build with a freestanding i686 cross toolchain instead — required on macOS
# (no gcc-multilib / ELF ld) and recommended everywhere for reproducibility:
#
#   make CROSS=i686-elf- run
#
# The devenv shell provides i686-elf-gcc / i686-elf-ld. CC/LD use `=` (not `?=`)
# so they override Make's built-in `cc`/`ld` defaults; command-line assignments
# still win.
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
CFLAGS := -O2 -std=c99 -Werror -Wall -Wextra \
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

# Sources and (out-of-tree) objects. The sources live in a flat src/ dir.
#
# x86-64 port in progress (see docs/x86-64-port.md): the tree is migrated from
# 32-bit one milestone at a time. Only the sources already ported to long mode
# are compiled — this list grows with each milestone so every commit builds and
# boots. The remaining 32-bit sources stay in src/ untouched until their turn.
# Limine enters the kernel directly at the C entry point kmain in 64-bit long
# mode, so there is no hand-written boot trampoline; the asm list stays empty
# until the interrupt/task-switch stubs are ported (milestones 2-3).
PORT64_CSOURCES := \
	$(SRC_DIR)/kernel.c \
	$(SRC_DIR)/serial.c \
	$(SRC_DIR)/ports.c \
	$(SRC_DIR)/utils.c \
	$(SRC_DIR)/bitset.c \
	$(SRC_DIR)/frames.c \
	$(SRC_DIR)/paging.c \
	$(SRC_DIR)/memory.c
PORT64_ASMSOURCES :=

CSOURCES   := $(PORT64_CSOURCES)
ASMSOURCES := $(PORT64_ASMSOURCES)
COBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CSOURCES))
ASMOBJS    := $(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(ASMSOURCES))
OBJS       := $(COBJS) $(ASMOBJS)
DEPS       := $(COBJS:.o=.d)

KERNEL := kernel.bin

# Every C source/header that clang-format should manage (kernel + userland).
# include/limine.h is vendored from the Limine project and kept verbatim, so it
# is excluded from our formatting rules.
FORMAT_FILES := $(filter-out $(INCLUDE_DIR)/limine.h,$(wildcard \
	$(SRC_DIR)/*.c $(INCLUDE_DIR)/*.h \
	$(BUILD_DIR)/tasks/*.c $(BUILD_DIR)/tasks/*.h \
	$(BUILD_DIR)/tasks/parser/*.c $(BUILD_DIR)/tasks/parser/*.h \
	$(BUILD_DIR)/bootstrap/*.c $(BUILD_DIR)/bootstrap/*.h))

# QEMU drives both `make run` and the tests (tests/run-qemu.sh). make run opens
# a GTK window; override the backend if you prefer, e.g.
# `make run QEMU_DISPLAY=curses` (text mode, in-terminal) or QEMU_DISPLAY=sdl.
QEMU         ?= qemu-system-x86_64
QEMU_DISPLAY ?= gtk
export QEMU

.PHONY: all init image run test clean format lint help boot

# During the x86-64 port the userland/disk pipeline (init, image, floppy) is not
# yet wired up; `all` builds the kernel and its bootable Limine image. The old
# targets remain defined below and return as their subsystems are ported.
all: $(KERNEL) boot.img

# --- Kernel -----------------------------------------------------------------

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.asm | $(OBJ_DIR)
	$(ASM) $(NASMFLAGS) -o $@ $<

$(KERNEL): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# --- Userland ---------------------------------------------------------------

# The bootstrap "init" process, loaded as a GRUB module.
init:
	$(MAKE) -C $(BUILD_DIR)/bootstrap CROSS=$(CROSS)

# --- Disk / boot images -----------------------------------------------------

# Builds the userland tasks and packs them into an ext2 disk image. This runs
# entirely in userspace (mke2fs + e2tools + debugfs) — no sudo, no loopback
# mount, and works on macOS too.
image:
	$(MAKE) -C $(BUILD_DIR)/tasks CROSS=$(CROSS) all
	cd $(BUILD_DIR) && ./build_image.sh

# Assemble the bootable GRUB floppy from the raw template image.
floppy.img: $(KERNEL) init
	cp floppy_raw.img $@
	e2cp $(KERNEL) $@:/
	e2cp $(BUILD_DIR)/menu.lst $@:/boot/grub/menu.lst
	e2cp $(BUILD_DIR)/bootstrap/init $@:/init

# --- Limine boot image (x86-64) ---------------------------------------------

# OVMF UEFI firmware, resolved from nixpkgs at recipe time (only `run`/`test`
# need it, so we don't pay for it on every make invocation). Override to point
# at a local firmware file if you are not using Nix.
OVMF_FD ?= $$(nix build --no-link --print-out-paths nixpkgs\#OVMF.fd)/FV/OVMF.fd

# Pack the kernel into a bootable UEFI image with Limine (sudo-free, mtools).
boot.img: $(KERNEL) $(BUILD_DIR)/limine.conf $(BUILD_DIR)/mkboot.sh
	bash $(BUILD_DIR)/mkboot.sh $(KERNEL) $@

# Boot the OS in QEMU under OVMF. Limine loads the kernel straight into 64-bit
# long mode. OVMF vars must be writable, so we boot from a private copy.
run: boot.img
	cp "$(OVMF_FD)" .ovmf.fd && chmod +w .ovmf.fd
	$(QEMU) -bios .ovmf.fd -drive file=boot.img,format=raw -m 512 \
		-display $(QEMU_DISPLAY) -serial stdio -no-reboot

# --- Integration tests (QEMU) -----------------------------------------------

# A separate kernel built with -DKTEST that runs in-kernel self-tests at boot
# and exits QEMU with a pass/fail code. Built into obj-test/ so it never
# clobbers the normal objects.
TEST_OBJ_DIR := obj-test
TEST_CFLAGS  := $(CFLAGS) -DKTEST
TEST_KERNEL  := kernel-test.bin

TEST_COBJS   := $(patsubst $(SRC_DIR)/%.c,$(TEST_OBJ_DIR)/%.o,$(CSOURCES)) \
	$(TEST_OBJ_DIR)/ktest.o
TEST_ASMOBJS := $(patsubst $(SRC_DIR)/%.asm,$(TEST_OBJ_DIR)/%.o,$(ASMSOURCES))
TEST_OBJS    := $(TEST_COBJS) $(TEST_ASMOBJS)
TEST_DEPS    := $(TEST_COBJS:.o=.d)

$(TEST_OBJ_DIR):
	mkdir -p $(TEST_OBJ_DIR)

$(TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/%.o: tests/%.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/%.o: $(SRC_DIR)/%.asm | $(TEST_OBJ_DIR)
	$(ASM) $(NASMFLAGS) -o $@ $<

$(TEST_KERNEL): $(TEST_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Boot the Limine image headless and assert the kernel reached 64-bit long mode
# and answered the boot protocol. The in-kernel ktest harness (TEST_KERNEL,
# machinery kept below) returns once the memory subsystems are ported.
test: boot.img
	OVMF_FD="$(OVMF_FD)" QEMU="$(QEMU)" tests/boot-smoke.sh

# --- Formatting / linting ---------------------------------------------------

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

lint:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# --- Housekeeping -----------------------------------------------------------

clean:
	$(MAKE) -C $(BUILD_DIR)/bootstrap clean
	$(MAKE) -C $(BUILD_DIR)/tasks clean
	rm -rf $(OBJ_DIR) $(TEST_OBJ_DIR) $(KERNEL) $(TEST_KERNEL) hdd.img floppy.img
	rm -f $(BUILD_DIR)/hdd.img

help:
	@echo "Targets: all init image floppy.img run format lint clean"
	@echo "  make            build kernel, userland and floppy.img"
	@echo "  make kernel.bin build just the kernel (no sudo)"
	@echo "  make image      build the Minix disk image (no sudo)"
	@echo "  make run        build everything and boot in QEMU"
	@echo "  make test       run the in-kernel test suite under QEMU"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make lint       check formatting (CI); does not modify files"
	@echo "  make clean      remove build artifacts"

-include $(DEPS)
-include $(TEST_DEPS)
