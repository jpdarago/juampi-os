// QOA audio decoder. A file is an 8-byte header ("qoaf" + big-endian u32 of
// total samples per channel) followed by frames. Each frame is a packed 64-bit
// header (channels, samplerate, samples-per-channel, byte size), then per
// channel a 16-byte LMS state (4 history + 4 weights, big-endian s16), then
// slices: for each group of up to 20 samples, one 64-bit word per channel
// holding a 4-bit scalefactor and up to 20 three-bit residuals. Each sample is
// an LMS prediction plus a dequantized residual, clamped to s16. See the spec
// at https://qoaformat.org. Poll the whole thing into interleaved s16 PCM.
//
// The dequant table + LMS below were verified byte-for-byte against ffmpeg's
// independent QOA decoder (see the qoa validation tooling).

#include <qoa.h>

#define QOA_SLICE_LEN 20
#define QOA_FRAME_LEN 5120 // max samples per channel per frame (256 * 20)
#define QOA_LMS_LEN 4
#define QOA_MAX_CHANNELS 8

// dequant_tab[scalefactor][quantized] -> residual.
static const int qoa_dequant_tab[16][8] = {
        {1, -1, 3, -3, 5, -5, 7, -7},
        {5, -5, 18, -18, 32, -32, 49, -49},
        {16, -16, 53, -53, 95, -95, 147, -147},
        {34, -34, 113, -113, 203, -203, 315, -315},
        {63, -63, 210, -210, 378, -378, 588, -588},
        {104, -104, 345, -345, 621, -621, 966, -966},
        {158, -158, 528, -528, 950, -950, 1477, -1477},
        {228, -228, 760, -760, 1368, -1368, 2128, -2128},
        {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
        {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
        {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
        {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
        {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
        {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
        {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
        {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

struct qoa_lms {
    int history[QOA_LMS_LEN];
    int weights[QOA_LMS_LEN];
};

static int lms_predict(const struct qoa_lms* l)
{
    long p = 0;
    for (int i = 0; i < QOA_LMS_LEN; i++) {
        p += (long)l->weights[i] * l->history[i];
    }
    return (int)(p >> 13);
}

static void lms_update(struct qoa_lms* l, int sample, int residual)
{
    int delta = residual >> 4;
    for (int i = 0; i < QOA_LMS_LEN; i++) {
        l->weights[i] += l->history[i] < 0 ? -delta : delta;
    }
    for (int i = 0; i < QOA_LMS_LEN - 1; i++) {
        l->history[i] = l->history[i + 1];
    }
    l->history[QOA_LMS_LEN - 1] = sample;
}

static int clamp16(int v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static uint64_t be64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}
static int16_t be16(const uint8_t* p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int16_t* qoa_decode(struct allocator* mem, const void* data, size_t size,
                    struct qoa_desc* desc)
{
    const uint8_t* p = data;
    if (size < 16 || p[0] != 'q' || p[1] != 'o' || p[2] != 'a' || p[3] != 'f') {
        return NULL;
    }
    uint32_t total = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                     ((uint32_t)p[6] << 8) | (uint32_t)p[7];
    size_t pos = 8;

    // Peek the first frame header for channel count + rate to size the output.
    if (pos + 8 > size) {
        return NULL;
    }
    uint64_t fh = be64(p + pos);
    uint32_t channels = (uint32_t)((fh >> 56) & 0xFF);
    uint32_t rate = (uint32_t)((fh >> 32) & 0xFFFFFF);
    if (total == 0 || channels == 0 || channels > QOA_MAX_CHANNELS ||
        rate == 0) {
        return NULL;
    }
    // Bound the allocation so a corrupt header can't ask for gigabytes.
    uint64_t nsamp = (uint64_t)total * channels;
    if (nsamp > (1u << 27)) {
        return NULL;
    }
    int16_t* out = alloc(mem, 2, 2, (ptrdiff_t)nsamp);

    struct qoa_lms lms[QOA_MAX_CHANNELS];
    uint32_t done = 0; // samples per channel decoded so far
    while (done < total && pos + 8 <= size) {
        fh = be64(p + pos);
        pos += 8;
        uint32_t fchan = (uint32_t)((fh >> 56) & 0xFF);
        uint32_t fsamples = (uint32_t)((fh >> 16) & 0xFFFF);
        if (fchan != channels || fsamples == 0 || fsamples > QOA_FRAME_LEN) {
            break;
        }
        // Per-channel LMS state (history then weights, big-endian s16).
        if (pos + (size_t)channels * 16 > size) {
            break;
        }
        for (uint32_t c = 0; c < channels; c++) {
            for (int i = 0; i < QOA_LMS_LEN; i++) {
                lms[c].history[i] = be16(p + pos);
                pos += 2;
            }
            for (int i = 0; i < QOA_LMS_LEN; i++) {
                lms[c].weights[i] = be16(p + pos);
                pos += 2;
            }
        }
        // Slices: one 64-bit word per channel per 20-sample group.
        if (done + fsamples > total) {
            fsamples = total - done;
        }
        for (uint32_t off = 0; off < fsamples; off += QOA_SLICE_LEN) {
            for (uint32_t c = 0; c < channels; c++) {
                if (pos + 8 > size) {
                    goto out_of_data;
                }
                uint64_t slice = be64(p + pos);
                pos += 8;
                int sf = (int)((slice >> 60) & 0xF);
                uint32_t end = off + QOA_SLICE_LEN;
                if (end > fsamples) {
                    end = fsamples;
                }
                for (uint32_t i = off; i < end; i++) {
                    int predicted = lms_predict(&lms[c]);
                    int q = (int)((slice >> 57) & 0x7);
                    int deq = qoa_dequant_tab[sf][q];
                    int rec = clamp16(predicted + deq);
                    out[(size_t)(done + i) * channels + c] = (int16_t)rec;
                    slice <<= 3;
                    lms_update(&lms[c], rec, deq);
                }
            }
        }
        done += fsamples;
    }
out_of_data:
    desc->channels = channels;
    desc->samplerate = rate;
    desc->samples = done;
    return out;
}
