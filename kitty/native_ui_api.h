/*
 * native_ui_api.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include <Python.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the native user interface of a platform provides. Native tabs are OS windows that the
// platform shows as tabs of one window. macOS: kitty/native_ui_cocoa.m. Linux: kitty/gtk_ui.c, a
// module loaded on demand so that GTK is not a dependency of the terminal.
struct OSWindow;

enum NativeWindowOp { NATIVE_OP_TOGGLE_FULLSCREEN, NATIVE_OP_TOGGLE_MAXIMIZED, NATIVE_OP_MAXIMIZE, NATIVE_OP_MINIMIZE, NATIVE_OP_RESTORE, NATIVE_OP_FOCUS };
enum NativeWindowQuery { NATIVE_QUERY_IS_FULLSCREEN, NATIVE_QUERY_IS_MAXIMIZED };

typedef struct NativeUIBackend {
    // True when the platform can show OS windows as native tabs, otherwise only the Settings window and
    // menus are available, and the terminal draws its own tab bar
    bool (*tabs_supported)(void);
    // Called for every new OS window, before it is first shown. parent is NULL unless the window
    // is a new tab of the group of parent. location is first, before, after or last. Returns true if the
    // window is now shown inside a native window, so that its window state is the state of that window.
    bool (*adopt_os_window)(struct OSWindow *w, struct OSWindow *parent, const char *location);
    // Called before the OS window is destroyed
    void (*os_window_closing)(struct OSWindow *w);
    // Move an existing OS window to the tab group of target
    void (*attach_tab)(struct OSWindow *w, struct OSWindow *target, const char *location);
    void (*select_tab)(struct OSWindow *w);
    void (*move_tab)(struct OSWindow *w, int delta);
    void (*detach_tab)(struct OSWindow *w);
    // Fill ids, of capacity cap, with the members of the tab group of w in display order. Returns the number
    // of members, which can exceed cap in which case the caller retries with more space.
    size_t (*tab_members)(struct OSWindow *w, unsigned long long *ids, size_t cap);
    void (*title_changed)(struct OSWindow *w, const char *title);
    // The program status badge of the native tab of w: a dict made by update_native_tab_status() in
    // kitty/native_ui.py, None to remove the badge. Called on the main thread with the GIL held.
    void (*tab_status)(struct OSWindow *w, PyObject *status);
    // Show or hide the gear on the native tab of w that runs the open_tab_settings action for it
    void (*tab_settings_button)(struct OSWindow *w, bool show);
    // Show the settings of the native tab of w in a popover, anchored to its gear when that is visible. model is
    // made by tab_settings_model() in kitty/tab_settings.py. callback(verb, name, value) returns an error message.
    void (*show_tab_settings)(struct OSWindow *w, PyObject *model, PyObject *callback);
    void (*window_op)(struct OSWindow *w, enum NativeWindowOp op);
    bool (*window_query)(struct OSWindow *w, enum NativeWindowQuery q);
    int (*saved_height_adjustment)(struct OSWindow *w);
    // Wayland: the connection to the compositor of the toolkit, for the terminal to share. Called before the terminal connects.
    void *(*wayland_display)(void);
    void (*show_settings)(PyObject *model, PyObject *callback);
    void (*update_settings)(PyObject *model);
    void (*update_menu)(PyObject *model);
    // For tests: draw the Settings window, as it is now, to a PNG file. Works without any screen capture permission.
    bool (*snapshot_settings)(const char *path);
    // For tests: draw the native window holding w, with its tab bar, to a PNG file.
    bool (*snapshot_tab)(struct OSWindow *w, const char *path);
} NativeUIBackend;

// What a module loaded at runtime can use. It cannot link against the symbols of fast_data_types.
typedef struct NativeUIHost {
    bool is_wayland;
    unsigned long long (*os_window_id)(struct OSWindow *w);
    struct OSWindow *(*os_window_for_id)(unsigned long long id);
    unsigned long (*x11_window)(struct OSWindow *w);
    void (*get_size)(struct OSWindow *w, int *width, int *height);
    void (*set_size)(struct OSWindow *w, int width, int height);
    const char *(*title)(struct OSWindow *w);
    void (*request_close)(struct OSWindow *w);
    void (*run_action)(const char *action);
    void (*wakeup)(void);
    unsigned long long (*add_fd_watch)(int fd, int events, void (*callback)(int, int, void *), void *data);
    void (*remove_fd_watch)(unsigned long long id);
    void (*set_event_loop_hook)(int64_t (*hook)(int64_t timeout));
    void (*refresh)(struct OSWindow *w);
    void (*wayland_embed)(struct OSWindow *w, void *parent_surface);
    void (*wayland_set_position)(struct OSWindow *w, int x, int y);
    void (*wayland_set_mapped)(struct OSWindow *w, bool mapped);
    void (*wayland_activate)(struct OSWindow *w);
} NativeUIHost;

typedef const NativeUIBackend *(*NativeUIBackendCreator)(const NativeUIHost *);
