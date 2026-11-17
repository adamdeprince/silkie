/*
 * deep-colors.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "data-types.h"

// Text colors with 16 bits per channel, see docs/deep-color-protocol.rst.
//
// A cell color word has 24 bits of payload above its type bits, enough for an
// 8 bit RGB color but not for three 16 bit channels and a gamut. Widening the
// color words would grow every cell of every screen and its scrollback for
// the few that use deep colors, so a deep color is interned in a per-screen
// table instead and the cell holds its index, with DEEP_COLOR_TYPE as the
// type. The table is deduplicated, so equal colors have equal color words and
// cells compare as before, and it is garbage collected by mark and sweep: the
// screen marks the index of every color word it holds and the unmarked
// entries are put on a free list. Live entries never move, so a GC changes no
// cell and invalidates nothing on the GPU.
//
// The layout of DeepColor is that of the RGBA16UI texture the cell shader
// reads it from, so the table is uploaded as it is.
typedef struct DeepColor {
    uint16_t r, g, b;
    // the gamut id of docs/wide-gamut-protocol.rst in the low bits,
    // DEEP_COLOR_ALLOCATED marks a slot that is in use
    uint16_t gamut;
} DeepColor;
static_assert(sizeof(DeepColor) == 4 * sizeof(uint16_t), "DeepColor must match the RGBA16UI texel layout");

#define DEEP_COLOR_GAMUT_MASK 3u
#define DEEP_COLOR_ALLOCATED 0x8000u

// The index of a deep color is the 24 bit payload of a cell color word, the
// most entries a table can address. The renderer lowers the limit to what its
// texture can hold, see deep_colors_set_max_entries().
#define DEEP_COLORS_MAX_INDEX_BITS 24u
#define DEEP_COLORS_ADDRESSABLE (1u << DEEP_COLORS_MAX_INDEX_BITS)

typedef struct DeepColors DeepColors;

DeepColors *deep_colors_alloc(void);
void deep_colors_free(DeepColors *self);

// The color word referring to the interned color, with the type bits set and
// the bg alpha bits clear, or 0 if it could not be interned: the table is at
// its limit with live colors or memory ran out. The caller then falls back to
// an 8 bit RGB color, the closest a cell can show without the table.
color_type deep_colors_intern(DeepColors *self, DeepColor c);
// The color a color word of DEEP_COLOR_TYPE refers to, false for a stale or
// out of range index
bool deep_colors_get(const DeepColors *self, color_type c, DeepColor *ans);
// The nearest 8 bit sRGB encoded color as 0xRRGGBB, for the code paths that
// only have 8 bits, such as the colors of the window edges
color_type deep_color_to_rgb(DeepColor c);

// Number of colors in use and number of slots, used and free
size_t deep_colors_live(const DeepColors *self);
size_t deep_colors_slots(const DeepColors *self);
const DeepColor *deep_colors_data(const DeepColors *self);

// The renderer's limit on the number of slots, from the largest texture the
// GPU supports. Shared by every table since every table is drawn by the same
// GPU. Never more than DEEP_COLORS_ADDRESSABLE.
void deep_colors_set_max_entries(size_t n);
size_t deep_colors_max_entries(void);

// The slots written since the renderer last uploaded the table, [*start, *end).
// Returns false if there are none. deep_colors_clear_dirty() resets them.
bool deep_colors_dirty_range(const DeepColors *self, size_t *start, size_t *end);
void deep_colors_clear_dirty(DeepColors *self);

// Garbage collection. The owner of every color word that may refer to the
// table, the Screen, calls deep_colors_gc_begin(), then deep_colors_gc_mark()
// for every color word it holds, of any type, and finally deep_colors_gc_end().
// should_gc() amortizes the cost of a collection, linear in the number of
// cells, over as many insertions as there were live colors.
bool deep_colors_should_gc(const DeepColors *self);
bool deep_colors_gc_begin(DeepColors *self);
void deep_colors_gc_mark(DeepColors *self, color_type c);
void deep_colors_gc_end(DeepColors *self);
