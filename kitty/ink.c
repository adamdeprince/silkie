/*
 * ink.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

// Terminal-side ink, see docs/ink-protocol.rst. A program hands the terminal
// a brush for one of its vector images, and from then on the terminal draws
// the strokes of the pen itself, as the reports of the tablet pointer protocol
// are made, by applying to that image the records the program would have sent.

#include "state.h"
#include "screen.h"
#include "control-codes.h"
#include "extension-fields.h"
#include <math.h>

// The width of the stroke is worked out exactly as the specification writes it
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

#define DEFAULT_TOOLS (1u << 1 | 1u << 3 | 1u << 4 | 1u << 5)  // pen, brush, pencil, airbrush
#define ALL_TOOLS 511u                                         // the tool values 0 to 8

static bool
parse_u32(const char *s, uint32_t *out) {
    if (!*s) return false;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (unsigned)(*s - '0');
        if (v > UINT32_MAX) return false;
    }
    *out = (uint32_t)v;
    return true;
}

// A number as the records of a vector image write one
static bool
parse_number(const char *s, double *out) {
    if (!*s) return false;
    for (const char *p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) return false;
    }
    char *end;
    *out = strtod(s, &end);
    return !*end && isfinite(*out);
}

// The shortest text that reads back as the same number
static const char *
number(double v, char *buf, size_t sz) {
    for (int digits = 1; digits <= 17; digits++) {
        snprintf(buf, sz, "%.*g", digits, v);
        if (strtod(buf, NULL) == v) break;
    }
    return buf;
}

static void
end_strokes(Screen *self) {
    for (size_t i = 0; i < self->ink.capacity; i++) self->ink.strokes[i].active = false;
    self->ink.num_strokes = 0;
}

// Turns ink off as mode=0 does: the brush is kept, the strokes being drawn stay as they are
void
screen_ink_off(Screen *self) {
    self->ink.on = false;
    end_strokes(self);
}

void
screen_ink_reset(Screen *self) {
    free(self->ink.strokes);
    free(self->ink.brush.fill);
    zero_at_ptr(&self->ink);
}

static void
set_brush(Screen *self, const ExtensionFields *f) {
    InkBrush b = {.tools = DEFAULT_TOOLS, .remaining = 1, .exponent = 1, .w0 = -1, .w1 = -1};
    char *fill = strdup("#000000");
    bool has_first = false;
    if (!fill) return;
    for (size_t i = 0; i < f->count; i++) {
        const char *key = f->keys[i], *val = f->values[i];
        if (!strcmp(key, "i")) {
            if (!parse_u32(val, &b.image)) goto fail;
        } else if (!strcmp(key, "g")) {
            if (!parse_u32(val, &b.group)) goto fail;
        } else if (!strcmp(key, "k")) {
            if (!parse_u32(val, &b.next_id)) goto fail;
            has_first = true;
        } else if (!strcmp(key, "n")) {
            if (!parse_u32(val, &b.remaining)) goto fail;
        } else if (!strcmp(key, "tools")) {
            uint32_t tools;
            if (!parse_u32(val, &tools) || !tools || tools > ALL_TOOLS) goto fail;
            b.tools = tools;
        } else if (!strcmp(key, "w0")) {
            if (!parse_number(val, &b.w0) || !isfinite(b.w0) || b.w0 < 0) goto fail;
        } else if (!strcmp(key, "w1")) {
            if (!parse_number(val, &b.w1) || !isfinite(b.w1) || b.w1 < 0) goto fail;
        } else if (!strcmp(key, "e")) {
            if (!parse_number(val, &b.exponent) || !isfinite(b.exponent) || !(b.exponent > 0)) goto fail;
        } else if (!strcmp(key, "fill")) {
            const size_t len = strlen(val);
            char *next;
            if (!vector_color_paints(val, len)) goto fail;
            next = malloc(len + 1);
            if (!next) goto fail;
            memcpy(next, val, len + 1);
            free(fill);
            fill = next;
        }
    }
    // the image, the first of the ids and the width at full pressure have to be given
    if (!b.image || !has_first || !b.next_id || !b.remaining || b.w1 < 0) goto fail;
    if (b.remaining - 1 > UINT32_MAX - b.next_id) goto fail;  // the ids run past the largest there is
    if (b.w0 < 0) b.w0 = b.w1;
    free(self->ink.brush.fill);
    self->ink.brush = b;
    self->ink.brush.fill = fill;
    self->ink.on = true;
    return;
fail:
    free(fill);
}

// A frame from the program, see docs/extension-framing.rst for what is rejected
void
screen_ink(Screen *self, const char *data, size_t sz) {
    EXTENSION_FIELDS(f);
    char reply[320];
    const char *op;
    long long mode = -1, id = -1;
    if (!extension_fields_parse(&f, data, sz) || !extension_frame_header(&f, 1, &op)) return;
    if (!extension_field_int(&f, "mode", 0, INT32_MAX, &mode) || !extension_field_int(&f, "id", 0, INT32_MAX, &id)) return;
    const InkBrush *b = &self->ink.brush;
    if (!strcmp(op, "query") && id >= 0) {
        int n = snprintf(reply, sizeof(reply), "777;ink;v=1:op=cap:id=%lld:caps=1:mode=%d", id, self->ink.on);
        if (b->image) {
            char w0[32], w1[32], e[32];
            snprintf(
                reply + n, sizeof(reply) - n, ":i=%u:g=%u:k=%u:n=%u:fill=%s:w0=%s:w1=%s:e=%s:tools=%u", b->image, b->group, b->next_id, b->remaining, b->fill,
                number(b->w0, w0, sizeof(w0)), number(b->w1, w1, sizeof(w1)), number(b->exponent, e, sizeof(e)), b->tools);
        }
        write_escape_code_to_child(self, ESC_OSC, reply);
    } else if (!strcmp(op, "mode")) {
        if (mode < 0) {  // a read
            snprintf(reply, sizeof(reply), "777;ink;v=1:op=mode:mode=%d", self->ink.on);
            write_escape_code_to_child(self, ESC_OSC, reply);
        } else if (mode == 0) screen_ink_off(self);
        else if (mode == 1 && b->image) self->ink.on = true;  // with the brush last given, there is nothing to turn on without one
    } else if (!strcmp(op, "brush")) set_brush(self, &f);
}

static InkStroke *
stroke_of(Screen *self, const PointerReport *r) {
    for (size_t i = 0; self->ink.num_strokes && i < self->ink.capacity; i++) {
        InkStroke *s = self->ink.strokes + i;
        if (s->active && s->device == r->device && s->contact == r->contact) return s;
    }
    return NULL;
}

static void
end_stroke(Screen *self, InkStroke *s) {
    s->active = false;
    self->ink.num_strokes--;
}

// The point a report adds to a stroke: where the tool is in the cell area, in
// pixels, and the width that the pressure on it gives. A tool that does not
// measure pressure draws at the width of full pressure.
static InkPoint
point_of(const PointerReport *r, double w0, double w1, double exponent) {
    const double f = r->valid & 1 ? r->p / 65535. : 1.;
    const double curved = pow(f, exponent);
    const double span = w1 - w0;
    const double grown = span * curved;
    return (InkPoint){.x = r->x / 256., .y = r->y / 256., .width = w0 + grown};
}

static InkView
view_of(const Screen *self) {
    return (InkView){.cell = self->cell_size, .scrolled_by = self->scrolled_by, .scroll_offset = pixel_scroll_enabled(self) ? self->pixel_scroll_offset_y : 0};
}

static void
start_stroke(Screen *self, PointerReport *r) {
    InkBrush *b = &self->ink.brush;
    if (!self->ink.on || !b->remaining || r->tool < 0 || r->tool > 8 || !(b->tools & (1u << r->tool))) return;
    InkStroke *s = NULL;
    for (size_t i = 0; i < self->ink.capacity && !s; i++) {
        if (!self->ink.strokes[i].active) s = self->ink.strokes + i;
    }
    if (!s) {
        const size_t capacity = self->ink.capacity;
        const size_t max = SIZE_MAX / sizeof(InkStroke);
        if (capacity == max) {
            log_error("Cannot grow ink stroke storage: size overflow");
            return;
        }
        const size_t nc = capacity > max / 2 ? max : (capacity ? capacity * 2 : 8);
        InkStroke *strokes = realloc(self->ink.strokes, nc * sizeof(InkStroke));
        if (!strokes) {
            log_error("Cannot grow ink stroke storage: out of memory");
            return;
        }
        memset(strokes + capacity, 0, (nc - capacity) * sizeof(InkStroke));
        self->ink.strokes = strokes;
        self->ink.capacity = nc;
        s = strokes + capacity;
    }
    const InkView view = view_of(self);
    InkStrokeTarget target;
    if (!grman_ink_start(self->grman, &view, b->image, b->group, b->next_id, b->fill, point_of(r, b->w0, b->w1, b->exponent), &target)) return;
    *s = (InkStroke){
        .active = true, .device = r->device, .contact = r->contact, .grman = self->grman, .target = target, .element = b->next_id, .group = b->group,
        .w0 = b->w0, .w1 = b->w1, .exponent = b->exponent,
    };
    self->ink.num_strokes++;
    r->ink = b->next_id;
    b->next_id++;
    b->remaining--;
    self->is_dirty = true;
}

// Called with every report of a tool that is in a stroke or beginning one,
// before the report is sent to the program
void
screen_ink_report(Screen *self, PointerReport *r) {
    InkStroke *s = stroke_of(self, r);
    switch (r->phase) {
        case POINTER_DOWN:
            if (s) end_stroke(self, s);  // a down for a contact that is still down replaces its stroke
            start_stroke(self, r);
            break;
        case POINTER_MOVE:
        case POINTER_UP: {
            if (!s) break;
            const InkView view = view_of(self);
            // the stroke stays with the image it began on, which is no longer shown when the other screen is
            const bool drawn = s->grman == self->grman && grman_ink_append(s->grman, &view, s->target, s->group, s->element, point_of(r, s->w0, s->w1, s->exponent));
            if (drawn) self->is_dirty = true;
            if (!drawn || r->phase == POINTER_UP) end_stroke(self, s);
        } break;
        case POINTER_CANCEL:
            if (!s) break;
            // a stroke that is cancelled never happened
            grman_ink_remove(s->grman, s->target, s->element);
            end_stroke(self, s);
            self->is_dirty = true;
            break;
        default: break;
    }
}
