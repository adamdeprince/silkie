//========================================================================
// GLFW 3.4 Wayland - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2014 Jonas Ådahl <jadahl@gmail.com>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================
// It is fine to use C99 in this file because it will not be built with VS
//========================================================================

#define _GNU_SOURCE
#include "internal.h"
#include "backend_utils.h"
#include "wl_client_side_decorations.h"
#include "linux_desktop_settings.h"
#include "../kitty/monotonic.h"
#include "wl_text_input.h"
#include "wayland-text-input-unstable-v3-client-protocol.h"
#include "wayland-pointer-gestures-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <wayland-client.h>
#include <stdio.h>
// errno.h needed for BSD code paths
#include <errno.h>
// Needed for the BTN_* defines
#ifdef __has_include
#if __has_include(<linux/input.h>)
#include <linux/input.h>
#elif __has_include(<dev/evdev/input.h>)
#include <dev/evdev/input.h>
#endif
#else
#include <linux/input.h>
#endif

#define debug debug_rendering

#define x window->wl.allCursorPosX
#define y window->wl.allCursorPosY

static _GLFWwindow *
get_window_from_surface(struct wl_surface *surface) {
    if (!surface) return NULL;
    // Surfaces of a toolkit that shares the connection have user data that is none of our business
    for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) {
        if (w->wl.surface == surface) return w;
    }
    return NULL;
}

// Compositors differ on whether a tool in proximity also drives wl_pointer. Its
// own events are enough, so the pointer is ignored while a tool is over the window
static bool
toolOver(_GLFWwindow *window) {
    return _glfw.wl.tablet_tool && _glfw.wl.tablet_tool->window_id == window->id;
}

static void
pointerHandleEnter(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    _GLFWwindow *window = get_window_from_surface(surface);
    if (!window) return;
    _glfw.wl.serial = serial;
    _glfw.wl.input_serial = serial;
    _glfw.wl.pointer_serial = serial;
    _glfw.wl.pointer_enter_serial = serial;
    _glfw.wl.pointerFocus = window;
    window->wl.allCursorPosX = wl_fixed_to_double(sx);
    window->wl.allCursorPosY = wl_fixed_to_double(sy);
    if (surface != window->wl.surface) {
        csd_handle_pointer_event(window, -2, -2, surface);
    } else {
        window->wl.decorations.focus = CENTRAL_WINDOW;
        window->wl.hovered = true;
        window->wl.cursorPosX = x;
        window->wl.cursorPosY = y;
        _glfwPlatformSetCursor(window, window->wl.currentCursor);
        _glfwInputCursorEnter(window, true);
    }
}

// Ends the scroll gesture of fingers whose stop will not arrive, because the
// pointer they scroll with has left the window or gone away
static void
cancelFingerScroll(_GLFWwindow *window) {
    if (!window->wl.finger_scroll_active) return;
    window->wl.finger_scroll_active = false;
    glfw_cancel_momentum_scroll();
    const GLFWScrollEvent ev = {
        .offset_type = GLFW_SCROLL_OFFEST_HIGHRES, .scroll_phase = GLFW_MOMENTUM_PHASE_CANCELED, .keyboard_modifiers = _glfw.wl.xkb.states.modifiers};
    _glfwInputScroll(window, &ev);
}

static void
pointerHandleLeave(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t serial, struct wl_surface *surface) {
    // The pointer never leaves the surface during an implicit grab, so a
    // leave event means any implicit grab is over (e.g. the compositor took
    // over the pointer for drag-and-drop). The matching button releases will
    // never be delivered to us.
    _glfw.wl.pointer_button_count = 0;
    // A DND grab taking over the pointer sends a leave, making this the
    // earliest proof that a just requested start_drag was accepted.
    _glfwWaylandConfirmDragSession();
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window) return;
    _glfw.wl.serial = serial;
    _glfw.wl.pointerFocus = NULL;
    cancelFingerScroll(window);
    if (window->wl.surface == surface) {
        window->wl.hovered = false;
        _glfwInputCursorEnter(window, false);
        _glfw.wl.cursorPreviousShape = GLFW_INVALID_CURSOR;
    } else csd_handle_pointer_event(window, -3, -3, surface);
}

static void
pointerHandleMotion(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t time UNUSED, wl_fixed_t sx, wl_fixed_t sy) {
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window || window->cursorMode == GLFW_CURSOR_DISABLED || toolOver(window)) return;
    window->wl.allCursorPosX = wl_fixed_to_double(sx);
    window->wl.allCursorPosY = wl_fixed_to_double(sy);
    if (window->wl.decorations.focus != CENTRAL_WINDOW) {
        csd_handle_pointer_event(window, -1, -1, NULL);
    } else {
        window->wl.cursorPosX = x;
        window->wl.cursorPosY = y;
        _glfwInputCursorPos(window, x, y);
        _glfw.wl.cursorPreviousShape = GLFW_INVALID_CURSOR;
    }
}

static void
pointerHandleButton(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t serial, uint32_t time UNUSED, uint32_t button, uint32_t state) {
    glfw_cancel_momentum_scroll();
    _glfw.wl.serial = serial;
    _glfw.wl.input_serial = serial;
    _glfw.wl.pointer_serial = serial;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (_glfw.wl.pointer_button_count++ == 0) _glfw.wl.pointer_grab_serial = serial;
    } else if (_glfw.wl.pointer_button_count > 0) _glfw.wl.pointer_button_count--;

    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window || toolOver(window)) return;
    if (window->wl.decorations.focus != CENTRAL_WINDOW) {
        csd_handle_pointer_event(window, button, state, NULL);
        return;
    }
    /* Makes left, right and middle 0, 1 and 2. Overall order follows evdev
     * codes. */
    int glfwButton = button - BTN_LEFT;
    _glfwInputMouseClick(window, glfwButton, state == WL_POINTER_BUTTON_STATE_PRESSED ? GLFW_PRESS : GLFW_RELEASE, _glfw.wl.xkb.states.modifiers);
}
#undef x
#undef y

#define info (window->wl.pointer_curr_axis_info)

static void
pointer_handle_axis_common(enum _GLFWWaylandAxisEvent type, uint32_t axis, wl_fixed_t value) {
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window || window->wl.decorations.focus != CENTRAL_WINDOW) return;
    float fval = (float)wl_fixed_to_double(value);

#define CASE(type, type_const, axis, fval)                      \
    case type_const:                                            \
        if (info.type.axis##_axis_type == AXIS_EVENT_UNKNOWN) { \
            info.type.axis##_axis_type = type_const;            \
            info.type.axis = 0.f;                               \
        }                                                       \
        info.type.axis += fval;                                 \
        break;

    if (window) {
        switch ((enum wl_pointer_axis)axis) {
            case WL_POINTER_AXIS_VERTICAL_SCROLL:
                switch (type) {
                    case AXIS_EVENT_UNKNOWN:
                        break;
                        CASE(discrete, AXIS_EVENT_DISCRETE, y, -fval);     // wheel event
                        CASE(discrete, AXIS_EVENT_VALUE120, y, -fval);     // wheel event higher res than plain discrete
                        CASE(continuous, AXIS_EVENT_CONTINUOUS, y, -fval); // touchpad, etc. high res
                }
                break;
            case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
                switch (type) {
                    case AXIS_EVENT_UNKNOWN:
                        break;
                        CASE(discrete, AXIS_EVENT_DISCRETE, x, fval);     // wheel event
                        CASE(discrete, AXIS_EVENT_VALUE120, x, fval);     // wheel event higher res than plain discrete
                        CASE(continuous, AXIS_EVENT_CONTINUOUS, x, fval); // touchpad, etc. high res
                }
                break;
        }
    }
#undef CASE
}

static void
pointer_handle_axis(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t time, uint32_t axis, wl_fixed_t value) {
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window) return;
    switch (axis) {
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            if (!info.y_start_time) info.y_start_time = ms_to_monotonic_t(time);
            break;
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            if (!info.x_start_time) info.x_start_time = ms_to_monotonic_t(time);
            break;
    }
    info.event_time = time;
    info.has_event_time = true;
    pointer_handle_axis_common(AXIS_EVENT_CONTINUOUS, axis, value);
}

static void
pointer_handle_frame(void *data UNUSED, struct wl_pointer *pointer UNUSED) {
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window) return;
    GLFWScrollEvent ev = {.keyboard_modifiers = _glfw.wl.xkb.states.modifiers};
    if (info.has_event_time) ev.timestamp = event_time_to_monotonic(info.event_time);
    bool found = false;

    if (info.discrete.y_axis_type != AXIS_EVENT_UNKNOWN) {
        ev.unscaled.y = info.discrete.y;
        if (info.discrete.y_axis_type == AXIS_EVENT_VALUE120) ev.offset_type = GLFW_SCROLL_OFFEST_V120;
        found = true;
    } else if (info.continuous.y_axis_type != AXIS_EVENT_UNKNOWN) {
        ev.offset_type = GLFW_SCROLL_OFFEST_HIGHRES;
        ev.unscaled.y = info.continuous.y;
        found = true;
    }

    if (info.discrete.x_axis_type != AXIS_EVENT_UNKNOWN) {
        ev.unscaled.x = info.discrete.x;
        if (info.discrete.x_axis_type == AXIS_EVENT_VALUE120) ev.offset_type = GLFW_SCROLL_OFFEST_V120;
        found = true;
    } else if (info.continuous.x_axis_type != AXIS_EVENT_UNKNOWN) {
        ev.offset_type = GLFW_SCROLL_OFFEST_HIGHRES;
        ev.unscaled.x = info.continuous.x;
        found = true;
    }
    bool stopped = info.y_stop_received || info.x_stop_received;
    if (!found && stopped) ev.offset_type = window->wl.prev_frame_offset_type;
    // Fingers scrolling on a touchpad say so with every frame, and the
    // compositor promises a stop when they lift, which makes a gesture of
    // their frames: began, continues, ended
    if (stopped) {
        if (window->wl.finger_scroll_active) ev.scroll_phase = GLFW_MOMENTUM_PHASE_ENDED;
        window->wl.finger_scroll_active = false;
    } else if (found && info.source_type == WL_POINTER_AXIS_SOURCE_FINGER && ev.offset_type == GLFW_SCROLL_OFFEST_HIGHRES) {
        ev.scroll_phase = window->wl.finger_scroll_active ? GLFW_MOMENTUM_PHASE_ACTIVE : GLFW_MOMENTUM_PHASE_BEGAN;
        window->wl.finger_scroll_active = true;
    }
    ev.unscaled.x *= -1;
    const double scale = ev.offset_type == GLFW_SCROLL_OFFEST_HIGHRES ? _glfwWaylandWindowScale(window) : 1;
    ev.x_offset = scale * ev.unscaled.x;
    ev.y_offset = scale * ev.unscaled.y;
    glfw_handle_scroll_event_for_momentum(window, &ev, stopped, info.source_type == WL_POINTER_AXIS_SOURCE_FINGER);
    if (found) window->wl.prev_frame_offset_type = ev.offset_type;
    /* clear pointer_curr_axis_info for next frame */
    memset(&info, 0, sizeof(info));
}

static void
pointer_handle_axis_source(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t source) {
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window) return;
    info.source_type = source;
}

static void
pointer_handle_axis_stop(void *data UNUSED, struct wl_pointer *wl_pointer UNUSED, uint32_t time, uint32_t axis) {
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window) return;
    info.event_time = time;
    info.has_event_time = true;
    switch (axis) {
        case WL_POINTER_AXIS_VERTICAL_SCROLL:
            info.y_stop_received = true;
            info.y_stop_time = ms_to_monotonic_t(time);
            break;
        case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
            info.x_stop_received = true;
            info.x_stop_time = ms_to_monotonic_t(time);
            break;
    }
}


static void
pointer_handle_axis_discrete(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t axis, int32_t discrete) {
    pointer_handle_axis_common(AXIS_EVENT_DISCRETE, axis, wl_fixed_from_int(discrete));
}

static void
pointer_handle_axis_value120(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t axis, int32_t value120) {
    pointer_handle_axis_common(AXIS_EVENT_VALUE120, axis, wl_fixed_from_int(value120));
}

static void
pointer_handle_axis_relative_direction(void *data UNUSED, struct wl_pointer *pointer UNUSED, uint32_t axis UNUSED, uint32_t axis_relative_direction UNUSED) {}

#undef info
static const struct wl_pointer_listener pointerListener = {
    .enter = pointerHandleEnter,
    .leave = pointerHandleLeave,
    .motion = pointerHandleMotion,
    .button = pointerHandleButton,

    .axis = pointer_handle_axis,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    .axis_value120 = pointer_handle_axis_value120,
#endif
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    .axis_relative_direction = pointer_handle_axis_relative_direction,
#endif
};

#define touch (_glfw.wl.touch_state)

static ssize_t
touch_point_index(int32_t id) {
    for (size_t i = 0; i < touch.count; i++)
        if (touch.points[i].id == id) return i;
    return -1;
}

static void
touch_record_time(uint32_t time) {
    touch.last_event_time = time;
    touch.has_event_time = true;
}

// The time of the last wl_touch event as monotonic() time
static monotonic_t
touch_event_timestamp(void) {
    return touch.has_event_time ? event_time_to_monotonic(touch.last_event_time) : monotonic();
}

static bool
touch_point_is_tap(const _GLFWWaylandTouchPoint *p) {
    return p->pressed_in_frame && p->state == GLFW_TOUCH_POINT_RELEASED;
}

typedef enum TouchReportMode {
    // Every point in its own state
    TOUCH_REPORT_OWN_STATE,
    // The points that were pressed and released in this frame as pressed, the
    // rest in their own state
    TOUCH_REPORT_TAPS_AS_PRESSED,
    // The points that were pressed and released in this frame as released, the
    // rest as stationary, as they were reported by TOUCH_REPORT_TAPS_AS_PRESSED
    TOUCH_REPORT_TAPS_AS_RELEASED,
    // Only the points the window has heard of, all as released
    TOUCH_REPORT_CANCELED,
} TouchReportMode;

// Sends one window the points it owns, in the given event type, with their
// states decided by mode
static void
touch_send_to_window(GLFWid window_id, GLFWTouchEventType type, TouchReportMode mode, monotonic_t timestamp) {
    _GLFWwindow *window = _glfwWindowForId(window_id);
    if (!window) return;
    GLFWTouchPoint points[arraysz(touch.points)];
    size_t n = 0;
    for (size_t i = 0; i < touch.count; i++) {
        const _GLFWWaylandTouchPoint *p = touch.points + i;
        if (p->window_id != window_id) continue;
        GLFWTouchPointState state = p->state;
        switch (mode) {
            case TOUCH_REPORT_OWN_STATE: break;
            case TOUCH_REPORT_TAPS_AS_PRESSED:
                if (touch_point_is_tap(p)) state = GLFW_TOUCH_POINT_PRESSED;
                break;
            case TOUCH_REPORT_TAPS_AS_RELEASED:
                if (!touch_point_is_tap(p)) state = GLFW_TOUCH_POINT_STATIONARY;
                break;
            case TOUCH_REPORT_CANCELED:
                if (p->pressed_in_frame) continue;
                state = GLFW_TOUCH_POINT_RELEASED;
                break;
        }
        points[n++] = (GLFWTouchPoint){.id = p->id, .state = state, .x = p->x, .y = p->y, .major = p->major, .minor = p->minor, .angle = p->orientation};
    }
    if (!n) return;
    GLFWTouchEvent ev = {.type = type, .keyboard_modifiers = _glfw.wl.xkb.states.modifiers, .timestamp = timestamp, .num_points = n, .points = points};
    _glfwInputTouch(window, &ev);
}

static void
touchHandleDown(
    void *data UNUSED, struct wl_touch *wl_touch UNUSED, uint32_t serial, uint32_t time, struct wl_surface *surface, int32_t id, wl_fixed_t sx, wl_fixed_t sy) {
    // A finger landing stops any scroll that is coasting, as on every touch system
    glfw_cancel_momentum_scroll();
    _GLFWwindow *window = get_window_from_surface(surface);
    // A touch on a client-side decoration is not reported
    if (!window || surface != window->wl.surface) return;
    if (touch.count >= arraysz(touch.points) || touch_point_index(id) > -1) return;
    _glfw.wl.serial = serial;
    _glfw.wl.input_serial = serial;
    // Requests such as setting the primary selection need the serial of the
    // latest input event, which can come from touch as well as the pointer
    _glfw.wl.pointer_serial = serial;
    touch_record_time(time);
    touch.points[touch.count++] = (_GLFWWaylandTouchPoint){
        .id = id,
        .window_id = window->id,
        .state = GLFW_TOUCH_POINT_PRESSED,
        .pressed_in_frame = true,
        .x = wl_fixed_to_double(sx),
        .y = wl_fixed_to_double(sy),
    };
}

static void
touchHandleUp(void *data UNUSED, struct wl_touch *wl_touch UNUSED, uint32_t serial, uint32_t time, int32_t id) {
    const ssize_t i = touch_point_index(id);
    if (i < 0) return;
    _glfw.wl.serial = serial;
    _glfw.wl.input_serial = serial;
    _glfw.wl.pointer_serial = serial;
    touch_record_time(time);
    touch.points[i].state = GLFW_TOUCH_POINT_RELEASED;
}

static void
touchHandleMotion(void *data UNUSED, struct wl_touch *wl_touch UNUSED, uint32_t time, int32_t id, wl_fixed_t sx, wl_fixed_t sy) {
    const ssize_t i = touch_point_index(id);
    if (i < 0) return;
    touch_record_time(time);
    touch.points[i].x = wl_fixed_to_double(sx);
    touch.points[i].y = wl_fixed_to_double(sy);
    if (touch.points[i].state == GLFW_TOUCH_POINT_STATIONARY) touch.points[i].state = GLFW_TOUCH_POINT_MOVED;
}

// A frame ends a group of events that happened at the same time. Each window with a
// changed point gets one event holding all of its points. A point that was pressed
// and released within the frame is first reported as pressed in an event of its
// own, so that every point is reported pressed before it is reported released
static void
touchHandleFrame(void *data UNUSED, struct wl_touch *wl_touch UNUSED) {
    const monotonic_t timestamp = touch_event_timestamp();
    GLFWid done[arraysz(touch.points)];
    size_t num_done = 0;
    for (size_t i = 0; i < touch.count; i++) {
        const GLFWid window_id = touch.points[i].window_id;
        bool changed = false, seen = false, has_taps = false;
        for (size_t d = 0; d < num_done && !seen; d++) seen = done[d] == window_id;
        if (seen) continue;
        done[num_done++] = window_id;
        size_t down_before = 0, down_after = 0;
        for (size_t j = i; j < touch.count; j++) {
            const _GLFWWaylandTouchPoint *p = touch.points + j;
            if (p->window_id != window_id) continue;
            if (p->state != GLFW_TOUCH_POINT_STATIONARY) changed = true;
            if (touch_point_is_tap(p)) has_taps = true;
            if (!p->pressed_in_frame) down_before++;
            if (p->state != GLFW_TOUCH_POINT_RELEASED) down_after++;
        }
        if (!changed) continue;
        const GLFWTouchEventType opening = down_before ? GLFW_TOUCH_UPDATE : GLFW_TOUCH_BEGIN;
        const GLFWTouchEventType closing = down_after ? GLFW_TOUCH_UPDATE : GLFW_TOUCH_END;
        if (has_taps) {
            touch_send_to_window(window_id, opening, TOUCH_REPORT_TAPS_AS_PRESSED, timestamp);
            touch_send_to_window(window_id, closing, TOUCH_REPORT_TAPS_AS_RELEASED, timestamp);
        } else touch_send_to_window(window_id, !down_before ? GLFW_TOUCH_BEGIN : closing, TOUCH_REPORT_OWN_STATE, timestamp);
    }
    size_t kept = 0;
    for (size_t i = 0; i < touch.count; i++) {
        if (touch.points[i].state == GLFW_TOUCH_POINT_RELEASED) continue;
        touch.points[kept] = touch.points[i];
        touch.points[kept].state = GLFW_TOUCH_POINT_STATIONARY;
        touch.points[kept].pressed_in_frame = false;
        kept++;
    }
    touch.count = kept;
}

// The compositor took every active sequence over, for a gesture of its own. A
// window hears of it only if it was told the sequence began
static void
touchHandleCancel(void *data UNUSED, struct wl_touch *wl_touch UNUSED) {
    const monotonic_t timestamp = monotonic();
    GLFWid done[arraysz(touch.points)];
    size_t num_done = 0;
    for (size_t i = 0; i < touch.count; i++) {
        const GLFWid window_id = touch.points[i].window_id;
        bool seen = false;
        for (size_t d = 0; d < num_done && !seen; d++) seen = done[d] == window_id;
        if (seen) continue;
        done[num_done++] = window_id;
        touch_send_to_window(window_id, GLFW_TOUCH_CANCEL, TOUCH_REPORT_CANCELED, timestamp);
    }
    touch.count = 0;
}

// The shape and orientation of a point arrive before the motion or down event
// they belong to, and are reported with it
static void
touchHandleShape(void *data UNUSED, struct wl_touch *wl_touch UNUSED, int32_t id, wl_fixed_t major, wl_fixed_t minor) {
    touch.shape_seen = true;
    const ssize_t i = touch_point_index(id);
    if (i < 0) return;
    touch.points[i].major = wl_fixed_to_double(major);
    touch.points[i].minor = wl_fixed_to_double(minor);
}

static void
touchHandleOrientation(void *data UNUSED, struct wl_touch *wl_touch UNUSED, int32_t id, wl_fixed_t orientation) {
    const ssize_t i = touch_point_index(id);
    if (i < 0) return;
    touch.points[i].orientation = wl_fixed_to_double(orientation);
}

#undef touch

static const struct wl_touch_listener touchListener = {
    .down = touchHandleDown,
    .up = touchHandleUp,
    .motion = touchHandleMotion,
    .frame = touchHandleFrame,
    .cancel = touchHandleCancel,
    .shape = touchHandleShape,
    .orientation = touchHandleOrientation,
};

// Tablet {{{
#ifndef BTN_STYLUS3
#define BTN_STYLUS3 0x149
#endif

// What a tablet says about itself, which its tools pass on
typedef struct TabletInfo {
    uint32_t vid, pid;
    uint64_t number;  // from one, in the order the tablets were attached
    char name[128];
    // The size of its surface in millimeters and whether that is a display, as
    // for GLFWTabletEvent, which the kernel says of the device node in node
    int width_mm, height_mm, direct;
    InputNodeInfo node;
    bool has_node;
} TabletInfo;

static void
tabletHandleName(void *data, struct zwp_tablet_v2 *tablet UNUSED, const char *name) {
    TabletInfo *info = data;
    if (info) snprintf(info->name, sizeof(info->name), "%s", name);
}

static void
tabletHandleId(void *data, struct zwp_tablet_v2 *tablet UNUSED, uint32_t vid, uint32_t pid) {
    TabletInfo *info = data;
    if (!info) return;
    info->vid = vid;
    info->pid = pid;
}

// The protocol has neither the size of a tablet nor whether it is a display,
// but it names the device node of the tablet, and the kernel and udev say both
// of that. A tablet can have several nodes: the one of the pen is the one whose
// surface a tool is tracked on, and one of its pad or of the fingers on it is
// not looked at
static void
tabletHandlePath(void *data, struct zwp_tablet_v2 *tablet UNUSED, const char *path) {
    TabletInfo *info = data;
    InputNodeInfo node;
    if (!info || !input_node_info(path, &node) || !node.is_tablet || node.is_pad) return;
    info->node = node;
    info->has_node = true;
}

// A device node is a name on the machine the compositor runs on, which need
// not be this one, so what was found at it counts only when the kernel here
// has the device under the ids the compositor gave, or failing those its name
static void
tabletHandleDone(void *data, struct zwp_tablet_v2 *tablet UNUSED) {
    TabletInfo *info = data;
    if (!info || !info->has_node) return;
    const InputNodeInfo *node = &info->node;
    const bool same = info->vid || info->pid ? info->vid == node->vendor && info->pid == node->product : info->name[0] && !strcmp(info->name, node->name);
    if (!same) return;
    info->width_mm = node->width_mm;
    info->height_mm = node->height_mm;
    info->direct = node->direct;
}

static void
tabletHandleRemoved(void *data, struct zwp_tablet_v2 *tablet) {
    zwp_tablet_v2_destroy(tablet);
    free(data);
}

static void
tabletHandleBustype(void *data UNUSED, struct zwp_tablet_v2 *tablet UNUSED, uint32_t bustype UNUSED) {}

static const struct zwp_tablet_v2_listener tabletListener = {
    .name = tabletHandleName,
    .id = tabletHandleId,
    .path = tabletHandlePath,
    .done = tabletHandleDone,
    .removed = tabletHandleRemoved,
    .bustype = tabletHandleBustype,
};

#define tt ((_GLFWWaylandTabletTool *)data)

static void
toolHandleType(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t type) {
    switch (type) {
        case ZWP_TABLET_TOOL_V2_TYPE_PEN: tt->ev.tool = GLFW_TABLET_TOOL_PEN; break;
        case ZWP_TABLET_TOOL_V2_TYPE_ERASER: tt->ev.tool = GLFW_TABLET_TOOL_ERASER; break;
        case ZWP_TABLET_TOOL_V2_TYPE_BRUSH: tt->ev.tool = GLFW_TABLET_TOOL_BRUSH; break;
        case ZWP_TABLET_TOOL_V2_TYPE_PENCIL: tt->ev.tool = GLFW_TABLET_TOOL_PENCIL; break;
        case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH: tt->ev.tool = GLFW_TABLET_TOOL_AIRBRUSH; break;
        case ZWP_TABLET_TOOL_V2_TYPE_MOUSE: tt->ev.tool = GLFW_TABLET_TOOL_MOUSE; break;
        case ZWP_TABLET_TOOL_V2_TYPE_LENS: tt->ev.tool = GLFW_TABLET_TOOL_LENS; break;
        default: tt->ev.tool = GLFW_TABLET_TOOL_UNKNOWN; break;
    }
}

static void
toolHandleHardwareSerial(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t hi, uint32_t lo) {
    tt->ev.serial = ((uint64_t)hi << 32) | lo;
}

static void
toolHandleHardwareIdWacom(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t hi, uint32_t lo) {
    tt->ev.hardware_id = ((uint64_t)hi << 32) | lo;
}

static void
toolHandleCapability(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t capability) {
    switch (capability) {
        case ZWP_TABLET_TOOL_V2_CAPABILITY_TILT: tt->ev.axes |= GLFW_TABLET_TILT; break;
        case ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE: tt->ev.axes |= GLFW_TABLET_PRESSURE; break;
        case ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE: tt->ev.axes |= GLFW_TABLET_DISTANCE; break;
        case ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION: tt->ev.axes |= GLFW_TABLET_TWIST; break;
        case ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER: tt->ev.axes |= GLFW_TABLET_TANGENT; break;
        case ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL: tt->ev.axes |= GLFW_TABLET_WHEEL; break;
    }
}

static void
toolHandleDone(void *data UNUSED, struct zwp_tablet_tool_v2 *t UNUSED) {}

static void
toolHandleRemoved(void *data, struct zwp_tablet_tool_v2 *t) {
    _GLFWwindow *window = _glfwWindowForId(tt->window_id);
    if (window) {
        tt->ev.type = GLFW_TABLET_CANCEL;
        tt->ev.timestamp = monotonic();
        _glfwInputTablet(window, &tt->ev);
    }
    if (_glfw.wl.tablet_tool == tt) _glfw.wl.tablet_tool = NULL;
    if (tt->cursor_shape_device) wp_cursor_shape_device_v1_destroy(tt->cursor_shape_device);
    zwp_tablet_tool_v2_destroy(t);
    free(tt);
}

static void
toolHandleProximityIn(void *data, struct zwp_tablet_tool_v2 *t, uint32_t serial, struct zwp_tablet_v2 *tablet, struct wl_surface *surface) {
    _GLFWwindow *window = get_window_from_surface(surface);
    if (!window || surface != window->wl.surface) return;
    const TabletInfo *info = zwp_tablet_v2_get_user_data(tablet);
    tt->window_id = window->id;
    tt->serial = serial;
    tt->entered = true;
    tt->ev.vendor_id = info ? info->vid : 0;
    tt->ev.product_id = info ? info->pid : 0;
    tt->ev.tablet = info ? info->number : 0;
    tt->ev.tablet_width = info ? info->width_mm : 0;
    tt->ev.tablet_height = info ? info->height_mm : 0;
    tt->ev.tablet_direct = info ? info->direct : 0;
    snprintf(tt->tablet_name, sizeof(tt->tablet_name), "%s", info ? info->name : "");
    tt->ev.name = tt->tablet_name[0] ? tt->tablet_name : NULL;
    if (_glfw.wl.wp_cursor_shape_manager_v1 && !tt->cursor_shape_device) {
        tt->cursor_shape_device = wp_cursor_shape_manager_v1_get_tablet_tool_v2(_glfw.wl.wp_cursor_shape_manager_v1, t);
    }
    _glfw.wl.tablet_tool = tt;
    _glfwWaylandSetTabletToolCursor(tt);
}

static void
toolHandleProximityOut(void *data, struct zwp_tablet_tool_v2 *t UNUSED) {
    tt->left = true;
}

static void
toolHandleDown(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t serial) {
    _glfw.wl.serial = _glfw.wl.input_serial = serial;
    tt->ev.buttons |= GLFW_TABLET_BUTTON_TIP;
    tt->pressed |= 1u << GLFW_MOUSE_BUTTON_LEFT;
}

static void
toolHandleUp(void *data, struct zwp_tablet_tool_v2 *t UNUSED) {
    tt->ev.buttons &= ~GLFW_TABLET_BUTTON_TIP;
    tt->released |= 1u << GLFW_MOUSE_BUTTON_LEFT;
}

static void
toolHandleMotion(void *data, struct zwp_tablet_tool_v2 *t UNUSED, wl_fixed_t x, wl_fixed_t y) {
    tt->ev.x = wl_fixed_to_double(x);
    tt->ev.y = wl_fixed_to_double(y);
    tt->moved = true;
}

static void
toolHandlePressure(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t pressure) {
    tt->ev.pressure = pressure / 65535.;
}

static void
toolHandleDistance(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t distance) {
    tt->ev.distance = distance / 65535.;
}

static void
toolHandleTilt(void *data, struct zwp_tablet_tool_v2 *t UNUSED, wl_fixed_t tilt_x, wl_fixed_t tilt_y) {
    tt->ev.tilt_x = wl_fixed_to_double(tilt_x);
    tt->ev.tilt_y = wl_fixed_to_double(tilt_y);
}

static void
toolHandleRotation(void *data, struct zwp_tablet_tool_v2 *t UNUSED, wl_fixed_t degrees) {
    tt->ev.twist = wl_fixed_to_double(degrees);
}

static void
toolHandleSlider(void *data, struct zwp_tablet_tool_v2 *t UNUSED, int32_t position) {
    tt->ev.tangent = position / 65535.;
}

static void
toolHandleWheel(void *data, struct zwp_tablet_tool_v2 *t UNUSED, wl_fixed_t degrees, int32_t clicks) {
    tt->ev.wheel += wl_fixed_to_double(degrees);
    tt->ev.wheel_clicks += clicks;
}

static void
toolHandleButton(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t serial, uint32_t button, uint32_t state) {
    _glfw.wl.serial = _glfw.wl.input_serial = serial;
    unsigned bit = 0;
    int glfw_button = -1;
    switch (button) {
        case BTN_LEFT: bit = GLFW_TABLET_BUTTON_TIP; glfw_button = GLFW_MOUSE_BUTTON_LEFT; break;
        case BTN_RIGHT:
        case BTN_STYLUS: bit = GLFW_TABLET_BUTTON_BARREL; glfw_button = GLFW_MOUSE_BUTTON_RIGHT; break;
        case BTN_MIDDLE:
        case BTN_STYLUS2: bit = GLFW_TABLET_BUTTON_BARREL2; glfw_button = GLFW_MOUSE_BUTTON_MIDDLE; break;
        case BTN_SIDE: bit = GLFW_TABLET_BUTTON_BACK; glfw_button = GLFW_MOUSE_BUTTON_4; break;
        case BTN_EXTRA: bit = GLFW_TABLET_BUTTON_FORWARD; glfw_button = GLFW_MOUSE_BUTTON_5; break;
        case BTN_STYLUS3: bit = GLFW_TABLET_BUTTON_BARREL3; break;
        default: return;
    }
    const bool pressed = state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED;
    if (pressed) tt->ev.buttons |= bit;
    else tt->ev.buttons &= ~bit;
    if (glfw_button < 0) return;
    if (pressed) tt->pressed |= 1u << glfw_button;
    else tt->released |= 1u << glfw_button;
}

static void
toolHandleFrame(void *data, struct zwp_tablet_tool_v2 *t UNUSED, uint32_t time) {
    _GLFWwindow *window = _glfwWindowForId(tt->window_id);
    if (!window) {
        // Not over a window, or over one that was closed under the tool
        if (tt->window_id && _glfw.wl.tablet_tool == tt) _glfw.wl.tablet_tool = NULL;
        tt->window_id = 0;
        tt->ev.buttons = 0;
        tt->entered = tt->left = tt->moved = false;
        tt->pressed = tt->released = 0;
        tt->ev.wheel = 0;
        tt->ev.wheel_clicks = 0;
        return;
    }
    tt->ev.keyboard_modifiers = _glfw.wl.xkb.states.modifiers;
    tt->ev.timestamp = event_time_to_monotonic(time);
    // A tool that is flicked off the tablet lifts and leaves in one frame. The
    // lift is reported before the leaving, so that the stroke is ended rather
    // than taken to have been lost
    const bool lifted = tt->released & (1u << GLFW_MOUSE_BUTTON_LEFT);
    bool consumed = false;
    if (!tt->left || lifted) {
        tt->ev.type = GLFW_TABLET_POINT;
        consumed = _glfwInputTablet(window, &tt->ev);
    }
    if (tt->left) {
        tt->ev.type = GLFW_TABLET_OUT;
        const bool out_consumed = _glfwInputTablet(window, &tt->ev);
        if (!lifted) consumed = out_consumed;
    }
    if (!consumed) {
        // Compositors send no pointer events for a tool once this protocol is
        // bound, so make them here
        window->wl.cursorPosX = tt->ev.x;
        window->wl.cursorPosY = tt->ev.y;
        if (tt->entered) _glfwInputCursorEnter(window, true);
        if (tt->moved) _glfwInputCursorPos(window, tt->ev.x, tt->ev.y);
        for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; b++) {
            if (tt->pressed & (1u << b)) _glfwInputMouseClick(window, b, GLFW_PRESS, tt->ev.keyboard_modifiers);
            if (tt->released & (1u << b)) _glfwInputMouseClick(window, b, GLFW_RELEASE, tt->ev.keyboard_modifiers);
        }
        if (tt->ev.wheel != 0) {
            // The wheel of a puck, which turns the way the scroll axis of a pointer does: 15 degrees are a notch
            GLFWScrollEvent scroll = {.keyboard_modifiers = tt->ev.keyboard_modifiers, .offset_type = GLFW_SCROLL_OFFEST_V120};
            scroll.unscaled.y = scroll.y_offset = -tt->ev.wheel * 120 / 15;
            glfw_handle_scroll_event_for_momentum(window, &scroll, false, false);
        }
        if (tt->left) _glfwInputCursorEnter(window, false);
    }
    if (tt->left) {
        tt->window_id = 0;
        if (_glfw.wl.tablet_tool == tt) _glfw.wl.tablet_tool = NULL;
        // A tool should come up before it leaves, but not every compositor says
        // so, and it is not down when it comes back
        tt->ev.buttons = 0;
    }
    tt->entered = tt->left = tt->moved = false;
    tt->pressed = tt->released = 0;
    tt->ev.wheel = 0;
    tt->ev.wheel_clicks = 0;
}
#undef tt

static const struct zwp_tablet_tool_v2_listener toolListener = {
    .type = toolHandleType,
    .hardware_serial = toolHandleHardwareSerial,
    .hardware_id_wacom = toolHandleHardwareIdWacom,
    .capability = toolHandleCapability,
    .done = toolHandleDone,
    .removed = toolHandleRemoved,
    .proximity_in = toolHandleProximityIn,
    .proximity_out = toolHandleProximityOut,
    .down = toolHandleDown,
    .up = toolHandleUp,
    .motion = toolHandleMotion,
    .pressure = toolHandlePressure,
    .distance = toolHandleDistance,
    .tilt = toolHandleTilt,
    .rotation = toolHandleRotation,
    .slider = toolHandleSlider,
    .wheel = toolHandleWheel,
    .button = toolHandleButton,
    .frame = toolHandleFrame,
};

static void
tabletSeatHandleTabletAdded(void *data UNUSED, struct zwp_tablet_seat_v2 *seat UNUSED, struct zwp_tablet_v2 *tablet) {
    static uint64_t tablet_count = 0;
    TabletInfo *info = calloc(1, sizeof(TabletInfo));
    if (info) info->number = ++tablet_count;
    zwp_tablet_v2_add_listener(tablet, &tabletListener, info);
}

static void
tabletSeatHandleToolAdded(void *data UNUSED, struct zwp_tablet_seat_v2 *seat UNUSED, struct zwp_tablet_tool_v2 *t) {
    static uint64_t device_count = 0;
    _GLFWWaylandTabletTool *tool = calloc(1, sizeof(_GLFWWaylandTabletTool));
    if (!tool) {
        zwp_tablet_tool_v2_destroy(t);
        return;
    }
    tool->tool = t;
    tool->ev.device = ++device_count;
    zwp_tablet_tool_v2_add_listener(t, &toolListener, tool);
}

// Pads: the keys, rings, strips and dials of a tablet. A pad has the focus of
// the surface that has the keyboard focus

// Reports an event of a pad to the window it has the focus of
static void
padSendEvent(_GLFWWaylandTabletPad *pad, GLFWTabletPadEvent *ev, const uint32_t *time) {
    _GLFWwindow *window = _glfwWindowForId(pad->window_id);
    if (!window) return;
    unsigned num_modes[arraysz(pad->groups)], modes[arraysz(pad->groups)];
    // The group of each button, and of each ring, strip and dial by the place it has among the controls of its kind
    int button_groups[64], control_groups[arraysz(pad->controls)];
    const unsigned num_grouped = MIN(pad->num_buttons, (unsigned)arraysz(button_groups));
    for (unsigned i = 0; i < num_grouped; i++) button_groups[i] = -1;
    bool modes_known = pad->num_groups > 0;
    for (size_t i = 0; i < pad->num_groups; i++) {
        const _GLFWWaylandTabletPadGroup *g = pad->groups[i];
        num_modes[i] = MAX(1u, g->num_modes);  // a group with a single mode does not say how many it has
        modes[i] = g->mode;
        modes_known = modes_known && g->has_mode_serial;
        for (size_t b = 0; b < g->num_buttons; b++) {
            if (g->buttons[b] < num_grouped) button_groups[g->buttons[b]] = (int)i;
        }
    }
    // The controls are kept in the order they were announced, which numbers
    // each kind from zero, so the rings come first here, then the strips, then
    // the dials, each kind in the order of its numbers
    unsigned first[3] = {0, pad->num_rings, pad->num_rings + pad->num_strips};
    for (size_t i = 0; i < pad->num_controls; i++) {
        const _GLFWWaylandTabletPadControl *c = pad->controls[i];
        const unsigned kind = c->type == GLFW_TABLET_PAD_RING ? 0 : (c->type == GLFW_TABLET_PAD_STRIP ? 1 : 2);
        control_groups[first[kind] + c->index] = (int)c->group->index;
    }
    ev->keyboard_modifiers = _glfw.wl.xkb.states.modifiers;
    ev->timestamp = time ? event_time_to_monotonic(*time) : monotonic();
    ev->device = pad->device;
    ev->num_buttons = pad->num_buttons;
    ev->num_rings = pad->num_rings;
    ev->num_strips = pad->num_strips;
    ev->num_dials = pad->num_dials;
    ev->num_groups = pad->num_groups;
    ev->num_modes = num_modes;
    ev->modes = modes_known ? modes : NULL;
    // A pad with more buttons than there is room for here says nothing of their groups
    ev->button_groups = pad->num_buttons <= arraysz(button_groups) ? button_groups : NULL;
    ev->ring_groups = control_groups;
    ev->strip_groups = control_groups + first[1];
    ev->dial_groups = control_groups + first[2];
    ev->tablet = pad->tablet;
    ev->name = pad->tablet_name[0] ? pad->tablet_name : NULL;
    ev->vendor_id = pad->vendor_id;
    ev->product_id = pad->product_id;
    _glfwInputTabletPad(window, ev);
}

static void
padLoseFocus(_GLFWWaylandTabletPad *pad) {
    if (!pad->window_id) return;
    GLFWTabletPadEvent ev = {.type = GLFW_TABLET_PAD_LEAVE};
    padSendEvent(pad, &ev, NULL);
    pad->window_id = 0;
}

static void
destroyTabletPad(_GLFWWaylandTabletPad *pad) {
    for (size_t i = 0; i < pad->num_controls; i++) {
        _GLFWWaylandTabletPadControl *c = pad->controls[i];
        switch (c->type) {
            case GLFW_TABLET_PAD_RING: zwp_tablet_pad_ring_v2_destroy(c->ring); break;
            case GLFW_TABLET_PAD_STRIP: zwp_tablet_pad_strip_v2_destroy(c->strip); break;
            default: zwp_tablet_pad_dial_v2_destroy(c->dial); break;
        }
        free(c);
    }
    for (size_t i = 0; i < pad->num_groups; i++) {
        zwp_tablet_pad_group_v2_destroy(pad->groups[i]->group);
        free(pad->groups[i]->buttons);
        free(pad->groups[i]);
    }
    zwp_tablet_pad_v2_destroy(pad->pad);
    for (_GLFWWaylandTabletPad **p = &_glfw.wl.tablet_pads; *p; p = &(*p)->next) {
        if (*p == pad) {
            *p = pad->next;
            break;
        }
    }
    free(pad);
}

#define pc ((_GLFWWaylandTabletPadControl *)data)

// The events of a ring, a strip or a dial come in groups that end with a
// frame, which is what has the time
static void
padControlFrame(_GLFWWaylandTabletPadControl *c, uint32_t time) {
    GLFWTabletPadEvent ev = {.type = c->type, .index = c->index, .from_finger = c->from_finger};
    if (c->type == GLFW_TABLET_PAD_DIAL) {
        ev.delta = c->delta;
        if (c->delta) padSendEvent(c->pad, &ev, &time);
    } else {
        if (c->has_value) {
            ev.value = c->value;
            padSendEvent(c->pad, &ev, &time);
        }
        if (c->stopped) {
            ev.value = 0;
            ev.stopped = true;
            padSendEvent(c->pad, &ev, &time);
        }
    }
    c->has_value = c->stopped = c->from_finger = false;
    c->delta = 0;
}

static void
padRingHandleSource(void *data, struct zwp_tablet_pad_ring_v2 *ring UNUSED, uint32_t source) {
    pc->from_finger = source == ZWP_TABLET_PAD_RING_V2_SOURCE_FINGER;
}

static void
padRingHandleAngle(void *data, struct zwp_tablet_pad_ring_v2 *ring UNUSED, wl_fixed_t degrees) {
    pc->value = wl_fixed_to_double(degrees);
    pc->has_value = true;
}

static void
padRingHandleStop(void *data, struct zwp_tablet_pad_ring_v2 *ring UNUSED) {
    pc->stopped = true;
}

static void
padRingHandleFrame(void *data, struct zwp_tablet_pad_ring_v2 *ring UNUSED, uint32_t time) {
    padControlFrame(pc, time);
}

static const struct zwp_tablet_pad_ring_v2_listener padRingListener = {
    .source = padRingHandleSource,
    .angle = padRingHandleAngle,
    .stop = padRingHandleStop,
    .frame = padRingHandleFrame,
};

static void
padStripHandleSource(void *data, struct zwp_tablet_pad_strip_v2 *strip UNUSED, uint32_t source) {
    pc->from_finger = source == ZWP_TABLET_PAD_STRIP_V2_SOURCE_FINGER;
}

static void
padStripHandlePosition(void *data, struct zwp_tablet_pad_strip_v2 *strip UNUSED, uint32_t position) {
    pc->value = MIN(position, 65535u) / 65535.;
    pc->has_value = true;
}

static void
padStripHandleStop(void *data, struct zwp_tablet_pad_strip_v2 *strip UNUSED) {
    pc->stopped = true;
}

static void
padStripHandleFrame(void *data, struct zwp_tablet_pad_strip_v2 *strip UNUSED, uint32_t time) {
    padControlFrame(pc, time);
}

static const struct zwp_tablet_pad_strip_v2_listener padStripListener = {
    .source = padStripHandleSource,
    .position = padStripHandlePosition,
    .stop = padStripHandleStop,
    .frame = padStripHandleFrame,
};

static void
padDialHandleDelta(void *data, struct zwp_tablet_pad_dial_v2 *dial UNUSED, int32_t value120) {
    pc->delta += value120;
}

static void
padDialHandleFrame(void *data, struct zwp_tablet_pad_dial_v2 *dial UNUSED, uint32_t time) {
    padControlFrame(pc, time);
}

static const struct zwp_tablet_pad_dial_v2_listener padDialListener = {
    .delta = padDialHandleDelta,
    .frame = padDialHandleFrame,
};
#undef pc

#define pg ((_GLFWWaylandTabletPadGroup *)data)

// Rings, strips and dials are numbered across the pad in the order the
// compositor lists them, which is the order of their numbers in libinput
static _GLFWWaylandTabletPadControl *
padAddControl(_GLFWWaylandTabletPadGroup *group, GLFWTabletPadEventType type, unsigned *count) {
    _GLFWWaylandTabletPad *pad = group->pad;
    _GLFWWaylandTabletPadControl *c = pad->num_controls < arraysz(pad->controls) ? calloc(1, sizeof(_GLFWWaylandTabletPadControl)) : NULL;
    if (!c) return NULL;
    c->pad = pad;
    c->group = group;
    c->type = type;
    c->index = (*count)++;
    pad->controls[pad->num_controls++] = c;
    return c;
}

// A pad or a group must not be destroyed while the compositor is still
// announcing what is in it: libwayland drops the events of a destroyed object
// without taking note of the objects they bring, and the compositor's next
// new object is then a protocol error that ends the connection. So a pad or a
// group that cannot be kept, there being no memory or no room for it, is
// listened to with no data until its done event and destroyed then. Rings,
// strips and dials bring nothing and can be destroyed as they arrive

static void
padGroupHandleButtons(void *data, struct zwp_tablet_pad_group_v2 *group UNUSED, struct wl_array *buttons) {
    if (!pg) return;
    free(pg->buttons);
    pg->num_buttons = 0;
    pg->buttons = buttons->size ? malloc(buttons->size) : NULL;
    if (pg->buttons) {
        memcpy(pg->buttons, buttons->data, buttons->size);
        pg->num_buttons = buttons->size / sizeof(uint32_t);
    }
    // A compositor may take buttons out of a group for its own use, or give
    // them back, while the pad is in use, and says so by listing the buttons
    // of the group again. Nothing is reported of a pad that has no focus
    GLFWTabletPadEvent ev = {.type = GLFW_TABLET_PAD_CHANGED};
    padSendEvent(pg->pad, &ev, NULL);
}

static void
padGroupHandleRing(void *data, struct zwp_tablet_pad_group_v2 *group UNUSED, struct zwp_tablet_pad_ring_v2 *ring) {
    _GLFWWaylandTabletPadControl *c = pg ? padAddControl(pg, GLFW_TABLET_PAD_RING, &pg->pad->num_rings) : NULL;
    if (!c) {
        zwp_tablet_pad_ring_v2_destroy(ring);
        return;
    }
    c->ring = ring;
    zwp_tablet_pad_ring_v2_add_listener(ring, &padRingListener, c);
}

static void
padGroupHandleStrip(void *data, struct zwp_tablet_pad_group_v2 *group UNUSED, struct zwp_tablet_pad_strip_v2 *strip) {
    _GLFWWaylandTabletPadControl *c = pg ? padAddControl(pg, GLFW_TABLET_PAD_STRIP, &pg->pad->num_strips) : NULL;
    if (!c) {
        zwp_tablet_pad_strip_v2_destroy(strip);
        return;
    }
    c->strip = strip;
    zwp_tablet_pad_strip_v2_add_listener(strip, &padStripListener, c);
}

static void
padGroupHandleDial(void *data, struct zwp_tablet_pad_group_v2 *group UNUSED, struct zwp_tablet_pad_dial_v2 *dial) {
    _GLFWWaylandTabletPadControl *c = pg ? padAddControl(pg, GLFW_TABLET_PAD_DIAL, &pg->pad->num_dials) : NULL;
    if (!c) {
        zwp_tablet_pad_dial_v2_destroy(dial);
        return;
    }
    c->dial = dial;
    zwp_tablet_pad_dial_v2_add_listener(dial, &padDialListener, c);
}

static void
padGroupHandleModes(void *data, struct zwp_tablet_pad_group_v2 *group UNUSED, uint32_t modes) {
    if (pg) pg->num_modes = modes;
}

static void
padGroupHandleDone(void *data, struct zwp_tablet_pad_group_v2 *group) {
    if (!pg) zwp_tablet_pad_group_v2_destroy(group);
}

// Compositors also send this for every group when the pad gets the focus, so
// an application learns the modes the pad is in
static void
padGroupHandleModeSwitch(void *data, struct zwp_tablet_pad_group_v2 *group UNUSED, uint32_t time, uint32_t serial, uint32_t mode) {
    if (!pg) return;
    pg->mode = mode;
    pg->mode_serial = serial;
    pg->has_mode_serial = true;
    GLFWTabletPadEvent ev = {.type = GLFW_TABLET_PAD_MODE, .index = pg->index, .mode = mode};
    padSendEvent(pg->pad, &ev, &time);
}

static const struct zwp_tablet_pad_group_v2_listener padGroupListener = {
    .buttons = padGroupHandleButtons,
    .ring = padGroupHandleRing,
    .strip = padGroupHandleStrip,
    .modes = padGroupHandleModes,
    .done = padGroupHandleDone,
    .mode_switch = padGroupHandleModeSwitch,
    .dial = padGroupHandleDial,
};
#undef pg

#define pp ((_GLFWWaylandTabletPad *)data)

static void
padHandleGroup(void *data, struct zwp_tablet_pad_v2 *pad UNUSED, struct zwp_tablet_pad_group_v2 *group) {
    _GLFWWaylandTabletPadGroup *g = pp && pp->num_groups < arraysz(pp->groups) ? calloc(1, sizeof(_GLFWWaylandTabletPadGroup)) : NULL;
    if (g) {
        g->pad = pp;
        g->group = group;
        g->index = (unsigned)pp->num_groups;
        pp->groups[pp->num_groups++] = g;
    }
    zwp_tablet_pad_group_v2_add_listener(group, &padGroupListener, g);
}

static void
padHandlePath(void *data UNUSED, struct zwp_tablet_pad_v2 *pad UNUSED, const char *path UNUSED) {}

static void
padHandleButtons(void *data, struct zwp_tablet_pad_v2 *pad UNUSED, uint32_t buttons) {
    if (pp) pp->num_buttons = buttons;
}

static void
padHandleDone(void *data, struct zwp_tablet_pad_v2 *pad) {
    if (!pp) zwp_tablet_pad_v2_destroy(pad);
}

static void
padHandleButton(void *data, struct zwp_tablet_pad_v2 *pad UNUSED, uint32_t time, uint32_t button, uint32_t state) {
    if (!pp) return;
    GLFWTabletPadEvent ev = {.type = GLFW_TABLET_PAD_BUTTON, .index = button, .pressed = state == ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED};
    padSendEvent(pp, &ev, &time);
}

static void
padHandleEnter(void *data, struct zwp_tablet_pad_v2 *pad UNUSED, uint32_t serial, struct zwp_tablet_v2 *tablet, struct wl_surface *surface) {
    if (!pp) return;
    padLoseFocus(pp);
    _GLFWwindow *window = get_window_from_surface(surface);
    if (!window || surface != window->wl.surface) return;
    const TabletInfo *info = tablet ? zwp_tablet_v2_get_user_data(tablet) : NULL;
    pp->window_id = window->id;
    pp->enter_serial = serial;
    pp->vendor_id = info ? info->vid : 0;
    pp->product_id = info ? info->pid : 0;
    pp->tablet = info ? info->number : 0;
    snprintf(pp->tablet_name, sizeof(pp->tablet_name), "%s", info ? info->name : "");
    // The modes are from before the pad had the focus here, and so are their serials
    for (size_t i = 0; i < pp->num_groups; i++) pp->groups[i]->has_mode_serial = false;
    GLFWTabletPadEvent ev = {.type = GLFW_TABLET_PAD_ENTER};
    padSendEvent(pp, &ev, NULL);
}

static void
padHandleLeave(void *data, struct zwp_tablet_pad_v2 *pad UNUSED, uint32_t serial UNUSED, struct wl_surface *surface UNUSED) {
    if (pp) padLoseFocus(pp);
}

static void
padHandleRemoved(void *data, struct zwp_tablet_pad_v2 *pad UNUSED) {
    if (!pp) return;  // it was destroyed when it was done
    padLoseFocus(pp);
    destroyTabletPad(pp);
}
#undef pp

static const struct zwp_tablet_pad_v2_listener padListener = {
    .group = padHandleGroup,
    .path = padHandlePath,
    .buttons = padHandleButtons,
    .done = padHandleDone,
    .button = padHandleButton,
    .enter = padHandleEnter,
    .leave = padHandleLeave,
    .removed = padHandleRemoved,
};

static void
tabletSeatHandlePadAdded(void *data UNUSED, struct zwp_tablet_seat_v2 *seat UNUSED, struct zwp_tablet_pad_v2 *pad) {
    static uint64_t device_count = 0;
    _GLFWWaylandTabletPad *p = calloc(1, sizeof(_GLFWWaylandTabletPad));
    if (p) {
        p->pad = pad;
        p->device = ++device_count;
        p->next = _glfw.wl.tablet_pads;
        _glfw.wl.tablet_pads = p;
    }
    zwp_tablet_pad_v2_add_listener(pad, &padListener, p);
}

// Tells the compositor what a control of a pad does at the moment, which it
// can show to the user. Only the window the pad has the focus of can say, and
// the compositor wants the serial of the last mode switch of the control's
// group, since what a control does depends on the mode
static bool
set_tablet_pad_label(const _GLFWwindow *window, uint64_t device, int control, unsigned index, const char *text) {
    for (_GLFWWaylandTabletPad *pad = _glfw.wl.tablet_pads; pad; pad = pad->next) {
        if (pad->device != device) continue;
        if (pad->window_id != window->id) return false;
#define serial_of(g) ((g) && (g)->has_mode_serial ? (g)->mode_serial : pad->enter_serial)
        if (control == GLFW_TABLET_PAD_BUTTON) {
            if (index >= pad->num_buttons) return false;
            const _GLFWWaylandTabletPadGroup *group = NULL;
            for (size_t i = 0; i < pad->num_groups && !group; i++) {
                for (size_t k = 0; k < pad->groups[i]->num_buttons; k++) {
                    if (pad->groups[i]->buttons[k] == index) group = pad->groups[i];
                }
            }
            zwp_tablet_pad_v2_set_feedback(pad->pad, index, text, serial_of(group));
            return true;
        }
        for (size_t i = 0; i < pad->num_controls; i++) {
            const _GLFWWaylandTabletPadControl *c = pad->controls[i];
            if ((int)c->type != control || c->index != index) continue;
            switch (c->type) {
                case GLFW_TABLET_PAD_RING: zwp_tablet_pad_ring_v2_set_feedback(c->ring, text, serial_of(c->group)); break;
                case GLFW_TABLET_PAD_STRIP: zwp_tablet_pad_strip_v2_set_feedback(c->strip, text, serial_of(c->group)); break;
                default: zwp_tablet_pad_dial_v2_set_feedback(c->dial, text, serial_of(c->group)); break;
            }
            return true;
        }
#undef serial_of
        return false;
    }
    return false;
}

GLFWAPI bool
glfwWaylandSetTabletPadLabel(GLFWwindow *handle, uint64_t device, int control, unsigned index, const char *text) {
    const _GLFWwindow *window = (const _GLFWwindow *)handle;
    if (!window) return false;
    if (!text) text = "";
    // libwayland refuses a message longer than 4096 bytes and the connection
    // dies with it. The longest of these requests carries the 8 byte header,
    // two integers and the length of the string besides the string and its
    // terminating NUL, so longer text is cut, at a character boundary
    static const size_t max_text = 4096 - 8 - 4 * 3 - 1;
    char *cut = NULL;
    size_t len = strlen(text);
    if (len > max_text) {
        len = max_text;
        while (len && ((unsigned char)text[len] & 0xC0) == 0x80) len--;
        if (!(cut = malloc(len + 1))) return false;
        memcpy(cut, text, len);
        cut[len] = 0;
        text = cut;
    }
    const bool ans = set_tablet_pad_label(window, device, control, index, text);
    free(cut);
    return ans;
}

static const struct zwp_tablet_seat_v2_listener tabletSeatListener = {
    .tablet_added = tabletSeatHandleTabletAdded,
    .tool_added = tabletSeatHandleToolAdded,
    .pad_added = tabletSeatHandlePadAdded,
};

static void
setupTabletSeat(void) {
    if (!_glfw.wl.seat || !_glfw.wl.tablet_manager || _glfw.wl.tablet_seat) return;
    _glfw.wl.tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(_glfw.wl.tablet_manager, _glfw.wl.seat);
    zwp_tablet_seat_v2_add_listener(_glfw.wl.tablet_seat, &tabletSeatListener, NULL);
}
// }}}

static void
keyboardHandleKeymap(void *data UNUSED, struct wl_keyboard *keyboard UNUSED, uint32_t format, int fd, uint32_t size) {
    char *mapStr;

    if (format == WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP) {
        close(fd);
        return;
    }
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Unknown keymap format: %u", format);
        close(fd);
        return;
    }

    mapStr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapStr == MAP_FAILED) {
        close(fd);
        _glfwInputError(GLFW_PLATFORM_ERROR, "Mapping of keymap file descriptor failed: %u", format);
        return;
    }
    glfw_xkb_compile_keymap(&_glfw.wl.xkb, mapStr);
    munmap(mapStr, size);
    close(fd);
}

static bool
needs_synthetic_key_repeat(void) {
    return _glfw.wl.keyboardRepeatRate > 0;
}

static void
start_key_repeat_timer(bool initial) {
#ifdef HAS_TIMER_FD
    (void)initial;
    struct itimerspec new_value = {
        .it_value = {.tv_nsec = _glfw.wl.keyboardRepeatDelay}, .it_interval = {.tv_nsec = (s_to_monotonic_t(1ll) / (monotonic_t)_glfw.wl.keyboardRepeatRate)}};
    if (_glfw.wl.eventLoopData.key_repeat_fd > -1) timerfd_settime(_glfw.wl.eventLoopData.key_repeat_fd, 0, &new_value, NULL);
#else
    monotonic_t interval = _glfw.wl.keyboardRepeatDelay;
    if (!initial) interval = (s_to_monotonic_t(1ll) / (monotonic_t)_glfw.wl.keyboardRepeatRate);
    changeTimerInterval(&_glfw.wl.eventLoopData, _glfw.wl.keyRepeatInfo.keyRepeatTimer, interval);
    toggleTimer(&_glfw.wl.eventLoopData, _glfw.wl.keyRepeatInfo.keyRepeatTimer, 1);
#endif
}

static void
stop_key_repeat_timer(void) {
#ifdef HAS_TIMER_FD
    struct itimerspec new_value = {0};
    if (_glfw.wl.eventLoopData.key_repeat_fd > -1) timerfd_settime(_glfw.wl.eventLoopData.key_repeat_fd, 0, &new_value, NULL);
#else
    toggleTimer(&_glfw.wl.eventLoopData, _glfw.wl.keyRepeatInfo.keyRepeatTimer, 0);
#endif
}

#ifndef HAS_TIMER_FD
static void
send_key_repeat_timer_event(id_type timer_id UNUSED, void *data UNUSED) {
    char b = 1;
    b += write(_glfw.wl.eventLoopData.key_repeat_fds[1], &b, 1);
    if (needs_synthetic_key_repeat()) start_key_repeat_timer(false);
}
#endif

static void
keyboardHandleEnter(void *data UNUSED, struct wl_keyboard *keyboard UNUSED, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    _GLFWwindow *window = get_window_from_surface(surface);
    _glfw.wl.keyboard_focus_parent = window ? NULL : surface;
    if (!window) {
        // The keyboard focus is on the surface of a toolkit window, it belongs to the tab showing in it
        for (_GLFWwindow *w = _glfw.windowListHead; w && !window; w = w->next) {
            if (w->wl.embedded.is_embedded && w->wl.embedded.is_active && w->wl.embedded.parent == surface) window = w;
        }
    }
    if (!window) return;

    _glfw.wl.serial = serial;
    _glfw.wl.input_serial = serial;
    _glfw.wl.keyboard_enter_serial = serial;
    _glfw.wl.keyboardFocusId = window->id;
    _glfw.wl.lastKeyboardFocusId = window->id;
    _glfwInputWindowFocus(window, true);
    uint32_t *key;
    if (keys && _glfw.wl.keyRepeatInfo.key) {
        wl_array_for_each(key, keys) {
            if (*key == _glfw.wl.keyRepeatInfo.key) {
                if (needs_synthetic_key_repeat()) start_key_repeat_timer(true);
                break;
            }
        }
    }
}

static void
keyboardHandleLeave(void *data UNUSED, struct wl_keyboard *keyboard UNUSED, uint32_t serial, struct wl_surface *surface UNUSED) {
    _GLFWwindow *window = _glfwWindowForId(_glfw.wl.keyboardFocusId);

    if (!window) return;

    _glfw.wl.serial = serial;
    _glfw.wl.keyboardFocusId = 0;
    _glfwInputWindowFocus(window, false);
    stop_key_repeat_timer();
}

static void
keyboardHandleKey(void *data UNUSED, struct wl_keyboard *keyboard UNUSED, uint32_t serial, uint32_t time UNUSED, uint32_t key, uint32_t state) {
    glfw_cancel_momentum_scroll();
    _GLFWwindow *window = _glfwWindowForId(_glfw.wl.keyboardFocusId);
    if (!window) return;
    int action = GLFW_PRESS;
    switch (state) {
        case WL_KEYBOARD_KEY_STATE_PRESSED: action = GLFW_PRESS; break;
        case WL_KEYBOARD_KEY_STATE_RELEASED: action = GLFW_RELEASE; break;
#ifdef WL_KEYBOARD_KEY_STATE_REPEATED_SINCE_VERSION
        case WL_KEYBOARD_KEY_STATE_REPEATED:
            if (needs_synthetic_key_repeat()) return;
            action = GLFW_REPEAT;
            break;
#endif
    }

    _glfw.wl.serial = serial;
    _glfw.wl.input_serial = serial;
    glfw_xkb_handle_key_event(window, &_glfw.wl.xkb, key, action);
    if (action == GLFW_PRESS && needs_synthetic_key_repeat() && glfw_xkb_should_repeat(&_glfw.wl.xkb, key)) {
        _glfw.wl.keyRepeatInfo.key = key;
        _glfw.wl.keyRepeatInfo.keyboardFocusId = window->id;
        start_key_repeat_timer(true);
    } else if (action == GLFW_RELEASE && key == _glfw.wl.keyRepeatInfo.key) {
        _glfw.wl.keyRepeatInfo.key = 0;
        stop_key_repeat_timer();
    }
}

static void
keyboardHandleModifiers(
    void *data UNUSED,
    struct wl_keyboard *keyboard UNUSED,
    uint32_t serial,
    uint32_t modsDepressed,
    uint32_t modsLatched,
    uint32_t modsLocked,
    uint32_t group) {
    _glfw.wl.serial = serial;
    _glfw.wl.input_serial = serial;
    glfw_xkb_update_modifiers(&_glfw.wl.xkb, modsDepressed, modsLatched, modsLocked, 0, 0, group);
}

static void
keyboardHandleRepeatInfo(void *data UNUSED, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    if (keyboard != _glfw.wl.keyboard) return;
    _glfw.wl.keyboardRepeatRate = rate;
    _glfw.wl.keyboardRepeatDelay = ms_to_monotonic_t(delay);
}

static const struct wl_keyboard_listener keyboardListener = {
    keyboardHandleKeymap,
    keyboardHandleEnter,
    keyboardHandleLeave,
    keyboardHandleKey,
    keyboardHandleModifiers,
    keyboardHandleRepeatInfo,
};

static void
inputGesture(GLFWGestureKind kind, GLFWGesturePhase phase, uint32_t time, double delta, int dx, int dy, uint32_t fingers) {
    _GLFWwindow *window = _glfw.wl.pointerFocus;
    if (!window) return;
    GLFWGestureEvent ev = {
        .kind = kind,
        .phase = phase,
        .keyboard_modifiers = _glfw.wl.xkb.states.modifiers,
        .timestamp = event_time_to_monotonic(time),
        .x = window->wl.cursorPosX,
        .y = window->wl.cursorPosY,
        .delta = delta,
        .dx = dx,
        .dy = dy,
        .fingers = fingers,
    };
    _glfwInputGesture(window, &ev);
}

// Fingers resting on the touchpad stop a fling, and are a gesture of their own
static void
holdGestureHandleBegin(
    void *data UNUSED,
    struct zwp_pointer_gesture_hold_v1 *gesture UNUSED,
    uint32_t serial UNUSED,
    uint32_t time,
    struct wl_surface *surface UNUSED,
    uint32_t fingers) {
    glfw_cancel_momentum_scroll();
    inputGesture(GLFW_GESTURE_HOLD, GLFW_GESTURE_BEGIN, time, 0, 0, 0, fingers);
}

static void
holdGestureHandleEnd(void *data UNUSED, struct zwp_pointer_gesture_hold_v1 *gesture UNUSED, uint32_t serial UNUSED, uint32_t time, int32_t cancelled) {
    // A hold is cancelled when its fingers start to move, which makes it some other gesture
    inputGesture(GLFW_GESTURE_HOLD, cancelled ? GLFW_GESTURE_CANCEL : GLFW_GESTURE_END, time, 0, 0, 0, 0);
}

static const struct zwp_pointer_gesture_hold_v1_listener holdGestureListener = {
    .begin = holdGestureHandleBegin,
    .end = holdGestureHandleEnd,
};

// A pinch carries both scale and rotation, reported as the two gestures macOS
// recognizes separately
static void
pinchGestureHandleBegin(
    void *data UNUSED,
    struct zwp_pointer_gesture_pinch_v1 *gesture UNUSED,
    uint32_t serial UNUSED,
    uint32_t time,
    struct wl_surface *surface UNUSED,
    uint32_t fingers) {
    _glfw.wl.pinch_scale = 1;
    inputGesture(GLFW_GESTURE_PINCH, GLFW_GESTURE_BEGIN, time, 0, 0, 0, fingers);
    inputGesture(GLFW_GESTURE_ROTATE, GLFW_GESTURE_BEGIN, time, 0, 0, 0, fingers);
}

static void
pinchGestureHandleUpdate(
    void *data UNUSED,
    struct zwp_pointer_gesture_pinch_v1 *gesture UNUSED,
    uint32_t time,
    wl_fixed_t dx UNUSED,
    wl_fixed_t dy UNUSED,
    wl_fixed_t scale,
    wl_fixed_t rotation) {
    const double s = wl_fixed_to_double(scale), r = wl_fixed_to_double(rotation);
    if (s > 0 && _glfw.wl.pinch_scale > 0) inputGesture(GLFW_GESTURE_PINCH, GLFW_GESTURE_UPDATE, time, s / _glfw.wl.pinch_scale - 1, 0, 0, 0);
    _glfw.wl.pinch_scale = s;
    if (r != 0) inputGesture(GLFW_GESTURE_ROTATE, GLFW_GESTURE_UPDATE, time, r, 0, 0, 0);
}

static void
pinchGestureHandleEnd(void *data UNUSED, struct zwp_pointer_gesture_pinch_v1 *gesture UNUSED, uint32_t serial UNUSED, uint32_t time, int32_t cancelled) {
    const GLFWGesturePhase phase = cancelled ? GLFW_GESTURE_CANCEL : GLFW_GESTURE_END;
    inputGesture(GLFW_GESTURE_PINCH, phase, time, 0, 0, 0, 0);
    inputGesture(GLFW_GESTURE_ROTATE, phase, time, 0, 0, 0, 0);
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinchGestureListener = {
    .begin = pinchGestureHandleBegin,
    .update = pinchGestureHandleUpdate,
    .end = pinchGestureHandleEnd,
};

// A swipe is reported once it ends, as its dominant direction
static void
swipeGestureHandleBegin(
    void *data UNUSED,
    struct zwp_pointer_gesture_swipe_v1 *gesture UNUSED,
    uint32_t serial UNUSED,
    uint32_t time UNUSED,
    struct wl_surface *surface UNUSED,
    uint32_t fingers) {
    _glfw.wl.swipe_dx = 0;
    _glfw.wl.swipe_dy = 0;
    _glfw.wl.swipe_fingers = fingers;
}

static void
swipeGestureHandleUpdate(void *data UNUSED, struct zwp_pointer_gesture_swipe_v1 *gesture UNUSED, uint32_t time UNUSED, wl_fixed_t dx, wl_fixed_t dy) {
    _glfw.wl.swipe_dx += wl_fixed_to_double(dx);
    _glfw.wl.swipe_dy += wl_fixed_to_double(dy);
}

static void
swipeGestureHandleEnd(void *data UNUSED, struct zwp_pointer_gesture_swipe_v1 *gesture UNUSED, uint32_t serial UNUSED, uint32_t time, int32_t cancelled) {
    const double ax = fabs(_glfw.wl.swipe_dx), ay = fabs(_glfw.wl.swipe_dy);
    if (cancelled || (ax < 1 && ay < 1)) return;
    const int dx = ax >= ay ? (_glfw.wl.swipe_dx > 0 ? 1 : -1) : 0, dy = ax < ay ? (_glfw.wl.swipe_dy > 0 ? 1 : -1) : 0;
    inputGesture(GLFW_GESTURE_SWIPE, GLFW_GESTURE_BEGIN, time, 0, dx, dy, _glfw.wl.swipe_fingers);
    inputGesture(GLFW_GESTURE_SWIPE, GLFW_GESTURE_END, time, 0, dx, dy, 0);
}

static const struct zwp_pointer_gesture_swipe_v1_listener swipeGestureListener = {
    .begin = swipeGestureHandleBegin,
    .update = swipeGestureHandleUpdate,
    .end = swipeGestureHandleEnd,
};

static void
seatHandleCapabilities(void *data UNUSED, struct wl_seat *seat, enum wl_seat_capability caps) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !_glfw.wl.pointer) {
        _glfw.wl.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(_glfw.wl.pointer, &pointerListener, NULL);
        if (_glfw.wl.wp_cursor_shape_manager_v1) {
            if (_glfw.wl.wp_cursor_shape_device_v1) wp_cursor_shape_device_v1_destroy(_glfw.wl.wp_cursor_shape_device_v1);
            _glfw.wl.wp_cursor_shape_device_v1 = NULL;
            _glfw.wl.wp_cursor_shape_device_v1 = wp_cursor_shape_manager_v1_get_pointer(_glfw.wl.wp_cursor_shape_manager_v1, _glfw.wl.pointer);
        }
        if (_glfw.wl.pointer_gestures) {
            _glfw.wl.pointer_gesture_hold = zwp_pointer_gestures_v1_get_hold_gesture(_glfw.wl.pointer_gestures, _glfw.wl.pointer);
            zwp_pointer_gesture_hold_v1_add_listener(_glfw.wl.pointer_gesture_hold, &holdGestureListener, NULL);
            _glfw.wl.pointer_gesture_pinch = zwp_pointer_gestures_v1_get_pinch_gesture(_glfw.wl.pointer_gestures, _glfw.wl.pointer);
            zwp_pointer_gesture_pinch_v1_add_listener(_glfw.wl.pointer_gesture_pinch, &pinchGestureListener, NULL);
            _glfw.wl.pointer_gesture_swipe = zwp_pointer_gestures_v1_get_swipe_gesture(_glfw.wl.pointer_gestures, _glfw.wl.pointer);
            zwp_pointer_gesture_swipe_v1_add_listener(_glfw.wl.pointer_gesture_swipe, &swipeGestureListener, NULL);
        }
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && _glfw.wl.pointer) {
        if (_glfw.wl.wp_cursor_shape_device_v1) wp_cursor_shape_device_v1_destroy(_glfw.wl.wp_cursor_shape_device_v1);
        _glfw.wl.wp_cursor_shape_device_v1 = NULL;
        if (_glfw.wl.pointer_gesture_hold) {
            zwp_pointer_gesture_hold_v1_destroy(_glfw.wl.pointer_gesture_hold);
            _glfw.wl.pointer_gesture_hold = NULL;
        }
        if (_glfw.wl.pointer_gesture_pinch) {
            zwp_pointer_gesture_pinch_v1_destroy(_glfw.wl.pointer_gesture_pinch);
            _glfw.wl.pointer_gesture_pinch = NULL;
        }
        if (_glfw.wl.pointer_gesture_swipe) {
            zwp_pointer_gesture_swipe_v1_destroy(_glfw.wl.pointer_gesture_swipe);
            _glfw.wl.pointer_gesture_swipe = NULL;
        }
        for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) cancelFingerScroll(w);
        wl_pointer_destroy(_glfw.wl.pointer);
        _glfw.wl.pointer = NULL;
        if (_glfw.wl.cursorAnimationTimer) toggleTimer(&_glfw.wl.eventLoopData, _glfw.wl.cursorAnimationTimer, 0);
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !_glfw.wl.touch) {
        _glfw.wl.touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(_glfw.wl.touch, &touchListener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && _glfw.wl.touch) {
        touchHandleCancel(NULL, _glfw.wl.touch);
        wl_touch_destroy(_glfw.wl.touch);
        _glfw.wl.touch = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !_glfw.wl.keyboard) {
        _glfw.wl.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(_glfw.wl.keyboard, &keyboardListener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && _glfw.wl.keyboard) {
        wl_keyboard_destroy(_glfw.wl.keyboard);
        _glfw.wl.keyboard = NULL;
        _glfw.wl.keyboardFocusId = 0;
        stop_key_repeat_timer();
    }
}

static void
seatHandleName(void *data UNUSED, struct wl_seat *seat UNUSED, const char *name UNUSED) {}

static const struct wl_seat_listener seatListener = {
    seatHandleCapabilities,
    seatHandleName,
};
static void
wmBaseHandlePing(void *data UNUSED, struct xdg_wm_base *wmBase, uint32_t serial) {
    xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener wmBaseListener = {wmBaseHandlePing};

static void
extBackgroundEffectHandleCapabilities(void *data UNUSED, struct ext_background_effect_manager_v1 *manager UNUSED, uint32_t flags) {
    _glfw.wl.ext_background_effect_capabilities = flags;
}

static const struct ext_background_effect_manager_v1_listener extBackgroundEffectManagerListener = {extBackgroundEffectHandleCapabilities};

static void
registryHandleGlobal(void *data UNUSED, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
#define is(x) strcmp(interface, x##_interface.name) == 0
    if (is(wl_compositor)) {
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
        _glfw.wl.compositorVersion = MIN(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION, (int)version);
        _glfw.wl.has_preferred_buffer_scale = _glfw.wl.compositorVersion >= WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION;
#else
        _glfw.wl.compositorVersion = MIN(3, (int)version);
#endif
        _glfw.wl.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, _glfw.wl.compositorVersion);
    } else if (is(wl_subcompositor)) {
        _glfw.wl.subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    } else if (is(wl_shm)) {
        _glfw.wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (is(wl_output)) {
        _glfwAddOutputWayland(name, version);
    } else if (is(wl_seat)) {
        if (!_glfw.wl.seat) {
            _glfw.wl.has_key_repeat_events = false;
#if defined(WL_KEYBOARD_KEY_STATE_REPEATED_SINCE_VERSION)
            _glfw.wl.seatVersion = MIN(WL_KEYBOARD_KEY_STATE_REPEATED_SINCE_VERSION, (int)version);
            _glfw.wl.has_key_repeat_events = _glfw.wl.seatVersion >= WL_KEYBOARD_KEY_STATE_REPEATED_SINCE_VERSION;
#elif defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
            _glfw.wl.seatVersion = MIN(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION, (int)version);
#elif defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
            _glfw.wl.seatVersion = MIN(WL_POINTER_AXIS_VALUE120_SINCE_VERSION, (int)version);
#else
            _glfw.wl.seatVersion = MIN(WL_POINTER_AXIS_DISCRETE_SINCE_VERSION, version);
#endif
            _glfw.wl.seat = wl_registry_bind(registry, name, &wl_seat_interface, _glfw.wl.seatVersion);
            wl_seat_add_listener(_glfw.wl.seat, &seatListener, NULL);
        }
        if (_glfw.wl.seat) {
            if (_glfw.wl.dataDeviceManager && !_glfw.wl.dataDevice) _glfwSetupWaylandDataDevice();
            if (_glfw.wl.primarySelectionDeviceManager && !_glfw.wl.primarySelectionDevice) { _glfwSetupWaylandPrimarySelectionDevice(); }
            setupTabletSeat();
        }
    } else if (is(xdg_wm_base)) {
        _glfw.wl.xdg_wm_base_version = 1;
#ifdef XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION
        _glfw.wl.xdg_wm_base_version = MIN(XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION, (int)version);
#elif defined(XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
        _glfw.wl.xdg_wm_base_version = MIN(XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION, (int)version);
#endif
        _glfw.wl.wmBase = wl_registry_bind(registry, name, &xdg_wm_base_interface, _glfw.wl.xdg_wm_base_version);
        xdg_wm_base_add_listener(_glfw.wl.wmBase, &wmBaseListener, NULL);
    } else if (is(zxdg_decoration_manager_v1)) {
        _glfw.wl.decorationManager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (is(zwp_relative_pointer_manager_v1)) {
        _glfw.wl.relativePointerManager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
    } else if (is(zwp_pointer_constraints_v1)) {
        _glfw.wl.pointerConstraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (is(zwp_text_input_manager_v3)) {
        _glfwWaylandBindTextInput(registry, name);
    } else if (is(wl_data_device_manager)) {
        _glfw.wl.dataDeviceManager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
        if (_glfw.wl.seat && _glfw.wl.dataDeviceManager && !_glfw.wl.dataDevice) { _glfwSetupWaylandDataDevice(); }
    } else if (is(zwp_primary_selection_device_manager_v1)) {
        _glfw.wl.primarySelectionDeviceManager = wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
        if (_glfw.wl.seat && _glfw.wl.primarySelectionDeviceManager && !_glfw.wl.primarySelectionDevice) { _glfwSetupWaylandPrimarySelectionDevice(); }
    } else if (is(wp_single_pixel_buffer_manager_v1)) {
        _glfw.wl.wp_single_pixel_buffer_manager_v1 = wl_registry_bind(registry, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
    } else if (is(xdg_activation_v1)) {
        _glfw.wl.xdg_activation_v1 = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
    } else if (is(wp_cursor_shape_manager_v1)) {
        _glfw.wl.wp_cursor_shape_manager_v1 = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (is(wp_fractional_scale_manager_v1)) {
        _glfw.wl.wp_fractional_scale_manager_v1 = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (is(wp_color_manager_v1)) {
        _glfwWaylandBindColorManager(registry, name, version);
    } else if (is(wp_viewporter)) {
        _glfw.wl.wp_viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (is(org_kde_kwin_blur_manager)) {
        _glfw.wl.org_kde_kwin_blur_manager = wl_registry_bind(registry, name, &org_kde_kwin_blur_manager_interface, 1);
    } else if (is(ext_background_effect_manager_v1)) {
        _glfw.wl.ext_background_effect_manager_v1 = wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, 1);
        ext_background_effect_manager_v1_add_listener(_glfw.wl.ext_background_effect_manager_v1, &extBackgroundEffectManagerListener, NULL);
    } else if (is(zwlr_layer_shell_v1)) {
        if (version >= 4) {
            _glfw.wl.zwlr_layer_shell_v1_version = version;
            _glfw.wl.zwlr_layer_shell_v1 = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version);
        }
    } else if (is(zwp_idle_inhibit_manager_v1)) {
        _glfw.wl.idle_inhibit_manager = wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (is(zwp_keyboard_shortcuts_inhibit_manager_v1)) {
        _glfw.wl.keyboard_shortcuts_inhibit_manager = wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    } else if (is(zwp_tablet_manager_v2)) {
        // Version 2 is where the dials of a pad are
        _glfw.wl.tablet_manager = wl_registry_bind(registry, name, &zwp_tablet_manager_v2_interface, MIN(version, 2u));
        setupTabletSeat();
    } else if (is(zwp_pointer_gestures_v1)) {
        if (version >= 3) _glfw.wl.pointer_gestures = wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, 3);
    } else if (is(xdg_toplevel_icon_manager_v1)) {
        _glfw.wl.xdg_toplevel_icon_manager_v1 = wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (is(xdg_system_bell_v1)) {
        _glfw.wl.xdg_system_bell_v1 = wl_registry_bind(registry, name, &xdg_system_bell_v1_interface, 1);
    } else if (is(xdg_toplevel_tag_manager_v1)) {
        _glfw.wl.xdg_toplevel_tag_manager_v1 = wl_registry_bind(registry, name, &xdg_toplevel_tag_manager_v1_interface, 1);
    } else if (is(xdg_toplevel_drag_manager_v1)) {
        _glfw.wl.xdg_toplevel_drag_manager_v1 = wl_registry_bind(registry, name, &xdg_toplevel_drag_manager_v1_interface, 1);
    } else if (is(zxdg_output_manager_v1)) {
        _glfw.wl.xdg_output_manager = wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, MIN(version, 3u));
        _glfwCreateXdgOutputsWayland();
    } else if (is(zwlr_virtual_pointer_manager_v1)) {
        _glfw.wl.virtual_pointer_manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    } else if (is(zwp_virtual_keyboard_manager_v1)) {
        _glfw.wl.virtual_keyboard_manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    }
#undef is
}

static void
registryHandleGlobalRemove(void *data UNUSED, struct wl_registry *registry UNUSED, uint32_t name) {
    _glfwRemoveOutputWayland(name);
}


static const struct wl_registry_listener registryListener = {registryHandleGlobal, registryHandleGlobalRemove};

GLFWAPI GLFWColorScheme
glfwGetCurrentSystemColorTheme(bool query_if_unintialized) {
    return glfw_current_system_color_theme(query_if_unintialized);
}

static pid_t
get_socket_peer_pid(int fd) {
    (void)fd;
#ifdef __linux__
    struct ucred ucred;
    socklen_t len = sizeof(struct ucred);
    return (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) ? -1 : ucred.pid;
#elif defined(LOCAL_PEERCRED) && defined(XUCRED_VERSION)
    struct xucred peercred;
    socklen_t peercredlen = sizeof(peercred);
    return (getsockopt(c->fd, LOCAL_PEERCRED, 1, (void *)&peercred, &peercredlen) == 0 && peercred.cr_version == XUCRED_VERSION) ? peercred.cr_pid : -1;
#elif defined(LOCAL_PEERPID)
    pid_t pid;
    socklen_t pid_size = sizeof(pid);
    return getsockopt(client, SOL_LOCAL, LOCAL_PEERPID, &pid, &pid_size) == -1 ? -1 : pid;
#else
    errno = ENOSYS;
    return -1;
#endif
}

GLFWAPI pid_t
glfwWaylandCompositorPID(void) {
    if (!_glfw.wl.display) return -1;
    int fd = wl_display_get_fd(_glfw.wl.display);
    if (fd < 0) return -1;
    return get_socket_peer_pid(fd);
}

const char *
_glfwWaylandCompositorName(void) {
    static bool probed = false;
    if (!probed) {
        probed = true;
        static const size_t sz = 1024;
        _glfw.wl.compositor_name = malloc(sz);
        if (!_glfw.wl.compositor_name) return "";
        char *ans = _glfw.wl.compositor_name;
        ans[0] = 0;
        pid_t cpid = glfwWaylandCompositorPID();
        if (cpid < 0) return ans;
        snprintf(ans, sz, "/proc/%d/cmdline", cpid);
        int fd = open(ans, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            ans[0] = 0;
        } else {
            ssize_t n;
            while (true) {
                n = read(fd, ans, sz - 1);
                if (n < 0 && errno == EINTR) continue;
                close(fd);
                break;
            }
            ans[n < 0 ? 0 : n] = 0;
        }
    }
    return _glfw.wl.compositor_name ? _glfw.wl.compositor_name : "";
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

static const char *
get_compositor_missing_capabilities(void) {
#define C(title, x) \
    if (!_glfw.wl.x) p += snprintf(p, sizeof(buf) - (p - buf), "%s ", #title);
    static char buf[512];
    char *p = buf;
    *p = 0;
    C(viewporter, wp_viewporter);
    C(fractional_scale, wp_fractional_scale_manager_v1);
    C(color_management, color.manager);
    if (!_glfw.wl.org_kde_kwin_blur_manager && !_glfw.wl.ext_background_effect_manager_v1) p += snprintf(p, sizeof(buf) - (p - buf), "%s ", "blur");
    C(server_side_decorations, decorationManager);
    C(cursor_shape, wp_cursor_shape_manager_v1);
    C(layer_shell, zwlr_layer_shell_v1);
    C(single_pixel_buffer, wp_single_pixel_buffer_manager_v1);
    C(preferred_scale, has_preferred_buffer_scale);
    C(idle_inhibit, idle_inhibit_manager);
    C(icon, xdg_toplevel_icon_manager_v1);
    C(bell, xdg_system_bell_v1);
    C(window - tag, xdg_toplevel_tag_manager_v1);
    C(keyboard_shortcuts_inhibit, keyboard_shortcuts_inhibit_manager);
    C(key - repeat, has_key_repeat_events);
    C(top_level_drag, xdg_toplevel_drag_manager_v1);
    C(output_manager, xdg_output_manager);
    C(pointer_gestures, pointer_gestures);
#define P(x) p += snprintf(p, sizeof(buf) - (p - buf), "%s ", x);
    if (_glfw.wl.xdg_wm_base_version < 6) P("window-state-suspended");
    if (_glfw.wl.xdg_wm_base_version < 5) P("window-capabilities");
#undef P
#undef C
    while (p > buf && (p - 1)[0] == ' ') {
        p--;
        *p = 0;
    }
    return buf;
}

GLFWAPI const char *
glfwWaylandMissingCapabilities(void) {
    return get_compositor_missing_capabilities();
}

static struct wl_display *external_display = NULL;

GLFWAPI void
glfwWaylandSetExternalDisplay(void *display) {
    external_display = display;
}

int
_glfwPlatformInit(bool *supports_window_occlusion) {
    int i;
    _GLFWmonitor *monitor;

    _glfw.wl.cursor.handle = _glfw_dlopen("libwayland-cursor.so.0");
    if (!_glfw.wl.cursor.handle) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Failed to open libwayland-cursor");
        return false;
    }

    glfw_dlsym(_glfw.wl.cursor.theme_load, _glfw.wl.cursor.handle, "wl_cursor_theme_load");
    glfw_dlsym(_glfw.wl.cursor.theme_destroy, _glfw.wl.cursor.handle, "wl_cursor_theme_destroy");
    glfw_dlsym(_glfw.wl.cursor.theme_get_cursor, _glfw.wl.cursor.handle, "wl_cursor_theme_get_cursor");
    glfw_dlsym(_glfw.wl.cursor.image_get_buffer, _glfw.wl.cursor.handle, "wl_cursor_image_get_buffer");

    _glfw.wl.egl.handle = _glfw_dlopen("libwayland-egl.so.1");
    if (!_glfw.wl.egl.handle) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Failed to open libwayland-egl");
        return false;
    }

    glfw_dlsym(_glfw.wl.egl.window_create, _glfw.wl.egl.handle, "wl_egl_window_create");
    glfw_dlsym(_glfw.wl.egl.window_destroy, _glfw.wl.egl.handle, "wl_egl_window_destroy");
    glfw_dlsym(_glfw.wl.egl.window_resize, _glfw.wl.egl.handle, "wl_egl_window_resize");

    if (external_display) {
        _glfw.wl.display = external_display;
        _glfw.wl.display_is_external = true;
        _glfw.wl.queue = wl_display_create_queue(_glfw.wl.display);
        _glfw.wl.display_wrapper = wl_proxy_create_wrapper(_glfw.wl.display);
        if (!_glfw.wl.queue || !_glfw.wl.display_wrapper) {
            _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Failed to create an event queue on the shared display");
            return false;
        }
        wl_proxy_set_queue((struct wl_proxy *)_glfw.wl.display_wrapper, _glfw.wl.queue);
    } else _glfw.wl.display = wl_display_connect(NULL);
    if (!_glfw.wl.display) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Failed to connect to display");
        return false;
    }
    if (!initPollData(&_glfw.wl.eventLoopData, wl_display_get_fd(_glfw.wl.display))) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Failed to initialize event loop data");
    }
    glfw_dbus_init(&_glfw.wl.dbus, &_glfw.wl.eventLoopData);
    glfw_initialize_desktop_settings();
#ifndef HAS_TIMER_FD
    _glfw.wl.keyRepeatInfo.keyRepeatTimer =
        addTimer(&_glfw.wl.eventLoopData, "wayland-key-repeat", ms_to_monotonic_t(500ll), 0, true, send_key_repeat_timer_event, NULL, NULL);
#endif
    _glfw.wl.cursorAnimationTimer =
        addTimer(&_glfw.wl.eventLoopData, "wayland-cursor-animation", ms_to_monotonic_t(500ll), 0, true, animateCursorImage, NULL, NULL);

    _glfw.wl.registry = wl_display_get_registry(wl_glfw_display_for_requests());
    wl_registry_add_listener(_glfw.wl.registry, &registryListener, NULL);

    if (!glfw_xkb_create_context(&_glfw.wl.xkb)) return false;

    // Sync so we got all registry objects
    wl_glfw_roundtrip();
    _glfwWaylandInitTextInput();

    // Sync so we got all initial output events
    wl_glfw_roundtrip();

    for (i = 0; i < _glfw.monitorCount; ++i) {
        monitor = _glfw.monitors[i];
        if (monitor->widthMM <= 0 || monitor->heightMM <= 0) {
            // If Wayland does not provide a physical size, assume the default 96 DPI
            monitor->widthMM = (int)(monitor->modes[monitor->wl.currentMode].width * 25.4f / 96.f);
            monitor->heightMM = (int)(monitor->modes[monitor->wl.currentMode].height * 25.4f / 96.f);
            monitor->physicalSizeAssumed = true;
        }
    }

    if (!_glfw.wl.wmBase) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Failed to find xdg-shell in your compositor");
        return false;
    }

    if (_glfw.wl.shm) {
        _glfw.wl.cursorSurface = wl_compositor_create_surface(_glfw.wl.compositor);
    } else {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Failed to find Wayland SHM");
        return false;
    }
    if (_glfw.hints.init.debugRendering) {
        const char *mc = get_compositor_missing_capabilities();
        if (mc && mc[0]) debug("Compositor missing capabilities: %s\n", mc);
    }
    *supports_window_occlusion = _glfw.wl.xdg_wm_base_version > 5;

    return true;
}

void
_glfwPlatformTerminate(void) {
    if (_glfw.wl.activation_requests.array) {
        for (size_t i = 0; i < _glfw.wl.activation_requests.sz; i++) {
            glfw_wl_xdg_activation_request *r = _glfw.wl.activation_requests.array + i;
            if (r->callback) r->callback(NULL, NULL, r->callback_data);
            xdg_activation_token_v1_destroy(r->token);
        }
        free(_glfw.wl.activation_requests.array);
    }
    _glfwFreePendingOutputsWayland();
    glfw_xkb_release(&_glfw.wl.xkb);
    glfw_dbus_terminate(&_glfw.wl.dbus);

    glfw_wlc_destroy();
    if (_glfw.wl.cursor.handle) {
        _glfw_dlclose(_glfw.wl.cursor.handle);
        _glfw.wl.cursor.handle = NULL;
    }

    if (_glfw.wl.cursorSurface) wl_surface_destroy(_glfw.wl.cursorSurface);
    if (_glfw.wl.subcompositor) wl_subcompositor_destroy(_glfw.wl.subcompositor);
    if (_glfw.wl.compositor) wl_compositor_destroy(_glfw.wl.compositor);
    if (_glfw.wl.shm) wl_shm_destroy(_glfw.wl.shm);
    if (_glfw.wl.decorationManager) zxdg_decoration_manager_v1_destroy(_glfw.wl.decorationManager);
    if (_glfw.wl.wmBase) xdg_wm_base_destroy(_glfw.wl.wmBase);
    if (_glfw.wl.virtual_pointer) zwlr_virtual_pointer_v1_destroy(_glfw.wl.virtual_pointer);
    if (_glfw.wl.virtual_pointer_manager) zwlr_virtual_pointer_manager_v1_destroy(_glfw.wl.virtual_pointer_manager);
    if (_glfw.wl.virtual_keyboard) zwp_virtual_keyboard_v1_destroy(_glfw.wl.virtual_keyboard);
    if (_glfw.wl.virtual_keyboard_manager) zwp_virtual_keyboard_manager_v1_destroy(_glfw.wl.virtual_keyboard_manager);
    if (_glfw.wl.pointer) wl_pointer_destroy(_glfw.wl.pointer);
    if (_glfw.wl.keyboard) wl_keyboard_destroy(_glfw.wl.keyboard);
    if (_glfw.wl.touch) wl_touch_destroy(_glfw.wl.touch);
    if (_glfw.wl.seat) wl_seat_destroy(_glfw.wl.seat);
    if (_glfw.wl.relativePointerManager) zwp_relative_pointer_manager_v1_destroy(_glfw.wl.relativePointerManager);
    if (_glfw.wl.pointerConstraints) zwp_pointer_constraints_v1_destroy(_glfw.wl.pointerConstraints);
    _glfwWaylandDestroyTextInput();
    if (_glfw.wl.dataSourceForClipboard) wl_data_source_destroy(_glfw.wl.dataSourceForClipboard);
    if (_glfw.wl.dataSourceForPrimarySelection) zwp_primary_selection_source_v1_destroy(_glfw.wl.dataSourceForPrimarySelection);
    for (size_t doi = 0; doi < arraysz(_glfw.wl.untyped_data_offers); doi++) {
        if (_glfw.wl.untyped_data_offers[doi].id) { destroy_data_offer(&_glfw.wl.untyped_data_offers[doi]); }
    }
    if (_glfw.wl.primary_data_offer.id) destroy_data_offer(&_glfw.wl.primary_data_offer);
    if (_glfw.wl.clipboard_data_offer.id) destroy_data_offer(&_glfw.wl.clipboard_data_offer);
    if (_glfw.wl.drop_data_offer.id) destroy_data_offer(&_glfw.wl.drop_data_offer);
    if (_glfw.wl.dataDevice) wl_data_device_destroy(_glfw.wl.dataDevice);
    if (_glfw.wl.dataDeviceManager) wl_data_device_manager_destroy(_glfw.wl.dataDeviceManager);
    if (_glfw.wl.primarySelectionDevice) zwp_primary_selection_device_v1_destroy(_glfw.wl.primarySelectionDevice);
    if (_glfw.wl.primarySelectionDeviceManager) zwp_primary_selection_device_manager_v1_destroy(_glfw.wl.primarySelectionDeviceManager);
    if (_glfw.wl.xdg_activation_v1) xdg_activation_v1_destroy(_glfw.wl.xdg_activation_v1);
    if (_glfw.wl.xdg_toplevel_icon_manager_v1) xdg_toplevel_icon_manager_v1_destroy(_glfw.wl.xdg_toplevel_icon_manager_v1);
    if (_glfw.wl.xdg_system_bell_v1) xdg_system_bell_v1_destroy(_glfw.wl.xdg_system_bell_v1);
    if (_glfw.wl.xdg_toplevel_tag_manager_v1) xdg_toplevel_tag_manager_v1_destroy(_glfw.wl.xdg_toplevel_tag_manager_v1);
    if (_glfw.wl.xdg_toplevel_drag_manager_v1) xdg_toplevel_drag_manager_v1_destroy(_glfw.wl.xdg_toplevel_drag_manager_v1);
    if (_glfw.wl.xdg_output_manager) zxdg_output_manager_v1_destroy(_glfw.wl.xdg_output_manager);
    if (_glfw.wl.wp_single_pixel_buffer_manager_v1) wp_single_pixel_buffer_manager_v1_destroy(_glfw.wl.wp_single_pixel_buffer_manager_v1);
    if (_glfw.wl.wp_cursor_shape_manager_v1) wp_cursor_shape_manager_v1_destroy(_glfw.wl.wp_cursor_shape_manager_v1);
    if (_glfw.wl.wp_viewporter) wp_viewporter_destroy(_glfw.wl.wp_viewporter);
    if (_glfw.wl.wp_fractional_scale_manager_v1) wp_fractional_scale_manager_v1_destroy(_glfw.wl.wp_fractional_scale_manager_v1);
    _glfwWaylandTerminateColorManager();
    if (_glfw.wl.org_kde_kwin_blur_manager) org_kde_kwin_blur_manager_destroy(_glfw.wl.org_kde_kwin_blur_manager);
    if (_glfw.wl.ext_background_effect_manager_v1) ext_background_effect_manager_v1_destroy(_glfw.wl.ext_background_effect_manager_v1);
    if (_glfw.wl.zwlr_layer_shell_v1) zwlr_layer_shell_v1_destroy(_glfw.wl.zwlr_layer_shell_v1);
    if (_glfw.wl.idle_inhibit_manager) zwp_idle_inhibit_manager_v1_destroy(_glfw.wl.idle_inhibit_manager);
    if (_glfw.wl.keyboard_shortcuts_inhibit_manager) zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(_glfw.wl.keyboard_shortcuts_inhibit_manager);
    if (_glfw.wl.pointer_gesture_hold) zwp_pointer_gesture_hold_v1_destroy(_glfw.wl.pointer_gesture_hold);
    if (_glfw.wl.pointer_gestures) zwp_pointer_gestures_v1_release(_glfw.wl.pointer_gestures);
    while (_glfw.wl.tablet_pads) destroyTabletPad(_glfw.wl.tablet_pads);
    if (_glfw.wl.tablet_seat) zwp_tablet_seat_v2_destroy(_glfw.wl.tablet_seat);
    if (_glfw.wl.tablet_manager) zwp_tablet_manager_v2_destroy(_glfw.wl.tablet_manager);

    if (_glfw.wl.registry) wl_registry_destroy(_glfw.wl.registry);
    // Call eglTerminate before disconnecting from the display: eglTerminate
    // sends Wayland protocol requests which must be flushed and their closures
    // drained before we unload the EGL library.  The dlclose calls move to
    // after wl_display_disconnect so the message format tables remain mapped
    // while the queue is drained, preventing a segfault in
    // destroy_queued_closure().
    if (_glfw.egl.display) {
        eglTerminate(_glfw.egl.display);
        _glfw.egl.display = EGL_NO_DISPLAY;
    }
    if (_glfw.wl.display_wrapper) wl_proxy_wrapper_destroy(_glfw.wl.display_wrapper);
    if (_glfw.wl.queue) wl_event_queue_destroy(_glfw.wl.queue);
    if (_glfw.wl.display) {
        wl_display_flush(_glfw.wl.display);
        if (!_glfw.wl.display_is_external) wl_display_disconnect(_glfw.wl.display);
        _glfw.wl.display = NULL;
    }
    if (_glfw.egl.handle) {
        _glfw_dlclose(_glfw.egl.handle);
        _glfw.egl.handle = NULL;
    }
    if (_glfw.wl.egl.handle) {
        _glfw_dlclose(_glfw.wl.egl.handle);
        _glfw.wl.egl.handle = NULL;
    }
    finalizePollData(&_glfw.wl.eventLoopData);
    if (_glfw.wl.compositor_name) {
        free(_glfw.wl.compositor_name);
        _glfw.wl.compositor_name = NULL;
    }
}

#define GLFW_LOOP_BACKEND wl
#include "main_loop.h"

const char *
_glfwPlatformGetVersionString(void) {
    (void)keep_going;
    return _GLFW_VERSION_NUMBER " Wayland EGL OSMesa"
#if defined(_POSIX_TIMERS) && defined(_POSIX_MONOTONIC_CLOCK)
                                " clock_gettime"
#else
                                " gettimeofday"
#endif
                                " evdev"
#if defined(_GLFW_BUILD_DLL)
                                " shared"
#endif
        ;
}
