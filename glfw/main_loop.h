/*
 * Copyright (C) 2019 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "internal.h"
#include "../kitty/monotonic.h"

#ifndef GLFW_LOOP_BACKEND
#define GLFW_LOOP_BACKEND x11
#endif

static bool keep_going = false;


void
_glfwPlatformStopMainLoop(void) {
    if (keep_going) {
        keep_going = false;
        _glfwPlatformPostEmptyEvent();
    }
}

void
_glfwPlatformRunMainLoop(GLFWtickcallback tick_callback, void *data) {
    keep_going = 1;
    EventLoopData *eld = &_glfw.GLFW_LOOP_BACKEND.eventLoopData;
    while (keep_going) {
        _glfwPlatformWaitEvents();
        EVDBG("--------- loop tick, wakeups_happened: %d ----------", eld->wakeup_data_read);
        if (eld->wakeup_data_read) {
            eld->wakeup_data_read = false;
            tick_callback(data);
        }
    }
    EVDBG("main loop exiting");
}

unsigned long long
_glfwPlatformAddTimer(monotonic_t interval, bool repeats, GLFWuserdatafreefun callback, void *callback_data, GLFWuserdatafreefun free_callback) {
    return addTimer(&_glfw.GLFW_LOOP_BACKEND.eventLoopData, "user timer", interval, 1, repeats, callback, callback_data, free_callback);
}

unsigned long long
_glfwPlatformAddFDWatch(int fd, int events, void (*callback)(int, int, void *), void *callback_data) {
    return addWatch(&_glfw.GLFW_LOOP_BACKEND.eventLoopData, "external fd", fd, events, 1, callback, callback_data);
}

void
_glfwPlatformRemoveFDWatch(unsigned long long watch_id) {
    removeWatch(&_glfw.GLFW_LOOP_BACKEND.eventLoopData, watch_id);
}

void
_glfwPlatformRemoveTimer(unsigned long long timer_id) {
    removeTimer(&_glfw.GLFW_LOOP_BACKEND.eventLoopData, timer_id);
}

void
_glfwPlatformUpdateTimer(unsigned long long timer_id, monotonic_t interval, monotonic_t tolerance, bool enabled) {
    updateTimer(&_glfw.GLFW_LOOP_BACKEND.eventLoopData, timer_id, interval, tolerance, enabled);
}
