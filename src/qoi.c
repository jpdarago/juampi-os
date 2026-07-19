// QOI image decoder. The format is a 14-byte header ("qoif", width and height
// as big-endian u32, a channel count and a colorspace byte) followed by a
// stream of one-byte-tagged chunks that each describe one or more pixels
// relative to the previous one or to a 64-entry cache of recently seen pixels.
// See the annotated spec at https://qoiformat.org/qoi-specification.pdf.

#include <qoi.h>

// Chunk tags. The two-bit tags share their low bits with pixel data, so they
// are matched after masking off the high two bits (QOI_MASK_2); the two 8-bit
// tags (RGB/RGBA) are matched whole and must be tested first.
#define QOI_OP_INDEX 0x00 // 00xxxxxx: index into the running pixel cache
#define QOI_OP_DIFF 0x40  // 01xxxxxx: small per-channel diff (bias 2)
#define QOI_OP_LUMA 0x80  // 10xxxxxx: green diff + red/blue relative to it
#define QOI_OP_RUN 0xc0   // 11xxxxxx: run of the previous pixel (bias 1)
#define QOI_OP_RGB 0xfe   // full RGB, alpha unchanged
#define QOI_OP_RGBA 0xff  // full RGBA
#define QOI_MASK_2 0xc0

// A pixel's slot in the 64-entry running cache.
#define QOI_HASH(r, g, b, a) ((r) * 3 + (g) * 5 + (b) * 7 + (a) * 11)

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint32_t* qoi_decode(allocator* mem, const void* data, size_t size,
                     qoi_image* img)
{
    const uint8_t* p = data;
    // Header (14 bytes) + 8-byte end marker is the smallest possible file.
    if (size < 14 + 8 || p[0] != 'q' || p[1] != 'o' || p[2] != 'i' ||
        p[3] != 'f') {
        return NULL;
    }
    uint32_t w = be32(p + 4), h = be32(p + 8);
    uint8_t channels = p[12];
    if (w == 0 || h == 0 || (channels != 3 && channels != 4)) {
        return NULL;
    }
    // Bound the allocation so a corrupt header can't ask for gigabytes.
    uint64_t npixels = (uint64_t)w * h;
    if (npixels > (1u << 24)) {
        return NULL;
    }

    uint32_t* out = alloc(mem, 4, 4, (ptrdiff_t)npixels);

    // Running state: the previous pixel (seeded opaque black) and the cache.
    uint8_t r = 0, g = 0, b = 0, a = 255;
    uint8_t cache[64][4] = {{0}};
    uint64_t run = 0;
    size_t pos = 14;
    size_t chunks_end = size - 8; // don't read into the end marker

    for (uint64_t px = 0; px < npixels; px++) {
        if (run > 0) {
            run--; // still emitting a run: repeat the previous pixel
        } else if (pos < chunks_end) {
            uint8_t op = p[pos++];
            if (op == QOI_OP_RGB) {
                r = p[pos++];
                g = p[pos++];
                b = p[pos++];
            } else if (op == QOI_OP_RGBA) {
                r = p[pos++];
                g = p[pos++];
                b = p[pos++];
                a = p[pos++];
            } else if ((op & QOI_MASK_2) == QOI_OP_INDEX) {
                uint8_t* c = cache[op & 0x3f];
                r = c[0];
                g = c[1];
                b = c[2];
                a = c[3];
            } else if ((op & QOI_MASK_2) == QOI_OP_DIFF) {
                r += ((op >> 4) & 0x3) - 2;
                g += ((op >> 2) & 0x3) - 2;
                b += (op & 0x3) - 2;
            } else if ((op & QOI_MASK_2) == QOI_OP_LUMA) {
                uint8_t op2 = p[pos++];
                int dg = (op & 0x3f) - 32;
                r += dg - 8 + ((op2 >> 4) & 0x0f);
                g += dg;
                b += dg - 8 + (op2 & 0x0f);
            } else {             // QOI_OP_RUN
                run = op & 0x3f; // bias 1: this pixel plus `run` more
            }
            uint8_t* c = cache[QOI_HASH(r, g, b, a) % 64];
            c[0] = r;
            c[1] = g;
            c[2] = b;
            c[3] = a;
        }
        out[px] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                  ((uint32_t)g << 8) | (uint32_t)b;
    }

    img->width = w;
    img->height = h;
    return out;
}
