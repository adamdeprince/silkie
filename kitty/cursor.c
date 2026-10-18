/*
 * cursor.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "line.h"
#include "fonts.h"

#include <structmember.h>

static PyObject *
new_cursor_object(PyTypeObject *type, PyObject UNUSED *args, PyObject UNUSED *kwds) {
    Cursor *self;

    self = (Cursor *)type->tp_alloc(type, 0);
    return (PyObject *)self;
}

static void
dealloc(Cursor *self) {
    Py_TYPE(self)->tp_free((PyObject *)self);
}

#define EQ(x) (a->x == b->x)
static int
__eq__(Cursor *a, Cursor *b) {
    return memcmp(&a->sgr, &b->sgr, sizeof(a->sgr)) == 0 && EQ(x) && EQ(y) && EQ(shape) && EQ(non_blinking);
}
#undef EQ

static const char *cursor_names[NUM_OF_CURSOR_SHAPES] = {"NO_SHAPE", "BLOCK", "BEAM", "UNDERLINE", "HOLLOW"};

#define BOOL(x) ((x) ? Py_True : Py_False)
static PyObject *
repr(Cursor *self) {
    return PyUnicode_FromFormat(
        "Cursor(x=%u, y=%u, shape=%s, blink=%R, fg=#%08x, bg=#%08x, bold=%R, italic=%R, reverse=%R, strikethrough=%R, dim=%R, decoration=%d, "
        "decoration_fg=#%08x, text_blink=%R)",
        self->x,
        self->y,
        (self->shape < NUM_OF_CURSOR_SHAPES ? cursor_names[self->shape] : "INVALID"),
        BOOL(!self->non_blinking),
        self->sgr.fg,
        self->sgr.bg,
        BOOL(self->sgr.bold),
        BOOL(self->sgr.italic),
        BOOL(self->sgr.reverse),
        BOOL(self->sgr.strikethrough),
        BOOL(self->sgr.dim),
        self->sgr.decoration,
        self->sgr.decoration_fg,
        BOOL(self->sgr.blink));
}

void
cursor_reset_display_attrs(Cursor *self) {
    zero_at_ptr(&self->sgr);
}


static uint8_t
hdr_code(int percent) {
    // quarter steps above SDR white, see HDR_MASK
    int code = (percent - 100 + 12) / 25;
    return MAX(0, MIN(code, (int)HDR_MASK));
}

static int
bg_alpha_code(int percent) {
    // 2% steps, zero being opaque, see BG_ALPHA_BITS
    if (percent >= 100) return 0;
    return MAX(1, MIN((100 - MAX(0, percent) + 1) / 2, 50));
}

static void
parse_color(int *params, unsigned int *i, unsigned int count, bool is_group, uint32_t *result, uint8_t *gamut) {
    unsigned int attr;
    uint8_t r, g, b, cs = 0;
    if (*i < count) {
        attr = params[(*i)++];
        switch (attr) {
            case 5:
                if (*i < count) {
                    *result = (params[(*i)++] & 0xFF) << 8 | 1;
                    if (gamut) *gamut = 0;
                }
                break;
            case 2:
                if (*i + 2 < count) {
                    /* The first parameter in a four parameter RGB sequence is the color space id, */
                    /* see https://github.com/kovidgoyal/kitty/issues/227. It selects the gamut in */
                    /* the colon form, see docs/wide-gamut-protocol.rst, and is ignored otherwise. */
                    if (*i + 3 < count) {
                        int id = params[(*i)++];
                        if (is_group && (id == GAMUT_DISPLAY_P3 || id == GAMUT_REC2020)) cs = id;
                    }
                    r = params[(*i)++] & 0xFF;
                    g = params[(*i)++] & 0xFF;
                    b = params[(*i)++] & 0xFF;
                    *result = r << 24 | g << 16 | b << 8 | 2;
                    if (gamut) *gamut = cs;
                }
                break;
        }
    }
}


void
cursor_from_sgr(Cursor *self, int *params, unsigned int count, bool is_group) {
#define SET_COLOR(which, gamut)                                          \
    { parse_color(params, &i, count, is_group, &self->sgr.which, gamut); } \
    break;
#define PALETTE(which, val)      \
    self->sgr.which = (val);     \
    self->sgr.gamut_##which = 0; \
    break;
    START_ALLOW_CASE_RANGE
    unsigned int i = 0, attr;
    if (!count) {
        params[0] = 0;
        count = 1;
    }
    while (i < count) {
        attr = params[i++];
        switch (attr) {
            case 0: cursor_reset_display_attrs(self); break;
            case 1: self->sgr.bold = true; break;
            case 2: self->sgr.dim = true; break;
            case 3: self->sgr.italic = true; break;
            case 4:
                if (is_group && i < count) {
                    self->sgr.decoration = MIN(5, params[i]);
                    i++;
                } else self->sgr.decoration = 1;
                break;
            case 5: self->sgr.blink = true; break;
            case 7: self->sgr.reverse = true; break;
            case HDR_FG_CODE:
            case HDR_BG_CODE: {
                uint8_t val = 0;
                if (is_group && i < count) {
                    val = hdr_code(params[i]);
                    i++;
                }
                if (attr == HDR_FG_CODE) self->sgr.hdr_fg = val;
                else self->sgr.hdr_bg = val;
            } break;
            case 9: self->sgr.strikethrough = true; break;
            case 10 ... 19: {
                PyObject *module = text_style_module();
                RAII_PyObject(result, module ? PyObject_CallMethod(module, "select_slot", "II", self->sgr.text_style, attr - 10) : NULL);
                if (result) {
                    unsigned long style = PyLong_AsUnsignedLong(result);
                    if (!PyErr_Occurred() && style <= UINT32_MAX) self->sgr.text_style = (uint32_t)style;
                }
                if (PyErr_Occurred()) PyErr_Print();
            } break;
            case 21: self->sgr.decoration = 2; break;
            case 221: self->sgr.bold = false; break;
            case 222: self->sgr.dim = false; break;
            case 22:
                self->sgr.bold = false;
                self->sgr.dim = false;
                break;
            case 23: self->sgr.italic = false; break;
            case 24: self->sgr.decoration = 0; break;
            case 25: self->sgr.blink = false; break;
            case 27: self->sgr.reverse = false; break;
            case 29: self->sgr.strikethrough = false; break;
            case 30 ... 37: PALETTE(fg, ((attr - 30) << 8) | 1);
            case 38: SET_COLOR(fg, &self->sgr.gamut_fg);
            case 39: PALETTE(fg, 0);
            case 40 ... 47: PALETTE(bg, (self->sgr.bg & BG_ALPHA_BITS) | ((attr - 40) << 8) | 1);
            case 48: {
                const color_type alpha = self->sgr.bg & BG_ALPHA_BITS;
                parse_color(params, &i, count, is_group, &self->sgr.bg, &self->sgr.gamut_bg);
                self->sgr.bg |= alpha;
            } break;
            case 49: PALETTE(bg, self->sgr.bg & BG_ALPHA_BITS);
            case 90 ... 97: PALETTE(fg, ((attr - 90 + 8) << 8) | 1);
            case 100 ... 107: PALETTE(bg, (self->sgr.bg & BG_ALPHA_BITS) | ((attr - 100 + 8) << 8) | 1);
            case BG_ALPHA_CODE: {
                uint8_t val = 0;
                if (is_group && i < count) {
                    val = bg_alpha_code(params[i]);
                    i++;
                }
                self->sgr.bg = (self->sgr.bg & ~BG_ALPHA_BITS) | (val << BG_ALPHA_SHIFT);
            } break;
            case DECORATION_FG_CODE: SET_COLOR(decoration_fg, NULL);
            case DECORATION_FG_CODE + 1: self->sgr.decoration_fg = 0; break;
        }
        if (is_group) break;
    }
#undef SET_COLOR
#undef PALETTE
    END_ALLOW_CASE_RANGE
}

void
apply_sgr_to_cells(GPUCell *first_cell, unsigned int cell_count, int *params, unsigned int count, bool is_group) {
#define RANGE for (unsigned c = 0; c < cell_count; c++, cell++)
#define SET_COLOR(which, set_gamut)                               \
    {                                                             \
        color_type color = 0;                                     \
        uint8_t gamut = 0;                                        \
        parse_color(params, &i, count, is_group, &color, &gamut); \
        if (color) {                                              \
            RANGE {                                               \
                cell->which = color;                              \
                set_gamut;                                        \
            }                                                     \
        }                                                         \
    }                                                             \
    break;
#define SIMPLE(which, val)         \
    RANGE { cell->which = (val); } \
    break;
#define PALETTE(which, val)            \
    RANGE {                            \
        cell->which = (val);           \
        cell->attrs.gamut_##which = 0; \
    }                                  \
    break;
#define S(which, val)                    \
    RANGE { cell->attrs.which = (val); } \
    break;
#define SET_BG(val)                                        \
    RANGE {                                                \
        cell->bg = (cell->bg & BG_ALPHA_BITS) | (val);     \
        cell->attrs.gamut_bg = 0;                          \
    }                                                      \
    break;

    unsigned int i = 0, attr;
    if (!count) {
        params[0] = 0;
        count = 1;
    }
    while (i < count) {
        GPUCell *cell = first_cell;
        attr = params[i++];
        switch (attr) {
            case 0: {
                const CellAttrs remove_sgr_mask = {.val = ~SGR_MASK};
                RANGE {
                    cell->attrs.val &= remove_sgr_mask.val;
                    cell->fg = 0;
                    cell->bg = 0;
                    cell->decoration_fg = 0;
                    cell->text_style = 0;
                }
            } break;
            case 1: S(bold, true);
            case 2: S(dim, true);
            case 3: S(italic, true);
            case 4: {
                uint8_t val = 1;
                if (is_group && i < count) {
                    val = MIN(5, params[i]);
                    i++;
                }
                S(decoration, val);
            }
            case 5: S(blink, true);
            case 7: S(reverse, true);
            case HDR_FG_CODE:
            case HDR_BG_CODE: {
                uint8_t val = 0;
                if (is_group && i < count) {
                    val = hdr_code(params[i]);
                    i++;
                }
                if (attr == HDR_FG_CODE) { S(hdr_fg, val); }
                S(hdr_bg, val);
            }
            case 9: S(strike, true);
            case 21: S(decoration, 2);
            case 221: S(bold, false);
            case 222: S(dim, false);
            case 22:
                RANGE {
                    cell->attrs.bold = false;
                    cell->attrs.dim = false;
                }
                break;
            case 23: S(italic, false);
            case 24: S(decoration, 0);
            case 25: S(blink, false);
            case 27: S(reverse, false);
            case 29: S(strike, false); START_ALLOW_CASE_RANGE
            case 30 ... 37: PALETTE(fg, ((attr - 30) << 8) | 1);
            case 38: SET_COLOR(fg, cell->attrs.gamut_fg = gamut);
            case 39: PALETTE(fg, 0);
            case 40 ... 47: SET_BG(((attr - 40) << 8) | 1);
            case 48: {
                color_type color = 0;
                uint8_t gamut = 0;
                parse_color(params, &i, count, is_group, &color, &gamut);
                if (color) {
                    RANGE {
                        cell->bg = (cell->bg & BG_ALPHA_BITS) | color;
                        cell->attrs.gamut_bg = gamut;
                    }
                }
            } break;
            case 49: SET_BG(0);
            case 90 ... 97: PALETTE(fg, ((attr - 90 + 8) << 8) | 1);
            case 100 ... 107: SET_BG(((attr - 100 + 8) << 8) | 1); END_ALLOW_CASE_RANGE
            case BG_ALPHA_CODE: {
                uint8_t val = 0;
                if (is_group && i < count) {
                    val = bg_alpha_code(params[i]);
                    i++;
                }
                RANGE { cell->bg = (cell->bg & ~BG_ALPHA_BITS) | (val << BG_ALPHA_SHIFT); }
            } break;
            case DECORATION_FG_CODE: SET_COLOR(decoration_fg, (void)gamut);
            case DECORATION_FG_CODE + 1: SIMPLE(decoration_fg, 0);
        }
        if (is_group) break;
    }
#undef SET_COLOR
#undef RANGE
#undef SIMPLE
#undef PALETTE
#undef S
#undef SET_BG
}

const char *
cursor_as_sgr(const Cursor *self) {
    GPUCell blank_cell = {0}, cursor_cell = {
                                  .attrs = cursor_to_attrs(self),
                                  .fg = self->sgr.fg & COL_MASK,
                                  .bg = self->sgr.bg & COL_MASK,
                                  .decoration_fg = self->sgr.decoration_fg & COL_MASK,
                              };
    return cell_as_sgr(&cursor_cell, &blank_cell);
}

static PyObject *
reset_display_attrs(Cursor *self, PyObject *a UNUSED) {
#define reset_display_attrs_doc "Reset all display attributes to unset"
    cursor_reset_display_attrs(self);
    Py_RETURN_NONE;
}

void
cursor_reset(Cursor *self) {
    cursor_reset_display_attrs(self);
    self->x = 0;
    self->y = 0;
    self->shape = NO_CURSOR_SHAPE;
    self->non_blinking = false;
}

void
cursor_copy_to(Cursor *src, Cursor *dest) {
#define CCY(x) dest->x = src->x;
    CCY(x);
    CCY(y);
    CCY(shape);
    CCY(non_blinking);
#undef CCY
    memcpy(&dest->sgr, &src->sgr, sizeof(dest->sgr));
}

static PyObject *copy(Cursor *self, PyObject *);
#define copy_doc "Create a clone of this cursor"

// Boilerplate {{{

#define SGR_BOOL_GETSET(x)                                                    \
    static PyObject *x##_get(Cursor *self, void UNUSED *closure) {            \
        PyObject *ans = self->sgr.x ? Py_True : Py_False;                     \
        Py_INCREF(ans);                                                       \
        return ans;                                                           \
    }                                                                         \
    static int x##_set(Cursor *self, PyObject *value, void UNUSED *closure) { \
        if (value == NULL) {                                                  \
            PyErr_SetString(PyExc_TypeError, "Cannot delete attribute");      \
            return -1;                                                        \
        }                                                                     \
        self->sgr.x = PyObject_IsTrue(value) ? true : false;                  \
        return 0;                                                             \
    }

SGR_BOOL_GETSET(bold)
SGR_BOOL_GETSET(italic)
SGR_BOOL_GETSET(reverse)
SGR_BOOL_GETSET(strikethrough)
SGR_BOOL_GETSET(dim)
#undef SGR_BOOL_GETSET

static PyObject *
blink_get(Cursor *self, void UNUSED *closure) {
    PyObject *ans = !self->non_blinking ? Py_True : Py_False;
    Py_INCREF(ans);
    return ans;
}

static int
blink_set(Cursor *self, PyObject *value, void UNUSED *closure) {
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete attribute");
        return -1;
    }
    self->non_blinking = PyObject_IsTrue(value) ? false : true;
    return 0;
}

static PyObject *
text_blink_get(Cursor *self, void UNUSED *closure) {
    return Py_NewRef(self->sgr.blink ? Py_True : Py_False);
}

static int
text_blink_set(Cursor *self, PyObject *value, void UNUSED *closure) {
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete attribute");
        return -1;
    }
    int blink = PyObject_IsTrue(value);
    if (blink < 0) return -1;
    self->sgr.blink = blink;
    return 0;
}


#define SGR_UINT_GETSET(x)                                                                                                       \
    static PyObject *x##_get(Cursor *self, void UNUSED *closure) { return PyLong_FromUnsignedLong((unsigned long)self->sgr.x); } \
    static int x##_set(Cursor *self, PyObject *value, void UNUSED *closure) {                                                    \
        if (value == NULL) {                                                                                                     \
            PyErr_SetString(PyExc_TypeError, "Cannot delete attribute");                                                         \
            return -1;                                                                                                           \
        }                                                                                                                        \
        if (!PyLong_Check(value)) {                                                                                              \
            PyErr_SetString(PyExc_TypeError, "value must be an int");                                                            \
            return -1;                                                                                                           \
        }                                                                                                                        \
        self->sgr.x = PyLong_AsUnsignedLong(value);                                                                              \
        return 0;                                                                                                                \
    }

SGR_UINT_GETSET(decoration)
SGR_UINT_GETSET(fg)
SGR_UINT_GETSET(bg)
SGR_UINT_GETSET(decoration_fg)
SGR_UINT_GETSET(hdr_fg)
SGR_UINT_GETSET(hdr_bg)
SGR_UINT_GETSET(gamut_fg)
SGR_UINT_GETSET(gamut_bg)
SGR_UINT_GETSET(text_style)

static PyObject *
bg_alpha_get(Cursor *self, void UNUSED *closure) {
    return PyLong_FromLong(100 - 2 * (long)((self->sgr.bg & BG_ALPHA_BITS) >> BG_ALPHA_SHIFT));
}

static int
bg_alpha_set(Cursor *self, PyObject *value, void UNUSED *closure) {
    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete attribute");
        return -1;
    }
    if (!PyLong_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "value must be an int");
        return -1;
    }
    self->sgr.bg = (self->sgr.bg & ~BG_ALPHA_BITS) | (bg_alpha_code(PyLong_AsLong(value)) << BG_ALPHA_SHIFT);
    return 0;
}

static PyMemberDef members[] = {
    {"x", T_UINT, offsetof(Cursor, x), 0, "x"},
    {"y", T_UINT, offsetof(Cursor, y), 0, "y"},
    {"shape", T_INT, offsetof(Cursor, shape), 0, "shape"},
    {NULL} /* Sentinel */
};

static PyGetSetDef getseters[] = {
    GETSET(bold) GETSET(italic) GETSET(reverse) GETSET(strikethrough) GETSET(dim) GETSET(blink) GETSET(text_blink) GETSET(decoration) GETSET(fg) GETSET(bg)
        GETSET(decoration_fg) GETSET(hdr_fg) GETSET(hdr_bg) GETSET(gamut_fg) GETSET(gamut_bg) GETSET(bg_alpha) GETSET(text_style){NULL} /* Sentinel */
};

static PyMethodDef methods[] = {
    METHOD(copy, METH_NOARGS) METHOD(reset_display_attrs, METH_NOARGS){NULL} /* Sentinel */
};


static PyObject *richcmp(PyObject *obj1, PyObject *obj2, int op);

PyTypeObject Cursor_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "fast_data_types.Cursor",
    .tp_basicsize = sizeof(Cursor),
    .tp_dealloc = (destructor)dealloc,
    .tp_repr = (reprfunc)repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Cursors",
    .tp_richcompare = richcmp,
    .tp_methods = methods,
    .tp_members = members,
    .tp_getset = getseters,
    .tp_new = new_cursor_object,
};

RICHCMP(Cursor)

// }}}

Cursor *
cursor_copy(Cursor *self) {
    Cursor *ans;
    ans = alloc_cursor();
    if (ans == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    cursor_copy_to(self, ans);
    return ans;
}

static PyObject *
copy(Cursor *self, PyObject *a UNUSED) {
    return (PyObject *)cursor_copy(self);
}

Cursor *
alloc_cursor(void) {
    return (Cursor *)new_cursor_object(&Cursor_Type, NULL, NULL);
}

INIT_TYPE(Cursor)
