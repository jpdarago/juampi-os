#ifndef __QOA_H
#define __QOA_H

#include <alloc.h>

#include <stdint.h>
#include <stddef.h>

// A small, self-contained decoder for the QOA ("Quite OK Audio") format — a
// simple lossy audio codec (https://qoaformat.org), the audio sibling of QOI.
// Like QOI it is a few dozen lines with no dependencies beyond an allocator,
// which suits a kernel: we can play sound assets without a heavyweight codec.
// (Table + LMS verified byte-for-byte against ffmpeg's independent decoder.)

typedef struct {
    uint32_t channels;
    uint32_t samplerate;
    uint32_t samples; // per channel
} qoa_desc;

// Decode `size` bytes of QOA into a freshly allocated array of
// samples*channels interleaved signed-16 PCM values, from `mem` (the caller
// frees with heap_free). Fills `*desc` and returns the samples, or NULL if the
// data is not a valid QOA stream.
int16_t* qoa_decode(allocator* mem, const void* data, size_t size,
                    qoa_desc* desc);

#endif
