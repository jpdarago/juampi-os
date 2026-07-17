/* Linker script for the Limine boot protocol (x86-64, higher-half).
 * Based on the upstream Limine C template. The kernel is linked into the top
 * 2 GiB of the canonical address space (-2 GiB, which -mcmodel=kernel targets)
 * and loaded there by the bootloader. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS
{
    limine_requests PT_LOAD;
    text            PT_LOAD;
    rodata          PT_LOAD;
    data            PT_LOAD;
}

SECTIONS
{
    /* -2 GiB: the higher-half window used by -mcmodel=kernel. */
    . = 0xffffffff80000000;

    /* Limine scans this section for the base-revision tag and requests; the
     * start/end markers must bracket them, so keep the ordering. */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
        __TEXT_END__ = .;
    } :text

    /* User-mode signal trampolines copied into user address spaces; they are
     * code, so they live in the executable segment. */
    .__signal_handlers ALIGN(CONSTANT(MAXPAGESIZE)) : {
        signal_handlers_start = .;
        *(.__signal_handlers)
        signal_handlers_end = .;
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
        __RODATA_END__ = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
        __DATA_END__ = .;
    } :data

    .bss : {
        sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        ebss = .;
    } :data

    kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
    }
}
