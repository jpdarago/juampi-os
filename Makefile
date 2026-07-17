# ---------------------------------------------------------------------------
# juampi-os top-level build
#
# Common targets:
#   make            build the kernel, userland and bootable floppy image
#   make kernel.bin build just the kernel binary (no disk image, no sudo)
#   make image      build the Minix hard-disk image (needs sudo, see below)
#   make run        build everything and boot it in Bochs
#   make format     reformat all C sources/headers in place with clang-format
#   make lint       check formatting without modifying files (used by CI)
#   make clean      remove all build artifacts
#   make help       list the available targets
# ---------------------------------------------------------------------------

# Toolchain. Uses `=` rather than `?=` on purpose: CC/LD are built-in Make
# variables (defaulting to `cc`/`ld`), and `?=` would NOT override a built-in
# default. A plain `=` is still overridable on the command line, e.g.
# `make CC=clang`, since command-line assignments win over the makefile.
CC           = gcc
ASM          = nasm
LD           = ld
CLANG_FORMAT = clang-format

# Directories.
SRC_DIR     := src
INCLUDE_DIR := include
OBJ_DIR     := obj
BUILD_DIR   := build

# Compiler / assembler / linker flags.
CFLAGS := -m32 -O2 -std=c99 -Werror -Wall -Wextra \
	-Wno-unused-parameter -Wno-override-init \
	-Wno-address-of-packed-member \
	-Wunreachable-code -Wshadow -Wcast-qual \
	-Wformat=2 -Wwrite-strings -Wstrict-prototypes \
	-Wredundant-decls -Wnested-externs -Wmissing-include-dirs \
	-Wjump-misses-init -Wlogical-op \
	-nostdlib -fno-builtin -nostartfiles \
	-nodefaultlibs -fno-stack-protector -I$(INCLUDE_DIR)

# Generate per-object .d dependency files so header edits trigger rebuilds.
CPPFLAGS  := -MMD -MP
NASMFLAGS := -i$(INCLUDE_DIR)/ -felf32
LINKSCRIPT := $(BUILD_DIR)/linker.ld
LDFLAGS   := -melf_i386 -T $(LINKSCRIPT)

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
	$(BUILD_DIR)/tasks/*.c $(BUILD_DIR)/tasks/*.h \
	$(BUILD_DIR)/tasks/parser/*.c $(BUILD_DIR)/tasks/parser/*.h \
	$(BUILD_DIR)/bootstrap/*.c $(BUILD_DIR)/bootstrap/*.h)

# Bochs configuration (override on the command line if needed).
BOCHSDIR  ?= ./bochs/bin
BOCHSCONF ?= run/bochsrc.txt

.PHONY: all init image run clean format lint help

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
	$(MAKE) -C $(BUILD_DIR)/bootstrap

# --- Disk / boot images -----------------------------------------------------

# Builds the userland tasks and packs them into a Minix disk image.
# WARNING: build_image.sh mounts a loopback device and needs sudo. Read it
# before running this target. It attaches the image to the first free loop
# device and mounts it on a temporary directory, cleaning up on exit.
image:
	$(MAKE) -C $(BUILD_DIR)/tasks
	cd $(BUILD_DIR) && ./build_image.sh

# Assemble the bootable GRUB floppy from the raw template image.
floppy.img: $(KERNEL) init
	cp floppy_raw.img $@
	e2cp $(KERNEL) $@:/
	e2cp $(BUILD_DIR)/menu.lst $@:/boot/grub/menu.lst
	e2cp $(BUILD_DIR)/bootstrap/init $@:/init

run: all
	$(BOCHSDIR)/bochs -q -f $(BOCHSCONF)

# --- Formatting / linting ---------------------------------------------------

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

lint:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# --- Housekeeping -----------------------------------------------------------

clean:
	$(MAKE) -C $(BUILD_DIR)/bootstrap clean
	$(MAKE) -C $(BUILD_DIR)/tasks clean
	rm -rf $(OBJ_DIR) $(KERNEL) hdd.img floppy.img

help:
	@echo "Targets: all init image floppy.img run format lint clean"
	@echo "  make            build kernel, userland and floppy.img"
	@echo "  make kernel.bin build just the kernel (no sudo)"
	@echo "  make image      build the Minix disk image (needs sudo)"
	@echo "  make run        build everything and boot in Bochs"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make lint       check formatting (CI); does not modify files"
	@echo "  make clean      remove build artifacts"

-include $(DEPS)
