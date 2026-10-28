/*
 * extension-fields.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// The fields of a frame a program sends to one of the OSC 777 extensions, see
// docs/extension-framing.rst. The extensions share the rules, so this header is
// the same on the branch of every extension
typedef struct ExtensionFields {
    char *buf;
    unsigned count, cap;
    const char **keys, **values;
} ExtensionFields;

static inline void
extension_fields_free(ExtensionFields *f) {
    free(f->buf);
    free(f->keys);
    free(f->values);
    memset(f, 0, sizeof(*f));
}

static inline void
extension_fields_cleanup(ExtensionFields *f) {
    extension_fields_free(f);
}

// Zero-initialized, and freed on every return of the function that declares it.
#define EXTENSION_FIELDS(name) ExtensionFields name __attribute__((cleanup(extension_fields_cleanup))) = {0}

// Cuts a frame into its fields. False when the frame is to be rejected as a
// whole: one of them is not key=value with a key, a key is given twice, or
// storage for the frame cannot be allocated. f must be zeroed; a previous
// frame in f is freed. There is no fixed size.
static inline bool
extension_fields_parse(ExtensionFields *f, const char *data, size_t sz) {
    extension_fields_free(f);
    if (sz == SIZE_MAX) return false;
    f->buf = malloc(sz + 1);
    if (!f->buf) return false;
    memcpy(f->buf, data, sz);
    f->buf[sz] = 0;
    f->cap = 8;
    f->keys = malloc(f->cap * sizeof(*f->keys));
    f->values = malloc(f->cap * sizeof(*f->values));
    if (!f->keys || !f->values) {
        extension_fields_free(f);
        return false;
    }
    for (char *p = f->buf;;) {
        char *end = strchr(p, ':'), *eq;
        if (end) *end = 0;
        if (!(eq = strchr(p, '=')) || eq == p) {
            extension_fields_free(f);
            return false;
        }
        *eq = 0;
        for (unsigned i = 0; i < f->count; i++) {
            if (!strcmp(f->keys[i], p)) {
                extension_fields_free(f);
                return false;
            }
        }
        if (f->count == f->cap) {
            unsigned cap = f->cap * 2u;
            const char **keys = realloc(f->keys, cap * sizeof(*keys));
            const char **values = realloc(f->values, cap * sizeof(*values));
            if (!keys || !values) {
                if (keys) f->keys = keys;
                if (values) f->values = values;
                extension_fields_free(f);
                return false;
            }
            f->keys = keys;
            f->values = values;
            f->cap = cap;
        }
        f->keys[f->count] = p;
        f->values[f->count++] = eq + 1;
        if (!end) break;
        p = end + 1;
    }
    return true;
}

// The value of a field, NULL when the frame does not have it
static inline const char *
extension_field(const ExtensionFields *f, const char *key) {
    for (unsigned i = 0; i < f->count; i++) {
        if (!strcmp(f->keys[i], key)) return f->values[i];
    }
    return NULL;
}

// A decimal integer from min to max: digits with an optional leading minus,
// no plus, nothing before or after them
static inline bool
extension_int(const char *val, long long min, long long max, long long *ans) {
    if (!val) return false;
    const bool negative = *val == '-';
    const char *p = val + negative;
    if (*p < '0' || *p > '9') return false;
    long long v = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        const int digit = *p - '0';
        if (v > (LLONG_MAX - digit) / 10) return false;
        v = v * 10 + digit;
    }
    if (*p) return false;
    if (negative) v = -v;
    if (v < min || v > max) return false;
    *ans = v;
    return true;
}

// A field that holds an integer from min to max. True when the frame has it
// and it is one, or when the frame does not have it, which leaves *ans as it
// was. False, when the frame is to be rejected, when the value is not one
static inline bool
extension_field_int(const ExtensionFields *f, const char *key, long long min, long long max, long long *ans) {
    const char *val = extension_field(f, key);
    return !val || extension_int(val, min, max, ans);
}

// The version and the op of a frame, which every frame has. False when the
// frame is to be rejected: either is missing, or the version is not the one
// this terminal speaks
static inline bool
extension_frame_header(const ExtensionFields *f, long long version, const char **op) {
    long long v;
    if (!extension_int(extension_field(f, "v"), 0, 2147483647ll, &v) || v != version) return false;
    return (*op = extension_field(f, "op")) != NULL;
}
