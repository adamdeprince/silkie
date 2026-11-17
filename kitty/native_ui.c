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
static const char *native_ui_module_path = NULL;
static bool native_ui_on_wayland = false, native_ui_share_wayland_display = false;
#endif

#ifndef __APPLE__
#include "glfw-wrapper.h"

static unsigned long long host_os_window_id(struct OSWindow *w) { return w->id; }
static struct OSWindow *host_os_window_for_id(unsigned long long id) { return os_window_for_id(id); }
static unsigned long host_x11_window(struct OSWindow *w) { return w->handle && !global_state.is_wayland ? glfwGetX11Window(w->handle) : 0; }
// A window can be gone by the time the toolkit gets around to something about it, hence the NULL checks
static void host_get_size(struct OSWindow *w, int *width, int *height) { if (w && w->handle) glfwGetWindowSize(w->handle, width, height); }
static void host_set_size(struct OSWindow *w, int width, int height) { if (w && w->handle) glfwSetWindowSize(w->handle, width, height); }
static const char *host_title(struct OSWindow *w) { return w && w->window_title ? PyUnicode_AsUTF8(w->window_title) : ""; }

static void
host_request_close(struct OSWindow *w) {
    if (!w) return;
    w->close_request = CONFIRMABLE_CLOSE_REQUESTED;
    global_state.has_pending_closes = true;
    wakeup_main_loop();
}

static void
host_run_action(const char *action) {
    if (!global_state.boss) return;
    PyObject *ret = PyObject_CallMethod(global_state.boss, "combine", "s", action);
    if (ret) Py_DECREF(ret);
    else PyErr_Print();
}

static void
host_refresh(struct OSWindow *w) {
    if (!w) return;
    if (!w->redraw_count) w->redraw_count++;
    wakeup_main_loop();
}

static void host_wayland_embed(struct OSWindow *w, void *parent) { if (w && w->handle) glfwWaylandEmbedSetParent(w->handle, parent); }
static void host_wayland_set_position(struct OSWindow *w, int x, int y) { if (w && w->handle) glfwWaylandEmbedSetPosition(w->handle, x, y); }
static void host_wayland_set_mapped(struct OSWindow *w, bool mapped) { if (w && w->handle) glfwWaylandEmbedSetMapped(w->handle, mapped); }
static void host_wayland_activate(struct OSWindow *w) { if (w && w->handle) glfwWaylandEmbedActivate(w->handle); }

static unsigned long long host_add_fd_watch(int fd, int events, void (*cb)(int, int, void *), void *data) { return glfwAddFDWatch(fd, events, cb, data); }

static const NativeUIBackend *
load_linux_backend(void) {
    // The module links against GTK. Without GTK it cannot be loaded, which is not an error: the
    // terminal then draws its own tab bar.
    if (!native_ui_module_path) return NULL;
    void *handle = dlopen(native_ui_module_path, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        if (global_state.debug_rendering) log_error("The native user interface is not available: %s", err ? err : "unknown error");
        return NULL;
    }
    NativeUIBackendCreator create = NULL;
    *(void **)(&create) = dlsym(handle, "native_ui_gtk_backend");
    if (!create) return NULL;
    static NativeUIHost host;
    host = (NativeUIHost){
        .is_wayland = native_ui_on_wayland,
        .os_window_id = host_os_window_id,
        .os_window_for_id = host_os_window_for_id,
        .x11_window = host_x11_window,
        .get_size = host_get_size,
        .set_size = host_set_size,
        .title = host_title,
        .request_close = host_request_close,
        .run_action = host_run_action,
        .wakeup = wakeup_main_loop,
        .add_fd_watch = host_add_fd_watch,
        .remove_fd_watch = glfwRemoveFDWatch,
        .set_event_loop_hook = glfwSetEventLoopHook,
        .refresh = host_refresh,
        .wayland_embed = host_wayland_embed,
        .wayland_set_position = host_wayland_set_position,
        .wayland_set_mapped = host_wayland_set_mapped,
        .wayland_activate = host_wayland_activate,
    };
    const NativeUIBackend *ans = create(&host);
    // GTK has installed its own process wide X error handler, which would hide
    // the X errors GLFW traps, see glfwX11ReinstallErrorHandler(). Only the X11
    // build of GLFW has the function.
    if (ans && !native_ui_on_wayland && glfwX11ReinstallErrorHandler_impl) glfwX11ReinstallErrorHandler();
    return ans;
}
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

void *
native_ui_wayland_display(void) {
#ifndef __APPLE__
    if (!native_ui_share_wayland_display) return NULL;
    const NativeUIBackend *b = native_ui_backend();
    return b && b->wayland_display ? b->wayland_display() : NULL;
#else
    return NULL;
#endif
}

bool
native_ui_is_loaded(void) {
    return backend != NULL;
}

void
native_ui_adopt_os_window(OSWindow *w, id_type parent, const char *location) {
    const NativeUIBackend *b = native_ui_backend();
    if (!b || !b->adopt_os_window) return;
    OSWindow *p = parent ? os_window_for_id(parent) : NULL;
    w->hosted_in_native_window = b->adopt_os_window(w, p, location ? location : "last");
}

bool
native_ui_window_op(OSWindow *w, enum NativeWindowOp op) {
    if (!w->hosted_in_native_window || !backend || !backend->window_op) return false;
    backend->window_op(w, op);
    return true;
}

bool
native_ui_window_query(OSWindow *w, enum NativeWindowQuery q, bool *result) {
    if (!w->hosted_in_native_window || !backend || !backend->window_query) return false;
    *result = backend->window_query(w, q);
    return true;
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

static PyObject *
native_tabs_supported(PyObject *self UNUSED, PyObject *args UNUSED) {
    const NativeUIBackend *b = native_ui_backend();
    return Py_NewRef(b && (!b->tabs_supported || b->tabs_supported()) ? Py_True : Py_False);
}

#ifndef __APPLE__
static PyObject *
set_native_ui_module_path(PyObject *self UNUSED, PyObject *args) {
    const char *path;
    int on_wayland, want_tabs;
    if (!PyArg_ParseTuple(args, "spp", &path, &on_wayland, &want_tabs)) return NULL;
    free((void *)native_ui_module_path);
    native_ui_module_path = strdup(path);
    if (!native_ui_module_path) return PyErr_NoMemory();
    native_ui_on_wayland = on_wayland;
    native_ui_share_wayland_display = on_wayland && want_tabs;
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
    // Start with room for every OS window and grow if the toolkit reports more members, for instance pages
    // whose OS window is being closed, rather than dropping the members that do not fit
    size_t cap = global_state.num_os_windows + 1, n;
    RAII_ALLOC(id_type, ids, NULL);
    while (true) {
        if (cap > SIZE_MAX / sizeof(id_type)) return PyErr_NoMemory();
        id_type *q = realloc(ids, cap * sizeof(id_type));
        if (!q) return PyErr_NoMemory();
        ids = q;
        n = b->tab_members(w, ids, cap);
        if (n <= cap) break;
        cap = n;
    }
    PyObject *ans = PyTuple_New(n);
    if (!ans) return NULL;
    for (size_t i = 0; i < n; i++) {
        PyObject *id = PyLong_FromUnsignedLongLong(ids[i]);
        if (!id) { Py_DECREF(ans); return NULL; }
        PyTuple_SET_ITEM(ans, i, id);
    }
    return ans;
}

static PyObject *
native_tab_status(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    PyObject *status;
    if (!PyArg_ParseTuple(args, "KO", &id, &status)) return NULL;
    if (status != Py_None && !PyDict_Check(status)) {
        PyErr_SetString(PyExc_TypeError, "status must be a dict or None");
        return NULL;
    }
    const NativeUIBackend *b = native_ui_backend();
    // The badge is decoration: nothing to do without a native tab bar or once the OS window is gone
    OSWindow *w = os_window_for_id(id);
    if (!w || !b || !b->tab_status) Py_RETURN_FALSE;
    b->tab_status(w, status);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_TRUE;
}

static PyObject *
native_find_bar(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    PyObject *model, *callback;
    if (!PyArg_ParseTuple(args, "KOO", &id, &model, &callback)) return NULL;
    if (model != Py_None && !PyDict_Check(model)) {
        PyErr_SetString(PyExc_TypeError, "model must be a dict or None");
        return NULL;
    }
    const NativeUIBackend *b = native_ui_backend();
    OSWindow *w = os_window_for_id(id);
    if (!w || !b || !b->find_bar) Py_RETURN_FALSE;
    const bool ans = b->find_bar(w, model, callback);
    if (PyErr_Occurred()) return NULL;
    return Py_NewRef(ans ? Py_True : Py_False);
}

static PyObject *
native_tab_settings_button(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    int show;
    if (!PyArg_ParseTuple(args, "Kp", &id, &show)) return NULL;
    const NativeUIBackend *b = native_ui_backend();
    OSWindow *w = os_window_for_id(id);
    if (!w || !b || !b->tab_settings_button) Py_RETURN_FALSE;
    b->tab_settings_button(w, show);
    Py_RETURN_TRUE;
}

static PyObject *
native_tab_settings_show(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    PyObject *model, *callback;
    if (!PyArg_ParseTuple(args, "KO!O", &id, &PyDict_Type, &model, &callback)) return NULL;
    B(show_tab_settings);
    OSWindow *w = window_arg(id);
    if (!w) return NULL;
    b->show_tab_settings(w, model, callback);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}

static PyObject *
native_tab_snapshot(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    const char *path;
    if (!PyArg_ParseTuple(args, "Ks", &id, &path)) return NULL;
    B(snapshot_tab);
    OSWindow *w = window_arg(id);
    if (!w) return NULL;
    return Py_NewRef(b->snapshot_tab(w, path) ? Py_True : Py_False);
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

static PyObject *
native_saved_height_adjustment(PyObject *self UNUSED, PyObject *pyid) {
    OSWindow *w = os_window_for_id(PyLong_AsUnsignedLongLong(pyid));
    if (PyErr_Occurred()) return NULL;
    return PyLong_FromLong(w && w->hosted_in_native_window && backend && backend->saved_height_adjustment ? backend->saved_height_adjustment(w) : 0);
}

static PyMethodDef methods[] = {
    {"native_ui_available", native_ui_available, METH_NOARGS, ""},
    {"native_tabs_supported", native_tabs_supported, METH_NOARGS, ""},
#ifndef __APPLE__
    {"set_native_ui_module_path", set_native_ui_module_path, METH_VARARGS, ""},
#endif
    {"native_saved_height_adjustment", native_saved_height_adjustment, METH_O, ""},
    {"native_tabs_attach", native_tabs_attach, METH_VARARGS, ""},
    {"native_tabs_select", native_tabs_select, METH_O, ""},
    {"native_tabs_detach", native_tabs_detach, METH_O, ""},
    {"native_tabs_move", native_tabs_move, METH_VARARGS, ""},
    {"native_tabs_members", native_tabs_members, METH_O, ""},
    {"native_tab_status", native_tab_status, METH_VARARGS, ""},
    {"native_find_bar", native_find_bar, METH_VARARGS, ""},
    {"native_tab_snapshot", native_tab_snapshot, METH_VARARGS, ""},
    {"native_tab_settings_button", native_tab_settings_button, METH_VARARGS, ""},
    {"native_tab_settings_show", native_tab_settings_show, METH_VARARGS, ""},
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
