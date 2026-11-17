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

#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>


static void
outputHandleGeometry(
    void *data,
    struct wl_output *output UNUSED,
    int32_t x,
    int32_t y,
    int32_t physicalWidth,
    int32_t physicalHeight,
    int32_t subpixel UNUSED,
    const char *make UNUSED,
    const char *model UNUSED,
    int32_t transform) {
    struct _GLFWmonitor *monitor = data;
    monitor->wl.x = x;
    monitor->wl.y = y;
    monitor->widthMM = physicalWidth;
    monitor->heightMM = physicalHeight;
    monitor->physicalSizeAssumed = false;
    monitor->wl.transform = transform;
}

static void
outputHandleMode(void *data, struct wl_output *output UNUSED, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    struct _GLFWmonitor *monitor = data;
    GLFWvidmode mode;

    mode.width = width;
    mode.height = height;
    mode.redBits = 8;
    mode.greenBits = 8;
    mode.blueBits = 8;
    mode.refreshRate = (int)round(refresh / 1000.0);

    monitor->modeCount++;
    monitor->modes = realloc(monitor->modes, monitor->modeCount * sizeof(GLFWvidmode));
    monitor->modes[monitor->modeCount - 1] = mode;

    if (flags & WL_OUTPUT_MODE_CURRENT) monitor->wl.currentMode = monitor->modeCount - 1;
}

static bool
removePendingMonitor(_GLFWmonitor *monitor) {
    for (int i = 0; i < _glfw.wl.pending_monitors.count; i++) {
        if (_glfw.wl.pending_monitors.array[i] == monitor) {
            remove_i_from_array(_glfw.wl.pending_monitors.array, i, _glfw.wl.pending_monitors.count);
            return true;
        }
    }
    return false;
}

static void
outputHandleDone(void *data, struct wl_output *output UNUSED) {
    struct _GLFWmonitor *monitor = data;
    // The color manager can be bound after the output, before the output is in the list of monitors
    _glfwWaylandColorManagementCreateOutput(monitor);
    // Only monitors still pending need to be announced, done is re-sent
    // whenever output properties change.
    if (removePendingMonitor(monitor)) _glfwInputMonitor(monitor, GLFW_CONNECTED, _GLFW_INSERT_LAST);
}

static void
outputHandleScale(void *data, struct wl_output *output UNUSED, int32_t factor) {
    struct _GLFWmonitor *monitor = data;
    if (factor > 0 && factor < 24) monitor->wl.scale = factor;
}

static void
outputHandleName(void *data, struct wl_output *output UNUSED, const char *name) {
    struct _GLFWmonitor *monitor = data;
    if (name) {
        if (monitor->name) free((void *)monitor->name);
        monitor->name = _glfw_strdup(name);
    }
}

static void
outputHandleDescription(void *data, struct wl_output *output UNUSED, const char *description) {
    struct _GLFWmonitor *monitor = data;
    if (description) {
        if (monitor->description) free((void *)monitor->description);
        monitor->description = _glfw_strdup(description);
    }
}

static const struct wl_output_listener outputListener = {
    outputHandleGeometry,
    outputHandleMode,
    outputHandleDone,
    outputHandleScale,
    outputHandleName,
    outputHandleDescription,
};


static void xdgGeometryMaybeArrived(_GLFWmonitor *monitor);

static void
xdgOutputHandleLogicalPosition(void *data, struct zxdg_output_v1 *xdg_output UNUSED, int32_t x, int32_t y) {
    struct _GLFWmonitor *monitor = data;
    monitor->wl.x = x;
    monitor->wl.y = y;
    monitor->wl.xdg_position_received = true;
    xdgGeometryMaybeArrived(monitor);
}

static void
xdgOutputHandleLogicalSize(void *data, struct zxdg_output_v1 *xdg_output UNUSED, int32_t width, int32_t height) {
    struct _GLFWmonitor *monitor = data;
    monitor->wl.xdg_logical_width = width;
    monitor->wl.xdg_logical_height = height;
    monitor->wl.xdg_size_received = true;
    xdgGeometryMaybeArrived(monitor);
}

static void
computeXdgFractionalScale(struct _GLFWmonitor *monitor) {
    if (monitor->wl.xdg_logical_width <= 0 || monitor->wl.xdg_logical_height <= 0) return;
    if (monitor->modeCount == 0 || monitor->wl.currentMode < 0) return;
    GLFWvidmode *mode = &monitor->modes[monitor->wl.currentMode];
    if (mode->width <= 0 || mode->height <= 0) return;

    // For 90° and 270° rotations the compositor maps physical width↔height,
    // so use the swapped axis when dividing by the logical size.
    int phys_w = mode->width, phys_h = mode->height;
    int32_t t = monitor->wl.transform;
    if (t == WL_OUTPUT_TRANSFORM_90 || t == WL_OUTPUT_TRANSFORM_270 || t == WL_OUTPUT_TRANSFORM_FLIPPED_90 || t == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        int tmp = phys_w;
        phys_w = phys_h;
        phys_h = tmp;
    }

    double sx = (double)phys_w / monitor->wl.xdg_logical_width;
    double sy = (double)phys_h / monitor->wl.xdg_logical_height;
    // Use the average in case of minor rounding differences between axes.
    monitor->wl.fractional_scale = (sx + sy) * 0.5;
}

static void
xdgOutputHandleDone(void *data, struct zxdg_output_v1 *xdg_output UNUSED) {
    struct _GLFWmonitor *monitor = data;
    computeXdgFractionalScale(monitor);
}

static void
xdgOutputHandleName(void *data, struct zxdg_output_v1 *xdg_output UNUSED, const char *name) {
    // wl_output already provides the name; skip to avoid duplicate frees.
    (void)data;
    (void)name;
}

static void
xdgOutputHandleDescription(void *data, struct zxdg_output_v1 *xdg_output UNUSED, const char *description) {
    (void)data;
    (void)description;
}

static const struct zxdg_output_v1_listener xdgOutputListener = {
    xdgOutputHandleLogicalPosition,
    xdgOutputHandleLogicalSize,
    xdgOutputHandleDone,
    xdgOutputHandleName,
    xdgOutputHandleDescription,
};


//////////////////////////////////////////////////////////////////////////
//////             Waiting for xdg_output geometry                  //////
//////////////////////////////////////////////////////////////////////////

// The compositor sends xdg_output logical geometry in response to
// get_xdg_output, so it is normally there after one roundtrip. Some
// compositors never send it, for instance for an xdg_output created on an
// output whose global has just been removed. So nothing ever waits for more
// than that one roundtrip: a monitor still without geometry is marked pending
// and its fallback values (registry order, the integer wl_output scale) are
// used meanwhile. Its geometry is asked for again in the background, by
// destroying and recreating its xdg_output on a timer, waiting
// XDG_GEOMETRY_RETRY_BASE_MS after the first request and twice as long after
// each further one: requests at 0, 50, 150, 350, 750, 1550, 3150 and 6350 ms,
// and giving up 6400 ms after the last one, 12.75 s after the first. Whenever
// the geometry does arrive, retrying or not, the monitor is updated and the
// windows that took their scale from the fallback are rechecked.
#define XDG_GEOMETRY_RETRY_BASE_MS 50ll
// The number of get_xdg_output requests, the first included, after which a
// monitor is left at its fallback values. This is the give-up point the user
// chose, not something the protocol needs: a later spontaneous geometry event
// still updates the monitor.
#define XDG_GEOMETRY_MAX_REQUESTS 8u

// The state machine below is kept free of Wayland calls so that it can be
// tested without a compositor, see _glfwTestXdgGeometryBackoff().

// The wait for a reply to the request numbered requests, counted from 1
static monotonic_t
xdgGeometryRetryDelay(unsigned requests) {
    return ms_to_monotonic_t(XDG_GEOMETRY_RETRY_BASE_MS << (requests - 1));
}

// The geometry is still missing after the roundtrip that followed the first
// request. Returns how long to wait before asking again.
static monotonic_t
xdgGeometryRetryStart(_GLFWxdgGeometryRetry *r) {
    r->pending = true;
    r->gave_up = false;
    r->requests = 1;
    return xdgGeometryRetryDelay(r->requests);
}

// The retry timer fired with the geometry still missing. Returns true with the
// wait for the reply in *delay if another request is to be made, false if it
// is time to give up.
static bool
xdgGeometryRetryTimerFired(_GLFWxdgGeometryRetry *r, monotonic_t *delay) {
    if (r->requests >= XDG_GEOMETRY_MAX_REQUESTS) {
        r->gave_up = true;
        return false;
    }
    r->requests++;
    *delay = xdgGeometryRetryDelay(r->requests);
    return true;
}

// The geometry arrived. Returns true if it was pending, that is, the monitor
// was in use with fallback values that now have to be replaced.
static bool
xdgGeometryRetryArrived(_GLFWxdgGeometryRetry *r) {
    const bool was_pending = r->pending;
    r->pending = false;
    r->gave_up = false;
    r->requests = 0;
    return was_pending;
}

static bool
xdgGeometryMissing(const _GLFWmonitor *m) {
    return m->wl.xdg_output && !(m->wl.xdg_size_received && m->wl.xdg_position_received);
}

static const char *
monitorNameForLog(const _GLFWmonitor *m) {
    return m->name ? m->name : "(unnamed)";
}

static void
cancelXdgGeometryRetryTimer(_GLFWmonitor *m) {
    if (m->wl.xdg_geometry.timer) {
        removeTimer(&_glfw.wl.eventLoopData, m->wl.xdg_geometry.timer);
        m->wl.xdg_geometry.timer = 0;
    }
}

static _GLFWmonitor *
anyMonitorForRegistryName(uint32_t name) {
    for (int i = 0; i < _glfw.monitorCount; i++) {
        if (_glfw.monitors[i]->wl.name == name) return _glfw.monitors[i];
    }
    for (int i = 0; i < _glfw.wl.pending_monitors.count; i++) {
        if (_glfw.wl.pending_monitors.array[i]->wl.name == name) return _glfw.wl.pending_monitors.array[i];
    }
    return NULL;
}

static void xdgGeometryRetryTimer(id_type timer_id, void *data);

static void
scheduleXdgGeometryRetry(_GLFWmonitor *m, monotonic_t delay) {
    // The monitor is found again by its registry name rather than kept as a
    // pointer, so a timer that outlives it can do no harm
    m->wl.xdg_geometry.timer = addTimer(
        &_glfw.wl.eventLoopData, "wayland-xdg-geometry-retry", delay, 1, false, xdgGeometryRetryTimer, (void *)(uintptr_t)m->wl.name, NULL);
    if (!m->wl.xdg_geometry.timer) {
        m->wl.xdg_geometry.gave_up = true;
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Wayland: could not schedule another request for the xdg_output logical geometry of output %s, using fallback values",
            monitorNameForLog(m));
    }
}

static void
xdgGeometryRetryTimer(id_type timer_id, void *data) {
    const uint32_t name = (uint32_t)(uintptr_t)data;
    _GLFWmonitor *m = anyMonitorForRegistryName(name);
    // The timer may have been cancelled after it was picked for dispatch
    if (!m || m->wl.xdg_geometry.timer != timer_id) return;
    m->wl.xdg_geometry.timer = 0;
    // Timers run after the events of this turn of the event loop have been
    // read from the socket but before they are dispatched, so the reply may
    // be waiting in the queue. Dispatch it, which does not block, rather than
    // ask again and discard it with the old xdg_output. The events can free
    // the monitor, so look it up again.
    wl_glfw_dispatch_pending();
    m = anyMonitorForRegistryName(name);
    if (!m || !m->wl.xdg_geometry.pending || !xdgGeometryMissing(m)) return;
    monotonic_t delay;
    if (!xdgGeometryRetryTimerFired(&m->wl.xdg_geometry, &delay)) {
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Wayland: compositor did not send xdg_output logical geometry for output %s after %u requests, using fallback values",
            monitorNameForLog(m),
            m->wl.xdg_geometry.requests);
        return;
    }
    // Ask again. This must not block: the request is sent and the reply comes
    // through the event loop, to the listeners of the new xdg_output.
    debug_rendering(
        "Wayland: asking again for the xdg_output logical geometry of output %s, request %u\n", monitorNameForLog(m), m->wl.xdg_geometry.requests);
    zxdg_output_v1_destroy(m->wl.xdg_output);
    m->wl.xdg_output = NULL;
    _glfwCreateXdgOutputWayland(m);
    if (!m->wl.xdg_output) {
        m->wl.xdg_geometry.gave_up = true;
        _glfwInputError(
            GLFW_PLATFORM_ERROR, "Wayland: could not ask again for the xdg_output logical geometry of output %s, using fallback values", monitorNameForLog(m));
        return;
    }
    scheduleXdgGeometryRetry(m, delay);
    wl_display_flush(_glfw.wl.display);
}

// Starts asking again in the background for the geometry of a monitor that
// did not get it within the roundtrip after its first request
static void
startXdgGeometryRetries(_GLFWmonitor *m) {
    debug_rendering("Wayland: no xdg_output logical geometry yet for output %s, using fallback values meanwhile\n", monitorNameForLog(m));
    scheduleXdgGeometryRetry(m, xdgGeometryRetryStart(&m->wl.xdg_geometry));
}

static void
xdgGeometryMaybeArrived(_GLFWmonitor *m) {
    if (!m->wl.xdg_geometry.pending || !m->wl.xdg_size_received || !m->wl.xdg_position_received) return;
    const unsigned requests = m->wl.xdg_geometry.requests;
    cancelXdgGeometryRetryTimer(m);
    xdgGeometryRetryArrived(&m->wl.xdg_geometry);
    computeXdgFractionalScale(m);
    debug_rendering(
        "Wayland: xdg_output logical geometry for output %s arrived after %u request(s): %dx%d at %d,%d, scale %.3f\n",
        monitorNameForLog(m),
        requests,
        m->wl.xdg_logical_width,
        m->wl.xdg_logical_height,
        m->wl.x,
        m->wl.y,
        m->wl.fractional_scale);
    _glfwWaylandMonitorGeometryArrived(m);
}

// Makes sure the monitors have xdg_output geometry, all of them if only_name
// is zero or else the one with that registry name, making at most one
// roundtrip and never waiting for a monitor already pending. Monitors can be
// freed by events dispatched during the roundtrip, so none is held across it.
static void
fetchXdgGeometry(uint32_t only_name) {
    bool missing = false;
    for (int i = 0; i < _glfw.monitorCount && !missing; i++) {
        _GLFWmonitor *m = _glfw.monitors[i];
        if ((!only_name || m->wl.name == only_name) && xdgGeometryMissing(m) && !m->wl.xdg_geometry.pending) missing = true;
    }
    if (!missing) return;
    // A failed roundtrip means the connection is gone, the monitors are then
    // marked pending like any other, and the retries are harmless
    wl_glfw_roundtrip();
    for (int i = 0; i < _glfw.monitorCount; i++) {
        _GLFWmonitor *m = _glfw.monitors[i];
        if ((!only_name || m->wl.name == only_name) && xdgGeometryMissing(m) && !m->wl.xdg_geometry.pending) startXdgGeometryRetries(m);
    }
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW internal API                      //////
//////////////////////////////////////////////////////////////////////////

void
_glfwCreateXdgOutputWayland(_GLFWmonitor *monitor) {
    if (!_glfw.wl.xdg_output_manager || monitor->wl.xdg_output) return;
    monitor->wl.xdg_output = zxdg_output_manager_v1_get_xdg_output(_glfw.wl.xdg_output_manager, monitor->wl.output);
    if (monitor->wl.xdg_output) zxdg_output_v1_add_listener(monitor->wl.xdg_output, &xdgOutputListener, monitor);
}

void
_glfwAddOutputWayland(uint32_t name, uint32_t version) {
    _GLFWmonitor *monitor;
    struct wl_output *output;

    if (version < 2) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Wayland: Unsupported output interface version");
        return;
    }

    // The actual name of this output will be set in the handlers.
    monitor = _glfwAllocMonitor("unnamed", 0, 0);

    output = wl_registry_bind(_glfw.wl.registry, name, &wl_output_interface, MIN(version, (unsigned)WL_OUTPUT_NAME_SINCE_VERSION));
    if (!output) {
        _glfwFreeMonitor(monitor);
        return;
    }

    monitor->wl.scale = 1;
    monitor->wl.output = output;
    monitor->wl.name = name;

    _GLFWmonitor **pending = realloc(_glfw.wl.pending_monitors.array, sizeof(pending[0]) * ((size_t)_glfw.wl.pending_monitors.count + 1));
    if (!pending) {
        _glfwInputError(GLFW_OUT_OF_MEMORY, "Wayland: Out of memory while adding output");
        _glfwFreeMonitor(monitor);
        return;
    }
    _glfw.wl.pending_monitors.array = pending;
    _glfw.wl.pending_monitors.array[_glfw.wl.pending_monitors.count++] = monitor;

    wl_output_add_listener(output, &outputListener, monitor);
    _glfwCreateXdgOutputWayland(monitor);
    _glfwWaylandColorManagementCreateOutput(monitor);
}

// Handle removal of the wl_output global with the specified registry name.
// Returns true if an output was found and removed.
bool
_glfwRemoveOutputWayland(uint32_t name) {
    // The global may be removed before wl_output.done is received, for
    // example when an output is unplugged right after being plugged in. Such
    // monitors were never announced, so just free them, otherwise they would
    // be announced on a later done event despite being dead.
    for (int i = 0; i < _glfw.wl.pending_monitors.count; i++) {
        _GLFWmonitor *monitor = _glfw.wl.pending_monitors.array[i];
        if (monitor->wl.name == name) {
            remove_i_from_array(_glfw.wl.pending_monitors.array, i, _glfw.wl.pending_monitors.count);
            _glfwFreeMonitor(monitor);
            return true;
        }
    }
    for (int i = 0; i < _glfw.monitorCount; ++i) {
        _GLFWmonitor *monitor = _glfw.monitors[i];
        if (monitor->wl.name == name) {
            for (_GLFWwindow *window = _glfw.windowListHead; window; window = window->next) {
                bool was_on_monitor = false;
                for (int m = window->wl.monitorsCount - 1; m >= 0; m--) {
                    if (window->wl.monitors[m] == monitor) {
                        remove_i_from_array(window->wl.monitors, m, window->wl.monitorsCount);
                        was_on_monitor = true;
                    }
                }
                if (was_on_monitor) _glfwWaylandSurfaceOutputsChanged(window);
            }
            _glfwInputMonitor(monitor, GLFW_DISCONNECTED, 0);
            return true;
        }
    }
    return false;
}

void
_glfwFreePendingOutputsWayland(void) {
    for (int i = 0; i < _glfw.wl.pending_monitors.count; i++) _glfwFreeMonitor(_glfw.wl.pending_monitors.array[i]);
    free(_glfw.wl.pending_monitors.array);
    _glfw.wl.pending_monitors.array = NULL;
    _glfw.wl.pending_monitors.count = 0;
}

void
_glfwCreateXdgOutputsWayland(void) {
    for (int i = 0; i < _glfw.monitorCount; i++) _glfwCreateXdgOutputWayland(_glfw.monitors[i]);
    for (int i = 0; i < _glfw.wl.pending_monitors.count; i++) _glfwCreateXdgOutputWayland(_glfw.wl.pending_monitors.array[i]);
}


//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

void
_glfwPlatformFreeMonitor(_GLFWmonitor *monitor) {
    // Covers the removal of the output's global, the usual reason for the
    // geometry never arriving, and termination
    cancelXdgGeometryRetryTimer(monitor);
    _glfwWaylandColorManagementDestroyOutput(monitor);
    if (monitor->wl.xdg_output) zxdg_output_v1_destroy(monitor->wl.xdg_output);
    if (monitor->wl.output) wl_output_destroy(monitor->wl.output);
}

void
_glfwPlatformGetMonitorPos(_GLFWmonitor *monitor, int *xpos, int *ypos) {
    if (xpos) *xpos = monitor->wl.x;
    if (ypos) *ypos = monitor->wl.y;
}

void
_glfwPlatformGetMonitorContentScale(_GLFWmonitor *monitor, float *xscale, float *yscale) {
    float scale = monitor->wl.fractional_scale > 0.0 ? (float)monitor->wl.fractional_scale : (float)monitor->wl.scale;
    if (xscale) *xscale = scale;
    if (yscale) *yscale = scale;
}

void
_glfwPlatformGetMonitorWorkarea(_GLFWmonitor *monitor, int *xpos, int *ypos, int *width, int *height) {
    if (xpos) *xpos = monitor->wl.x;
    if (ypos) *ypos = monitor->wl.y;
    if (width) *width = monitor->modes[monitor->wl.currentMode].width;
    if (height) *height = monitor->modes[monitor->wl.currentMode].height;
}

GLFWvidmode *
_glfwPlatformGetVideoModes(_GLFWmonitor *monitor, int *found) {
    *found = monitor->modeCount;
    return monitor->modes;
}

bool
_glfwPlatformGetVideoMode(_GLFWmonitor *monitor, GLFWvidmode *mode) {
    if (monitor->modeCount > monitor->wl.currentMode) {
        *mode = monitor->modes[monitor->wl.currentMode];
        return true;
    }
    return false;
}

bool
_glfwPlatformGetGammaRamp(_GLFWmonitor *monitor UNUSED, GLFWgammaramp *ramp UNUSED) {
    _glfwInputError(GLFW_FEATURE_UNAVAILABLE, "Wayland: Gamma ramp access is not available");
    return false;
}

void
_glfwPlatformSetGammaRamp(_GLFWmonitor *monitor UNUSED, const GLFWgammaramp *ramp UNUSED) {
    _glfwInputError(GLFW_FEATURE_UNAVAILABLE, "Wayland: Gamma ramp access is not available");
}


//////////////////////////////////////////////////////////////////////////
//////                        GLFW native API                       //////
//////////////////////////////////////////////////////////////////////////

GLFWAPI struct wl_output *
glfwGetWaylandMonitor(GLFWmonitor *handle) {
    _GLFWmonitor *monitor = (_GLFWmonitor *)handle;
    assert(monitor != NULL);

    _GLFW_REQUIRE_INIT_OR_RETURN(NULL);
    return monitor->wl.output;
}

static _GLFWmonitor *
monitorForRegistryName(uint32_t name) {
    for (int i = 0; i < _glfw.monitorCount; i++) {
        if (_glfw.monitors[i]->wl.name == name) return _glfw.monitors[i];
    }
    return NULL;
}

GLFWAPI double
glfwGetWaylandCurrentMonitorFractionalScale(void) {
    _GLFW_REQUIRE_INIT_OR_RETURN(1.0);
    if (!_glfw.wl.xdg_output_manager || _glfw.monitorCount == 0) return 1.0;

    // Use the monitor of the currently focused window, then the most recently
    // focused window, then fall back to the primary monitor.
    _GLFWmonitor *monitor = NULL;
    GLFWid focus_ids[2] = {_glfw.wl.keyboardFocusId, _glfw.wl.lastKeyboardFocusId};
    for (int i = 0; i < 2 && !monitor; i++) {
        if (!focus_ids[i]) continue;
        _GLFWwindow *window = _glfwWindowForId(focus_ids[i]);
        if (window && window->wl.monitorsCount > 0) monitor = window->wl.monitors[0];
    }
    if (!monitor) {
        // No focused window to key off of (typically the first window at
        // startup). Prefer the output at the logical origin (0, 0): the
        // compositor's top-left/primary output is a better guess for where a
        // new window will be placed than registry order. Fall back to the
        // first output if no output reports position (0, 0).
        //
        // xdg_output geometry arrives asynchronously and x/y remain zero until
        // it does, so only trust the positions of monitors that have it
        // (otherwise an as-yet-unpositioned output would spuriously match
        // (0, 0)). Monitors whose geometry is pending are skipped, and when it
        // arrives the windows that used the fallback are rechecked.
        fetchXdgGeometry(0);
        for (int i = 0; i < _glfw.monitorCount; i++) {
            _GLFWmonitor *m = _glfw.monitors[i];
            if (m->wl.xdg_output && m->wl.xdg_size_received && m->wl.xdg_position_received && m->wl.x == 0 && m->wl.y == 0) {
                monitor = m;
                break;
            }
        }
        if (!monitor) {
            // Monitors may have been removed during the roundtrip
            if (_glfw.monitorCount == 0) return 1.0;
            monitor = _glfw.monitors[0];
        }
    }

    if (!monitor->wl.xdg_output) return monitor->wl.scale > 0 ? (double)monitor->wl.scale : 1.0;

    // Get the xdg_output logical size and position, if not pending. The
    // monitor may be removed during the roundtrip, so look it up again
    // afterwards.
    const uint32_t name = monitor->wl.name;
    const int fallback_scale = monitor->wl.scale > 0 ? monitor->wl.scale : 1;
    fetchXdgGeometry(name);
    monitor = monitorForRegistryName(name);
    if (!monitor) return (double)fallback_scale;

    // While the geometry is pending the integer scale is the fallback
    if (monitor->wl.fractional_scale <= 0.0 && !xdgGeometryMissing(monitor)) computeXdgFractionalScale(monitor);

    return monitor->wl.fractional_scale > 0.0 ? monitor->wl.fractional_scale : (monitor->wl.scale > 0 ? (double)monitor->wl.scale : 1.0);
}

// For kitty_tests/glfw.py: plays the retry state machine above without a
// compositor. The geometry arrives after arrive_after_timers firings of the
// retry timer, or never if that is negative; when it is larger than the number
// of firings before giving up, it arrives after giving up. The delay of every
// timer scheduled is stored, in milliseconds, in delays_ms while there is room
// for it, and *num_delays gets the number of them all. Returns the final state
// as bits: 1 pending, 2 gave up, 4 the arrival was reported as replacing
// fallback values.
GLFWAPI int
_glfwTestXdgGeometryBackoff(int arrive_after_timers, long long *delays_ms, size_t capacity, size_t *num_delays) {
    _GLFWxdgGeometryRetry r = {0};
    size_t n = 0;
#define record(delay)                                              \
    {                                                              \
        if (n < capacity) delays_ms[n] = monotonic_t_to_ms(delay); \
        n++;                                                       \
    }
    record(xdgGeometryRetryStart(&r));
    for (int fired = 0; arrive_after_timers < 0 || fired < arrive_after_timers; fired++) {
        monotonic_t delay;
        if (!xdgGeometryRetryTimerFired(&r, &delay)) break;
        record(delay);
    }
#undef record
    bool replaced = arrive_after_timers >= 0 && xdgGeometryRetryArrived(&r);
    *num_delays = n;
    return (r.pending ? 1 : 0) | (r.gave_up ? 2 : 0) | (replaced ? 4 : 0);
}
