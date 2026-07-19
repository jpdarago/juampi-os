// mklogo — generate the boot logo (build/scripts/logo.qoi) at build time.
//
// This is a HOST program (built with the native compiler, not the kernel
// toolchain): it draws a small emblem procedurally and encodes it with the
// reference QOI encoder (build/tools/qoi.h). Producing the image with the
// reference encoder means the kernel's own from-scratch decoder (src/qoi.c) is
// exercised against a real, independently-encoded QOI file rather than only its
// own output. No external image asset or converter is needed.

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

static unsigned char img[W * H * 4]; // RGBA, top-left origin

static void put(int x, int y, int r, int g, int b, int a)
{
    if (x < 0 || y < 0 || x >= W || y >= H) {
        return;
    }
    unsigned char* p = &img[(y * W + x) * 4];
    p[0] = (unsigned char)r;
    p[1] = (unsigned char)g;
    p[2] = (unsigned char)b;
    p[3] = (unsigned char)a;
}

// Is (x,y) inside the rectangle [x0,x1) x [y0,y1) with rounded corners of the
// given radius?
static int in_rounded(int x, int y, int x0, int y0, int x1, int y1, int rad)
{
    if (x < x0 || x >= x1 || y < y0 || y >= y1) {
        return 0;
    }
    int dx = 0, dy = 0;
    if (x < x0 + rad) {
        dx = x0 + rad - x;
    } else if (x >= x1 - rad) {
        dx = x - (x1 - rad - 1);
    }
    if (y < y0 + rad) {
        dy = y0 + rad - y;
    } else if (y >= y1 - rad) {
        dy = y - (y1 - rad - 1);
    }
    return dx * dx + dy * dy <= rad * rad;
}

// L1 ("diamond") distance from centre, normalised so <= 256 means inside a
// diamond of half-extent `half`.
static int diamond(int x, int y, int cx, int cy, int half)
{
    int d = abs(x - cx) + abs(y - cy);
    return d * 256 / half; // 256 == on the edge
}

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "logo.qoi";

    // Background stays fully transparent (img is zeroed) so the logo composes
    // over whatever is already on the framebuffer.
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int badge = in_rounded(x, y, 16, 16, 240, 240, 30);
            if (!badge) {
                continue;
            }
            int inner = in_rounded(x, y, 20, 20, 236, 236, 26);
            if (!inner) {
                put(x, y, 130, 160, 225, 255); // soft border
                continue;
            }
            // Vertical gradient: deep indigo (top) -> teal (bottom).
            int t = (y - 20) * 256 / (236 - 20);
            int r = 36 + (11 - 36) * t / 256;
            int g = 26 + (110 - 26) * t / 256;
            int b = 92 + (107 - 92) * t / 256;

            // Concentric diamond emblem, from outside in.
            int d = diamond(x, y, 128, 128, 82);
            if (d <= 256) {
                if (d <= 90) {
                    r = 60, g = 205, b = 190; // bright teal core
                } else if (d <= 170) {
                    r = 244, g = 246, b = 250; // white ring
                } else {
                    r = 255, g = 157, b = 60; // orange ring
                }
            }
            put(x, y, r, g, b, 255);
        }
    }

    qoi_desc desc = {
            .width = W,
            .height = H,
            .channels = 4,
            .colorspace = QOI_SRGB,
    };
    if (!qoi_write(out, img, &desc)) {
        fprintf(stderr, "mklogo: failed to write %s\n", out);
        return 1;
    }
    return 0;
}
