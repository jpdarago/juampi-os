// png2qoi — encode a raw 8-bit RGBA buffer into a QOI file, using the reference
// QOI codec (build/tools/qoi.h). A HOST tool: run it to (re)generate the boot
// logo from an image, e.g.
//
//   magick logo.png -resize 256x256\! -alpha on -depth 8 RGBA:logo.rgba
//   png2qoi 256 256 logo.rgba build/scripts/logo.qoi
//
// See `make logo`. Kept separate from the kernel decoder so the normal build
// needs no image tooling: logo.qoi is a checked-in asset.

#define QOI_IMPLEMENTATION
#include "qoi.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <w> <h> <in.rgba> <out.qoi>\n", argv[0]);
        return 2;
    }
    int w = atoi(argv[1]), h = atoi(argv[2]);
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "png2qoi: bad dimensions\n");
        return 1;
    }
    long n = (long)w * h * 4;
    unsigned char* buf = malloc(n);
    FILE* f = fopen(argv[3], "rb");
    if (!f || fread(buf, 1, n, f) != (size_t)n) {
        fprintf(stderr, "png2qoi: cannot read %ld bytes of RGBA from %s\n", n,
                argv[3]);
        return 1;
    }
    fclose(f);

    qoi_desc desc = {
            .width = (unsigned)w,
            .height = (unsigned)h,
            .channels = 4,
            .colorspace = QOI_SRGB,
    };
    if (!qoi_write(argv[4], buf, &desc)) {
        fprintf(stderr, "png2qoi: failed to write %s\n", argv[4]);
        return 1;
    }
    return 0;
}
