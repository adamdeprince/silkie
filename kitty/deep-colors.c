/*
 * deep-colors.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#include "deep-colors.h"

#define NAME deep_color_map
#define KEY_TY uint64_t
#define VAL_TY uint32_t
#define HASH_FN vt_hash_integer
#define CMPR_FN vt_cmpr_integer
#include "kitty-verstable.h"

struct DeepColors {
    DeepColor *items;
    // slots in use or on the free list, and allocated
    size_t count, capacity;
    size_t live;
    // head of the free list plus one, zero when it is empty. A free slot holds
    // the next link, plus one, in r and g, and a gamut without DEEP_COLOR_ALLOCATED.
    size_t free_head;
    deep_color_map map;
    size_t dirty_start, dirty_end;
    size_t adds_since_last_gc, adds_between_gcs;
    uint8_t *marks;
};

// Before the renderer knows its GPU every table may address all of the index space
static size_t max_entries = DEEP_COLORS_ADDRESSABLE;

void
deep_colors_set_max_entries(size_t n) {
    max_entries = MAX((size_t)1, MIN(n, (size_t)DEEP_COLORS_ADDRESSABLE));
}

size_t
deep_colors_max_entries(void) {
    return max_entries;
}

static uint64_t
key_for(DeepColor c) {
    return (uint64_t)c.r | ((uint64_t)c.g << 16) | ((uint64_t)c.b << 32) | ((uint64_t)(c.gamut & DEEP_COLOR_GAMUT_MASK) << 48);
}

DeepColors *
deep_colors_alloc(void) {
    DeepColors *ans = calloc(1, sizeof(DeepColors));
    if (!ans) return NULL;
    vt_init(&ans->map);
    return ans;
}

void
deep_colors_free(DeepColors *self) {
    if (!self) return;
    vt_cleanup(&self->map);
    free(self->items);
    free(self->marks);
    free(self);
}

static void
mark_dirty(DeepColors *self, size_t idx) {
    if (self->dirty_start >= self->dirty_end) {
        self->dirty_start = idx;
        self->dirty_end = idx + 1;
    } else {
        self->dirty_start = MIN(self->dirty_start, idx);
        self->dirty_end = MAX(self->dirty_end, idx + 1);
    }
}

static bool
grow(DeepColors *self) {
    const size_t limit = max_entries;
    if (self->count >= limit) return false;
    if (self->count < self->capacity) return true;
    size_t cap = MAX((size_t)64, self->capacity * 2);
    cap = MIN(cap, limit);
    if (cap <= self->count || cap > SIZE_MAX / sizeof(DeepColor)) return false;
    DeepColor *items = realloc(self->items, cap * sizeof(DeepColor));
    if (!items) return false;
    self->items = items;
    self->capacity = cap;
    return true;
}

static size_t
pop_free_slot(DeepColors *self) {
    size_t idx = self->free_head - 1;
    const DeepColor *s = self->items + idx;
    self->free_head = (size_t)s->r | ((size_t)s->g << 16);
    return idx;
}

static void
push_free_slot(DeepColors *self, size_t idx) {
    // the indices fit in 24 bits, so the link plus one fits in 32
    self->items[idx] = (DeepColor){.r = self->free_head & 0xffff, .g = (self->free_head >> 16) & 0xffff};
    self->free_head = idx + 1;
}

color_type
deep_colors_intern(DeepColors *self, DeepColor c) {
    if (!self) return 0;
    c.gamut &= DEEP_COLOR_GAMUT_MASK;
    const uint64_t key = key_for(c);
    deep_color_map_itr i = vt_get(&self->map, key);
    if (!vt_is_end(i)) return ((color_type)i.data->val << 8) | DEEP_COLOR_TYPE;
    self->adds_since_last_gc++;
    size_t idx;
    bool from_free_list = false;
    if (self->free_head) {
        idx = pop_free_slot(self);
        from_free_list = true;
    } else {
        if (!grow(self)) return 0;
        idx = self->count++;
    }
    if (vt_is_end(vt_insert(&self->map, key, (uint32_t)idx))) {
        if (from_free_list) push_free_slot(self, idx);
        else self->count--;
        return 0;
    }
    c.gamut |= DEEP_COLOR_ALLOCATED;
    self->items[idx] = c;
    self->live++;
    mark_dirty(self, idx);
    return ((color_type)idx << 8) | DEEP_COLOR_TYPE;
}

bool
deep_colors_get(const DeepColors *self, color_type c, DeepColor *ans) {
    if (!self || (c & COLOR_TYPE_MASK) != DEEP_COLOR_TYPE) return false;
    const size_t idx = c >> 8;
    if (idx >= self->count || !(self->items[idx].gamut & DEEP_COLOR_ALLOCATED)) return false;
    *ans = self->items[idx];
    ans->gamut &= DEEP_COLOR_GAMUT_MASK;
    return true;
}

static uint8_t
to_8bit(uint16_t x) {
    // round to nearest, 257 being 65535 / 255
    return (uint8_t)(((uint32_t)x * 255u + 32767u) / 65535u);
}

color_type
deep_color_to_rgb(DeepColor c) {
    return ((color_type)to_8bit(c.r) << 16) | ((color_type)to_8bit(c.g) << 8) | to_8bit(c.b);
}

size_t
deep_colors_live(const DeepColors *self) {
    return self ? self->live : 0;
}

size_t
deep_colors_slots(const DeepColors *self) {
    return self ? self->count : 0;
}

const DeepColor *
deep_colors_data(const DeepColors *self) {
    return self ? self->items : NULL;
}

bool
deep_colors_dirty_range(const DeepColors *self, size_t *start, size_t *end) {
    if (!self || self->dirty_start >= self->dirty_end) return false;
    *start = self->dirty_start;
    *end = MIN(self->dirty_end, self->count);
    return *start < *end;
}

void
deep_colors_clear_dirty(DeepColors *self) {
    if (self) self->dirty_start = self->dirty_end = 0;
}

// Collections are amortized like those of the text cache: at least this many
// insertions apart, and as many as there were live colors after the last one.
#define DEEP_COLORS_ADDS_BETWEEN_GCS 4096u

bool
deep_colors_should_gc(const DeepColors *self) {
    return self && self->adds_since_last_gc > MAX((size_t)DEEP_COLORS_ADDS_BETWEEN_GCS, self->adds_between_gcs);
}

bool
deep_colors_gc_begin(DeepColors *self) {
    free(self->marks);
    self->marks = calloc(MAX((size_t)1, (self->count + 7) / 8), 1);
    return self->marks != NULL;
}

void
deep_colors_gc_mark(DeepColors *self, color_type c) {
    if ((c & COLOR_TYPE_MASK) != DEEP_COLOR_TYPE) return;
    const size_t idx = c >> 8;
    if (idx < self->count) self->marks[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

void
deep_colors_gc_end(DeepColors *self) {
    for (size_t idx = 0; idx < self->count; idx++) {
        DeepColor *s = self->items + idx;
        if (!(s->gamut & DEEP_COLOR_ALLOCATED) || (self->marks[idx >> 3] & (1u << (idx & 7)))) continue;
        vt_erase(&self->map, key_for(*s));
        s->gamut = 0;
        self->live--;
    }
    // Free slots at the end are given back, so the table and the texture
    // drawn from it shrink when their colors are gone, and the free list is
    // rebuilt lowest index first, so that new colors fill the holes from the
    // bottom and the table stays compact.
    while (self->count && !(self->items[self->count - 1].gamut & DEEP_COLOR_ALLOCATED)) self->count--;
    self->free_head = 0;
    for (size_t idx = self->count; idx-- > 0;)
        if (!(self->items[idx].gamut & DEEP_COLOR_ALLOCATED)) push_free_slot(self, idx);
    if (self->capacity > 64 && self->count < self->capacity / 4) {
        const size_t cap = MAX((size_t)64, self->count * 2);
        DeepColor *items = realloc(self->items, cap * sizeof(DeepColor));
        if (items) {
            self->items = items;
            self->capacity = cap;
        }
    }
    free(self->marks);
    self->marks = NULL;
    self->adds_since_last_gc = 0;
    self->adds_between_gcs = self->live;
    if (self->dirty_end > self->count) self->dirty_end = self->count;
}
