/*
 * wl_color_management.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

// The color-management-v1 protocol. It tells a client what the display a
// surface is on can show, as the image description the compositor prefers for
// that surface, and lets a client say what the contents of a surface are, by
// setting an image description of its own.
//
// A request for something the compositor did not advertise is a protocol
// error, which ends the connection. So every request here is made only after
// checking what was advertised, and a description that cannot be expressed is
// refused before anything is sent.

#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define debug debug_rendering

// Version 2 replaced the 32 bit identities of image descriptions with 64 bit
// ones and the ambiguous sRGB transfer function with compound_power_2_4. The
// version spoken is the lower of what the compositor has and what the
// wayland-protocols this was built against defines
#ifdef WP_IMAGE_DESCRIPTION_V1_READY2_SINCE_VERSION
#define IMPLEMENTED_VERSION 2
#else
#define IMPLEMENTED_VERSION 1
#endif
// Absent from the headers of version 1
#define TRANSFER_FUNCTION_COMPOUND_POWER_2_4 14

// The state of the manager, as opposed to that of a window
#define cm _glfw.wl.color

static bool
has(uint32_t set, uint32_t value) {
    return value < 32 && (set & (1u << value));
}

static void
add(uint32_t *set, uint32_t value) {
    if (value < 32) *set |= 1u << value;
}

static bool
is_zero(const float *chromaticities) {
    for (size_t i = 0; i < 8; i++) {
        if (chromaticities[i] != 0) return false;
    }
    return true;
}

// Names, for the debug log {{{
static const char *const intent_names[] = {"perceptual", "relative", "saturation", "absolute", "relative_bpc", "absolute_no_adaptation"};
static const char *const feature_names[] = {"icc_v2_v4",      "parametric",                      "set_primaries",          "set_tf_power",
                                            "set_luminances", "set_mastering_display_primaries", "extended_target_volume", "windows_scrgb"};
static const char *const transfer_function_names[] = {"0",     "bt1886", "gamma22",  "gamma28",   "st240", "ext_linear", "log_100",           "log_316",
                                                      "xvycc", "srgb",   "ext_srgb", "st2084_pq", "st428", "hlg",        "compound_power_2_4"};
static const char *const primaries_names[] = {"0",      "srgb",        "pal_m",  "pal",        "ntsc",     "generic_film",
                                              "bt2020", "cie1931_xyz", "dci_p3", "display_p3", "adobe_rgb"};
static const char *const glfw_primaries_names[] = {"unnamed", "srgb", "display_p3", "bt2020", "dci_p3", "adobe_rgb"};
static const char *const glfw_transfer_names[] = {"unknown", "gamma22", "srgb", "bt1886", "power", "ext_linear", "pq", "hlg"};

static const char *
names_in(uint32_t set, const char *const *names, size_t count) {
    static char buf[512];
    char *p = buf;
    *p = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (!(set & (1u << i))) continue;
        if (i < count) p += snprintf(p, sizeof(buf) - (p - buf), "%s ", names[i]);
        else p += snprintf(p, sizeof(buf) - (p - buf), "%u ", i);
        if ((size_t)(p - buf) >= sizeof(buf)) break;
    }
    return buf;
}

static void
debug_description(const char *what, unsigned long long window_id, const GLFWcolordescription *d) {
    if (d->windows_scrgb) {
        debug("%s of window %llu: windows_scrgb\n", what, window_id);
        return;
    }
    const float *c = d->chromaticities, *t = d->target_chromaticities;
    debug(
        "%s of window %llu: primaries: %s r(%.4f %.4f) g(%.4f %.4f) b(%.4f %.4f) w(%.4f %.4f) transfer: %s exponent: %.4f "
        "luminance: %.4f to %g reference %g target: r(%.4f %.4f) g(%.4f %.4f) b(%.4f %.4f) w(%.4f %.4f) luminance: %.4f to %g max_cll: %g max_fall: %g\n",
        what, window_id, glfw_primaries_names[d->primaries], c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], glfw_transfer_names[d->transfer],
        d->transfer_exponent, d->min_luminance, d->max_luminance, d->reference_luminance, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
        d->target_min_luminance, d->target_max_luminance, d->max_cll, d->max_fall);
}
// }}}

// Conversions between the names of the protocol and those of GLFW {{{
static const struct {
    GLFWcolorprimaries glfw;
    uint32_t wayland;
    float chromaticities[8];
} named_primaries[] = {
    {GLFW_COLOR_PRIMARIES_SRGB, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, {0.64f, 0.33f, 0.30f, 0.60f, 0.15f, 0.06f, 0.3127f, 0.3290f}},
    {GLFW_COLOR_PRIMARIES_DISPLAY_P3, WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3, {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.3127f, 0.3290f}},
    {GLFW_COLOR_PRIMARIES_BT2020, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020, {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f}},
    {GLFW_COLOR_PRIMARIES_DCI_P3, WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3, {0.680f, 0.320f, 0.265f, 0.690f, 0.150f, 0.060f, 0.314f, 0.351f}},
    {GLFW_COLOR_PRIMARIES_ADOBE_RGB, WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB, {0.64f, 0.33f, 0.21f, 0.71f, 0.15f, 0.06f, 0.3127f, 0.3290f}},
};

static GLFWcolorprimaries
primaries_from_wayland(uint32_t primaries) {
    for (size_t i = 0; i < arraysz(named_primaries); i++) {
        if (named_primaries[i].wayland == primaries) return named_primaries[i].glfw;
    }
    return GLFW_COLOR_PRIMARIES_UNKNOWN;
}

static GLFWcolortransfer
transfer_from_wayland(uint32_t tf) {
    switch (tf) {
        case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22: return GLFW_COLOR_TRANSFER_GAMMA22;
        case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
        case TRANSFER_FUNCTION_COMPOUND_POWER_2_4: return GLFW_COLOR_TRANSFER_SRGB;
        case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886: return GLFW_COLOR_TRANSFER_BT1886;
        case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR: return GLFW_COLOR_TRANSFER_EXT_LINEAR;
        case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ: return GLFW_COLOR_TRANSFER_PQ;
        case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG: return GLFW_COLOR_TRANSFER_HLG;
        default: return GLFW_COLOR_TRANSFER_UNKNOWN;
    }
}

// The named transfer function to ask for, zero when the compositor has none that fits
static uint32_t
transfer_to_wayland(GLFWcolortransfer transfer) {
    uint32_t tf = 0;
    switch (transfer) {
        case GLFW_COLOR_TRANSFER_GAMMA22: tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22; break;
        case GLFW_COLOR_TRANSFER_SRGB:
            // the two piece curve changed its name, compositors advertise one or the other
            tf = has(cm.transfer_functions, TRANSFER_FUNCTION_COMPOUND_POWER_2_4) ? TRANSFER_FUNCTION_COMPOUND_POWER_2_4 :
                                                                                         WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
            break;
        case GLFW_COLOR_TRANSFER_BT1886: tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886; break;
        case GLFW_COLOR_TRANSFER_EXT_LINEAR: tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR; break;
        case GLFW_COLOR_TRANSFER_PQ: tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ; break;
        case GLFW_COLOR_TRANSFER_HLG: tf = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG; break;
        case GLFW_COLOR_TRANSFER_UNKNOWN:
        case GLFW_COLOR_TRANSFER_POWER: break;
    }
    return has(cm.transfer_functions, tf) ? tf : 0;
}

static uint32_t
intent_to_wayland(GLFWcolorintent intent) {
    uint32_t ans = WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL;
    switch (intent) {
        case GLFW_COLOR_INTENT_PERCEPTUAL: break;
        case GLFW_COLOR_INTENT_RELATIVE: ans = WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE; break;
        case GLFW_COLOR_INTENT_SATURATION: ans = WP_COLOR_MANAGER_V1_RENDER_INTENT_SATURATION; break;
        case GLFW_COLOR_INTENT_ABSOLUTE: ans = WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE; break;
        case GLFW_COLOR_INTENT_RELATIVE_BPC: ans = WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE_BPC; break;
    }
    // compositors must offer the perceptual intent, the others are optional
    return has(cm.intents, ans) ? ans : WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL;
}
// }}}

// The manager {{{
static void
manager_supported_intent(void *data UNUSED, struct wp_color_manager_v1 *manager UNUSED, uint32_t intent) {
    add(&cm.intents, intent);
}

static void
manager_supported_feature(void *data UNUSED, struct wp_color_manager_v1 *manager UNUSED, uint32_t feature) {
    add(&cm.features, feature);
}

static void
manager_supported_tf_named(void *data UNUSED, struct wp_color_manager_v1 *manager UNUSED, uint32_t tf) {
    add(&cm.transfer_functions, tf);
}

static void
manager_supported_primaries_named(void *data UNUSED, struct wp_color_manager_v1 *manager UNUSED, uint32_t primaries) {
    add(&cm.primaries, primaries);
}

static void
manager_done(void *data UNUSED, struct wp_color_manager_v1 *manager UNUSED) {
    cm.done = true;
    debug("Compositor has color management version %u, using version %u\n", cm.compositor_version, cm.version);
    debug("    intents: %s\n", names_in(cm.intents, intent_names, arraysz(intent_names)));
    debug("    features: %s\n", names_in(cm.features, feature_names, arraysz(feature_names)));
    debug("    transfer functions: %s\n", names_in(cm.transfer_functions, transfer_function_names, arraysz(transfer_function_names)));
    debug("    primaries: %s\n", names_in(cm.primaries, primaries_names, arraysz(primaries_names)));
}

static const struct wp_color_manager_v1_listener manager_listener = {
    .supported_intent = manager_supported_intent,
    .supported_feature = manager_supported_feature,
    .supported_tf_named = manager_supported_tf_named,
    .supported_primaries_named = manager_supported_primaries_named,
    .done = manager_done,
};

void
_glfwWaylandBindColorManager(struct wl_registry *registry, uint32_t name, uint32_t version) {
    if (cm.manager) return;
    cm.compositor_version = version;
    cm.version = MIN(version, (uint32_t)IMPLEMENTED_VERSION);
    cm.manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, cm.version);
    if (cm.manager) {
        wp_color_manager_v1_add_listener(cm.manager, &manager_listener, NULL);
        for (int i = 0; i < _glfw.monitorCount; i++) _glfwWaylandColorManagementCreateOutput(_glfw.monitors[i]);
    }
}

void
_glfwWaylandTerminateColorManager(void) {
    if (cm.queue) wl_event_queue_destroy(cm.queue);
    if (cm.manager) wp_color_manager_v1_destroy(cm.manager);
    cm.queue = NULL;
    cm.manager = NULL;
}
// }}}

// What the display a window is on can show {{{

// The compositor prefers one image description for each surface and says when
// it changes. Reading one takes three steps, each answered by an event: ask for
// the description, wait for it to be ready, ask for its information.

static void
forget_description_being_read(_GLFWwindow *window) {
    if (window->wl.color.info) wp_image_description_info_v1_destroy(window->wl.color.info);
    if (window->wl.color.pending) wp_image_description_v1_destroy(window->wl.color.pending);
    window->wl.color.info = NULL;
    window->wl.color.pending = NULL;
}

static bool
same_description(const GLFWcolordescription *a, const GLFWcolordescription *b) {
    return a->primaries == b->primaries && a->transfer == b->transfer && a->transfer_exponent == b->transfer_exponent &&
           !memcmp(a->chromaticities, b->chromaticities, sizeof(a->chromaticities)) &&
           !memcmp(a->target_chromaticities, b->target_chromaticities, sizeof(a->target_chromaticities)) && a->min_luminance == b->min_luminance &&
           a->max_luminance == b->max_luminance && a->reference_luminance == b->reference_luminance && a->target_min_luminance == b->target_min_luminance &&
           a->target_max_luminance == b->target_max_luminance && a->max_cll == b->max_cll && a->max_fall == b->max_fall;
}

#define being_read (((_GLFWwindow *)data)->wl.color.reading)

static void
set_chromaticities(float *dest, int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y) {
    const int32_t src[8] = {r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y};
    for (size_t i = 0; i < arraysz(src); i++) dest[i] = src[i] / 1e6f;
}

static void
info_done(void *data, struct wp_image_description_info_v1 *info UNUSED) {
    _GLFWwindow *window = data;
    // done is the last event of an information object, and so of the description it was read from
    forget_description_being_read(window);
    GLFWcolordescription *d = &window->wl.color.reading;
    // A description that is only an ICC profile has no parameters to keep
    if (d->primaries == GLFW_COLOR_PRIMARIES_UNKNOWN && is_zero(d->chromaticities)) return;
    if (d->transfer == GLFW_COLOR_TRANSFER_UNKNOWN && d->transfer_exponent > 0) d->transfer = GLFW_COLOR_TRANSFER_POWER;
    // A description without a target can show everything it can encode
    if (!window->wl.color.reading_has_target) memcpy(d->target_chromaticities, d->chromaticities, sizeof(d->chromaticities));
    if (d->target_max_luminance <= 0) {
        d->target_min_luminance = d->min_luminance;
        d->target_max_luminance = d->max_luminance;
    }
    if (window->wl.color.has_preferred && same_description(d, &window->wl.color.preferred)) return;
    window->wl.color.preferred = *d;
    window->wl.color.has_preferred = true;
    debug_description("Preferred color description", window->id, d);
    _glfwInputWindowDamage(window);
}

static void
info_icc_file(void *data UNUSED, struct wp_image_description_info_v1 *info UNUSED, int32_t fd, uint32_t size UNUSED) {
    // Only the parameters are of use here
    close(fd);
}

static void
info_primaries(
    void *data, struct wp_image_description_info_v1 *info UNUSED, int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x,
    int32_t w_y) {
    set_chromaticities(being_read.chromaticities, r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y);
}

static void
info_primaries_named(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t primaries) {
    being_read.primaries = primaries_from_wayland(primaries);
}

static void
info_tf_power(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t eexp) {
    being_read.transfer_exponent = eexp / 10000.f;
}

static void
info_tf_named(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t tf) {
    being_read.transfer = transfer_from_wayland(tf);
}

static void
info_luminances(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum) {
    being_read.min_luminance = min_lum / 10000.f;
    being_read.max_luminance = (float)max_lum;
    being_read.reference_luminance = (float)reference_lum;
}

static void
info_target_primaries(
    void *data, struct wp_image_description_info_v1 *info UNUSED, int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x,
    int32_t w_y) {
    set_chromaticities(being_read.target_chromaticities, r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y);
    ((_GLFWwindow *)data)->wl.color.reading_has_target = true;
}

static void
info_target_luminance(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t min_lum, uint32_t max_lum) {
    being_read.target_min_luminance = min_lum / 10000.f;
    being_read.target_max_luminance = (float)max_lum;
}

static void
info_target_max_cll(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t max_cll) {
    being_read.max_cll = (float)max_cll;
}

static void
info_target_max_fall(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t max_fall) {
    being_read.max_fall = (float)max_fall;
}
#undef being_read

static const struct wp_image_description_info_v1_listener info_listener = {
    .done = info_done,
    .icc_file = info_icc_file,
    .primaries = info_primaries,
    .primaries_named = info_primaries_named,
    .tf_power = info_tf_power,
    .tf_named = info_tf_named,
    .luminances = info_luminances,
    .target_primaries = info_target_primaries,
    .target_luminance = info_target_luminance,
    .target_max_cll = info_target_max_cll,
    .target_max_fall = info_target_max_fall,
};

static void
preferred_failed(void *data, struct wp_image_description_v1 *description UNUSED, uint32_t cause, const char *msg) {
    _GLFWwindow *window = data;
    debug("The compositor could not give the preferred color description of window %llu: cause: %u %s\n", window->id, cause, msg ? msg : "");
    forget_description_being_read(window);
}

static void
preferred_is_ready(_GLFWwindow *window, struct wp_image_description_v1 *description) {
    // Descriptions that come from the compositor allow their information to be read, once they are ready
    memset(&window->wl.color.reading, 0, sizeof(window->wl.color.reading));
    window->wl.color.reading_has_target = false;
    window->wl.color.info = wp_image_description_v1_get_information(description);
    if (window->wl.color.info) wp_image_description_info_v1_add_listener(window->wl.color.info, &info_listener, window);
    else forget_description_being_read(window);
}

static void
preferred_ready(void *data, struct wp_image_description_v1 *description, uint32_t identity UNUSED) {
    preferred_is_ready(data, description);
}

#if IMPLEMENTED_VERSION >= 2
static void
preferred_ready2(void *data, struct wp_image_description_v1 *description, uint32_t identity_hi UNUSED, uint32_t identity_lo UNUSED) {
    preferred_is_ready(data, description);
}
#endif

static const struct wp_image_description_v1_listener preferred_listener = {
    .failed = preferred_failed,
    .ready = preferred_ready,
#if IMPLEMENTED_VERSION >= 2
    .ready2 = preferred_ready2,
#endif
};

static void
request_preferred_description(_GLFWwindow *window) {
    if (!window->wl.color.feedback) return;
    // Only the newest description matters, one still being read is out of date
    forget_description_being_read(window);
    // Parameters are what is wanted. Without the parametric feature the description may be an ICC profile,
    // which is read all the same for whatever parameters come with it
    window->wl.color.pending = has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) ?
                                   wp_color_management_surface_feedback_v1_get_preferred_parametric(window->wl.color.feedback) :
                                   wp_color_management_surface_feedback_v1_get_preferred(window->wl.color.feedback);
    if (window->wl.color.pending) wp_image_description_v1_add_listener(window->wl.color.pending, &preferred_listener, window);
}

static void
feedback_preferred_changed(void *data, struct wp_color_management_surface_feedback_v1 *feedback UNUSED, uint32_t identity UNUSED) {
    request_preferred_description(data);
}

#if IMPLEMENTED_VERSION >= 2
static void
feedback_preferred_changed2(
    void *data, struct wp_color_management_surface_feedback_v1 *feedback UNUSED, uint32_t identity_hi UNUSED, uint32_t identity_lo UNUSED) {
    request_preferred_description(data);
}
#endif

static const struct wp_color_management_surface_feedback_v1_listener feedback_listener = {
    .preferred_changed = feedback_preferred_changed,
#if IMPLEMENTED_VERSION >= 2
    .preferred_changed2 = feedback_preferred_changed2,
#endif
};

// Called once the surface of a window exists
void
_glfwWaylandColorManagementCreate(_GLFWwindow *window) {
    if (!cm.manager || !window->wl.surface) return;
    window->wl.color.feedback = wp_color_manager_v1_get_surface_feedback(cm.manager, window->wl.surface);
    if (!window->wl.color.feedback) return;
    wp_color_management_surface_feedback_v1_add_listener(window->wl.color.feedback, &feedback_listener, window);
    request_preferred_description(window);
}

// Called before the surface of a window is destroyed
void
_glfwWaylandColorManagementDestroy(_GLFWwindow *window) {
    forget_description_being_read(window);
    if (window->wl.color.feedback) wp_color_management_surface_feedback_v1_destroy(window->wl.color.feedback);
    if (window->wl.color.surface) wp_color_management_surface_v1_destroy(window->wl.color.surface);
    window->wl.color.feedback = NULL;
    window->wl.color.surface = NULL;
    window->wl.color.has_preferred = false;
}

// What the outputs can show {{{

// An output whose image description has a transfer function made for HDR, or a peak above its reference white, is
// driven with more than 8 bits per channel: 8 bits are not enough for either. Nothing in the protocol gives the depth
// of an SDR output, which is therefore never known to be deep.
static bool
is_deep(GLFWcolortransfer transfer, float max, float reference, float target_max) {
    if (transfer == GLFW_COLOR_TRANSFER_PQ || transfer == GLFW_COLOR_TRANSFER_HLG || transfer == GLFW_COLOR_TRANSFER_EXT_LINEAR) return true;
    const float peak = target_max > 0 ? target_max : max;
    return reference >= 1 && peak > reference;
}

bool
_glfwWaylandColorDescriptionIsDeep(const GLFWcolordescription *d) {
    return is_deep(d->transfer, d->max_luminance, d->reference_luminance, d->target_max_luminance);
}

static void
forget_output_description_being_read(_GLFWmonitor *monitor) {
    if (monitor->wl.color.info) wp_image_description_info_v1_destroy(monitor->wl.color.info);
    if (monitor->wl.color.pending) wp_image_description_v1_destroy(monitor->wl.color.pending);
    monitor->wl.color.info = NULL;
    monitor->wl.color.pending = NULL;
}

static void
output_info_done(void *data, struct wp_image_description_info_v1 *info UNUSED) {
    _GLFWmonitor *monitor = data;
    forget_output_description_being_read(monitor);
    const bool deep = is_deep(monitor->wl.color.reading_transfer, monitor->wl.color.reading_max, monitor->wl.color.reading_reference,
        monitor->wl.color.reading_target_max);
    if (deep != monitor->wl.color.deep) debug("Output %s is %s\n", monitor->name ? monitor->name : "", deep ? "deep (HDR)" : "not known to be deep");
    monitor->wl.color.deep = deep;
}

static void
output_info_icc_file(void *data UNUSED, struct wp_image_description_info_v1 *info UNUSED, int32_t fd, uint32_t size UNUSED) {
    close(fd);
}

static void
output_info_primaries(void *data UNUSED, struct wp_image_description_info_v1 *info UNUSED, int32_t r_x UNUSED, int32_t r_y UNUSED,
    int32_t g_x UNUSED, int32_t g_y UNUSED, int32_t b_x UNUSED, int32_t b_y UNUSED, int32_t w_x UNUSED, int32_t w_y UNUSED) {}

static void
output_info_primaries_named(void *data UNUSED, struct wp_image_description_info_v1 *info UNUSED, uint32_t primaries UNUSED) {}

static void
output_info_tf_power(void *data UNUSED, struct wp_image_description_info_v1 *info UNUSED, uint32_t eexp UNUSED) {}

static void
output_info_tf_named(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t tf) {
    ((_GLFWmonitor *)data)->wl.color.reading_transfer = transfer_from_wayland(tf);
}

static void
output_info_luminances(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t min_lum UNUSED, uint32_t max_lum, uint32_t reference_lum) {
    ((_GLFWmonitor *)data)->wl.color.reading_max = (float)max_lum;
    ((_GLFWmonitor *)data)->wl.color.reading_reference = (float)reference_lum;
}

static void
output_info_target_luminance(void *data, struct wp_image_description_info_v1 *info UNUSED, uint32_t min_lum UNUSED, uint32_t max_lum) {
    ((_GLFWmonitor *)data)->wl.color.reading_target_max = (float)max_lum;
}

static void
output_info_target_value(void *data UNUSED, struct wp_image_description_info_v1 *info UNUSED, uint32_t value UNUSED) {}

static const struct wp_image_description_info_v1_listener output_info_listener = {
    .done = output_info_done,
    .icc_file = output_info_icc_file,
    .primaries = output_info_primaries,
    .primaries_named = output_info_primaries_named,
    .tf_power = output_info_tf_power,
    .tf_named = output_info_tf_named,
    .luminances = output_info_luminances,
    .target_primaries = output_info_primaries,
    .target_luminance = output_info_target_luminance,
    .target_max_cll = output_info_target_value,
    .target_max_fall = output_info_target_value,
};

static void
output_description_failed(void *data, struct wp_image_description_v1 *description UNUSED, uint32_t cause, const char *msg) {
    _GLFWmonitor *monitor = data;
    debug("The compositor could not give the color description of output %s: cause: %u %s\n", monitor->name ? monitor->name : "", cause, msg ? msg : "");
    forget_output_description_being_read(monitor);
}

static void
output_description_is_ready(_GLFWmonitor *monitor, struct wp_image_description_v1 *description) {
    monitor->wl.color.reading_transfer = GLFW_COLOR_TRANSFER_UNKNOWN;
    monitor->wl.color.reading_max = monitor->wl.color.reading_reference = monitor->wl.color.reading_target_max = 0;
    monitor->wl.color.info = wp_image_description_v1_get_information(description);
    if (monitor->wl.color.info) wp_image_description_info_v1_add_listener(monitor->wl.color.info, &output_info_listener, monitor);
    else forget_output_description_being_read(monitor);
}

static void
output_description_ready(void *data, struct wp_image_description_v1 *description, uint32_t identity UNUSED) {
    output_description_is_ready(data, description);
}

#if IMPLEMENTED_VERSION >= 2
static void
output_description_ready2(void *data, struct wp_image_description_v1 *description, uint32_t identity_hi UNUSED, uint32_t identity_lo UNUSED) {
    output_description_is_ready(data, description);
}
#endif

static const struct wp_image_description_v1_listener output_description_listener = {
    .failed = output_description_failed,
    .ready = output_description_ready,
#if IMPLEMENTED_VERSION >= 2
    .ready2 = output_description_ready2,
#endif
};

static void
request_output_description(_GLFWmonitor *monitor) {
    if (!monitor->wl.color.output) return;
    forget_output_description_being_read(monitor);
    monitor->wl.color.pending = wp_color_management_output_v1_get_image_description(monitor->wl.color.output);
    if (monitor->wl.color.pending) wp_image_description_v1_add_listener(monitor->wl.color.pending, &output_description_listener, monitor);
}

static void
output_description_changed(void *data, struct wp_color_management_output_v1 *output UNUSED) {
    request_output_description(data);
}

static const struct wp_color_management_output_v1_listener output_listener = {
    .image_description_changed = output_description_changed,
};

// Called for every output, once both it and the color manager are known, in whichever order they come
void
_glfwWaylandColorManagementCreateOutput(_GLFWmonitor *monitor) {
    if (!cm.manager || !monitor->wl.output || monitor->wl.color.output) return;
    monitor->wl.color.output = wp_color_manager_v1_get_output(cm.manager, monitor->wl.output);
    if (!monitor->wl.color.output) return;
    wp_color_management_output_v1_add_listener(monitor->wl.color.output, &output_listener, monitor);
    request_output_description(monitor);
}

void
_glfwWaylandColorManagementDestroyOutput(_GLFWmonitor *monitor) {
    forget_output_description_being_read(monitor);
    if (monitor->wl.color.output) wp_color_management_output_v1_destroy(monitor->wl.color.output);
    monitor->wl.color.output = NULL;
    monitor->wl.color.deep = false;
}
// }}}

bool
_glfwPlatformGetWindowPreferredColorDescription(_GLFWwindow *window, GLFWcolordescription *desc) {
    if (!window->wl.color.has_preferred) return false;
    *desc = window->wl.color.preferred;
    return true;
}
// }}}

// What the contents of a window are {{{

// The requests that make up a description, worked out and checked before any of them is sent
typedef struct DescriptionRequest {
    bool windows_scrgb;
    uint32_t primaries_named, tf_named, tf_power;  // each zero when not used
    int32_t primaries[8], target_primaries[8];
    bool has_luminances, has_target_primaries, has_target_luminance;
    uint32_t min_lum, max_lum, reference_lum, target_min_lum, target_max_lum, max_cll, max_fall;
} DescriptionRequest;

// Is the point p within the triangle abc, with a little room for rounding
static bool
in_triangle(const float *p, const float *a, const float *b, const float *c) {
#define side(u, v) ((p[0] - v[0]) * (u[1] - v[1]) - (u[0] - v[0]) * (p[1] - v[1]))
    const float d1 = side(a, b), d2 = side(b, c), d3 = side(c, a), eps = 1e-5f;
#undef side
    return !((d1 < -eps || d2 < -eps || d3 < -eps) && (d1 > eps || d2 > eps || d3 > eps));
}

static bool
gamut_contains(const float *outer, const float *inner) {
    return in_triangle(inner, outer, outer + 2, outer + 4) && in_triangle(inner + 2, outer, outer + 2, outer + 4) &&
           in_triangle(inner + 4, outer, outer + 2, outer + 4);
}

#define refuse(...) \
    { \
        debug("Cannot describe the colors of a window to the compositor: " __VA_ARGS__); \
        return false; \
    }

static bool
plan_description(const GLFWcolordescription *d, DescriptionRequest *r) {
    memset(r, 0, sizeof(*r));
    if ((unsigned)d->primaries >= arraysz(glfw_primaries_names) || (unsigned)d->transfer >= arraysz(glfw_transfer_names))
        refuse("unknown primaries or transfer function\n");
    if (d->windows_scrgb) {
        if (!has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB)) refuse("no windows_scrgb feature\n");
        r->windows_scrgb = true;
        return true;
    }
    // Nothing beyond what the integers of the protocol can carry, which also keeps out values that are not numbers
    const float luminances[] = {d->min_luminance,        d->max_luminance, d->reference_luminance, d->target_min_luminance,
                                d->target_max_luminance, d->max_cll,       d->max_fall};
    for (size_t i = 0; i < arraysz(luminances); i++) {
        if (!(luminances[i] >= 0 && luminances[i] <= 1e6f)) refuse("luminance %g out of range\n", luminances[i]);
    }
    for (size_t i = 0; i < 8; i++) {
        if (!(fabsf(d->chromaticities[i]) <= 2 && fabsf(d->target_chromaticities[i]) <= 2)) refuse("chromaticity out of range\n");
    }
    if (!has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)) refuse("no parametric feature\n");

    // The primaries the values are encoded with
    const float *encoding = d->chromaticities;
    if (d->primaries == GLFW_COLOR_PRIMARIES_UNKNOWN) {
        if (is_zero(d->chromaticities)) refuse("no primaries given\n");
        if (!has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES)) refuse("no set_primaries feature\n");
        for (size_t i = 0; i < 8; i++) r->primaries[i] = (int32_t)lroundf(d->chromaticities[i] * 1e6f);
    } else {
        for (size_t i = 0; i < arraysz(named_primaries); i++) {
            if (named_primaries[i].glfw != d->primaries) continue;
            r->primaries_named = named_primaries[i].wayland;
            encoding = named_primaries[i].chromaticities;
        }
        if (!has(cm.primaries, r->primaries_named)) refuse("primaries %s not supported\n", glfw_primaries_names[d->primaries]);
    }

    // The transfer function
    if (d->transfer == GLFW_COLOR_TRANSFER_POWER) {
        if (!has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER)) refuse("no set_tf_power feature\n");
        if (!(d->transfer_exponent >= 1.f && d->transfer_exponent <= 10.f)) refuse("power %g out of range\n", d->transfer_exponent);
        r->tf_power = (uint32_t)lroundf(d->transfer_exponent * 10000.f);
    } else {
        r->tf_named = transfer_to_wayland(d->transfer);
        if (!r->tf_named) refuse("transfer function %s not supported\n", glfw_transfer_names[d->transfer]);
    }

    // Reaching beyond what the primaries and their luminances encode is a feature of its own
    const bool extended = has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME);

    // The luminances, all three or none. The minimum travels in units of 0.0001 cd/m², the others in whole cd/m²
    if (d->min_luminance != 0 || d->max_luminance != 0 || d->reference_luminance != 0) {
        if (!has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES)) refuse("no set_luminances feature\n");
        if (!(d->max_luminance >= 1 && d->reference_luminance >= 1)) refuse("luminances are not all given\n");
        r->has_luminances = true;
        r->min_lum = (uint32_t)lroundf(d->min_luminance * 10000.f);
        r->max_lum = (uint32_t)lroundf(d->max_luminance);
        r->reference_lum = (uint32_t)lroundf(d->reference_luminance);
        if ((uint64_t)r->max_lum * 10000u <= r->min_lum || (uint64_t)r->reference_lum * 10000u <= r->min_lum)
            refuse("minimum luminance is not the smallest\n");
        // Mutter makes this a protocol error
        if (!extended && r->reference_lum > r->max_lum) refuse("no extended_target_volume feature for a reference white above the maximum luminance\n");
    }

    // The target, what can actually be shown, when it differs from what can be encoded. A target larger than the
    // encoding is its own feature, and without it the outcome of asking is undefined, so it is not asked for
    const bool has_target = has(cm.features, WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES);
    if (!is_zero(d->target_chromaticities) && memcmp(d->target_chromaticities, encoding, sizeof(d->target_chromaticities))) {
        if (!has_target) refuse("no set_mastering_display_primaries feature\n");
        if (!extended && !gamut_contains(encoding, d->target_chromaticities))
            refuse("no extended_target_volume feature for a target wider than the primaries\n");
        r->has_target_primaries = true;
        for (size_t i = 0; i < 8; i++) r->target_primaries[i] = (int32_t)lroundf(d->target_chromaticities[i] * 1e6f);
    }
    if (d->target_min_luminance != 0 || d->target_max_luminance != 0) {
        if (!has_target) refuse("no set_mastering_display_primaries feature\n");
        if (!(d->target_max_luminance >= 1)) refuse("target luminances are not both given\n");
        r->has_target_luminance = true;
        r->target_min_lum = (uint32_t)lroundf(d->target_min_luminance * 10000.f);
        r->target_max_lum = (uint32_t)lroundf(d->target_max_luminance);
        if ((uint64_t)r->target_max_lum * 10000u <= r->target_min_lum) refuse("minimum target luminance is not the smallest\n");
        // Whether the target lies within the encoding can only be told when the luminances of the encoding were given too
        if (!extended && (!r->has_luminances || r->target_max_lum > r->max_lum || r->target_min_lum < r->min_lum))
            refuse("no extended_target_volume feature for a target brighter than the primaries\n");
    }

    // The light levels
    r->max_cll = (uint32_t)lroundf(d->max_cll);
    r->max_fall = (uint32_t)lroundf(d->max_fall);
    if (r->max_cll && r->max_fall && r->max_fall > r->max_cll) refuse("max_fall above max_cll\n");
    if (cm.version < 2) {
        // Version 1 makes it an error for a light level to lie outside the luminance range of the target. They are
        // only hints, so leave out any that cannot be shown to lie within it
#define outside(level) (level && (!r->has_target_luminance || (uint64_t)level * 10000u <= r->target_min_lum || level > r->target_max_lum))
        if (outside(r->max_cll)) r->max_cll = 0;
        if (outside(r->max_fall)) r->max_fall = 0;
#undef outside
    }
    return true;
}
#undef refuse

// A description the client creates is ready or has failed some time after it was asked for, in an event
typedef struct CreatedDescription {
    enum { CREATION_PENDING, CREATION_READY, CREATION_FAILED } state;
    uint32_t cause;
    char msg[128];
} CreatedDescription;

static void
created_failed(void *data, struct wp_image_description_v1 *description UNUSED, uint32_t cause, const char *msg) {
    CreatedDescription *c = data;
    c->state = CREATION_FAILED;
    c->cause = cause;
    snprintf(c->msg, sizeof(c->msg), "%s", msg ? msg : "");
}

static void
created_ready(void *data, struct wp_image_description_v1 *description UNUSED, uint32_t identity UNUSED) {
    ((CreatedDescription *)data)->state = CREATION_READY;
}

#if IMPLEMENTED_VERSION >= 2
static void
created_ready2(void *data, struct wp_image_description_v1 *description UNUSED, uint32_t identity_hi UNUSED, uint32_t identity_lo UNUSED) {
    ((CreatedDescription *)data)->state = CREATION_READY;
}
#endif

static const struct wp_image_description_v1_listener created_listener = {
    .failed = created_failed,
    .ready = created_ready,
#if IMPLEMENTED_VERSION >= 2
    .ready2 = created_ready2,
#endif
};

bool
_glfwPlatformSetWindowColorDescription(_GLFWwindow *window, const GLFWcolordescription *desc, GLFWcolorintent intent) {
    if (!desc) {
        // Without a description the compositor treats the surface as sRGB, as it does one that never had any
        if (window->wl.color.surface) {
            wp_color_management_surface_v1_unset_image_description(window->wl.color.surface);
            debug("Removed the color description of window %llu\n", window->id);
        }
        return true;
    }
    if (!cm.manager || !cm.done || !window->wl.surface) return false;
    DescriptionRequest r;
    if (!plan_description(desc, &r)) return false;

    // The answer is waited for on a queue of its own, so that no other event is handled while waiting
    if (!cm.queue) cm.queue = wl_display_create_queue(_glfw.wl.display);
    struct wp_color_manager_v1 *manager = cm.queue ? wl_proxy_create_wrapper(cm.manager) : NULL;
    if (!manager) return false;
    wl_proxy_set_queue((struct wl_proxy *)manager, cm.queue);
    struct wp_image_description_v1 *description = NULL;
    if (r.windows_scrgb) description = wp_color_manager_v1_create_windows_scrgb(manager);
    else {
        struct wp_image_description_creator_params_v1 *creator = wp_color_manager_v1_create_parametric_creator(manager);
        if (creator) {
            if (r.primaries_named) wp_image_description_creator_params_v1_set_primaries_named(creator, r.primaries_named);
            else {
                const int32_t *p = r.primaries;
                wp_image_description_creator_params_v1_set_primaries(creator, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            }
            if (r.tf_named) wp_image_description_creator_params_v1_set_tf_named(creator, r.tf_named);
            else wp_image_description_creator_params_v1_set_tf_power(creator, r.tf_power);
            if (r.has_luminances) wp_image_description_creator_params_v1_set_luminances(creator, r.min_lum, r.max_lum, r.reference_lum);
            if (r.has_target_primaries) {
                const int32_t *p = r.target_primaries;
                wp_image_description_creator_params_v1_set_mastering_display_primaries(creator, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            }
            if (r.has_target_luminance) wp_image_description_creator_params_v1_set_mastering_luminance(creator, r.target_min_lum, r.target_max_lum);
            if (r.max_cll) wp_image_description_creator_params_v1_set_max_cll(creator, r.max_cll);
            if (r.max_fall) wp_image_description_creator_params_v1_set_max_fall(creator, r.max_fall);
            // create also destroys the creator
            description = wp_image_description_creator_params_v1_create(creator);
        }
    }
    wl_proxy_wrapper_destroy(manager);
    if (!description) return false;

    CreatedDescription created = {0};
    wp_image_description_v1_add_listener(description, &created_listener, &created);
    // Compositors answer at once, the extra round trips are for one that takes its time
    for (int i = 0; i < 8 && created.state == CREATION_PENDING; i++) {
        if (wl_display_roundtrip_queue(_glfw.wl.display, cm.queue) < 0) break;
    }
    if (created.state != CREATION_READY) {
        if (created.state == CREATION_FAILED) {
            debug("The compositor refused a color description: cause: %u %s\n", created.cause, created.msg);
        } else {
            debug("The compositor did not answer a color description\n");
        }
        wp_image_description_v1_destroy(description);
        return false;
    }
    // A surface can have only one of these, so it is made when first needed and kept
    if (!window->wl.color.surface) window->wl.color.surface = wp_color_manager_v1_get_surface(cm.manager, window->wl.surface);
    if (window->wl.color.surface) {
        // Applied by the next commit of the surface, and copied, so the description itself is not needed after this
        wp_color_management_surface_v1_set_image_description(window->wl.color.surface, description, intent_to_wayland(intent));
        debug_description("Set the color description", window->id, desc);
    }
    wp_image_description_v1_destroy(description);
    return window->wl.color.surface != NULL;
}
// }}}
