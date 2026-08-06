#include <ttf.h>
#include <alloc.h> // new()
#include <utils.h> // memcpy

// The stb_truetype API (declarations only — the implementation is compiled
// separately in src/stb/stb_truetype_impl.c with -w). The quoted path resolves
// relative to this file's directory.
#include "stb/stb_truetype.h"

struct ttf_font {
    struct heap_allocator* heap;
    stbtt_fontinfo info;
    uint8_t* data; // owned copy of the TTF bytes; info points into it
};

struct ttf_font* ttf_load(const void* ttf, size_t len,
                          struct heap_allocator* heap)
{
    if (ttf == NULL || len == 0) {
        return NULL;
    }
    struct ttf_font* f = new (&heap->base, struct ttf_font, 1);
    f->heap = heap;
    f->data = new (&heap->base, uint8_t, (ptrdiff_t)len);
    memcpy(f->data, ttf, len);
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, f->data, off)) {
        heap_free(heap, f->data);
        heap_free(heap, f);
        return NULL;
    }
    // stb passes this through to every STBTT_malloc/free during rasterisation,
    // so glyph scratch is drawn from this font's injected heap, not a global.
    f->info.userdata = heap;
    return f;
}

void ttf_free(struct ttf_font* f)
{
    if (f == NULL) {
        return;
    }
    heap_free(f->heap, f->data);
    heap_free(f->heap, f);
}

int ttf_line_height(struct ttf_font* f, float px)
{
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    float scale = stbtt_ScaleForPixelHeight(&f->info, px);
    return (int)((float)(ascent - descent + gap) * scale + 0.5f);
}

int ttf_text_width(struct ttf_font* f, const char* s, float px)
{
    float scale = stbtt_ScaleForPixelHeight(&f->info, px);
    float x = 0.0f;
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; p++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&f->info, *p, &advance, &lsb);
        x += (float)advance * scale;
        if (p[1] != '\0') {
            x += (float)stbtt_GetCodepointKernAdvance(&f->info, p[0], p[1]) *
                 scale;
        }
    }
    return (int)(x + 0.5f);
}

int ttf_draw(struct ttf_font* f, struct gfx_surface* dst, int x, int y,
             const char* s, float px, uint32_t rgb)
{
    float scale = stbtt_ScaleForPixelHeight(&f->info, px);
    float pen = (float)x; // pen x on the baseline
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; p++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&f->info, *p, &advance, &lsb);
        int gw = 0, gh = 0, gox = 0, goy = 0;
        // Coverage bitmap for this glyph plus its offset from the pen origin
        // (goy is measured down from the baseline, so it is usually negative).
        unsigned char* bmp = stbtt_GetCodepointBitmap(&f->info, scale, scale,
                                                      *p, &gw, &gh, &gox, &goy);
        if (bmp != NULL) {
            int ox = (int)(pen + 0.5f) + gox;
            int oy = y + goy;
            for (int gy = 0; gy < gh; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    unsigned a = bmp[gy * gw + gx];
                    if (a != 0) {
                        // Opaque text colour (alpha 0 -> opaque in gfx_plot),
                        // the glyph's coverage as the blend weight.
                        gfx_plot(dst, ox + gx, oy + gy, rgb, a, GFX_OVER);
                    }
                }
            }
            stbtt_FreeBitmap(bmp, NULL);
        }
        pen += (float)advance * scale;
        if (p[1] != '\0') {
            pen += (float)stbtt_GetCodepointKernAdvance(&f->info, p[0], p[1]) *
                   scale;
        }
    }
    return (int)(pen - (float)x + 0.5f);
}
