/*
 * iosurface.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void *iosurface_lookup(uint32_t id, uint32_t *width, uint32_t *height, bool *is_bgra, char *err, size_t errsz);
bool iosurface_bind_rectangle_texture(void *surface, uint32_t *texture_id, bool is_bgra);
void iosurface_release(void *surface);
// For the tests: a surface of this process with its id, NULL on failure, released by iosurface_release_for_tests(); and
// the number of surfaces iosurface_lookup() gave that iosurface_release() has not released. A lookup gives an object of
// its own even for a surface of this process, so the reference count of the created one does not show what is held.
void *iosurface_create_for_tests(uint32_t width, uint32_t height, uint32_t *id);
void iosurface_release_for_tests(void *surface);
long iosurface_held_count(void);
