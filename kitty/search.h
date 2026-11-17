/*
 * search.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "data-types.h"
#include "monotonic.h"

// Find: searching the text of a screen and its scrollback, see kitty/search.c

typedef struct ScreenSearch ScreenSearch;

// The bits of the per cell selection byte that the cell shaders read, see background.slang
#define SEARCH_MATCH_MASK 0x20u
#define SEARCH_CURRENT_MASK 0x40u

void screen_search_free(ScreenSearch *s);
// Called when the size of the screen changed, the matches are found again
void screen_search_invalidate(ScreenSearch *s);
// Do some of the pending search work of every screen, for at most budget. Returns the time until work is next due:
// 0 to come back as soon as possible, -1 when nothing is pending.
monotonic_t screen_search_tick_all(monotonic_t now, monotonic_t budget);
// screen_search_apply_highlights() and screen_search_render_dirty() are declared in screen.h

// The methods of the Screen type that drive a search, see search.c
#define SCREEN_SEARCH_METHODS                                                                                    \
    {"search_start", (PyCFunction)(void (*)(void))screen_search_start_py, METH_VARARGS | METH_KEYWORDS, NULL},    \
        {"search_stop", (PyCFunction)screen_search_stop_py, METH_NOARGS, NULL},                                   \
        {"search_tick", (PyCFunction)screen_search_tick_py, METH_VARARGS, NULL},                                  \
        {"search_run_to_completion", (PyCFunction)screen_search_run_to_completion_py, METH_NOARGS, NULL},         \
        {"search_navigate", (PyCFunction)screen_search_navigate_py, METH_VARARGS, NULL},                          \
        {"search_status", (PyCFunction)screen_search_status_py, METH_NOARGS, NULL},                               \
        {"search_matches", (PyCFunction)screen_search_matches_py, METH_NOARGS, NULL},                             \
        {"search_current", (PyCFunction)screen_search_current_py, METH_NOARGS, NULL},                             \
        {"search_highlights", (PyCFunction)screen_search_highlights_py, METH_NOARGS, NULL},

PyObject *screen_search_start_py(PyObject *self, PyObject *args, PyObject *kw);
PyObject *screen_search_stop_py(PyObject *self, PyObject *args);
PyObject *screen_search_tick_py(PyObject *self, PyObject *args);
PyObject *screen_search_run_to_completion_py(PyObject *self, PyObject *args);
PyObject *screen_search_navigate_py(PyObject *self, PyObject *args);
PyObject *screen_search_status_py(PyObject *self, PyObject *args);
PyObject *screen_search_matches_py(PyObject *self, PyObject *args);
PyObject *screen_search_current_py(PyObject *self, PyObject *args);
PyObject *screen_search_highlights_py(PyObject *self, PyObject *args);
