/*
 * image-transfer.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

// The transfer functions and high precision storage of raw images, see
// docs/silkie/precision-image-protocol.rst. The graphics shader decodes the
// texture of an image in the same way, see graphics.slang; this is for what
// reads the pixels on the CPU: the peak of an HDR image, vector images that
// draw a raster image and pointer images.

#include "data-types.h"
#include "image-transfer.h"
#include <math.h>
#include <stdlib.h>

bool
transfer_from_code_point(uint32_t code, uint8_t *transfer) {
    switch (code) {
        case 0:
        case 13: *transfer = TRANSFER_SRGB; return true;
        case 8: *transfer = TRANSFER_LINEAR; return true;
        case 16: *transfer = TRANSFER_PQ; return true;
        case 18: *transfer = TRANSFER_HLG; return true;
        default: return false;
    }
}

unsigned
deep_pixel_size(uint8_t deep) {
    switch (deep) {
        case DEEP_GRAY16: return 2;
        case DEEP_RGB10A2: return 4;
        case DEEP_RGBA16: return 8;
        default: return 0;
    }
}

uint8_t
image_decode_for(uint8_t deep, uint8_t transfer) {
    switch (transfer) {
        // an eight bit sRGB image is in an sRGB texture, which the GPU decodes
        case TRANSFER_SRGB: return deep ? IMAGE_DECODE_SRGB : IMAGE_DECODE_NONE;
        case TRANSFER_PQ: return IMAGE_DECODE_PQ;
        case TRANSFER_HLG: return IMAGE_DECODE_HLG;
        default: return IMAGE_DECODE_NONE;
    }
}

static float
srgb_eotf(float v) {
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

// SMPTE ST 2084, in multiples of SDR white
static float
pq_eotf(float v) {
    const float m1 = 2610.f / 16384.f, m2 = 2523.f / 4096.f * 128.f, c1 = 3424.f / 4096.f, c2 = 2413.f / 4096.f * 32.f, c3 = 2392.f / 4096.f * 32.f;
    const float p = powf(v, 1.f / m2);
    const float nits = 10000.f * powf(fmaxf(p - c1, 0.f) / (c2 - c3 * p), 1.f / m1);
    return nits / TRANSFER_SDR_WHITE_NITS;
}

// The inverse of the HLG OETF of BT.2100: scene linear light from 0 to 1
static float
hlg_inverse_oetf(float v) {
    const float a = 0.17883277f, b = 1.f - 4.f * a, c = 0.5f - a * logf(4.f * a);
    return v <= 0.5f ? v * v / 3.f : (expf((v - c) / a) + b) / 12.f;
}

// Linear RGB of each gamut to CIE XYZ, and XYZ to linear sRGB, all with the
// D65 white point, the same matrices as shaders/gamut.slang
static const double gamut_to_xyz[3][9] = {
    {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522, 0.0721750, 0.0193339, 0.1191920, 0.9503041},
    {0.4865709, 0.2656677, 0.1982173, 0.2289746, 0.6917385, 0.0792869, 0.0000000, 0.0451134, 1.0439444},
    {0.6369580, 0.1446169, 0.1688810, 0.2627002, 0.6779981, 0.0593017, 0.0000000, 0.0280727, 1.0609851},
};
static const double xyz_to_srgb[9] = {3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108, 0.0415560, 0.0556434, -0.2040259, 1.0572252};

static unsigned
bits_per_channel(uint8_t deep) {
    switch (deep) {
        case DEEP_RGB10A2: return 10;
        case DEEP_GRAY16:
        case DEEP_RGBA16: return 16;
        default: return 8;
    }
}

bool
pixel_decoder_init(PixelDecoder *d, uint8_t deep, bool is_opaque, uint8_t transfer, uint8_t gamut) {
    *d = (PixelDecoder){.deep = deep, .transfer = transfer, .gamut = gamut <= GAMUT_REC2020 ? gamut : 0, .is_opaque = is_opaque};
    // one entry per code of a channel: the channels have at most 16 bits
    const size_t num = (size_t)1 << bits_per_channel(deep);
    d->lut = malloc(num * sizeof(d->lut[0]));
    if (!d->lut) return false;
    const float maximum = (float)(num - 1);
    for (size_t i = 0; i < num; i++) {
        const float v = (float)i / maximum;
        switch (transfer) {
            case TRANSFER_LINEAR: d->lut[i] = v; break;
            case TRANSFER_PQ: d->lut[i] = pq_eotf(v); break;
            case TRANSFER_HLG: d->lut[i] = hlg_inverse_oetf(v); break;
            default: d->lut[i] = srgb_eotf(v); break;
        }
    }
    const double *m = gamut_to_xyz[d->gamut];
    for (unsigned i = 0; i < 3; i++) d->luma[i] = (float)m[3 + i];
    for (unsigned r = 0; r < 3; r++) {
        for (unsigned c = 0; c < 3; c++) {
            double sum = 0;
            for (unsigned k = 0; k < 3; k++) sum += xyz_to_srgb[3 * r + k] * m[3 * k + c];
            d->to_srgb[3 * r + c] = (float)sum;
        }
    }
    return true;
}

void
pixel_decoder_free(PixelDecoder *d) {
    free(d->lut);
    d->lut = NULL;
}

static inline unsigned
u16le(const uint8_t *p) {
    return p[0] | ((unsigned)p[1] << 8);
}

static inline uint32_t
u32le(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline size_t
stored_pixel_size(const PixelDecoder *d) {
    return d->deep ? deep_pixel_size(d->deep) : d->is_opaque ? 3 : 4;
}

// One pixel, in the gamut of the image
static inline void
decode_pixel(const PixelDecoder *d, const uint8_t *p, float *out) {
    switch (d->deep) {
        case DEEP_GRAY16: out[0] = out[1] = out[2] = d->lut[u16le(p)]; out[3] = 1.f; break;
        case DEEP_RGB10A2: {
            const uint32_t w = u32le(p);
            out[0] = d->lut[w & 1023];
            out[1] = d->lut[(w >> 10) & 1023];
            out[2] = d->lut[(w >> 20) & 1023];
            out[3] = (float)(w >> 30) / 3.f;
        } break;
        case DEEP_RGBA16:
            for (unsigned c = 0; c < 3; c++) out[c] = d->lut[u16le(p + 2 * c)];
            out[3] = (float)u16le(p + 6) / 65535.f;
            break;
        default:
            for (unsigned c = 0; c < 3; c++) out[c] = d->lut[p[c]];
            out[3] = d->is_opaque ? 1.f : (float)p[3] / 255.f;
            break;
    }
    if (d->transfer == TRANSFER_HLG) {
        // The OOTF of BT.2100 for a display with the nominal peak, which takes the reference white of HLG to SDR white
        const float y = d->luma[0] * out[0] + d->luma[1] * out[1] + d->luma[2] * out[2];
        const float scale = y > 0.f ? TRANSFER_HLG_PEAK_NITS / TRANSFER_SDR_WHITE_NITS * powf(y, 0.2f) : 0.f;
        for (unsigned c = 0; c < 3; c++) out[c] *= scale;
    }
}

void
pixel_decoder_decode(const PixelDecoder *d, const uint8_t *src, size_t num, bool to_srgb, float *out) {
    const size_t step = stored_pixel_size(d);
    const float *m = d->to_srgb;
    for (size_t i = 0; i < num; i++, src += step, out += 4) {
        decode_pixel(d, src, out);
        if (to_srgb && d->gamut) {
            const float r = out[0], g = out[1], b = out[2];
            for (unsigned c = 0; c < 3; c++) out[c] = m[3 * c] * r + m[3 * c + 1] * g + m[3 * c + 2] * b;
        }
    }
}

float
pixel_decoder_peak(const PixelDecoder *d, const uint8_t *src, size_t num) {
    const size_t step = stored_pixel_size(d);
    float ans = 1.f, px[4];
    for (size_t i = 0; i < num; i++, src += step) {
        decode_pixel(d, src, px);
        for (unsigned c = 0; c < 3; c++) {
            if (px[c] > ans) ans = px[c];
        }
    }
    return ans;
}
