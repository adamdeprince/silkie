/*
 * osc777.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#include "screen.h"
#include "control-codes.h"
#include "extension-fields.h"
#include "fonts.h"

// Every OSC 777 selector this terminal speaks, with the version of its
// protocol, see docs/capabilities-protocol.rst. A selector handled in
// dispatch_osc() must also be listed here.
static const struct { const char *selector; unsigned version; } extensions[] = {
    {"depth", 1},
    {"ssh-agent", 1},
    {"a11y", 1}, {"a11y", 2}, {"caps", 1}, {"display", 1}, {"font", 1}, {"frame", 1}, {"gesture", 1}, {"haptic", 1}, {"ink", 1}, {"latency", 1}, {"scroll", 1}, {"speech", 1}, {"tablet-pointer", 1},
};

void
screen_depth_protocol(Screen *self, const char *data, size_t sz) {
    EXTENSION_FIELDS(f);
    const char *op;
    long long depth = 0;
    float pixels = 0, phase = 0;
    if (!extension_fields_parse(&f, data, sz) || !extension_frame_header(&f, 1, &op)) return;
    if (!extension_field_int(&f, "d", INT32_MIN, INT32_MAX, &depth)) return;
    if (extension_field(&f, "p") && extension_field(&f, "d")) return;
    const char *keys[] = {"p", "y"};
    float *values[] = {&pixels, &phase};
    for (unsigned i = 0; i < arraysz(keys); i++) {
        const char *value = extension_field(&f, keys[i]);
        if (!value) continue;
        if (!*value || strspn(value, "-0123456789.eE+") != strlen(value)) return;
        char *end;
        const double number = strtod(value, &end);
        if (*end || !isfinite(number) || !isfinite((float)number) || (number != 0. && (float)number == 0.f)) return;
        *values[i] = (float)number;
    }
    if (!strcmp(op, "reset")) {
        self->cursor->sgr.depth = 0;
        self->cursor->sgr.depth_pixels = self->cursor->sgr.phase = 0.f;
    }
    else if (!strcmp(op, "set")) {
        if (extension_field(&f, "d") || extension_field(&f, "p")) {
            self->cursor->sgr.depth = (int32_t)depth;
            self->cursor->sgr.depth_pixels = pixels;
        }
        if (extension_field(&f, "y")) self->cursor->sgr.phase = phase;
    }
}

void
screen_font_protocol(Screen *self, const char *data, size_t sz) {
    if (sz > PY_SSIZE_T_MAX) {
        log_error("Font request exceeds the Python input-size representation");
        return;
    }
    PyObject *module = text_style_module();
    if (!module) { PyErr_Print(); return; }
    RAII_PyObject(result, PyObject_CallMethod(module, "parse", "Iy#i", self->cursor->sgr.text_style, data, (Py_ssize_t)sz, self->cursor->sgr.italic));
    if (!result) { PyErr_Print(); return; }
    unsigned style;
    int italic;
    const char *reply;
    if (!PyArg_ParseTuple(result, "Iis", &style, &italic, &reply)) { PyErr_Print(); return; }
    self->cursor->sgr.text_style = style;
    if (italic >= 0) self->cursor->sgr.italic = italic != 0;
    if (*reply) write_escape_code_to_child(self, ESC_OSC, reply);
}
static int
hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch |= 32;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static PyObject *
annotation_text(const char *encoded, bool multiline) {
    if (!encoded || !*encoded) return PyUnicode_FromString("");
    const size_t size = strlen(encoded);
    if (size > PY_SSIZE_T_MAX) return NULL;
    RAII_ALLOC(char, decoded, malloc(size));
    if (!decoded) return PyErr_NoMemory();
    size_t used = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char ch = encoded[i];
        if (ch == '%') {
            if (size - i < 3) return NULL;
            const int hi = hex_value(encoded[i + 1]), lo = hex_value(encoded[i + 2]);
            if (hi < 0 || lo < 0) return NULL;
            ch = (unsigned char)((hi << 4) | lo);
            i += 2;
        } else if (ch < ' ' || ch > '~') return NULL;
        decoded[used++] = ch;
    }
    RAII_PyObject(ans, PyUnicode_DecodeUTF8(decoded, (Py_ssize_t)used, "strict"));
    if (!ans) return NULL;
    const int kind = PyUnicode_KIND(ans);
    const void *str = PyUnicode_DATA(ans);
    for (Py_ssize_t i = 0, n = PyUnicode_GET_LENGTH(ans); i < n; i++) {
        const Py_UCS4 ch = PyUnicode_READ(kind, str, i);
        if (ch < ' ' && !(multiline && (ch == '\n' || ch == '\t'))) return NULL;
        if (ch >= 0x7f && ch <= 0x9f) return NULL;
    }
    return Py_NewRef(ans);
}

static void
accessibility_reply(Screen *self, int version, long long id, const char *status) {
    if (id < 0) return;
    RAII_PyObject(response, PyUnicode_FromFormat("777;a11y;v=%d:op=ack:id=%lld:status=%s", version, id, status));
    if (response) {
        const char *reply = PyUnicode_AsUTF8(response);
        if (reply) write_escape_code_to_child(self, ESC_OSC, reply);
    }
    if (PyErr_Occurred()) PyErr_Clear();
}

static void
accessible_text_request(Screen *self, const ExtensionFields *f, const char *op, long long id) {
    RAII_PyObject(response, NULL);
    if (!strcmp(op, "announce")) {
        const char *priority = extension_field(f, "priority");
        if (priority && strcmp(priority, "polite") && strcmp(priority, "assertive")) return;
        RAII_PyObject(text, annotation_text(extension_field(f, "text"), false));
        if (text && PyUnicode_GET_LENGTH(text) && self->callbacks) {
            response = PyObject_CallMethod(self->callbacks, "accessibility_announce", "OO", text,
                priority && !strcmp(priority, "assertive") ? Py_True : Py_False);
        }
    } else {
        long long key, row, column, end_row, end_column;
        if (!extension_int(extension_field(f, "range"), 1, UINT32_MAX, &key)) return;
        if (!strcmp(op, "clear-text")) {
            if (self->callbacks) response = PyObject_CallMethod(self->callbacks, "accessibility_clear_text", "K", (unsigned long long)key);
        } else if (!strcmp(op, "set-text")) {
            const char *role = extension_field(f, "role");
            if (!role || (strcmp(role, "prompt") && strcmp(role, "status") && strcmp(role, "completion") && strcmp(role, "decorative"))) return;
            if (!extension_int(extension_field(f, "row"), 1, INT32_MAX, &row) ||
                !extension_int(extension_field(f, "column"), 1, INT32_MAX, &column) ||
                !extension_int(extension_field(f, "end_row"), 1, INT32_MAX, &end_row) ||
                !extension_int(extension_field(f, "end_column"), 1, INT32_MAX, &end_column)) return;
            RAII_PyObject(label, annotation_text(extension_field(f, "label"), false));
            if (label && self->callbacks) {
                response = PyObject_CallMethod(self->callbacks, "accessibility_set_text", "K(LL)(LL)sO",
                    (unsigned long long)key, row - 1, column - 1, end_row - 1, end_column - 1, role, label);
            }
        }
    }
    if (response && PyUnicode_Check(response)) {
        const char *status = PyUnicode_AsUTF8(response);
        if (status) accessibility_reply(self, 2, id, status);
    }
    if (PyErr_ExceptionMatches(PyExc_MemoryError)) accessibility_reply(self, 2, id, "ENOMEM");
    if (PyErr_Occurred()) PyErr_Clear();
}

void
screen_accessibility_protocol(Screen *self, const char *data, size_t size) {
    EXTENSION_FIELDS(f);
    const char *op;
    long long image, placement, id = -1;
    if (memchr(data, 0, size) || !extension_fields_parse(&f, data, size) || !extension_field_int(&f, "id", 0, INT32_MAX, &id)) return;
    const int version = extension_frame_header(&f, 1, &op) ? 1 : extension_frame_header(&f, 2, &op) ? 2 : 0;
    if (!version) return;
    if (strcmp(op, "set-image") && strcmp(op, "clear-image")) {
        if (version == 2) accessible_text_request(self, &f, op, id);
        return;
    }
    if (!extension_int(extension_field(&f, "i"), 1, UINT32_MAX, &image) ||
        !extension_int(extension_field(&f, "p"), 1, UINT32_MAX, &placement)) return;

    RAII_PyObject(annotation, NULL);
    if (!strcmp(op, "set-image")) {
        const char *purpose = extension_field(&f, "purpose");
        if (purpose && strcmp(purpose, "essential") && strcmp(purpose, "informative") && strcmp(purpose, "decorative")) return;
        RAII_PyObject(name, annotation_text(extension_field(&f, "name"), false));
        RAII_PyObject(description, name ? annotation_text(extension_field(&f, "description"), true) : NULL);
        RAII_PyObject(details, description ? annotation_text(extension_field(&f, "details"), true) : NULL);
        if (details) annotation = Py_BuildValue("(sOOO)", purpose ? purpose : "", name, description, details);
        if (!annotation) {
            const bool no_memory = PyErr_ExceptionMatches(PyExc_MemoryError);
            PyErr_Clear();
            if (no_memory) accessibility_reply(self, version, id, "ENOMEM");
            return;
        }
    }

    const bool found = grman_set_image_annotation(self->grman, (uint32_t)image, (uint32_t)placement, annotation);
    accessibility_reply(self, version, id, found ? "OK" : "ENOENT");
}

void
screen_speech_protocol(Screen *self, const char *data, size_t size) {
    EXTENSION_FIELDS(f);
    const char *op;
    long long id = -1;
    if (memchr(data, 0, size) || !extension_fields_parse(&f, data, size) ||
        !extension_frame_header(&f, 1, &op) || !extension_field_int(&f, "id", 0, INT32_MAX, &id) || !self->callbacks) return;
    // Only the fields defined by the speech protocol are forwarded to the backend.
    static const char *keys[] = {"text", "style", "voice", "language", "rate", "pitch", "pitch_range",
                                 "volume", "punctuation", "split_caps", "name", "duration", "queued", "interrupt"};
    RAII_PyObject(fields, PyDict_New());
    if (!fields) { PyErr_Clear(); return; }
    for (size_t i = 0; i < arraysz(keys); i++) {
        const char *value = extension_field(&f, keys[i]);
        if (!value) continue;
        RAII_PyObject(decoded, annotation_text(value, i == 0));
        if (!decoded || PyDict_SetItemString(fields, keys[i], decoded) < 0) {
            PyErr_Clear();
            return;
        }
    }
    RAII_PyObject(answer, PyObject_CallMethod(self->callbacks, "speech_command", "sO", op, fields));
    if (id >= 0 && answer && PyUnicode_Check(answer)) {
        const char *reply = PyUnicode_AsUTF8(answer);
        if (reply) {
            const bool capability = strncmp(reply, "op=cap:", 7) == 0;
            RAII_PyObject(response, PyUnicode_FromFormat("777;speech;v=1:op=%s:id=%lld:%s",
                capability ? "cap" : "ack", id, reply + (capability ? 7 : 0)));
            if (response) {
                const char *text = PyUnicode_AsUTF8(response);
                if (text) write_escape_code_to_child(self, ESC_OSC, text);
            }
        }
    }
    if (PyErr_Occurred()) PyErr_Clear();
}

// The graphics protocol transmission media (the t key) and pixel formats (the
// f key) this terminal accepts. A branch that adds one adds it here too.
static const char *const media[] = {"d", "f", "t", "s",
#ifdef __APPLE__
    "g",
#endif
};
// The extensions of SGR this terminal understands: the gamut id of
// docs/wide-gamut-protocol.rst, the brightness of docs/hdr-protocol.rst, the
// cell background alpha of docs/cell-alpha-protocol.rst and the 16 bit colors
// of docs/deep-color-protocol.rst. A branch that adds one adds it here too.
static const char *const sgr_extensions[] = {"alpha", "deep", "gamut", "hdr"};
static const char *const formats[] = {"8", "16", "24", "32", "64", "100", "200", "201", "301", "302", "304", "308", "316", "401", "402", "404", "408", "516", "632", "664"};

static bool
append_list(PyObject **reply, const char *key, const char *const *items, size_t count) {
    PyUnicode_AppendAndDel(reply, PyUnicode_FromFormat(":%s=", key));
    if (!*reply) return false;
    for (size_t i = 0; i < count; i++) {
        PyUnicode_AppendAndDel(reply, PyUnicode_FromFormat("%s%s", i ? "," : "", items[i]));
        if (!*reply) return false;
    }
    return true;
}

// Turns off every extension mode of the window, as if each selector had been
// sent mode=0, with the cancels that sends. It runs on the caps reset request
// and at every shell prompt, so it must cost nothing when the modes are off.
// An extension that has a mode adds its lines here.
void
screen_reset_extension_modes(Screen *self) {
    self->cursor->sgr.depth = 0;
    self->cursor->sgr.depth_pixels = 0;
    self->cursor->sgr.phase = 0;
    // tablet-pointer
    screen_pointer_cancel(self, 0);
    self->pointer.mode = 0;
    // gesture
    screen_gesture_cancel(self);
    self->gesture.mode = 0;
    // scroll
    self->scroll_reports = 0;
    // frame
    self->frame_feedback.on = false;
    self->frame_feedback.pending = false;
    self->frame_feedback.seq = 0;
    // latency
    self->low_latency = false;
    // display
    self->display_events = false;
    // ink
    screen_ink_off(self);
}

// A frame from the program, see docs/extension-framing.rst for what is rejected
void
screen_caps_query(Screen *self, const char *data, size_t sz) {
    EXTENSION_FIELDS(f);
    const char *op;
    long long id = -1;
    if (!extension_fields_parse(&f, data, sz) || !extension_frame_header(&f, 1, &op)) return;
    if (!extension_field_int(&f, "id", 0, INT32_MAX, &id)) return;
    if (!strcmp(op, "reset")) {
        screen_reset_extension_modes(self);
        return;
    }
    if (strcmp(op, "query") || id < 0) return;
    // caps: the listing 1, the reset request 2
    RAII_PyObject(reply, PyUnicode_FromFormat("777;caps;v=1:op=cap:id=%lld:caps=3:ext=", id));
    for (size_t i = 0; reply && i < arraysz(extensions); i++)
        PyUnicode_AppendAndDel(&reply, PyUnicode_FromFormat("%s%s.%u", i ? "," : "", extensions[i].selector, extensions[i].version));
    if (reply && append_list(&reply, "sgr", sgr_extensions, arraysz(sgr_extensions)) && append_list(&reply, "media", media, arraysz(media)) && append_list(&reply, "format", formats, arraysz(formats))) {
        const char *data = PyUnicode_AsUTF8(reply);
        if (data) write_escape_code_to_child(self, ESC_OSC, data);
    }
    if (PyErr_Occurred()) PyErr_Print();
}
