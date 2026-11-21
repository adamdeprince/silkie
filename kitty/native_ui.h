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
void native_ui_cocoa_accessibility_update(OSWindow *w, PyObject *panes);
void native_ui_cocoa_accessibility_announce(OSWindow *w, const char *text, bool assertive);
bool native_ui_cocoa_accessibility_active(void);
PyObject *native_ui_cocoa_preferred_languages(void);
bool native_ui_cocoa_speech_control(id_type pane, const char *op);
bool native_ui_cocoa_speak(id_type pane, const char *text, const char *voice, const char *language, int rate, int pitch, int volume);
double native_ui_cocoa_speech_sound(id_type pane, const char *name);
#endif
