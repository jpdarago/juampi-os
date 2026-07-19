#ifndef __QOI_H
#define __QOI_H

#include <alloc.h>

#include <stdint.h>
#include <stddef.h>

// A small, self-contained decoder for the QOI ("Quite OK Image") format — a
// simple lossless image codec (https://qoiformat.org). The whole decoder is a
// few dozen lines with no dependencies beyond an allocator, which is exactly
// why it suits a kernel: we can display images (an OS logo, framebuffer assets)
// without pulling in a heavyweight codec.

typedef struct {
    uint32_t width, height;
} qoi_image;

// Decode `size` bytes of QOI data into a freshly allocated array of
// width*height pixels, each 0xAARRGGBB (alpha in the top byte). The pixels come
// from `mem`; the caller frees them (heap_free) when done. Fills `*img` with
// the dimensions and returns the pixels, or NULL if the data is not a valid QOI
// image.
uint32_t* qoi_decode(allocator* mem, const void* data, size_t size,
                     qoi_image* img);

#endif
