#include <gfx.h>

#include <stddef.h>

static volatile uint8_t* fb;
static uint64_t pitch, width, height;
static uint8_t r_shift, g_shift, b_shift;

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
    volatile uint32_t* row = (volatile uint32_t*)(fb + (uint64_t)y * pitch);
    row[x] = pack(rgb);
}

void gfx_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t rgb)
{
    if (fb == NULL) {
        return;
    }
    uint32_t px = pack(rgb);
    for (int64_t yy = y; yy < y + h; yy++) {
        if (yy < 0 || (uint64_t)yy >= height) {
            continue;
        }
        volatile uint32_t* row =
                (volatile uint32_t*)(fb + (uint64_t)yy * pitch);
        for (int64_t xx = x; xx < x + w; xx++) {
            if (xx >= 0 && (uint64_t)xx < width) {
                row[xx] = px;
            }
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
        volatile uint32_t* row =
                (volatile uint32_t*)(fb + (uint64_t)py * pitch);
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
