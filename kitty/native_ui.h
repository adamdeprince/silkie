/*
 * native_ui.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "state.h"

// What the native user interface of a platform provides. Native tabs are OS windows that the
// platform shows as tabs of one window. macOS: kitty/native_ui_cocoa.m. Linux: kitty/gtk_ui.c, a
// module loaded on demand so that GTK is not a dependency of the terminal.
typedef struct NativeUIBackend {
    // Called for every new OS window, before it is first shown. parent is NULL unless the window
    // is a new tab of the group of parent. location is first, before, after or last.
    void (*adopt_os_window)(OSWindow *w, OSWindow *parent, const char *location);
    // Called before the OS window is destroyed
    void (*os_window_closing)(OSWindow *w);
    // Move an existing OS window to the tab group of target
    void (*attach_tab)(OSWindow *w, OSWindow *target, const char *location);
    void (*select_tab)(OSWindow *w);
    void (*move_tab)(OSWindow *w, int delta);
    void (*detach_tab)(OSWindow *w);
    // Fill ids, of capacity cap, with the members of the tab group of w in display order. Returns the number
    // of members, which can exceed cap in which case the caller retries with more space.
    size_t (*tab_members)(OSWindow *w, id_type *ids, size_t cap);
    void (*title_changed)(OSWindow *w, const char *title);
    void (*show_settings)(PyObject *model, PyObject *callback);
    void (*update_settings)(PyObject *model);
    void (*update_menu)(PyObject *model);
    // For tests: draw the Settings window, as it is now, to a PNG file. Works without any screen capture permission.
    bool (*snapshot_settings)(const char *path);
} NativeUIBackend;

const NativeUIBackend *native_ui_backend(void);
void native_ui_adopt_os_window(OSWindow *w, id_type parent, const char *location);
void native_ui_os_window_closing(OSWindow *w);
void native_ui_title_changed(OSWindow *w, const char *title);
#ifdef __APPLE__
const NativeUIBackend *native_ui_cocoa_backend(void);
void native_ui_cocoa_add_menus(void *menu_bar);
#endif
