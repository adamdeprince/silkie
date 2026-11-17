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

// A vector image: the element table of a graphics protocol f=200 image, see docs/vector-protocol.rst
typedef struct VectorDoc VectorDoc;

// A raster image drawn by an `i` record: its size, and when asked for its
// pixels too, RGBA or RGB rows that the renderer frees
typedef struct {
    uint8_t *data;
    unsigned width, height;
    bool is_opaque;
} VectorImageData;
typedef bool (*vector_image_lookup)(void *ctx, uint32_t image_id, bool with_pixels, VectorImageData *out);

// Some of the pixels of a vector image rasterized at a given size
typedef struct {
    unsigned x, y, width, height;
} VectorRect;

VectorDoc *vector_doc_new(unsigned width, unsigned height);
void vector_doc_free(VectorDoc *doc);
// Applies the records in text. A record that cannot be parsed fails all of them, and then nothing is applied;
// one that cannot be applied stops there, with the records before it applied. Returns
// NULL on success or a static error message.
const char *vector_doc_apply(VectorDoc *doc, const uint8_t *text, size_t sz);
size_t vector_doc_size(const VectorDoc *doc);
unsigned vector_doc_element_count(const VectorDoc *doc);
// Rasterizes the document into width x height straight alpha RGBA pixels, malloced, or NULL on failure
uint8_t *vector_doc_render(VectorDoc *doc, unsigned width, unsigned height, vector_image_lookup lookup, void *ctx);
// The same for the pixels in rect alone, which come out exactly as they do when the whole is rasterized
uint8_t *vector_doc_render_rect(VectorDoc *doc, unsigned width, unsigned height, VectorRect rect, vector_image_lookup lookup, void *ctx);
// A part of a vector image rasterized at a size that need not fit in memory: the image is width x height
// pixels, and the part cols x rows of them from x, y, which are whole numbers. Every curve is flattened
// only as finely as what can be seen of it in that part needs, so the time and memory drawing takes depend
// on its size, not on the coordinates in the document.
typedef struct {
    double width, height, x, y;
    unsigned cols, rows;
} VectorView;
// The pixels in rect of the part held, rect being relative to it, as they come out when all of it is drawn
uint8_t *vector_doc_render_view(VectorDoc *doc, VectorView view, VectorRect rect, vector_image_lookup lookup, void *ctx);
// vector_doc_damage() for the part held, the rectangle being relative to it
bool vector_doc_damage_view(const VectorDoc *doc, VectorView view, VectorRect *rect);
// A document remembers what the records applied to it changed. This gives the pixels that differ, for the
// document rasterized at width x height, from what it was when the damage was last cleared: false when
// there are none, otherwise the rectangle around them, which is the whole image when the change could not
// be bounded and for a document that has never had its damage cleared.
bool vector_doc_damage(const VectorDoc *doc, unsigned width, unsigned height, VectorRect *rect);
void vector_doc_clear_damage(VectorDoc *doc);
