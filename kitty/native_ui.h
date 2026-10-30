/*
 * native_ui.h
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "state.h"

#include "native_ui_api.h"

const NativeUIBackend *native_ui_backend(void);
void native_ui_adopt_os_window(OSWindow *w, id_type parent, const char *location);
bool native_ui_window_op(OSWindow *w, enum NativeWindowOp op);
bool native_ui_window_query(OSWindow *w, enum NativeWindowQuery q, bool *result);
void native_ui_os_window_closing(OSWindow *w);
void *native_ui_wayland_display(void);
void native_ui_title_changed(OSWindow *w, const char *title);
#ifdef __APPLE__
const NativeUIBackend *native_ui_cocoa_backend(void);
void native_ui_cocoa_add_menus(void *menu_bar);
#endif
