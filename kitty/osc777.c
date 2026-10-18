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
    {"caps", 1}, {"display", 1}, {"font", 1}, {"frame", 1}, {"gesture", 1}, {"haptic", 1}, {"ink", 1}, {"latency", 1}, {"scroll", 1}, {"tablet-pointer", 1},
};

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
// The graphics protocol transmission media (the t key) and pixel formats (the
// f key) this terminal accepts. A branch that adds one adds it here too.
static const char *const media[] = {"d", "f", "t", "s",
#ifdef __APPLE__
    "g",
#endif
};
static const char *const formats[] = {"24", "32", "64", "100", "200"};

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
    if (reply && append_list(&reply, "media", media, arraysz(media)) && append_list(&reply, "format", formats, arraysz(formats))) {
        const char *data = PyUnicode_AsUTF8(reply);
        if (data) write_escape_code_to_child(self, ESC_OSC, data);
    }
    if (PyErr_Occurred()) PyErr_Print();
}
