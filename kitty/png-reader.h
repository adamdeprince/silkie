/*
 * Copyright (C) 2018 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <png.h>

typedef struct png_read_data png_read_data;

typedef void (*png_error_handler_func)(png_read_data *d, const char *, const char *);

typedef struct png_read_data {
    uint8_t *decompressed;
    bool ok;
    png_bytep *row_pointers;
    int width, height;
    size_t sz;
    png_error_handler_func err_handler;
    struct {
        char *buf;
        size_t used, capacity;
    } error;
    // The gamut of the decoded pixels, see docs/wide-gamut-protocol.rst. It is
    // sRGB unless the caller asks otherwise: with keep_wide_gamut a PNG whose
    // colors are Display P3 or Rec. 2020 stays in that gamut, and with
    // convert_to_gamut non-zero every PNG is converted into that gamut
    bool keep_wide_gamut;
    uint8_t convert_to_gamut, gamut;
    // the decoding failed for want of memory, and sz is the size of the pixels it needed
    bool out_of_memory;
} png_read_data;

// Decodes a PNG of any size the format allows, the only bound the memory there is
#define inflate_png_inner(d, buf, bufsz) inflate_png_inner_limited(d, buf, bufsz, 0)
// As above, refusing an image wider or taller than max_image_dimension when that is positive
void inflate_png_inner_limited(png_read_data *d, const uint8_t *buf, size_t bufsz, int max_image_dimension);
// Converts RGB or RGBA pixels in place from one gamut of docs/wide-gamut-protocol.rst to another
bool convert_pixels_between_gamuts(uint8_t *pixels, size_t num_pixels, bool has_alpha, unsigned from, unsigned to);
const char *png_from_32bit_rgba(const char *data, size_t width, size_t height, size_t *out_size, bool flip_vertically);
const char *png_from_24bit_rgb(const char *data, size_t width, size_t height, size_t *out_size, bool flip_vertically);
