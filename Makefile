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
	-mno-mmx -mno-sse -mno-sse2 -mno-3dnow
# The kernel never enables SSE (CR4.OSFXSR) nor saves XMM state on a context
# switch, so GCC must not auto-vectorize with SIMD — those instructions would
# #UD (or corrupt state). This is the standard freestanding-kernel flag set.

# A host compiler needs -m32 to emit 32-bit code; a cross i686 compiler is
# already 32-bit (and may reject -m32), so only add it for the host build.
ifeq ($(strip $(CROSS)),)
CFLAGS += -m32
endif

# Generate per-object .d dependency files so header edits trigger rebuilds.
CPPFLAGS  := -MMD -MP
NASMFLAGS := -i$(INCLUDE_DIR)/ -felf32
LINKSCRIPT := $(BUILD_DIR)/linker.ld
# -z noexecstack silences the "missing .note.GNU-stack" warning from the NASM
# objects; an executable-stack note is meaningless for a freestanding kernel.
LDFLAGS   := -melf_i386 -z noexecstack -T $(LINKSCRIPT)

# Sources and (out-of-tree) objects. The sources live in a flat src/ dir.
CSOURCES   := $(wildcard $(SRC_DIR)/*.c)
ASMSOURCES := $(wildcard $(SRC_DIR)/*.asm)
COBJS      := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CSOURCES))
ASMOBJS    := $(patsubst $(SRC_DIR)/%.asm,$(OBJ_DIR)/%.o,$(ASMSOURCES))
OBJS       := $(COBJS) $(ASMOBJS)
DEPS       := $(COBJS:.o=.d)

KERNEL := kernel.bin

# Every C source/header that clang-format should manage (kernel + userland).
FORMAT_FILES := $(wildcard \
	$(SRC_DIR)/*.c $(INCLUDE_DIR)/*.h \
	$(BUILD_DIR)/mkminixfs.c \
	$(BUILD_DIR)/tasks/*.c $(BUILD_DIR)/tasks/*.h \
	$(BUILD_DIR)/tasks/parser/*.c $(BUILD_DIR)/tasks/parser/*.h \
	$(BUILD_DIR)/bootstrap/*.c $(BUILD_DIR)/bootstrap/*.h)

# QEMU drives both `make run` and the tests (tests/run-qemu.sh). make run opens
# a GTK window; override the backend if you prefer, e.g.
# `make run QEMU_DISPLAY=curses` (text mode, in-terminal) or QEMU_DISPLAY=sdl.
QEMU         ?= qemu-system-i386
QEMU_DISPLAY ?= gtk
export QEMU

.PHONY: all init image run test clean format lint help

all: init image $(KERNEL) floppy.img

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

# Builds the userland tasks and packs them into a Minix disk image. This runs
# entirely in userspace (mkfs.minix + the mkminixfs populate tool) — no sudo,
# no loopback mount.
image:
	$(MAKE) -C $(BUILD_DIR)/tasks CROSS=$(CROSS)
	cd $(BUILD_DIR) && ./build_image.sh

# Assemble the bootable GRUB floppy from the raw template image.
floppy.img: $(KERNEL) init
	cp floppy_raw.img $@
	e2cp $(KERNEL) $@:/
	e2cp $(BUILD_DIR)/menu.lst $@:/boot/grub/menu.lst
	e2cp $(BUILD_DIR)/bootstrap/init $@:/init

# Boot the OS in QEMU. The kernel loads directly via multiboot (-kernel), with
# the userland "init" as a module (-initrd) and the Minix image as the disk.
# The disk format is stated explicitly; otherwise QEMU probes it as raw and
# restricts writes to block 0, which breaks the filesystem.
run: $(KERNEL) init image
	$(QEMU) -kernel $(KERNEL) -initrd $(BUILD_DIR)/bootstrap/init \
		-drive file=hdd.img,format=raw,if=ide -m 128 \
		-display $(QEMU_DISPLAY) -serial stdio

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

# Build the test kernel and run it under QEMU (headless). Override QEMU or
# TIMEOUT on the command line, e.g. `make test TIMEOUT=60`.
test: $(TEST_KERNEL)
	tests/run-qemu.sh $(TEST_KERNEL)

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
	rm -f $(BUILD_DIR)/mkminixfs $(BUILD_DIR)/hdd.img

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
