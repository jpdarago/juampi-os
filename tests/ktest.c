// In-kernel integration tests, run under QEMU (see tests/run-qemu.sh).
//
// These are compiled into a dedicated `kernel-test.bin` (built with -DKTEST).
// kmain() calls ktest_main() once the memory subsystems are initialized, but
// before disk/FS/scheduler init — so the tests exercise the real MMU and
// allocators without needing a disk image or GRUB modules.
//
// Output goes to the debug console (port 0xE9, captured by `-debugcon stdio`).
// The result is reported to QEMU's isa-debug-exit device (port 0xf4): the exit
// code is (value << 1) | 1, so 0x10 -> 33 (pass) and 0x11 -> 35 (fail).

#include <types.h>
#include <ports.h>
#include <bochs_debug.h>
#include <memory.h>
#include <frames.h>
#include <paging.h>
#include <serial.h>

#define QEMU_EXIT_PORT 0xf4
#define KTEST_PASS     0x10 // -> QEMU exit code 33
#define KTEST_FAIL     0x11 // -> QEMU exit code 35

static uint failures;

// Report a single check to the debug console and tally failures.
static void kt_check(bool ok, const char* name)
{
    dbg_printf(ok ? "  ok   - %s" : "  FAIL - %s", name);
    if(!ok) {
        failures++;
    }
}

static void qemu_exit(uchar code)
{
    outb(QEMU_EXIT_PORT, code);
    // If the isa-debug-exit device is not present, don't fall through.
    while(1) {
        ;
    }
}

// --- Kernel heap allocator ---------------------------------------------------

static void test_kmalloc(void)
{
    int* a = kmalloc(sizeof(int) * 4);
    kt_check(a != NULL, "kmalloc returns non-null");
    if(a) {
        a[0] = 0x1234;
        a[3] = 0x5678;
        kt_check(a[0] == 0x1234 && a[3] == 0x5678,
                 "kmalloc memory is readable/writable");
    }

    void* p1 = kmalloc(64);
    void* p2 = kmalloc(64);
    kt_check(p1 && p2 && p1 != p2, "kmalloc returns distinct blocks");

    void* aligned = kmem_alloc_aligned(get_kernel_heap(), PAGE_SZ);
    kt_check(aligned != NULL && ((uint) aligned & (PAGE_SZ - 1)) == 0,
             "kmem_alloc_aligned returns a page-aligned block");

    // Large request forces the heap to grow (kmem_append_core path).
    void* big = kmalloc(64 * 1024);
    kt_check(big != NULL, "large kmalloc succeeds (grows the heap)");

    kfree(a);
    kfree(p1);
    kfree(p2);
    kfree(aligned);
    kfree(big);
}

// --- Physical frame allocator ------------------------------------------------

static void test_frames(void)
{
    uint before = frames_available();
    uint f1 = frame_alloc();
    uint f2 = frame_alloc();
    kt_check(f1 && f2 && f1 != f2,
             "frame_alloc returns distinct, non-zero frames");
    kt_check(frames_available() == before - 2,
             "frames_available drops by 2 after two allocs");
    frame_free(f1);
    frame_free(f2);
    kt_check(frames_available() == before,
             "frames_available is restored after freeing");
}

// --- Paging (real MMU) -------------------------------------------------------

static void test_paging(void)
{
    // 1 MB is where the kernel is loaded, so it must be identity-mapped.
    kt_check(physical_address(current_directory, 0x100000) == 0x100000,
             "low memory is identity-mapped");

    // Map a fresh frame at a virtual address well above physical RAM and
    // below the user/kernel stacks, then read/write through it. No cleanup:
    // ktest_main exits the VM immediately after the checks.
    uint frame = frame_alloc();
    uint va = 0x40000000;
    map_page(current_directory, va, frame, PAGEF_P | PAGEF_RW);

    volatile uint32* p = (volatile uint32*) va;
    *p = 0xCAFEBABE;
    kt_check(*p == 0xCAFEBABE, "write/read through a freshly mapped page");
    kt_check(physical_address(current_directory, va) == frame,
             "physical_address reflects the new mapping");
}

// --- Serial (COM1 UART) ------------------------------------------------------

static void test_serial(void)
{
    // The UART was initialized in kmain. A robust, emulation-independent check:
    // the transmit-holding register reports empty, and serial_putc returns
    // (its wait is bounded, so a wedged UART can never hang the suite).
    kt_check((inb(SERIAL_COM1 + SERIAL_LSR) & SERIAL_LSR_THR_EMPTY) != 0,
             "serial UART reports transmit-ready after init");
    serial_print("[ktest] hello from the serial console\n");
    kt_check(true, "serial_putc/serial_print return without hanging");
}

void ktest_main(void)
{
    dbg_print("");
    dbg_print("=== juampi-os kernel integration tests ===");

    test_kmalloc();
    test_frames();
    test_paging();
    test_serial();

    if(failures == 0) {
        dbg_print("RESULT: all tests passed");
        qemu_exit(KTEST_PASS);
    } else {
        dbg_printf("RESULT: %d test(s) failed", failures);
        qemu_exit(KTEST_FAIL);
    }
}
