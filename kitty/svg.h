/*
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The in-process SVG renderer's C ABI, implemented in svg/src/lib.rs.
typedef struct SvgTree SvgTree;
typedef enum { SVG_OK, SVG_INVALID, SVG_OVERFLOW, SVG_NO_MEMORY } SvgStatus;

SvgStatus silkie_svg_parse(const uint8_t *data, size_t len, uint32_t *width, uint32_t *height, SvgTree **out);
void silkie_svg_free(SvgTree *tree);
bool silkie_svg_render(const SvgTree *tree, double image_width, double image_height, double x, double y,
                       uint32_t width, uint32_t height, uint8_t *pixels, size_t len);
