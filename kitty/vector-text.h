/* Vector text's platform independent, in-memory OpenType engine.
 * Included by fonts.c. HarfBuzz owns/sanitizes tables; nothing is installed. */
#include <hb-ot.h>
#include "vector.h"

static PyObject *
vector_validate_color(PyObject *self UNUSED, PyObject *arg) {
    Py_ssize_t len;
    const char *s = PyUnicode_AsUTF8AndSize(arg, &len);
    if (!s) return NULL;
    if (!(len == 4 && !memcmp(s, "none", 4)) && !vector_color_paints(s, (size_t)len)) return PyErr_Format(PyExc_ValueError, "Invalid vector color");
    Py_RETURN_NONE;
}

static void
vector_font_destroy(PyObject *capsule) {
    hb_font_destroy(PyCapsule_GetPointer(capsule, "silkie.vector.font"));
}

static void
vector_blob_release(void *data) { Py_DECREF((PyObject *)data); }

static hb_blob_t *
vector_font_table(hb_face_t *face UNUSED, hb_tag_t tag, void *data) {
    return hb_face_reference_table(hb_font_get_face(harfbuzz_font_for_face((PyObject *)data)), tag);
}

static PyObject *
vector_font(PyObject *self UNUSED, PyObject *args) {
    PyObject *source;
    unsigned index = 0;
    if (!PyArg_ParseTuple(args, "O|I", &source, &index)) return NULL;
    hb_face_t *face = NULL;
    PyObject *native = NULL;
    if (PyBytes_Check(source)) {
        const Py_ssize_t size = PyBytes_GET_SIZE(source);
        if ((size_t)size > UINT_MAX) return PyErr_Format(PyExc_OverflowError, "Font exceeds OpenType address space");
        Py_INCREF(source);
        hb_blob_t *blob = hb_blob_create(PyBytes_AS_STRING(source), (unsigned)size, HB_MEMORY_MODE_READONLY, source, vector_blob_release);
        if (index >= hb_face_count(blob)) {
            hb_blob_destroy(blob);
            return PyErr_Format(PyExc_ValueError, "Invalid font or collection index");
        }
        face = hb_face_create(blob, index);
        hb_blob_destroy(blob);
    } else {
        native = face_from_descriptor(source, NULL);
        if (!native) return NULL;
        // The FreeType backend's HB face borrows FT_Face. Keep the Python
        // backend alive through an independent table-provider face; attaching
        // it to its own HB face instead would create a reference cycle.
        face = hb_face_create_for_tables(vector_font_table, Py_NewRef(native), vector_blob_release);
    }
    hb_font_t *font = hb_font_create(face);
    hb_face_destroy(face);
    hb_ot_font_set_funcs(font);
    if (native) {
        unsigned n = 0;
        const float *coords = hb_font_get_var_coords_design(harfbuzz_font_for_face(native), &n);
        if (n) hb_font_set_var_coords_design(font, coords, n);
        Py_DECREF(native);
    }
    if (!hb_face_get_glyph_count(hb_font_get_face(font))) {
        hb_font_destroy(font);
        return PyErr_Format(PyExc_ValueError, "Font has no glyphs");
    }
    PyObject *ans = PyCapsule_New(font, "silkie.vector.font", vector_font_destroy);
    if (!ans) hb_font_destroy(font);
    return ans;
}

static PyObject *
vector_font_info(PyObject *self UNUSED, PyObject *arg) {
    hb_font_t *font = PyCapsule_GetPointer(arg, "silkie.vector.font");
    if (!font) return NULL;
    hb_face_t *face = hb_font_get_face(font);
    unsigned n = hb_ot_var_get_axis_count(face);
    hb_ot_var_axis_info_t *axes = n ? calloc(n, sizeof(*axes)) : NULL;
    if (n && !axes) return PyErr_NoMemory();
    hb_ot_var_get_axis_infos(face, 0, &n, axes);
    PyObject *axis_list = PyList_New(0), *ans = NULL;
    if (!axis_list) goto done;
    for (unsigned i = 0; i < n; i++) {
        char tag[5] = {0}; // OpenType tags are exactly four bytes
        hb_tag_to_string(axes[i].tag, tag);
        PyObject *a = Py_BuildValue("{ss sf sf sf}", "tag", tag, "minimum", axes[i].min_value, "default", axes[i].default_value, "maximum", axes[i].max_value);
        if (!a) goto done;
        const int ok = PyList_Append(axis_list, a);
        Py_DECREF(a);
        if (ok < 0) goto done;
    }
    unsigned len = 0, weight = 400;
    double slant = 0;
    hb_blob_t *table = hb_face_reference_table(face, HB_TAG('O','S','/','2'));
    const unsigned char *bytes = (const unsigned char *)hb_blob_get_data(table, &len);
    if (len >= 6) weight = (unsigned)bytes[4] * 256 + bytes[5];
    hb_blob_destroy(table);
    table = hb_face_reference_table(face, HB_TAG('p','o','s','t'));
    bytes = (const unsigned char *)hb_blob_get_data(table, &len);
    if (len >= 8) {
        uint32_t v = (uint32_t)bytes[4]<<24 | (uint32_t)bytes[5]<<16 | (uint32_t)bytes[6]<<8 | bytes[7];
        slant = (int32_t)v / 65536.0;
    }
    hb_blob_destroy(table);
    ans = Py_BuildValue("{sI sI sO sI sd}", "upem", hb_face_get_upem(face), "glyphs", hb_face_get_glyph_count(face), "axes", axis_list, "weight", weight, "slant", slant);
done:
    free(axes);
    Py_XDECREF(axis_list);
    return ans;
}

static PyObject *
vector_font_covers(PyObject *self UNUSED, PyObject *args) {
    PyObject *capsule, *text;
    if (!PyArg_ParseTuple(args, "OU", &capsule, &text)) return NULL;
    hb_font_t *font = PyCapsule_GetPointer(capsule, "silkie.vector.font");
    if (!font) return NULL;
    for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(text); i++) {
        const Py_UCS4 cp = PyUnicode_ReadChar(text, i);
        hb_codepoint_t glyph;
        // Default ignorables are handled by shaping, not font fallback.
        if (cp == 0x200c || cp == 0x200d || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef)) continue;
        if (!hb_font_get_nominal_glyph(font, cp, &glyph)) Py_RETURN_FALSE;
    }
    Py_RETURN_TRUE;
}

typedef struct {
    PyObject *commands;
    double scale, x, y, shear;
    bool failed;
} VectorGlyph;

static void
vector_draw_command(VectorGlyph *g, int op, const float *points, unsigned n) {
    if (g->failed) return;
    PyObject *cmd = PyTuple_New(1 + n * 2);
    if (!cmd) { g->failed = true; return; }
    PyTuple_SET_ITEM(cmd, 0, PyLong_FromLong(op));
    for (unsigned i = 0; i < n; i++) {
        PyTuple_SET_ITEM(cmd, 1 + 2 * i, PyFloat_FromDouble(g->x + (points[2 * i] + g->shear * points[2 * i + 1]) * g->scale));
        PyTuple_SET_ITEM(cmd, 2 + 2 * i, PyFloat_FromDouble(g->y - points[2 * i + 1] * g->scale));
    }
    if (PyErr_Occurred() || PyList_Append(g->commands, cmd) < 0) g->failed = true;
    Py_DECREF(cmd);
}

static void vector_move(hb_draw_funcs_t *f UNUSED, void *data, hb_draw_state_t *s UNUSED, float x, float y, void *u UNUSED) {
    vector_draw_command(data, 0, (float[]){x, y}, 1);
}
static void vector_line(hb_draw_funcs_t *f UNUSED, void *data, hb_draw_state_t *s UNUSED, float x, float y, void *u UNUSED) {
    vector_draw_command(data, 1, (float[]){x, y}, 1);
}
static void vector_quad(hb_draw_funcs_t *f UNUSED, void *data, hb_draw_state_t *s UNUSED, float x, float y, float x1, float y1, void *u UNUSED) {
    vector_draw_command(data, 2, (float[]){x, y, x1, y1}, 2);
}
static void vector_cubic(hb_draw_funcs_t *f UNUSED, void *data, hb_draw_state_t *s UNUSED, float x, float y, float x1, float y1, float x2, float y2, void *u UNUSED) {
    vector_draw_command(data, 3, (float[]){x, y, x1, y1, x2, y2}, 3);
}
static void vector_close(hb_draw_funcs_t *f UNUSED, void *data, hb_draw_state_t *s UNUSED, void *u UNUSED) {
    vector_draw_command(data, 4, NULL, 0);
}

static PyObject *
vector_shape(PyObject *self UNUSED, PyObject *args) {
    PyObject *capsule, *text, *axes, *features;
    double size, shear;
    const char *direction, *language, *script;
    if (!PyArg_ParseTuple(args, "OUdOOsssd", &capsule, &text, &size, &axes, &features, &direction, &language, &script, &shear)) return NULL;
    if (!PyDict_Check(axes) || !PyList_Check(features) || !isfinite(size) || size <= 0 || !isfinite(shear)) return PyErr_Format(PyExc_ValueError, "Invalid vector shaping parameters");
    hb_font_t *base = PyCapsule_GetPointer(capsule, "silkie.vector.font");
    if (!base) return NULL;
    hb_font_t *font = hb_font_create(hb_font_get_face(base));
    hb_ot_font_set_funcs(font);
    unsigned coord_count = 0;
    const float *base_coords = hb_font_get_var_coords_design(base, &coord_count);
    if (coord_count) hb_font_set_var_coords_design(font, base_coords, coord_count);
    hb_font_set_scale(font, (int)hb_face_get_upem(hb_font_get_face(base)), (int)hb_face_get_upem(hb_font_get_face(base)));
    hb_buffer_t *buf = hb_buffer_create();
    hb_draw_funcs_t *draw = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(draw, vector_move, NULL, NULL);
    hb_draw_funcs_set_line_to_func(draw, vector_line, NULL, NULL);
    hb_draw_funcs_set_quadratic_to_func(draw, vector_quad, NULL, NULL);
    hb_draw_funcs_set_cubic_to_func(draw, vector_cubic, NULL, NULL);
    hb_draw_funcs_set_close_path_func(draw, vector_close, NULL, NULL);
    PyObject *ans = NULL, *commands = PyList_New(0), *missing = PyList_New(0);
    hb_feature_t *ff = NULL;
    if (!commands || !missing) goto done;
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    while (PyDict_Next(axes, &pos, &key, &value)) {
        const char *tag = PyUnicode_AsUTF8(key);
        double v = PyFloat_AsDouble(value);
        if (!tag || PyErr_Occurred()) goto done;
        hb_font_set_variation(font, hb_tag_from_string(tag, -1), (float)v);
    }
    Py_ssize_t nfeatures = PySequence_Size(features);
    if (nfeatures < 0) goto done;
    if ((size_t)nfeatures > UINT_MAX || (size_t)nfeatures > SIZE_MAX / sizeof(*ff)) { PyErr_SetString(PyExc_OverflowError, "Too many shaping features"); goto done; }
    if (nfeatures && !(ff = calloc((size_t)nfeatures, sizeof(*ff)))) { PyErr_NoMemory(); goto done; }
    for (Py_ssize_t i = 0; i < nfeatures; i++) {
        PyObject *f = PySequence_GetItem(features, i);
        const char *str = f ? PyUnicode_AsUTF8(f) : NULL;
        bool ok = str && hb_feature_from_string(str, -1, ff + i);
        Py_XDECREF(f);
        if (!ok) { if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "Invalid OpenType feature"); goto done; }
    }
    // HB's buffer indices and lengths are unsigned/int respectively.
    if (PyUnicode_GET_LENGTH(text) > INT_MAX) { PyErr_SetString(PyExc_OverflowError, "Text exceeds HarfBuzz buffer address space"); goto done; }
    for (Py_ssize_t i = 0; i < PyUnicode_GET_LENGTH(text); i++) hb_buffer_add(buf, PyUnicode_ReadChar(text, i), (unsigned)i);
    hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    if (*direction) hb_buffer_set_direction(buf, hb_direction_from_string(direction, -1));
    if (*language) hb_buffer_set_language(buf, hb_language_from_string(language, -1));
    if (*script) hb_buffer_set_script(buf, hb_script_from_string(script, -1));
    hb_buffer_guess_segment_properties(buf);
    hb_shape(font, buf, ff, (unsigned)nfeatures);
    if (!hb_buffer_allocation_successful(buf)) { PyErr_NoMemory(); goto done; }
    unsigned n = 0;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *p = hb_buffer_get_glyph_positions(buf, &n);
    const double scale = size / hb_face_get_upem(hb_font_get_face(font));
    VectorGlyph glyph = {.commands = commands, .scale = scale, .shear = shear};
    double x = 0, y = 0;
    for (unsigned i = 0; i < n && !glyph.failed; i++) {
        glyph.x = x + p[i].x_offset * scale;
        glyph.y = y - p[i].y_offset * scale;
        if (!info[i].codepoint) {
            PyObject *idx = PyLong_FromUnsignedLong(info[i].cluster);
            if (!idx || PyList_Append(missing, idx) < 0) glyph.failed = true;
            Py_XDECREF(idx);
        }
        const Py_ssize_t before = PyList_GET_SIZE(commands);
        hb_font_draw_glyph(font, info[i].codepoint, draw, &glyph);
        if (PyList_GET_SIZE(commands) == before) {
            hb_glyph_extents_t extents = {0};
            if (hb_font_get_glyph_extents(font, info[i].codepoint, &extents) && (extents.width || extents.height)) {
                PyErr_SetString(PyExc_ValueError, "Glyph has no supported vector outline");
                glyph.failed = true;
            }
        }
        x += p[i].x_advance * scale;
        y -= p[i].y_advance * scale;
    }
    if (glyph.failed) goto done;
    hb_font_extents_t ext = {0};
    hb_font_get_h_extents(font, &ext);
    hb_position_t underline = 0, thickness = 0, strike = 0;
    hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &underline);
    hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &thickness);
    hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &strike);
    ans = Py_BuildValue("{sO s(dd) s(ddddddd) sO}", "path", commands, "advance", x, y, "metrics",
        ext.ascender * scale, -ext.descender * scale, ext.line_gap * scale, -underline * scale,
        thickness * scale, -strike * scale, size, "missing", missing);
done:
    free(ff);
    Py_XDECREF(commands);
    Py_XDECREF(missing);
    hb_draw_funcs_destroy(draw);
    hb_buffer_destroy(buf);
    hb_font_destroy(font);
    return ans;
}
