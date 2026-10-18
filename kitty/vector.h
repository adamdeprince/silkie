/*
 * vector.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <Python.h>

// 0 means no limit. Defined next to the other options so vector.c does not include state.h.
unsigned long kitty_vector_depth_limit(void);
unsigned long kitty_vector_dash_limit(void);
unsigned long kitty_vector_curve_limit(void);

// A vector image: the element table of a graphics protocol f=200 image, see docs/vector-protocol.rst
typedef struct VectorDoc VectorDoc;

// A raster image drawn by an `i` record: its size, and when asked for its
// pixels too, RGBA or RGB rows that the renderer frees
typedef struct {
    uint8_t *data;
    unsigned width, height;
    bool is_opaque, is_float;
    uint8_t gamut;
} VectorImageData;
typedef bool (*vector_image_lookup)(void *ctx, uint32_t image_id, bool with_pixels, VectorImageData *out);

// Some of the pixels of a vector image rasterized at a given size
typedef struct {
    unsigned x, y, width, height;
} VectorRect;

VectorDoc *vector_doc_new(unsigned width, unsigned height);
void vector_doc_set_font_session(VectorDoc *doc, PyObject *session);
void vector_doc_free(VectorDoc *doc);
// Applies the records in text, all or nothing. Returns NULL on success or a static error message.
const char *vector_doc_apply(VectorDoc *doc, const uint8_t *text, size_t sz);
size_t vector_doc_size(const VectorDoc *doc);
unsigned vector_doc_element_count(const VectorDoc *doc);
// Rasterizes the document into width x height straight alpha RGBA pixels, malloced, or NULL on failure
uint8_t *vector_doc_render(VectorDoc *doc, unsigned width, unsigned height, vector_image_lookup lookup, void *ctx);
// The same for the pixels in rect alone, which come out exactly as they do when the whole is rasterized
uint8_t *vector_doc_render_rect(VectorDoc *doc, unsigned width, unsigned height, VectorRect rect, vector_image_lookup lookup, void *ctx);
// Linear extended-sRGB, straight alpha floats; preserves negative/HDR channels.
float *vector_doc_render_linear(VectorDoc *doc, unsigned width, unsigned height, VectorRect rect, vector_image_lookup lookup, void *ctx);
// A document remembers what the records applied to it changed. This gives the pixels that differ, for the
// document rasterized at width x height, from what it was when the damage was last cleared: false when
// there are none, otherwise the rectangle around them, which is the whole image when the change could not
// be bounded and for a document that has never had its damage cleared.
bool vector_doc_damage(const VectorDoc *doc, unsigned width, unsigned height, VectorRect *rect);
void vector_doc_clear_damage(VectorDoc *doc);
// Invalidates raster references when their graphics resource changes.
bool vector_doc_image_changed(VectorDoc *doc, uint32_t image_id);
// Takes a point from the user units of the image into the coordinate system of a group, 0 being the root
// group. False when the group does not exist or its transforms cannot be inverted.
bool vector_doc_point_to_group(const VectorDoc *doc, uint32_t group, double *x, double *y);
// Whether the len bytes at s are a color, in the syntax of the records, other than none
bool vector_color_paints(const char *s, size_t len);
