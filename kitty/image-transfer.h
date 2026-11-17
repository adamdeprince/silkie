/*
 * image-transfer.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// How the pixels of a high precision raw image are stored, see
// docs/silkie/precision-image-protocol.rst. The stored bytes are the bytes on
// the wire, all little-endian.
typedef enum {
    DEEP_NONE = 0,     // eight bits per channel RGB or RGBA, or half float RGBA
    DEEP_GRAY16 = 1,   // f=316, one 16-bit gray value per pixel
    DEEP_RGB10A2 = 2,  // f=632, a 32-bit word with 10 bits of R, G and B and 2 of alpha
    DEEP_RGBA16 = 3,   // f=664, four 16-bit channels
} DeepStorage;

// The transfer function a raw image is encoded with, the E key of the
// graphics protocol, which carries ITU-T H.273 code points
typedef enum {
    TRANSFER_SRGB = 0,  // the default, as for every image before the key existed
    TRANSFER_LINEAR = 1,
    TRANSFER_PQ = 2,   // SMPTE ST 2084
    TRANSFER_HLG = 3,  // ITU-R BT.2100 hybrid log-gamma
} TransferFunction;

// The decoding the graphics shader applies to sampled values, the
// IMAGE_DECODE_ constants of graphics.slang
typedef enum { IMAGE_DECODE_NONE = 0, IMAGE_DECODE_SRGB = 1, IMAGE_DECODE_PQ = 2, IMAGE_DECODE_HLG = 3 } ImageDecode;

// The luminance of SDR white, which is 1.0 in the linear values of the
// terminal, for converting the absolute luminances of PQ: the reference white
// of ITU-R BT.2408. The nominal peak of an HLG display, at which the reference
// white of HLG (a signal of 0.75) lands on that same 203 nits.
#define TRANSFER_SDR_WHITE_NITS 203.f
#define TRANSFER_HLG_PEAK_NITS 1000.f

// The transfer function of an H.273 code point: 0 (no key) and 13 are sRGB, 8
// linear, 16 PQ and 18 HLG. False for any other value.
bool transfer_from_code_point(uint32_t code, uint8_t *transfer);
// The number of bytes of a stored pixel
unsigned deep_pixel_size(uint8_t deep);
// What the shader has to do to the values sampled from the texture of an image
uint8_t image_decode_for(uint8_t deep, uint8_t transfer);

static inline bool
transfer_is_hdr(uint8_t transfer) {
    return transfer == TRANSFER_PQ || transfer == TRANSFER_HLG;
}

// Turns stored pixels into linear RGBA floats with straight alpha, where 1.0
// is SDR white, as the graphics shader does. The decoder of an eight bit
// image (DEEP_NONE) reads RGB or RGBA bytes.
typedef struct {
    float *lut;  // the decoded value of every code of a channel
    float luma[3], to_srgb[9];
    uint8_t deep, transfer, gamut;
    bool is_opaque;
} PixelDecoder;

// False when out of memory
bool pixel_decoder_init(PixelDecoder *d, uint8_t deep, bool is_opaque, uint8_t transfer, uint8_t gamut);
void pixel_decoder_free(PixelDecoder *d);
// Decodes num pixels into out, four floats each. With to_srgb the colors are
// converted from the gamut of the image to the sRGB primaries, unclipped,
// otherwise they stay in the gamut of the image.
void pixel_decoder_decode(const PixelDecoder *d, const uint8_t *src, size_t num, bool to_srgb, float *out);
// The largest decoded color component, at least 1, in the gamut of the image
float pixel_decoder_peak(const PixelDecoder *d, const uint8_t *src, size_t num);
