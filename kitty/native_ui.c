/*
 * native_ui.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#include "native_ui.h"
#ifndef __APPLE__
#include <dlfcn.h>
#endif

static const NativeUIBackend *backend = NULL;
static bool backend_loaded = false;

#ifndef __APPLE__
static const NativeUIBackend *
load_linux_backend(void) {
    // The module links against GTK. Without GTK it cannot be loaded, which is not an error: the
    // terminal then draws its own tab bar. Set by the Python code, see set_native_ui_module_path()
    extern const char *native_ui_module_path;
    if (!native_ui_module_path) return NULL;
    void *handle = dlopen(native_ui_module_path, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        if (global_state.debug_rendering) log_error("Native UI not available: %s", err ? err : "unknown error");
        return NULL;
    }
    const NativeUIBackend *(*create)(void) = (const NativeUIBackend *(*)(void))dlsym(handle, "native_ui_gtk_backend");
    return create ? create() : NULL;
}

const char *native_ui_module_path = NULL;
#endif

const NativeUIBackend *
native_ui_backend(void) {
    if (!backend_loaded) {
        backend_loaded = true;
#ifdef __APPLE__
        backend = native_ui_cocoa_backend();
#else
        backend = load_linux_backend();
#endif
    }
    return backend;
}

void
native_ui_adopt_os_window(OSWindow *w, id_type parent, const char *location) {
    const NativeUIBackend *b = native_ui_backend();
    if (!b || !b->adopt_os_window) return;
    OSWindow *p = parent ? os_window_for_id(parent) : NULL;
    b->adopt_os_window(w, p, location ? location : "last");
}

void
native_ui_os_window_closing(OSWindow *w) {
    if (backend && backend->os_window_closing) backend->os_window_closing(w);
}

void
native_ui_title_changed(OSWindow *w, const char *title) {
    if (backend && backend->title_changed) backend->title_changed(w, title);
}

#define B(name)                                                                  \
    const NativeUIBackend *b = native_ui_backend();                              \
    if (!b || !b->name) {                                                        \
        PyErr_SetString(PyExc_RuntimeError, "No native user interface available"); \
        return NULL;                                                             \
    }

static PyObject *
native_ui_available(PyObject *self UNUSED, PyObject *args UNUSED) {
    return Py_NewRef(native_ui_backend() ? Py_True : Py_False);
}

#ifndef __APPLE__
static PyObject *
set_native_ui_module_path(PyObject *self UNUSED, PyObject *path) {
    free((void *)native_ui_module_path);
    native_ui_module_path = strdup(PyUnicode_AsUTF8(path));
    if (!native_ui_module_path) return PyErr_NoMemory();
    backend_loaded = false;
    backend = NULL;
    Py_RETURN_NONE;
}
#endif

static OSWindow *
window_arg(unsigned long long id) {
    OSWindow *w = os_window_for_id(id);
    if (!w) PyErr_Format(PyExc_KeyError, "No OS window with id: %llu", id);
    return w;
}

static PyObject *
native_tabs_attach(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id, target;
    const char *location = "last";
    if (!PyArg_ParseTuple(args, "KK|s", &id, &target, &location)) return NULL;
    B(attach_tab);
    OSWindow *w = window_arg(id), *t = w ? window_arg(target) : NULL;
    if (!t) return NULL;
    b->attach_tab(w, t, location);
    Py_RETURN_NONE;
}

static PyObject *
native_tabs_select(PyObject *self UNUSED, PyObject *pyid) {
    B(select_tab);
    OSWindow *w = window_arg(PyLong_AsUnsignedLongLong(pyid));
    if (!w) return NULL;
    b->select_tab(w);
    Py_RETURN_NONE;
}

static PyObject *
native_tabs_detach(PyObject *self UNUSED, PyObject *pyid) {
    B(detach_tab);
    OSWindow *w = window_arg(PyLong_AsUnsignedLongLong(pyid));
    if (!w) return NULL;
    b->detach_tab(w);
    Py_RETURN_NONE;
}

static PyObject *
native_tabs_move(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    int delta;
    if (!PyArg_ParseTuple(args, "Ki", &id, &delta)) return NULL;
    B(move_tab);
    OSWindow *w = window_arg(id);
    if (!w) return NULL;
    b->move_tab(w, delta);
    Py_RETURN_NONE;
}

static PyObject *
native_tabs_members(PyObject *self UNUSED, PyObject *pyid) {
    B(tab_members);
    OSWindow *w = window_arg(PyLong_AsUnsignedLongLong(pyid));
    if (!w) return NULL;
    size_t cap = global_state.num_os_windows + 1;
    RAII_ALLOC(id_type, ids, malloc(cap * sizeof(id_type)));
    if (!ids) return PyErr_NoMemory();
    // A group cannot hold more than all OS windows, so a second attempt is not needed
    const size_t n = MIN(b->tab_members(w, ids, cap), cap);
    PyObject *ans = PyTuple_New(n);
    if (!ans) return NULL;
    for (size_t i = 0; i < n; i++) PyTuple_SET_ITEM(ans, i, PyLong_FromUnsignedLongLong(ids[i]));
    return ans;
}

static PyObject *
native_settings_show(PyObject *self UNUSED, PyObject *args) {
    PyObject *model, *callback;
    if (!PyArg_ParseTuple(args, "O!O", &PyDict_Type, &model, &callback)) return NULL;
    B(show_settings);
    b->show_settings(model, callback);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
native_settings_update(PyObject *self UNUSED, PyObject *model) {
    B(update_settings);
    b->update_settings(model);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
native_menu_update(PyObject *self UNUSED, PyObject *model) {
    const NativeUIBackend *b = native_ui_backend();
    if (b && b->update_menu) b->update_menu(model);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
native_settings_snapshot(PyObject *self UNUSED, PyObject *path) {
    B(snapshot_settings);
    return Py_NewRef(b->snapshot_settings(PyUnicode_AsUTF8(path)) ? Py_True : Py_False);
}

#undef B

static PyMethodDef methods[] = {
    {"native_ui_available", native_ui_available, METH_NOARGS, ""},
#ifndef __APPLE__
    {"set_native_ui_module_path", set_native_ui_module_path, METH_O, ""},
#endif
    {"native_tabs_attach", native_tabs_attach, METH_VARARGS, ""},
    {"native_tabs_select", native_tabs_select, METH_O, ""},
    {"native_tabs_detach", native_tabs_detach, METH_O, ""},
    {"native_tabs_move", native_tabs_move, METH_VARARGS, ""},
    {"native_tabs_members", native_tabs_members, METH_O, ""},
    {"native_settings_show", native_settings_show, METH_VARARGS, ""},
    {"native_settings_update", native_settings_update, METH_O, ""},
    {"native_menu_update", native_menu_update, METH_O, ""},
    {"native_settings_snapshot", native_settings_snapshot, METH_O, ""},
    {NULL, NULL, 0, NULL},
};

bool
init_native_ui(PyObject *module) {
    return PyModule_AddFunctions(module, methods) == 0;
}
