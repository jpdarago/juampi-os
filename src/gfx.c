#include <gfx.h>
#include <memory.h>
#include <utils.h>
#include <pci.h>
#include <paging.h>
#include <ports.h>
#include <console.h>

#include <stddef.h>

// The framebuffer is written with plain (non-volatile) stores: there are no
// readers and no ordering requirements, so dropping volatile lets the compiler
// vectorize the full-screen fills and the back-buffer flip (a volatile
// per-pixel loop can't be widened, which made double buffering slow).
static uint8_t* fb;
static uint64_t pitch, width, height;
static uint8_t r_shift, g_shift, b_shift;

// Optional off-screen back buffer for double buffering. When non-NULL, every
// draw goes here (a tightly packed width*height array of native-layout pixels)
// instead of the live framebuffer, and gfx_flip() copies it to the screen in
// one pass — so an animation never shows a half-drawn frame. NULL means draws
// go straight to the framebuffer, as before.
static uint32_t* back;

// Start of scanline y in the current draw target: the back buffer if one is
// active, otherwise the hardware framebuffer. Pixels within a row are
// contiguous in both, so callers index [x] off the returned pointer.
static inline uint32_t* row_of(uint64_t y)
{
    if (back != NULL) {
        return back + y * width;
    }
    return (uint32_t*)(fb + y * pitch);
}

void gfx_init(struct limine_framebuffer* f)
{
    if (f == NULL || f->bpp != 32 ||
        f->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        return;
    }
    fb = f->address;
    pitch = f->pitch;
    width = f->width;
    height = f->height;
    r_shift = f->red_mask_shift;
    g_shift = f->green_mask_shift;
    b_shift = f->blue_mask_shift;
}

bool gfx_available(void)
{
    return fb != NULL;
}
uint64_t gfx_width(void)
{
    return width;
}
uint64_t gfx_height(void)
{
    return height;
}
uint64_t gfx_pitch(void)
{
    return pitch;
}
void gfx_shifts(uint8_t* r, uint8_t* g, uint8_t* b)
{
    *r = r_shift;
    *g = g_shift;
    *b = b_shift;
}
void* gfx_framebuffer(uint64_t* size, uint64_t* out_pitch)
{
    if (size != NULL) {
        *size = height * pitch;
    }
    if (out_pitch != NULL) {
        *out_pitch = pitch;
    }
    return fb;
}

// Pack an 0xRRGGBB colour into the framebuffer's channel layout.
static uint32_t pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    return (r << r_shift) | (g << g_shift) | (b << b_shift);
}

void gfx_pixel(int64_t x, int64_t y, uint32_t rgb)
{
    if (fb == NULL || x < 0 || y < 0 || (uint64_t)x >= width ||
        (uint64_t)y >= height) {
        return;
    }
    row_of((uint64_t)y)[x] = pack(rgb);
}

void gfx_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t rgb)
{
    if (fb == NULL) {
        return;
    }
    uint32_t px = pack(rgb);
    // Clip the rectangle to the screen once, so the inner loop is a tight,
    // branch-free fill the compiler can vectorize (this is the per-frame
    // clear).
    int64_t x0 = x < 0 ? 0 : x;
    int64_t y0 = y < 0 ? 0 : y;
    int64_t x1 = x + w > (int64_t)width ? (int64_t)width : x + w;
    int64_t y1 = y + h > (int64_t)height ? (int64_t)height : y + h;
    for (int64_t yy = y0; yy < y1; yy++) {
        uint32_t* row = row_of((uint64_t)yy);
        for (int64_t xx = x0; xx < x1; xx++) {
            row[xx] = px;
        }
    }
}

void gfx_clear(uint32_t rgb)
{
    gfx_rect(0, 0, (int64_t)width, (int64_t)height, rgb);
}

void gfx_blit(int64_t x, int64_t y, uint64_t w, uint64_t h,
              const uint32_t* pixels)
{
    if (fb == NULL) {
        return;
    }
    for (uint64_t j = 0; j < h; j++) {
        int64_t py = y + (int64_t)j;
        if (py < 0 || (uint64_t)py >= height) {
            continue;
        }
        uint32_t* row = row_of((uint64_t)py);
        const uint32_t* src = pixels + j * w;
        for (uint64_t i = 0; i < w; i++) {
            int64_t px = x + (int64_t)i;
            if (px < 0 || (uint64_t)px >= width) {
                continue;
            }
            uint32_t p = src[i];
            if ((p >> 24) == 0) {
                continue; // fully transparent
            }
            row[px] = pack(p & 0xffffff);
        }
    }
}

void gfx_line(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t rgb)
{
    // Bresenham's line algorithm.
    int64_t dx = x1 - x0, dy = y1 - y0;
    int64_t adx = dx < 0 ? -dx : dx;
    int64_t ady = dy < 0 ? -dy : dy;
    int64_t sx = dx < 0 ? -1 : 1;
    int64_t sy = dy < 0 ? -1 : 1;
    int64_t err = adx - ady;
    for (;;) {
        gfx_pixel(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int64_t e2 = 2 * err;
        if (e2 > -ady) {
            err -= ady;
            x0 += sx;
        }
        if (e2 < adx) {
            err += adx;
            y0 += sy;
        }
    }
}

bool gfx_buffered(void)
{
    return back != NULL;
}

bool gfx_buffer(bool on)
{
    if (fb == NULL) {
        return false;
    }
    if (on && back == NULL) {
        back = new (&heap_default()->base, uint32_t,
                    (ptrdiff_t)(width * height));
        // Seed the back buffer with what's on screen, so enabling buffering is
        // transparent: pixels never redrawn keep their current value until the
        // first flip.
        for (uint64_t y = 0; y < height; y++) {
            memcpy(back + y * width, fb + y * pitch, width * 4);
        }
    } else if (!on && back != NULL) {
        heap_free(heap_default(), back);
        back = NULL;
    }
    return back != NULL;
}

void gfx_flip(void)
{
    if (fb == NULL || back == NULL) {
        return;
    }
    if (pitch == width * 4) {
        // Rows are contiguous in the framebuffer: copy the whole buffer at
        // once.
        memcpy(fb, back, width * height * 4);
    } else {
        for (uint64_t y = 0; y < height; y++) {
            memcpy(fb + y * pitch, back + y * width, width * 4);
        }
    }
}

// --- Runtime mode setting (Bochs DISPI / QEMU stdvga) -----------------------
// The QEMU display (PCI 1234:1111) is Bochs-VBE compatible: its resolution can
// be reprogrammed at runtime through the DISPI index/data I/O ports. We map the
// whole LFB aperture (from the device's BAR0) once, then each mode change just
// reprograms DISPI and re-points the console/graphics at the new geometry.

#define DISPI_INDEX 0x01CE
#define DISPI_DATA 0x01CF
#define DISPI_ID 0
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP 3
#define DISPI_ENABLE 4
#define DISPI_VIRT_WIDTH 6
#define DISPI_ENABLED 0x01
#define DISPI_LFB_ENABLED 0x40

// A dedicated VA window for the linear framebuffer aperture (16 MiB, enough for
// modes up to ~2048x2048x32). Clear of KHEAP (0xffffc…) and the other fixed
// VAs.
#define FBWIN_VA 0xffffe00000000000ull
#define FBWIN_SZ 0x1000000ull

static bool win_mapped;

static void dispi_write(uint16_t idx, uint16_t val)
{
    outw(DISPI_INDEX, idx);
    outw(DISPI_DATA, val);
}
static uint16_t dispi_read(uint16_t idx)
{
    outw(DISPI_INDEX, idx);
    return inw(DISPI_DATA);
}

// Physical base of the display controller's linear framebuffer (its BAR0).
static uintptr_t vga_lfb_phys(void)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            if ((pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0) & 0xFFFF) ==
                0xFFFF) {
                continue;
            }
            uint32_t cls = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x08);
            if (((cls >> 24) & 0xFF) == 0x03) { // display controller
                uint32_t bar0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x10);
                return (uintptr_t)(bar0 & 0xFFFFFFF0u);
            }
        }
    }
    return 0;
}

bool gfx_set_mode(uint32_t w, uint32_t h)
{
    if (fb == NULL) {
        return false; // headless
    }
    // Bochs DISPI present? (ID register reads back 0xB0Cx.)
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0C5) {
        return false;
    }
    if (w < 64 || h < 64 || (uint64_t)w * h * 4 > FBWIN_SZ) {
        return false;
    }

    // Map the LFB aperture once, then reuse it for every mode.
    if (!win_mapped) {
        uintptr_t phys = vga_lfb_phys();
        if (phys == 0) {
            return false;
        }
        for (uint64_t off = 0; off < FBWIN_SZ; off += PAGE_SZ) {
            map_page(kernel_dir, FBWIN_VA + off, phys + off,
                     PAGEF_P | PAGEF_RW);
        }
        win_mapped = true;
    }

    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)w);
    dispi_write(DISPI_YRES, (uint16_t)h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, (uint16_t)w);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

    // Adopt the new geometry (DISPI 32bpp is xRGB: blue 0, green 8, red 16).
    if (back != NULL) {
        heap_free(heap_default(), back);
        back = NULL;
    }
    fb = (uint8_t*)FBWIN_VA;
    width = w;
    height = h;
    pitch = (uint64_t)w * 4;
    r_shift = 16;
    g_shift = 8;
    b_shift = 0;
    console_reinit(fb, width, height, pitch);
    return true;
}
