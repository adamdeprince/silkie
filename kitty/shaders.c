/*
 * shaders.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "fonts.h"
#include "gl.h"
#include "cleanup.h"
#include "colors.h"
#include <stddef.h>
#include <string.h>
#include "animation.h"
#include "animation-parse.h"
#include "text-cache.h"
#include "tupleobject.h"
#include "window_logo.h"
#include "srgb_gamma.h"
#include "state.h"
#include "spatial.h"
#include "deep-colors.h"

enum {
    CELL_PROGRAM,
    CELL_FG_PROGRAM,
    CELL_BG_PROGRAM,
    CELL_PROGRAM_SENTINEL,
    BORDERS_PROGRAM,
    GRAPHICS_PROGRAM,
    GRAPHICS_PREMULT_PROGRAM,
    GRAPHICS_ALPHA_MASK_PROGRAM,
    GRAPHICS_INDEXED_PROGRAM,
    GRAPHICS_INDEXED_UNSCALED_PROGRAM,
    GRAPHICS_PALETTE_PROGRAM,
    GRAPHICS_PALETTE_UNSCALED_PROGRAM,
    BGIMAGE_PROGRAM,
    TINT_PROGRAM,
    TRAIL_PROGRAM,
    BLIT_PROGRAM,
    SCREENSHOT_PROGRAM,
    ROUNDED_RECT_PROGRAM,
    PADDING_PROGRAM,

    CUSTOM_END_PROGRAM,

    NUM_PROGRAMS
};
enum {
    SPRITE_MAP_UNIT,
    GRAPHICS_UNIT,
    SPRITE_DECORATIONS_MAP_UNIT,
    CUSTOM_END_TEXTURE_A_UNIT,
    CUSTOM_END_TEXTURE_B_UNIT,
    CUSTOM_END_TEXTURE_PERSIST_UNIT,
    GRAPHICS_PALETTE_UNIT,
    DEEP_COLORS_UNIT
};

typedef struct UIRenderData {
    unsigned screen_width, screen_height, cell_width, cell_height, screen_left, screen_top, full_framebuffer_width, full_framebuffer_height;
    Window *window;
    Screen *screen;
    OSWindow *os_window;
    GraphicsRenderData grd;
    WindowLogoRenderData *window_logo;
    float bg_alpha, inactive_text_alpha;
    bool has_background_image;
    color_type background_color; // RGB only
    monotonic_t now;
} UIRenderData;

static inline float
row_offset_for_screen(const Screen *screen) {
    if (!pixel_scroll_enabled(screen) || !screen->cell_size.height) return 0.f;
    return -1.f + (float)(rendered_pixel_scroll_offset_y(screen) / (double)screen->cell_size.height);
}

// Sprites {{{
typedef struct {
    int xnum, ynum, x, y, z, last_num_of_layers, last_ynum;
    GLuint texture_id;
    GLint max_texture_size, max_array_texture_layers;
    struct decorations_map {
        GLuint texture_id;
        unsigned width, height;
        size_t count;
    } decorations_map;
} SpriteMap;

static const SpriteMap NEW_SPRITE_MAP = {.xnum = 1, .ynum = 1, .last_num_of_layers = 1, .last_ynum = -1};
static GLint max_texture_size = 0, max_array_texture_layers = 0;
// The largest texture of the GPU, as it reports it: max_texture_size is that of
// the sprite atlas, which the tests change
static GLint gl_max_texture_size = 0;

static GLfloat
srgb_color(uint8_t color) {
    return srgb_lut[color];
}

static void
color_vec3(GLint location, color_type color) {
    glUniform3f(location, srgb_lut[(color >> 16) & 0xFF], srgb_lut[(color >> 8) & 0xFF], srgb_lut[color & 0xFF]);
}

static void
color_vec4_premult(GLint location, color_type color, GLfloat alpha) {
    glUniform4f(location, srgb_lut[(color >> 16) & 0xFF] * alpha, srgb_lut[(color >> 8) & 0xFF] * alpha, srgb_lut[color & 0xFF] * alpha, alpha);
}

static void
color_vec4(GLint location, color_type color, GLfloat alpha) {
    glUniform4f(location, srgb_lut[(color >> 16) & 0xFF], srgb_lut[(color >> 8) & 0xFF], srgb_lut[color & 0xFF], alpha);
}

// The colorspace of the window, colors and images tagged with another gamut
// are converted to it in the shaders, see docs/wide-gamut-protocol.rst.
// Matches the GAMUT_ constants in gamut.slang
static GLuint
output_gamut(const OSWindow *os_window) {
#ifdef __APPLE__
    (void)os_window;
    switch (OPT(macos_colorspace)) {
        case SRGB_COLORSPACE: return 0;
        case DISPLAY_P3_COLORSPACE: return 1;
        default: return 3; // the native colorspace of the display, unmanaged
    }
#else
    // A window the compositor was told is in a wide gamut is drawn in sRGB
    // that extends beyond zero and one, GAMUT_EXTENDED, and converted to its
    // gamut when it is copied to the screen
    return os_window->wide_gamut ? (os_window->wide_gamut | 4u) : 0;
#endif
}
// Of the OS window being drawn, for what is drawn without being handed the window
static GLuint current_output_gamut = 0;
static OSWindow *spatial_window = NULL;

static float
depth_shift(double depth, double pixels, unsigned width) {
    OSWindow *w = spatial_window;
    if (!w || !w->spatial.mode || !width) return 0.f;
    const double scale = w->spatial.mode == 2 ? w->spatial.scale : 1.;
    return (float)(w->spatial.eye * spatial_requested_disparity(depth, pixels, w->spatial.multiplier, w->spatial.positive, w->spatial.negative, w->spatial.fov) *
        w->viewport_width / (scale * width));
}

// A window in a wide gamut is drawn into textures that hold values below
// zero and above one before it is copied to the screen, which needs a GPU
// that can render to floating point textures
bool
wide_gamut_can_be_drawn(void) {
    static int supported = -1;
    if (supported < 0) {
        GLint bound_framebuffer = 0;
        GLuint texture = 0, framebuffer = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_framebuffer);
        glGenTextures(1, &texture);
        glGenFramebuffers(1, &framebuffer);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 1, 1, 0, GL_RGBA, GL_FLOAT, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        supported = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound_framebuffer);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
    }
    return supported > 0;
}

// Write a color into a std140 float4 whose fourth component is unused. Uses
// the same table as the shaders so the result is identical to converting the
// color in the shader.
static void
linear_color_vec4(GLfloat *dest, color_type color) {
    dest[0] = srgb_lut[(color >> 16) & 0xFF];
    dest[1] = srgb_lut[(color >> 8) & 0xFF];
    dest[2] = srgb_lut[color & 0xFF];
    dest[3] = 0.f;
}

bool
backbuffer_is_float(void) {
    // An HDR window has a linear float framebuffer rather than an sRGB one
    GLint fb = 0, component_type = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (!fb) glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component_type);
    return component_type == GL_FLOAT;
}

static void
set_os_window_output(OSWindow *w, int r, int g, int b, bool is_float) {
    w->linear_output = is_float;
    w->output.is_float = is_float;
    w->output.red_bits = MAX(0, r);
    w->output.green_bits = MAX(0, g);
    w->output.blue_bits = MAX(0, b);
    const int shallowest = MIN(w->output.red_bits, MIN(w->output.green_bits, w->output.blue_bits));
    w->output.encode_in_shader = !w->output.is_float && shallowest > 8;
    w->linear_output_checked = true;
}

// Reads what the surface of the window, whose context is current, holds. A floating point one holds linear values: it
// is the surface of an HDR window or of a deep color one on macOS. Any other one holds sRGB encoded values, encoded by
// OpenGL when it is 8 bits deep and by the final blit otherwise, as there are no sRGB formats deeper than that
void
check_os_window_output(OSWindow *w) {
    GLint bound = 0, type = 0, r = 0, g = 0, b = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
    if (bound) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &r);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &g);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &b);
    if (bound) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)bound);
    set_os_window_output(w, r, g, b, type == GL_FLOAT);
    debug_rendering("The surface of the window %llu has %d:%d:%d bits of %s color%s\n", w->id, w->output.red_bits, w->output.green_bits,
        w->output.blue_bits, w->output.is_float ? "floating point" : "normalized", w->output.encode_in_shader ? ", encoded to sRGB in the shader" : "");
}

// Whether what is drawn into the window is dithered, and if so to how many steps above zero in each channel. A surface
// of 8 bits or fewer is dithered to its own steps. A deeper one is dithered to the steps of 8 bits unless the system
// says the display the window is on shows more, as compositors and the system round a deep surface to an 8 bit display
// without dithering it; then a surface of normalized integers is dithered to its own steps and a floating point one
// not at all.
bool
output_dither_levels(const OSWindow *w, float levels[3]) {
    const int bits[3] = {w->output.red_bits, w->output.green_bits, w->output.blue_bits};
    // Dither is grain on e-paper, which shows it as noise that ghosts as it changes
    if (!OPT(output_dither) || os_window_is_eink(w) || bits[0] <= 0 || bits[1] <= 0 || bits[2] <= 0) return false;
    const bool shallow = !w->output.is_float && bits[0] <= 8 && bits[1] <= 8 && bits[2] <= 8;
    if (!shallow && w->output.depth.acted && w->output.is_float) return false;
    for (int i = 0; i < 3; i++) {
        // normalized integer channels have at most 16 bits in OpenGL, what is above that is not a step any display has
        const int b = shallow || w->output.depth.acted ? MIN(bits[i], 16) : 8;
        if (levels) levels[i] = (float)((1u << b) - 1u);
    }
    return true;
}

// The bits of the steps dithered to, of the narrowest channel, 0 when not dithering
int
output_dither_bits(const OSWindow *w) {
    float levels[3];
    if (!output_dither_levels(w, levels)) return 0;
    const float narrowest = MIN(levels[0], MIN(levels[1], levels[2]));
    return (int)lroundf(log2f(narrowest + 1.f));
}

// How long a change in the display a window is on has to last before it is acted on
#define DISPLAY_CHANGE_DEBOUNCE ms_to_monotonic_t(300ll)

// Takes in what the system says now about a property of the display of a window and returns whether what was acted on
// changed. The first answer is acted on at once, later ones once they have lasted, as a window dragged across the
// boundary between displays can be on either for a moment. wait is how long until a change seen will have lasted, or -1
bool
debounce_display_change(DisplayChange *c, unsigned now_value, monotonic_t now, monotonic_t *wait) {
    if (wait) *wait = -1;
    if (!c->known) {
        c->known = true;
        c->acted = c->seen = now_value;
        c->seen_at = now;
        return true;
    }
    if (now_value != c->seen) {
        c->seen = now_value;
        c->seen_at = now;
    }
    if (c->seen == c->acted) return false;
    const monotonic_t lasted = now - c->seen_at;
    if (lasted < DISPLAY_CHANGE_DEBOUNCE) {
        if (wait) *wait = DISPLAY_CHANGE_DEBOUNCE - lasted;
        return false;
    }
    c->acted = now_value;
    return true;
}

// Takes in whether the system says the display of the window is deep now and returns whether what was acted on
// changed, see debounce_display_change()
bool
update_output_depth(OSWindow *w, bool deep_now, monotonic_t now, monotonic_t *wait) {
    if (!debounce_display_change(&w->output.depth, deep_now, now, wait)) return false;
    w->output.refused_switch = 0;
    return true;
}

// Whether a window whose surface can be switched, on macOS, should have a floating point one. For deep color it has
// one on every display: one that is not known to show more than 8 bits gets dithered to the steps of 8 bits instead,
// see output_dither_levels(). With deep color off, or for a window whose colors are passed to the display unmanaged,
// which cannot have one for deep color, see wants_deep_color() in cocoa_window.m, it has one only for HDR while that is
// active, which needs a display that shows more than 8 bits, as every display with HDR does. The change from one
// display to another is acted on once it has lasted, see update_output_depth().
bool
os_window_wants_float_surface(const OSWindow *w) {
    if (OPT(deep_color) == DEEP_COLOR_ALWAYS && OPT(macos_colorspace) != 0) return true;
    return w->output.depth.acted && hdr_should_be_active(w);
}

// The window has a new surface, with a new OpenGL context that shares all objects with the old one, except those that
// hold others, framebuffers among them, which are made again from the textures they held
void
os_window_surface_switched(OSWindow *w) {
#define F(x) if (w->indirect_output.x) free_framebuffer(&w->indirect_output.x);
    F(framebuffer_id) F(extra_fbo_id) F(fbo_a_id) F(fbo_b_id)
#undef F
    w->indirect_output.attached_texture_generation = 0;
    w->indirect_output.extra_fbo_generation = 0;
    w->indirect_output.fbo_a_generation = 0;
    w->indirect_output.fbo_b_generation = 0;
    if (w->persist_fbo_id) free_framebuffer(&w->persist_fbo_id);
    if (w->persist_texture_id) free_texture(&w->persist_texture_id);
    w->persist_texture_generation = 0;
    w->viewport_size_dirty = true;
}

float
effective_hdr_headroom(const OSWindow *w) {
    if (OPT(hdr) == HDR_OFF || (OPT(hdr) == HDR_DETECT && !w->hdr_enabled_by_default)) return 1.f;
    return MAX(1.f, MIN(w->hdr_headroom, OPT(hdr_max_headroom)));
}

bool
hdr_should_be_active(const OSWindow *w) {
    switch (OPT(hdr)) {
        case HDR_DETECT: return w->hdr_enabled_by_default;
        case HDR_AUTO: return w->hdr_content;
        case HDR_ALWAYS: return true;
        case HDR_OFF: return false;
    }
    return false;
}


static void
clear_current_framebuffer(void) {
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

// Deep colors {{{
// The deep color table of a screen, see deep-colors.h, is drawn from an
// RGBA16UI texture with one texel per color, in rows of this many texels
static GLint
deep_colors_texture_width(void) {
    return MIN(4096, max_texture_size);
}

// The most colors a texture can hold, the limit of every deep color table
static size_t
deep_colors_texture_capacity(void) {
    return (size_t)deep_colors_texture_width() * (size_t)max_texture_size;
}

static void
upload_deep_colors(const DeepColor *items, size_t start, size_t end, GLint width) {
    // [start, end) as at most three rectangles: the tail of the first row,
    // whole rows, and the head of the last row
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    while (start < end) {
        const GLint x = start % width, y = start / width;
        size_t n = MIN(end - start, (size_t)(width - x));
        GLsizei rows = 1;
        if (x == 0 && n == (size_t)width) {
            rows = (GLsizei)((end - start) / width);
            n = (size_t)rows * width;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, rows > 1 ? width : (GLsizei)n, rows, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, items + start);
        start += n;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

static void
send_deep_colors_to_gpu(Screen *screen) {
    DeepColors *dc = (DeepColors *)tc_deep_colors_if_present(screen->text_cache);
    size_t start = 0, end = 0;
    const bool has_dirty = deep_colors_dirty_range(dc, &start, &end);
    if (!has_dirty && !(screen->reload_all_gpu_data && deep_colors_slots(dc))) return;
#define G screen->deep_colors_gpu
    const GLint width = deep_colors_texture_width();
    const size_t slots = MIN(deep_colors_slots(dc), deep_colors_texture_capacity());
    const size_t rows = (slots + width - 1) / width;
    bool full = screen->reload_all_gpu_data;
    if (!G.texture_id || G.width != (unsigned)width || rows > G.height) {
        if (G.texture_id) free_texture(&G.texture_id);
        G.width = width;
        G.height = MIN((size_t)max_texture_size, MAX(rows, (size_t)G.height * 2));
        glGenTextures(1, &G.texture_id);
        glBindTexture(GL_TEXTURE_2D, G.texture_id);
        // integer textures are incomplete unless sampled without filtering
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, G.width, G.height, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, NULL);
        full = true;
    } else glBindTexture(GL_TEXTURE_2D, G.texture_id);
    if (full) {
        start = 0;
        end = slots;
    }
    upload_deep_colors(deep_colors_data(dc), start, MIN(end, slots), width);
    glBindTexture(GL_TEXTURE_2D, 0);
    deep_colors_clear_dirty(dc);
#undef G
}

static void
bind_deep_colors(const Screen *screen) {
    // A screen without deep colors still needs a texture of the right type
    // bound, so that the sampler is valid, but never reads from it
    static GLuint empty_texture = 0;
    if (!screen->deep_colors_gpu.texture_id && !empty_texture) {
        static const uint16_t texel[4] = {0};
        glGenTextures(1, &empty_texture);
        glBindTexture(GL_TEXTURE_2D, empty_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, 1, 1, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, texel);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    glActiveTexture(GL_TEXTURE0 + DEEP_COLORS_UNIT);
    glBindTexture(GL_TEXTURE_2D, screen->deep_colors_gpu.texture_id ? screen->deep_colors_gpu.texture_id : empty_texture);
    glActiveTexture(GL_TEXTURE0);
}
// }}}

// Must be called as soon as there is a current OpenGL context, see
// initialize_gpu(). Font groups are created before that happens and size their
// sprite layout using these limits, so the sooner they are known the better.
static void
query_gpu_texture_limits(void) {
    if (max_texture_size) return;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &(max_texture_size));
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &(max_array_texture_layers));
#ifdef __APPLE__
    // An image larger than this is drawn from tiles of at most this size, see graphics.c
    gl_max_texture_size = MIN(8192, max_texture_size);
#else
    gl_max_texture_size = max_texture_size;
#endif
#ifdef __APPLE__
    // Since on Apple we could have multiple GPUs, with different capabilities,
    // upper bound the values according to the data from https://developer.apple.com/graphicsimaging/opengl/capabilities/
    max_texture_size = MIN(8192, max_texture_size);
    max_array_texture_layers = MIN(512, max_array_texture_layers);
#endif
    sprite_tracker_set_limits(max_texture_size, max_array_texture_layers);
    deep_colors_set_max_entries(deep_colors_texture_capacity());
}

SPRITE_MAP_HANDLE
alloc_sprite_map(void) {
    query_gpu_texture_limits();
    SpriteMap *ans = calloc(1, sizeof(SpriteMap));
    if (!ans) fatal("Out of memory allocating a sprite map");
    *ans = NEW_SPRITE_MAP;
    ans->max_texture_size = max_texture_size;
    ans->max_array_texture_layers = max_array_texture_layers;
    return (SPRITE_MAP_HANDLE)ans;
}

void
free_sprite_data(FONTS_DATA_HANDLE fg) {
    SpriteMap *sprite_map = (SpriteMap *)fg->sprite_map;
    if (sprite_map) {
        if (sprite_map->texture_id) free_texture(&sprite_map->texture_id);
        if (sprite_map->decorations_map.texture_id) free_texture(&sprite_map->decorations_map.texture_id);
        free(sprite_map);
        fg->sprite_map = NULL;
    }
}


static void
discard_pending_gl_errors(void) {
    // glad's error checking callback is installed only with --debug-rendering,
    // so in normal runs errors queue up silently. Drain the queue so that a
    // subsequent glGetError() reports only what we are interested in.
    for (unsigned i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++);
}

static bool
copy_texture_via_framebuffer(GLuint old_texture, GLuint new_texture, GLenum texture_type, GLint width, GLint height, GLint layers) {
    GLint prev_read_fbo;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
    discard_pending_gl_errors();
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    bool ok = true;
    for (GLint z = 0; z < layers && ok; z++) {
        if (texture_type == GL_TEXTURE_2D_ARRAY) glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, old_texture, 0, z);
        else glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, old_texture, 0);
        ok = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!ok) break;
        glBindTexture(texture_type, new_texture);
        if (texture_type == GL_TEXTURE_2D_ARRAY) glCopyTexSubImage3D(texture_type, 0, 0, 0, z, 0, 0, width, height);
        else glCopyTexSubImage2D(texture_type, 0, 0, 0, 0, 0, width, height);
        // A complete framebuffer does not mean the copy itself was accepted,
        // for instance if the two internal formats are not copy compatible.
        // Without this check we would claim success and leave the new texture
        // blank, losing every already rendered sprite.
        ok = glGetError() == GL_NO_ERROR;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
    glDeleteFramebuffers(1, &fbo);
    if (!ok) glBindTexture(texture_type, old_texture);
    return ok;
}

static void
copy_32bit_texture(GLuint old_texture, GLuint new_texture, GLenum texture_type) {
    // requires new texture to be at least as big as old texture. Assumes textures are 32bits per pixel
    GLint width, height, layers;
    glBindTexture(texture_type, old_texture);
    glGetTexLevelParameteriv(texture_type, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(texture_type, 0, GL_TEXTURE_HEIGHT, &height);
    glGetTexLevelParameteriv(texture_type, 0, GL_TEXTURE_DEPTH, &layers);
    if (GLAD_GL_ARB_copy_image) {
        glCopyImageSubData(old_texture, texture_type, 0, 0, 0, 0, new_texture, texture_type, 0, 0, 0, 0, width, height, layers);
        return;
    }
    if (copy_texture_via_framebuffer(old_texture, new_texture, texture_type, width, height, layers)) return;

    static bool copy_image_warned = false;
    // Neither ARB_copy_image nor a framebuffer copy is usable, do a slow roundtrip copy
    if (!copy_image_warned) {
        copy_image_warned = true;
        log_error("WARNING: Your system's OpenGL implementation cannot copy between textures on the GPU, falling back to a slower implementation");
    }

    GLint internal_format;
    glGetTexLevelParameteriv(texture_type, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format);
    GLenum format, type;
    switch (internal_format) {
        case GL_R8UI:
        case GL_R8I:
        case GL_R16UI:
        case GL_R16I:
        case GL_R32UI:
        case GL_R32I:
        case GL_RG8UI:
        case GL_RG8I:
        case GL_RG16UI:
        case GL_RG16I:
        case GL_RG32UI:
        case GL_RG32I:
        case GL_RGB8UI:
        case GL_RGB8I:
        case GL_RGB16UI:
        case GL_RGB16I:
        case GL_RGB32UI:
        case GL_RGB32I:
        case GL_RGBA8UI:
        case GL_RGBA8I:
        case GL_RGBA16UI:
        case GL_RGBA16I:
        case GL_RGBA32UI:
        case GL_RGBA32I:
            format = GL_RED_INTEGER;
            type = GL_UNSIGNED_INT;
            break;
        default:
            format = GL_RGBA;
            type = GL_UNSIGNED_INT_8_8_8_8;
            break;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    RAII_ALLOC(uint8_t, pixels, malloc((size_t)width * height * layers * 4u));
    if (!pixels) fatal("Out of memory");
    glGetTexImage(texture_type, 0, format, type, pixels);
    glBindTexture(texture_type, new_texture);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (texture_type == GL_TEXTURE_2D_ARRAY) glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, width, height, layers, format, type, pixels);
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, pixels);
}

static GLuint
setup_new_sprites_texture(GLenum texture_type) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(texture_type, tex);
    // We use GL_NEAREST otherwise glyphs that touch the edge of the cell
    // often show a border between cells
    glTexParameteri(texture_type, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(texture_type, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(texture_type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(texture_type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

static bool
decorations_map_new_size(size_t count, size_t current_capacity, size_t max_size, GLint *width, GLint *height) {
    const size_t max_capacity = max_size * max_size;
    if (count >= max_capacity) return false;
    size_t capacity = MAX(MAX(count + 256, current_capacity * 2), (size_t)4096);
    capacity = MIN(capacity, max_capacity);
    if (capacity <= max_size) {
        *width = capacity;
        *height = 1;
    } else {
        *width = max_size;
        *height = (capacity + max_size - 1) / max_size;
    }
    return true;
}

static void
realloc_sprite_decorations_texture_if_needed(FONTS_DATA_HANDLE fg) {
#define dm (sm->decorations_map)
    SpriteMap *sm = (SpriteMap *)fg->sprite_map;
    size_t current_capacity = (size_t)dm.width * dm.height;
    if (dm.count < current_capacity && dm.texture_id) return;
    GLint width, height;
    if (!decorations_map_new_size(dm.count, current_capacity, sm->max_texture_size, &width, &height))
        fatal("Max texture size too small for sprite decorations map, maybe switch to using a GL_TEXTURE_2D_ARRAY");
    const GLenum texture_type = GL_TEXTURE_2D;
    GLuint tex = setup_new_sprites_texture(texture_type);
    glTexImage2D(texture_type, 0, GL_R32UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    if (dm.texture_id) { // copy data from old texture
        copy_32bit_texture(dm.texture_id, tex, texture_type);
        free_texture(&dm.texture_id);
    }
    glBindTexture(texture_type, 0);
    dm.texture_id = tex;
    dm.width = width;
    dm.height = height;
#undef dm
}

static void
realloc_sprite_texture(FONTS_DATA_HANDLE fg) {
    unsigned int xnum, ynum, z, znum, width, height;
    sprite_tracker_current_layout(fg, &xnum, &ynum, &z);
    znum = z + 1;
    SpriteMap *sprite_map = (SpriteMap *)fg->sprite_map;
    width = xnum * fg->fcm.cell_width;
    height = ynum * (fg->fcm.cell_height + 1);
    const GLenum texture_type = GL_TEXTURE_2D_ARRAY;
    GLuint tex = setup_new_sprites_texture(texture_type);
    glTexStorage3D(texture_type, 1, GL_SRGB8_ALPHA8, width, height, znum);
    if (sprite_map->texture_id) { // copy old texture data into new texture
        copy_32bit_texture(sprite_map->texture_id, tex, texture_type);
        free_texture(&sprite_map->texture_id);
    }
    glBindTexture(texture_type, 0);
    sprite_map->last_num_of_layers = znum;
    sprite_map->last_ynum = ynum;
    sprite_map->texture_id = tex;
}

static void
ensure_sprite_map(FONTS_DATA_HANDLE fg) {
    SpriteMap *sprite_map = (SpriteMap *)fg->sprite_map;
    if (!sprite_map->texture_id) realloc_sprite_texture(fg);
    if (!sprite_map->decorations_map.texture_id) realloc_sprite_decorations_texture_if_needed(fg);
    // We have to rebind since we don't know if the texture was ever bound
    // in the context of the current OSWindow
    glActiveTexture(GL_TEXTURE0 + SPRITE_DECORATIONS_MAP_UNIT);
    glBindTexture(GL_TEXTURE_2D, sprite_map->decorations_map.texture_id);
    glActiveTexture(GL_TEXTURE0 + SPRITE_MAP_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sprite_map->texture_id);
}

void
send_sprite_to_gpu(FONTS_DATA_HANDLE fg, sprite_index idx, pixel *buf, sprite_index decoration_idx) {
    SpriteMap *sprite_map = (SpriteMap *)fg->sprite_map;
    unsigned int xnum, ynum, znum, x, y, z;
#define dm (sprite_map->decorations_map)
    if (idx >= dm.count) dm.count = idx + 1;
    realloc_sprite_decorations_texture_if_needed(fg);
    div_t d = div(idx, dm.width);
    x = d.rem;
    y = d.quot;
    glActiveTexture(GL_TEXTURE0 + SPRITE_DECORATIONS_MAP_UNIT);
    glBindTexture(GL_TEXTURE_2D, dm.texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &decoration_idx);
#undef dm
    sprite_tracker_current_layout(fg, &xnum, &ynum, &znum);
    if ((int)znum >= sprite_map->last_num_of_layers || (znum == 0 && (int)ynum > sprite_map->last_ynum)) {
        realloc_sprite_texture(fg);
        sprite_tracker_current_layout(fg, &xnum, &ynum, &znum);
    }
    glActiveTexture(GL_TEXTURE0 + SPRITE_MAP_UNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sprite_map->texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    sprite_index_to_pos(idx, xnum, ynum, &x, &y, &z);
    x *= fg->fcm.cell_width;
    y *= (fg->fcm.cell_height + 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, x, y, z, fg->fcm.cell_width, fg->fcm.cell_height + 1, 1, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, buf);
}

void
send_image_to_gpu(GLuint *tex_id, const void *data, GLsizei width, GLsizei height, bool is_opaque, bool is_float, bool is_4byte_aligned, bool linear, RepeatStrategy repeat) {
    if (!(*tex_id)) { glGenTextures(1, tex_id); }
    glBindTexture(GL_TEXTURE_2D, *tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, is_4byte_aligned ? 4 : 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    RepeatStrategy r;
    switch (repeat) {
        case REPEAT_MIRROR: r = GL_MIRRORED_REPEAT; break;
        case REPEAT_CLAMP: {
            static const GLfloat border_color[4] = {0};
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
            r = GL_CLAMP_TO_BORDER;
            break;
        }
        default: r = GL_REPEAT;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, r);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, r);
    glTexImage2D(GL_TEXTURE_2D, 0, is_float ? GL_RGBA16F : GL_SRGB_ALPHA, width, height, 0, is_opaque ? GL_RGB : GL_RGBA, is_float ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, data);
}

// The pixels of an image that the graphics shader decodes itself, see docs/silkie/precision-image-protocol.rst: high
// precision pixels, kept at their precision in a normalized texture of their own size, and eight bit pixels with a
// transfer function other than sRGB, which the GPU must not take for sRGB. The formats are all required, color
// renderable and filterable, in the desktop OpenGL 3.1 and later that Silkie uses. OpenGL ES has R16 and RGBA16 only
// with EXT_texture_norm16; a port to it would fall back to RGBA32F, as half floats cannot hold sixteen bit values.
// The wire bytes are little-endian, the words are read in the byte order of the host.
void
send_encoded_image_to_gpu(GLuint *tex_id, const void *data, GLsizei width, GLsizei height, uint8_t deep, bool is_opaque, bool is_4byte_aligned) {
    if (!(*tex_id)) { glGenTextures(1, tex_id); }
    glBindTexture(GL_TEXTURE_2D, *tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    static const GLfloat border_color[4] = {0};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    GLint internal_format;
    GLenum format, type;
    switch (deep) {
        // gray is in the red channel alone, the shader copies it to green and blue
        case DEEP_GRAY16: internal_format = GL_R16; format = GL_RED; type = GL_UNSIGNED_SHORT; break;
        // red in bits 0-9 of the word, green in 10-19, blue in 20-29 and alpha in 30-31
        case DEEP_RGB10A2: internal_format = GL_RGB10_A2; format = GL_RGBA; type = GL_UNSIGNED_INT_2_10_10_10_REV; break;
        case DEEP_RGBA16: internal_format = GL_RGBA16; format = GL_RGBA; type = GL_UNSIGNED_SHORT; break;
        default: internal_format = GL_RGBA8; format = is_opaque ? GL_RGB : GL_RGBA; type = GL_UNSIGNED_BYTE; break;
    }
    // The rows of a sixteen bit gray image of odd width start at every other byte
    glPixelStorei(GL_UNPACK_ALIGNMENT, is_4byte_aligned ? 4 : deep == DEEP_GRAY16 ? 2 : 1);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (deep) glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, type, data);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (deep) glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
#endif
}

// Images larger than a texture {{{
// The largest texture an image can have, 0 until a GPU context has told
unsigned
gpu_image_texture_limit(void) {
    return gl_max_texture_size > 0 ? (unsigned)gl_max_texture_size : 0;
}

typedef struct {
    GLint internal_format;
    GLenum format, type;
    unsigned px_sz;
    bool is_integer;
} TileFormatInfo;

// The formats are those of send_image_to_gpu(), send_encoded_image_to_gpu()
// and send_indexed_image_to_gpu(), with RGB always as RGBA, so that the apron
// round a tile at the edge of the image can be transparent as the border of
// an image in one texture is. All are required, filterable and color
// renderable, so that glGenerateMipmap() works, in the desktop OpenGL 3.1 and
// later that Silkie uses, except the integer ones, which are never filtered.
static TileFormatInfo
tile_format_info(unsigned format) {
    switch ((ImageTileFormat)format) {
        case TILE_HALF: return (TileFormatInfo){GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, false};
        case TILE_LINEAR8: return (TileFormatInfo){GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false};
        case TILE_GRAY16: return (TileFormatInfo){GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2, false};
        case TILE_RGB10A2: return (TileFormatInfo){GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4, false};
        case TILE_RGBA16: return (TileFormatInfo){GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8, false};
        case TILE_INDEX8: return (TileFormatInfo){GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1, true};
        case TILE_INDEX_ALPHA8: return (TileFormatInfo){GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 2, true};
        case TILE_SRGB8: break;
    }
    return (TileFormatInfo){GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false};
}

// Makes a texture for a tile, its pixels yet to be sent. Every tile has an
// apron of a pixel all round, so its texture is clamped to its edge. False
// when the GPU has no memory for it.
bool
alloc_image_tile_texture(GLuint *tex_id, GLsizei width, GLsizei height, unsigned format) {
    const TileFormatInfo f = tile_format_info(format);
    while (glGetError() != GL_NO_ERROR);  // so that an error after the allocation is the allocation's
    if (!(*tex_id)) glGenTextures(1, tex_id);
    glBindTexture(GL_TEXTURE_2D, *tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f.is_integer ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f.is_integer ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, f.is_integer ? 0 : 1000);
    glTexImage2D(GL_TEXTURE_2D, 0, f.internal_format, width, height, 0, f.format, f.type, NULL);
    if (glGetError() == GL_OUT_OF_MEMORY) {
        free_texture(tex_id);
        return false;
    }
    return true;
}

// Sends rows of a tile, from row y of its texture on, the words in the byte order of the host
void
upload_image_tile_rows(GLuint tex_id, GLint y, GLsizei width, GLsizei rows, unsigned format, const void *data) {
    const TileFormatInfo f = tile_format_info(format);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // the wire words of the high precision formats are little-endian, as in send_encoded_image_to_gpu()
    const bool swap = format == TILE_GRAY16 || format == TILE_RGB10A2 || format == TILE_RGBA16;
    if (swap) glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
#endif
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, rows, f.format, f.type, data);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if (swap) glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
#endif
}

// Trilinear filtering of an image drawn smaller than it is: without mipmaps
// the GPU reads only the pixels round the center of each fragment, and a
// large image drawn small aliases
void
generate_image_texture_mipmaps(GLuint tex_id) {
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
}

// A level of the mipmaps of an integer texture of indices, made on the CPU as the GPU cannot make them
void
upload_index_texture_level(GLuint tex_id, GLint level, GLsizei width, GLsizei height, unsigned bytes_per_pixel, const void *data) {
    const bool has_alpha = bytes_per_pixel == 2;
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, level, has_alpha ? GL_RG8UI : GL_R8UI, width, height, 0, has_alpha ? GL_RG_INTEGER : GL_RED_INTEGER, GL_UNSIGNED_BYTE, data);
}

// The levels the shader can read, all of them defined, so that the texture is complete for texelFetch() at any of them
void
finish_index_texture_mipmaps(GLuint tex_id, GLint max_level) {
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_level);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, max_level ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
}
// }}}

// Copies a texture backed by a GPU surface, a rectangle texture on macOS and a 2D one on Linux, into an image texture
void
copy_surface_texture_to_texture(bool rectangle, GLuint source_tex_id, GLuint *tex_id, GLsizei width, GLsizei height) {
    const GLenum source_target = rectangle ? GL_TEXTURE_RECTANGLE : GL_TEXTURE_2D;
    GLint current_fb;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &current_fb);
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, source_target, source_tex_id, 0);
    if (!(*tex_id)) glGenTextures(1, tex_id);
    glBindTexture(GL_TEXTURE_2D, *tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    static const GLfloat border_color[4] = {0};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB_ALPHA, 0, 0, width, height, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, current_fb);
    glDeleteFramebuffers(1, &fb);
}

// Replaces a rectangle of an RGBA image sent with send_image_to_gpu()
void
update_image_on_gpu(GLuint tex_id, const void *data, GLint x, GLint y, GLsizei width, GLsizei height, bool is_float) {
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, is_float ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, data);
}

// The palette of a palette image is a row of slots, each straight alpha RGBA in
// sRGB, read by index in the shader. An sRGB texture gives linear colors as for
// other images.
void
send_palette_to_gpu(GLuint *tex_id, const void *rgba, GLsizei slots) {
    if (!(*tex_id)) { glGenTextures(1, tex_id); }
    glBindTexture(GL_TEXTURE_2D, *tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, slots, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

// Replaces count slots of a palette from first on
void
update_palette_on_gpu(GLuint tex_id, const void *rgba, GLint first, GLsizei count) {
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, first, 0, count, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

// Indexed color images are stored as integer textures with the palette index
// in the red channel and, if bytes_per_pixel is 2, alpha in the green channel.
// Integer textures cannot be filtered, the shader does its own filtering after
// the palette lookup.
void
send_indexed_image_to_gpu(GLuint *tex_id, const void *data, GLsizei width, GLsizei height, unsigned bytes_per_pixel, bool is_4byte_aligned) {
    if (!(*tex_id)) { glGenTextures(1, tex_id); }
    glBindTexture(GL_TEXTURE_2D, *tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, is_4byte_aligned ? 4 : 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // the mipmaps made for an earlier upload, if any, are of other pixels
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    const bool has_alpha = bytes_per_pixel == 2;
    glTexImage2D(GL_TEXTURE_2D, 0, has_alpha ? GL_RG8UI : GL_R8UI, width, height, 0, has_alpha ? GL_RG_INTEGER : GL_RED_INTEGER, GL_UNSIGNED_BYTE, data);
}

// }}}

// Rounded rect {{{
static double
thickness_as_float(const OSWindow *os_window, unsigned level) {
    level = MIN(level, arraysz(OPT(box_drawing_scale)));
    double pts = OPT(box_drawing_scale)[level];
    double dpi = (os_window->fonts_data->logical_dpi_x + os_window->fonts_data->logical_dpi_y) / 2.0;
    return line_thickness_scale_for_font_size(os_window->fonts_data) * pts * dpi / 72.0;
}


static void
draw_rounded_rect(
    Viewport rect,
    unsigned framebuffer_height,
    float thickness,
    unsigned corner_radius_px,
    color_type srgb_color,
    color_type srgb_background,
    float bg_alpha,
    color_type srgb_outer,
    float outer_alpha) {
    bind_program(ROUNDED_RECT_PROGRAM);
    color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "color"), srgb_color, 1.f);
    color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "background_color"), srgb_background, bg_alpha);
    color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "outer_color"), srgb_outer, outer_alpha);
    // y co-ord has to be changed to co-ord system with origin at bottom left
    float y = (float)framebuffer_height - (float)(rect.top + rect.height);
    glUniform4f(program_uniform_location(ROUNDED_RECT_PROGRAM, "rect"), rect.left, y, rect.width, rect.height);
    glUniform2f(program_uniform_location(ROUNDED_RECT_PROGRAM, "params"), thickness, corner_radius_px);
    save_viewport_using_top_left_origin(rect.left, rect.top, rect.width, rect.height, framebuffer_height);
    draw_quad(true, 0);
    restore_viewport();
}
// }}}

// Cell {{{

enum { CELL_RENDER_DATA_BINDING_POINT = 0, COLOR_TABLE_BINDING_POINT = 1, BORDER_COLORS_BINDING_POINT = 2, CUSTOM_END_DATA_BINDING_POINT = 3 };
enum { BORDER_COLORS_GLOBAL_BUFFER };
// VAOs used only to hold buffers for UBOs that are shared amongst programs/windows,
// their vertex attribute/array facilities are unused.
static ssize_t shader_globals_vao_idx = -1;
static ssize_t custom_end_vao_idx = -1;

// A VAO with one uniform buffer of size bytes, or -1 when there is no memory for it
static ssize_t
create_uniform_vao(GLsizeiptr size, GLenum usage) {
    ssize_t vao_idx = create_vao();
    if (vao_idx < 0) return -1;
    ssize_t bufnum = add_buffer_to_vao(vao_idx, GL_UNIFORM_BUFFER);
    if (bufnum < 0) {
        remove_vao(vao_idx);
        return -1;
    }
    alloc_vao_buffer(vao_idx, size, (size_t)bufnum, usage);
    return vao_idx;
}

// The border colors change on every draw, but are shared amongst all border VAOs (only one is ever drawn at a time).
// Returns whether the VAO that holds them exists, made now if it did not, as when there was no memory for it before
static bool
ensure_shader_globals_vao(void) {
    if (shader_globals_vao_idx < 0) shader_globals_vao_idx = create_uniform_vao(program_uniform_block(BORDERS_PROGRAM, "Colors").size, GL_STREAM_DRAW);
    return shader_globals_vao_idx >= 0;
}

typedef enum { CUSTOM_SHADER_TEXTURE_DEFAULT, CUSTOM_SHADER_TEXTURE_A, CUSTOM_SHADER_TEXTURE_B, CUSTOM_SHADER_TEXTURE_PERSIST } NamedTexture;

// The user events of a group, by their index in the pipeline
typedef struct ShaderUserEventList {
    size_t *indices;
    size_t count, capacity;
} ShaderUserEventList;

typedef struct CustomShaderGroup {
    struct {
        float x, y;
    } viewport_pos;
    struct {
        float w, h;
    } viewport_size;
    NamedTexture output_texture;
    // Builtin events, and below user events, that start the animation, that
    // start the animation when stopped and vice versa, and that stop it
    ShaderAnimEventMask animation_start_events, animation_toggle_events, animation_end_events;
    ShaderUserEventList animation_start_user_events, animation_toggle_user_events, animation_end_user_events;
    monotonic_t animation_end_duration;          // nanoseconds; 0 = no time limit, < 0 = use cursor_stop_blinking_after
    monotonic_t animation_duration;              // nanoseconds for animation_progress to reach one; < 0 = same as the stop duration
    monotonic_t animation_reverse;               // nanoseconds to play the animation in reverse when stopped; 0 = no, < 0 = animation_duration
    unsigned animation_loops;                    // times to play the animation forwards and in reverse when reversing
    monotonic_t animation_step;                  // nanoseconds between animation samples
    Animation *animation_curve;                  // parsed easing curve; NULL = no animation
    bool animation_curve_is_shared;              // true if animation_curve is owned by an earlier group
    bool attached;                               // true = always active when the preceding group is active
    bool uses_pointer_map;                       // true when the group has shaders whose pointer_map() is used
} CustomShaderGroup;

typedef struct CustomShaderPipeline {
    unsigned textures; // bit mask of named textures from above enum
    CustomShaderGroup *groups;
    size_t num_groups;
    // Words in a ShaderBitset of groups, never zero, as the active_groups uniform array has this size
    size_t group_words;
    // Scratch space owned by the pipeline, so that drawing a frame and mapping
    // pointer events do not allocate: the progress of each group and a set of
    // group_words words of groups
    float *group_progress;
    uint32_t *group_scratch;
    bool active;
    bool bypass_builtin_trail;                      // true when any group subscribes to cursor-trail-move
    ShaderAnimEventMask all_animation_start_events; // union of the builtin start and toggle events of all groups
    bool has_pointer_map;                           // true when a shader defines pointer_map() to move pointer events
    // Names (without the user: prefix) of the user events the pipeline uses,
    // owned by the pipeline. The index of a name is the index of the event.
    char **user_events;
    size_t num_user_events, user_events_capacity;
} CustomShaderPipeline;

static struct {
    CustomShaderPipeline end;
    size_t count;
    unsigned generation; // incremented every time the end pipeline changes
} custom_shaders;

static struct {
    GLuint texture_id, framebuffer_id;
} pointer_map_target;

static void
write_uint_array_to_ubo(void *dest, const GLuint *src, size_t count, const ArrayInformation *ai) {
    GLint stride = MAX((GLint)sizeof(GLuint), ai->stride);
    uint8_t *buf = (uint8_t *)dest + ai->offset;
    for (size_t i = 0; i < count; i++, buf += stride) memcpy(buf, src + i, sizeof(GLuint));
}

// The gamma LUT is a compile time constant that lives in the default uniform
// block of every program that needs it, rather than in a UBO shared between
// them. Uniform values are part of program object state, which the GL
// implementation must maintain, while the data store of a buffer object is
// ours and some drivers silently discard it, notably macOS waking from sleep
// or reconfiguring its displays. Every color in every affected program then
// resolves to garbage with nothing to ever restore it. Uploading it costs a
// single GL call per program, so this can simply be called again whenever we
// have reason to believe GPU state was lost.
// See https://github.com/kovidgoyal/kitty/issues/10571
void
upload_gamma_lut(void) {
#define U(program)                                                    \
    {                                                                 \
        bind_program(program);                                        \
        GLint loc = program_uniform_location(program, "gamma_lut");   \
        if (loc >= 0) glUniform1fv(loc, arraysz(srgb_lut), srgb_lut); \
    }
    for (int i = CELL_PROGRAM; i < CELL_PROGRAM_SENTINEL; i++) U(i);
    U(BORDERS_PROGRAM);
    U(PADDING_PROGRAM);
    U(GRAPHICS_INDEXED_PROGRAM);
    U(GRAPHICS_INDEXED_UNSCALED_PROGRAM);
#undef U
    unbind_program();
}

static void
init_cell_program(void) {
    for (int i = CELL_PROGRAM; i < CELL_PROGRAM_SENTINEL; i++) {
        UniformBlock crd = program_uniform_block(i, "CellRenderData");
        glUniformBlockBinding(program_id(i), crd.index, CELL_RENDER_DATA_BINDING_POINT);
        UniformBlock ct = program_uniform_block(i, "ColorTable");
        glUniformBlockBinding(program_id(i), ct.index, COLOR_TABLE_BINDING_POINT);
    }

    // Sanity check to ensure the attribute location binding worked
#define C(p, name, expected)                                                                                                             \
    {                                                                                                                                    \
        int aloc = attrib_location(p, #name);                                                                                            \
        if (aloc != expected && aloc != -1) fatal("The attribute location for %s is %d != %d in program: %d", #name, aloc, expected, p); \
    }
    for (int p = CELL_PROGRAM; p < CELL_PROGRAM_SENTINEL; p++) {
        C(p, colors, 0);
        C(p, sprite_idx, 1);
        C(p, is_selected, 2);
        C(p, decorations_sprite_map, 3);
    }
#undef C
    UniformBlock border_colors = program_uniform_block(BORDERS_PROGRAM, "Colors");
    glUniformBlockBinding(program_id(BORDERS_PROGRAM), border_colors.index, BORDER_COLORS_BINDING_POINT);

    // The padding program shares the cell background computation and so uses the
    // same uniform blocks bound to the same binding points as the cell programs.
    {
        UniformBlock crd = program_uniform_block(PADDING_PROGRAM, "CellRenderData");
        glUniformBlockBinding(program_id(PADDING_PROGRAM), crd.index, CELL_RENDER_DATA_BINDING_POINT);
        UniformBlock ct = program_uniform_block(PADDING_PROGRAM, "ColorTable");
        glUniformBlockBinding(program_id(PADDING_PROGRAM), ct.index, COLOR_TABLE_BINDING_POINT);
        // and the deep colors, bound by draw_cells()
        GLint loc = try_program_uniform_location(PADDING_PROGRAM, "deep_colors");
        if (loc >= 0) {
            bind_program(PADDING_PROGRAM);
            glUniform1i(loc, DEEP_COLORS_UNIT);
            unbind_program();
        }
    }
#define C(name, expected)                                                                                                                     \
    {                                                                                                                                         \
        int aloc = attrib_location(PADDING_PROGRAM, #name);                                                                                   \
        if (aloc != expected && aloc != -1) fatal("The attribute location for %s is %d != %d in the padding program", #name, aloc, expected); \
    }
    C(colors, 0);
    C(sprite_idx, 1);
    C(is_selected, 2);
#undef C
    // Indexed color images get their colors from the color table of the window they are in
    for (int i = GRAPHICS_INDEXED_PROGRAM; i <= GRAPHICS_INDEXED_UNSCALED_PROGRAM; i++) {
        UniformBlock ct = program_uniform_block(i, "ColorTable");
        glUniformBlockBinding(program_id(i), ct.index, COLOR_TABLE_BINDING_POINT);
    }

    ensure_shader_globals_vao();
    upload_gamma_lut();
    bind_shader_globals_to_current_context();
}

// Whether the group has events of its own that turn it on, as opposed to
// running on every frame or only following its predecessor via attach
static inline bool
group_has_own_animation(const CustomShaderGroup *cg) {
    return (cg->animation_start_events | cg->animation_toggle_events) != 0 || cg->animation_start_user_events.count ||
           cg->animation_toggle_user_events.count;
}

// Whether the OS window has animation state for every group of the current pipeline
static inline bool
has_group_anim_state(const OSWindow *os_window) {
    return os_window->shader_group_anim_capacity >= custom_shaders.end.num_groups;
}

// Whether any of the user events in l was fired in the OS window
static bool
user_events_fired(const ShaderUserEventList *l, const OSWindow *os_window) {
    if (!os_window->shader_user_events.pending) return false;
    for (size_t i = 0; i < l->count; i++) {
        if (shader_bitset_has(&os_window->shader_user_events.fired, l->indices[i])) return true;
    }
    return false;
}

static void
clear_user_events(OSWindow *os_window) {
    if (os_window->shader_user_events.fired.num_words) zero_at_ptr_count(os_window->shader_user_events.fired.words, os_window->shader_user_events.fired.num_words);
    os_window->shader_user_events.pending = false;
}

// Grows the set to hold bit i, the new words unset
static bool
ensure_bitset_holds(ShaderBitset *s, size_t i) {
    const size_t needed = i / 32 + 1;
    if (needed <= s->num_words) return true;
    if (needed > SIZE_MAX / sizeof(uint32_t)) return false;
    uint32_t *w = realloc(s->words, needed * sizeof(uint32_t));
    if (!w) return false;
    zero_at_ptr_count(w + s->num_words, needed - s->num_words);
    s->words = w;
    s->num_words = needed;
    return true;
}

// Queues the user event with the specified index for the OS window
static bool
queue_user_event(OSWindow *os_window, size_t idx) {
    if (!ensure_bitset_holds(&os_window->shader_user_events.fired, idx)) return false;
    os_window->shader_user_events.fired.words[idx / 32] |= 1u << (idx % 32);
    os_window->shader_user_events.pending = true;
    return true;
}

// Whether the group runs only when the animation state machine says it is active
static inline bool
group_is_animated(const CustomShaderGroup *cg) {
    return cg->attached || group_has_own_animation(cg);
}

// Whether group g draws this frame: always for non-animated, non-attached
// groups, otherwise when the animation state machine says it is active
static inline bool
group_is_running(const OSWindow *os_window, size_t g) {
    return !group_is_animated(custom_shaders.end.groups + g) || (g < os_window->shader_group_anim_capacity && os_window->shader_group_anim[g].active);
}

// How long after starting the group stops by itself, zero if it does not
static inline monotonic_t
group_stop_duration(const CustomShaderGroup *cg) {
    return cg->animation_end_duration < 0 ? OPT(cursor_stop_blinking_after) : cg->animation_end_duration;
}

// How long animation_progress takes to rise to one, zero if it stays at zero
static inline monotonic_t
group_ramp_duration(const CustomShaderGroup *cg) {
    return cg->animation_duration < 0 ? group_stop_duration(cg) : cg->animation_duration;
}

// How long animation_progress takes to fall from one to zero after the group
// is stopped, zero if the group turns off immediately instead
static inline monotonic_t
group_reverse_duration(const CustomShaderGroup *cg) {
    return cg->animation_reverse < 0 ? group_ramp_duration(cg) : cg->animation_reverse;
}

// The animation_progress of an active group before applying the easing curve
static double
group_linear_progress(const CustomShaderGroup *cg, const ShaderGroupAnimState *s, monotonic_t now) {
    double ans = 0;
    if (s->reverse_ends_at) {
        const monotonic_t d = group_reverse_duration(cg);
        if (d > 0) ans = (double)(s->reverse_ends_at - now) / (double)d;
    } else {
        const monotonic_t d = group_ramp_duration(cg);
        if (d > 0) ans = (double)(now - s->ramp_started_at) / (double)d;
    }
    return MAX(0., MIN(ans, 1.));
}

// The animation_progress of every group in the current pipeline. Attached
// groups without events of their own share the progress of the group they are
// attached to, so that multi-pass effects animate in sync.
static void
custom_shader_group_progress(const OSWindow *os_window, monotonic_t now, float *ans) {
    if (!has_group_anim_state(os_window)) {
        if (custom_shaders.end.num_groups) zero_at_ptr_count(ans, custom_shaders.end.num_groups);
        return;
    }
    for (size_t g = 0; g < custom_shaders.end.num_groups; g++) {
        const CustomShaderGroup *cg = custom_shaders.end.groups + g;
        const ShaderGroupAnimState *s = os_window->shader_group_anim + g;
        ans[g] = 0.f;
        if (!group_is_animated(cg) || !s->active) continue;
        if (g > 0 && cg->attached && !group_has_own_animation(cg)) {
            ans[g] = ans[g - 1];
            continue;
        }
        const double t = group_linear_progress(cg, s, now);
        ans[g] = (float)(cg->animation_curve ? apply_easing_curve(cg->animation_curve, t, group_ramp_duration(cg)) : t);
    }
}

static void
start_group_animation(const CustomShaderGroup *cg, ShaderGroupAnimState *s, monotonic_t now) {
    // Restarting while playing in reverse or looping continues from the
    // current progress rather than jumping back to zero
    if (s->active && (s->reverse_ends_at || s->looping)) {
        s->ramp_started_at = now - (monotonic_t)(group_linear_progress(cg, s, now) * (double)group_ramp_duration(cg));
    } else s->ramp_started_at = now;
    s->active = true;
    s->started_at = now;
    s->reverse_ends_at = 0;
    s->loops_left = 0;
    s->looping = false;
}

static void
stop_group_animation(const CustomShaderGroup *cg, ShaderGroupAnimState *s, monotonic_t now) {
    // Playing in reverse takes as long as falling from the current progress
    // to zero, so a group stopped part way through rising does not jump to one
    const monotonic_t reverse_for = (monotonic_t)(group_linear_progress(cg, s, now) * (double)group_reverse_duration(cg));
    if (reverse_for > 0) {
        s->reverse_ends_at = now + reverse_for;
        s->loops_left = cg->animation_loops > 1 ? cg->animation_loops - 1 : 0;
    } else s->active = false;
}

// Moves a group that is playing in reverse or looping on to whichever phase it
// should be in at now, turning it off once it has finished. The phases are
// advanced from the time the previous one ended rather than from now, so that
// loops do not drift when the state machine runs late.
static void
advance_stopped_group_animation(const CustomShaderGroup *cg, ShaderGroupAnimState *s, monotonic_t now, size_t i) {
    while (true) {
        if (s->reverse_ends_at && now >= s->reverse_ends_at) {
            if (s->loops_left) {
                s->loops_left--;
                s->ramp_started_at = s->reverse_ends_at;
                s->reverse_ends_at = 0;
                s->looping = true;
            } else {
                s->active = false;
                s->reverse_ends_at = 0;
                debug_rendering("Custom shader group %zu finished playing in reverse\n", i);
                break;
            }
        } else if (s->looping && now >= s->ramp_started_at + group_ramp_duration(cg)) {
            // Loops only start after a reverse, which needs both durations to be non-zero, so this terminates
            s->looping = false;
            s->reverse_ends_at = s->ramp_started_at + group_ramp_duration(cg) + group_reverse_duration(cg);
        } else break;
    }
}

// Non-animated, non-attached groups are active from the moment the pipeline
// loads; animated groups start inactive and attached groups inherit the state
// of their predecessor.
static bool
group_is_initially_active(const CustomShaderGroup *cg, bool predecessor_is_active) {
    return cg->attached ? predecessor_is_active : !group_has_own_animation(cg);
}

static bool
pipeline_has_initially_active_group(void) {
    bool prev = false;
    for (size_t i = 0; i < custom_shaders.end.num_groups; i++) {
        prev = group_is_initially_active(custom_shaders.end.groups + i, prev);
        if (prev) return true;
    }
    return false;
}

static void
init_custom_programs(void) {
    custom_shaders.count = 0;
    for (int i = CUSTOM_END_PROGRAM; i < NUM_PROGRAMS; i++) {
        Program *p = program_ptr(i);
        if (p && p->id) {
            custom_shaders.count++;
            if (i == CUSTOM_END_PROGRAM) {
                bind_program(i);
                glUniform1i(program_uniform_location(i, "backbuffer"), GRAPHICS_UNIT);
#define set_optional_sampler(name, unit)                   \
    {                                                      \
        GLint loc = try_program_uniform_location(i, name); \
        if (loc >= 0) glUniform1i(loc, unit);              \
    }
                set_optional_sampler("a", CUSTOM_END_TEXTURE_A_UNIT);
                set_optional_sampler("b", CUSTOM_END_TEXTURE_B_UNIT);
                set_optional_sampler("persist", CUSTOM_END_TEXTURE_PERSIST_UNIT);
#undef set_optional_sampler
                GLint group_loc = try_program_uniform_location(i, "group");
                if (group_loc >= 0) glUniform1i(group_loc, 0);
                else if (custom_shaders.end.num_groups > 1) {
                    // Every group is dispatched by setting this uniform, so without it
                    // all of them render as group zero, which for a multi pass pipeline
                    // means the intermediate passes are drawn to the screen.
                    log_error(
                        "The custom shader pipeline has %zu groups but its group uniform was optimized away by the"
                        " GPU driver. Every group will render as group zero.",
                        custom_shaders.end.num_groups);
                }
                UniformBlock ubd = program_uniform_block(i, "KittyCustomShaderData");
                glUniformBlockBinding(program_id(i), ubd.index, CUSTOM_END_DATA_BINDING_POINT);
                // Without memory for it the custom end shader is not run, see stop_os_window_rendering()
                if (custom_end_vao_idx < 0) custom_end_vao_idx = create_uniform_vao(ubd.size, GL_STREAM_DRAW);
            }
        }
    }
    // Free global named textures that are no longer declared in the pipeline
    const bool drop_a = !(custom_shaders.end.textures & (1u << CUSTOM_SHADER_TEXTURE_A));
    const bool drop_b = !(custom_shaders.end.textures & (1u << CUSTOM_SHADER_TEXTURE_B));
    if (drop_a) {
        if (global_state.layers_render_texture.texture_a_id) free_texture(&global_state.layers_render_texture.texture_a_id);
        if (global_state.layers_render_texture.texture_a_fbo_id) free_framebuffer(&global_state.layers_render_texture.texture_a_fbo_id);
    }
    if (drop_b) {
        if (global_state.layers_render_texture.texture_b_id) free_texture(&global_state.layers_render_texture.texture_b_id);
        if (global_state.layers_render_texture.texture_b_fbo_id) free_framebuffer(&global_state.layers_render_texture.texture_b_fbo_id);
    }
    // persist is per-window and freed lazily in start_os_window_rendering
    for (size_t w = 0; w < global_state.num_os_windows; w++) {
        OSWindow *osw = global_state.os_windows + w;
        // Per window framebuffers must be attached afresh to the textures that
        // replace the ones freed above, should a later pipeline declare them again
        if (drop_a) osw->indirect_output.fbo_a_generation = 0;
        if (drop_b) osw->indirect_output.fbo_b_generation = 0;
        init_shader_animation_state(osw);
        // Synthesize focus events for windows that already have focus so that
        // animations triggered by os-window-focus-in start on the first render
        // after shader compilation (the focus-in event fired before shaders were
        // compiled and was consumed without being processed).
        if (osw->is_focused) record_os_window_focus_change_for_shaders(osw, true);
    }
    debug_rendering(
        "Custom shaders: %zu program(s), %zu group(s), textures_mask=0x%x, initially_active=%d\n",
        custom_shaders.count,
        custom_shaders.end.num_groups,
        custom_shaders.end.textures,
        custom_shaders.count > 0 && pipeline_has_initially_active_group());
}

// Reset the per-OS-window animation state to what it should be immediately
// after the current pipeline was loaded. Called both when the pipeline changes
// and when an OS window is created, so that the MONOTONIC_T_MAX sentinels in
// ShaderAnimState are never left at their zero initialized values -- a zero
// min_step/next_end_at reads as "an animation is running and its deadline has
// already passed", which would make the renderer redraw on every single tick.
void
init_shader_animation_state(OSWindow *osw) {
    // User events are numbered by the pipeline, so ones pending are meaningless for another
    clear_user_events(osw);
    osw->shader_anim.min_step = MONOTONIC_T_MAX;
    osw->shader_anim.next_end_at = MONOTONIC_T_MAX;
    osw->shader_anim.has_active_shaders = false;
    const size_t n = custom_shaders.end.num_groups;
    if (n > osw->shader_group_anim_capacity) {
        ShaderGroupAnimState *s = n <= SIZE_MAX / sizeof(ShaderGroupAnimState) ? realloc(osw->shader_group_anim, n * sizeof(ShaderGroupAnimState)) : NULL;
        if (!s) {
            log_error("Out of memory allocating the animation state of %zu custom shader groups, the custom shaders will not be drawn", n);
            return;
        }
        osw->shader_group_anim = s;
        osw->shader_group_anim_capacity = n;
    }
    if (n) zero_at_ptr_count(osw->shader_group_anim, n);
    bool prev_init = false, any_initially_active = false;
    for (size_t i = 0; i < custom_shaders.end.num_groups; i++) {
        const CustomShaderGroup *cg = &custom_shaders.end.groups[i];
        const bool this_init = group_is_initially_active(cg, prev_init);
        // Only attached groups are tracked by the state machine when initially
        // active; always-active non-attached groups never enter it at all.
        if (cg->attached && this_init) osw->shader_group_anim[i].active = true;
        if (this_init) any_initially_active = true;
        prev_init = this_init;
    }
    osw->shader_anim.has_active_shaders = custom_shaders.count > 0 && any_initially_active;
}

// The names of the builtin events in mask and of the user events fired in the
// OS window, separated by |, for debug output. The returned string is valid
// until the next call.
static const char *
shader_anim_event_mask_str(ShaderAnimEventMask mask, const OSWindow *os_window) {
    static const struct {
        ShaderAnimEventMask bit;
        const char *name;
    } events[] = {
        {1u << SHADER_ANIM_EVENT_POINTER_LEFT_BUTTON_PRESS, "pointer-left-button-press"},
        {1u << SHADER_ANIM_EVENT_OS_WINDOW_FOCUS_IN, "os-window-focus-in"},
        {1u << SHADER_ANIM_EVENT_OS_WINDOW_FOCUS_OUT, "os-window-focus-out"},
        {1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_IN, "window-focus-in"},
        {1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_OUT, "window-focus-out"},
        {1u << SHADER_ANIM_EVENT_TAB_CHANGE, "tab-change"},
        {1u << SHADER_ANIM_EVENT_BELL_IN_WINDOW, "bell-in-window"},
        {1u << SHADER_ANIM_EVENT_USER_ACTIVITY, "user-activity"},
        {1u << SHADER_ANIM_EVENT_USER_IDLE, "user-idle"},
        {1u << SHADER_ANIM_EVENT_CURSOR_TRAIL_MOVE, "cursor-trail-move"},
        {1u << SHADER_ANIM_EVENT_CURSOR_TRAIL_STOP, "cursor-trail-stop"},
        {1u << SHADER_ANIM_EVENT_COMMAND_START, "command-start"},
        {1u << SHADER_ANIM_EVENT_COMMAND_FINISH, "command-finish"},
        {1u << SHADER_ANIM_EVENT_ALTERNATE_SCREEN_ENTER, "alternate-screen-enter"},
        {1u << SHADER_ANIM_EVENT_ALTERNATE_SCREEN_EXIT, "alternate-screen-exit"},
    };
    static const char user_prefix[] = "user:";
    static char *buf = NULL;
    static size_t capacity = 0;
    // Room for every name with a separator or the terminating null after each
    size_t needed = 0;
    for (size_t i = 0; i < arraysz(events); i++) {
        if (mask & events[i].bit) needed += strlen(events[i].name) + 1;
    }
    const ShaderBitset *fired = &os_window->shader_user_events.fired;
    for (size_t i = 0; i < custom_shaders.end.num_user_events; i++) {
        if (shader_bitset_has(fired, i)) needed += sizeof(user_prefix) - 1 + strlen(custom_shaders.end.user_events[i]) + 1;
    }
    if (!needed) return "(none)";
    if (needed > capacity) {
        char *nb = realloc(buf, needed);
        if (!nb) return "(out of memory)";
        buf = nb;
        capacity = needed;
    }
    char *p = buf;
#define add_name(prefix, prefix_len, name)       \
    {                                            \
        if (p != buf) *p++ = '|';                \
        memcpy(p, prefix, prefix_len);           \
        p += prefix_len;                         \
        const size_t name_len = strlen(name);    \
        memcpy(p, name, name_len);               \
        p += name_len;                           \
    }
    for (size_t i = 0; i < arraysz(events); i++) {
        if (mask & events[i].bit) add_name("", 0, events[i].name);
    }
    for (size_t i = 0; i < custom_shaders.end.num_user_events; i++) {
        if (shader_bitset_has(fired, i)) add_name(user_prefix, sizeof(user_prefix) - 1, custom_shaders.end.user_events[i]);
    }
#undef add_name
    *p = '\0';
    return buf;
}

monotonic_t
update_custom_shader_animations(ShaderAnimEventMask event_mask, monotonic_t now, OSWindow *os_window) {
    if (!custom_shaders.count || !has_group_anim_state(os_window)) {
        clear_user_events(os_window);
        os_window->shader_anim = (ShaderAnimState){.min_step = MONOTONIC_T_MAX, .next_end_at = MONOTONIC_T_MAX};
        return MONOTONIC_T_MAX;
    }
    // Fast path: no events and no duration-bounded animation expiring this frame.
    // Cached state is still valid — skip group iteration entirely.
    if (!event_mask && !os_window->shader_user_events.pending && now < os_window->shader_anim.next_end_at) return os_window->shader_anim.min_step;

    // Slow path: update per-group state and recompute cached values in one pass.
    CustomShaderPipeline *p = &custom_shaders.end;
    bool any_active = false;
    monotonic_t min_step = MONOTONIC_T_MAX;
    monotonic_t next_end = MONOTONIC_T_MAX;
    bool prev_active = false;
    for (size_t i = 0; i < p->num_groups; i++) {
        const CustomShaderGroup *cg = &p->groups[i];
        bool this_active;
        if (!group_is_animated(cg)) {
            // Always-active, non-attached group — never enters the state machine.
            any_active = true;
            this_active = true;
            min_step = MIN(min_step, cg->animation_step);
        } else {
            const monotonic_t eff_dur = group_stop_duration(cg);
            ShaderGroupAnimState *s = os_window->shader_group_anim + i;
            if (group_has_own_animation(cg)) {
                advance_stopped_group_animation(cg, s, now, i);
                // Whether the group is on, as opposed to off or playing in reverse or looping after being stopped
                const bool was_on = s->active && !s->reverse_ends_at && !s->looping;
                // Focus-out events are hard stops: if a focus-out stop condition is present in
                // the same tick as a start event (e.g. user-activity fired by the click that
                // moved focus away), the focus-out wins and the animation is not restarted.
                const ShaderAnimEventMask focus_out_mask = (1u << SHADER_ANIM_EVENT_OS_WINDOW_FOCUS_OUT) | (1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_OUT);
                const bool hard_stop = (cg->animation_end_events & event_mask & focus_out_mask) != 0;
                // A simultaneous start and end lets start win, except for focus-out
                // events which act as hard stops.
                bool turn_on = false, turn_off = false;
                if (was_on) {
                    turn_off = (event_mask & cg->animation_end_events) || user_events_fired(&cg->animation_end_user_events, os_window) ||
                               (eff_dur > 0 && now - s->started_at >= eff_dur);
                }
                if (((event_mask & cg->animation_start_events) || user_events_fired(&cg->animation_start_user_events, os_window)) && !hard_stop)
                    turn_on = true;
                // A toggle flips the state the group had before this tick and so
                // takes precedence over simultaneous start and stop events.
                if ((event_mask & cg->animation_toggle_events) || user_events_fired(&cg->animation_toggle_user_events, os_window)) {
                    turn_on = !was_on && !hard_stop;
                    turn_off = was_on;
                }
                if (turn_on) {
                    start_group_animation(cg, s, now);
                    debug_rendering("Custom shader group %zu animation started (events=%s)\n", i, shader_anim_event_mask_str(event_mask, os_window));
                } else if (turn_off) {
                    stop_group_animation(cg, s, now);
                    debug_rendering(
                        "Custom shader group %zu animation stopped%s (events=%s)\n",
                        i,
                        s->active ? ", playing in reverse" : "",
                        shader_anim_event_mask_str(event_mask, os_window));
                }
            }
            // Attach: follow the previous group's effective active state.
            if (cg->attached) {
                if (prev_active && !s->active) {
                    s->active = true;
                    // Sync animation timing with the previous group when possible.
                    monotonic_t prev_started = (i > 0) ? os_window->shader_group_anim[i - 1].started_at : 0;
                    s->started_at = prev_started ? prev_started : now;
                    s->ramp_started_at = s->started_at;
                    s->reverse_ends_at = 0;
                    s->loops_left = 0;
                    s->looping = false;
                    debug_rendering("Custom shader group %zu activated via attach\n", i);
                } else if (!prev_active && !group_has_own_animation(cg) && s->active) {
                    // No own events: mirror the previous group's deactivation.
                    s->active = false;
                }
            }
            this_active = s->active;
            if (this_active) {
                any_active = true;
                min_step = MIN(min_step, cg->animation_step);
                // Only track next_end for groups with own stop conditions; attached-only groups
                // deactivate when their predecessor does (already tracked by the predecessor).
                if (group_has_own_animation(cg)) {
                    if (s->reverse_ends_at) next_end = MIN(next_end, s->reverse_ends_at);
                    else if (s->looping) next_end = MIN(next_end, s->ramp_started_at + group_ramp_duration(cg));
                    else if (eff_dur > 0) next_end = MIN(next_end, s->started_at + eff_dur);
                }
            }
        }
        prev_active = this_active;
    }
    clear_user_events(os_window);
    bool prev_has_active = os_window->shader_anim.has_active_shaders;
    if (prev_has_active != any_active) debug_rendering("has_active_shaders: %d -> %d\n", prev_has_active, any_active);
    os_window->shader_anim.has_active_shaders = any_active;
    os_window->shader_anim.min_step = min_step;
    os_window->shader_anim.next_end_at = next_end;
    return min_step;
}

// The index of the user event with the specified name (without the user:
// prefix) in the pipeline, SIZE_MAX if the pipeline does not use it
static size_t
user_event_index(const CustomShaderPipeline *p, const char *name) {
    for (size_t i = 0; i < p->num_user_events; i++) {
        if (strcmp(p->user_events[i], name) == 0) return i;
    }
    return SIZE_MAX;
}

// Queue the user event with the specified name for the OS window with the
// specified id, or for all OS windows when the id is zero. Returns 0 when the
// current pipeline does not use the event, -1 with a Python exception set
// when out of memory, 1 otherwise.
static int
fire_custom_shader_user_event(const char *name, id_type os_window_id) {
    const size_t idx = user_event_index(&custom_shaders.end, name);
    if (idx == SIZE_MAX) {
        debug_rendering("Ignoring the custom shader event user:%s as the current pipeline does not use it\n", name);
        return 0;
    }
    bool queued = false, failed = false;
    for (size_t i = 0; i < global_state.num_os_windows; i++) {
        OSWindow *osw = global_state.os_windows + i;
        if (os_window_id && osw->id != os_window_id) continue;
        if (queue_user_event(osw, idx)) queued = true;
        else failed = true;
    }
    if (queued) wakeup_main_loop();
    if (failed) {
        PyErr_NoMemory();
        return -1;
    }
    return 1;
}

// Decide whether the custom shader layer needs a frame drawn this tick, given
// the cached state from the previous tick and the state just recomputed by
// update_custom_shader_animations(). Kept separate from the renderer so it can
// be unit tested -- see test_custom_shader_redraw_logic.
bool
custom_shader_needs_render(const ShaderAnimState *before, const ShaderAnimState *after, bool had_events, monotonic_t now) {
    // Groups turned on or off: draw once so the change becomes visible. This is
    // what produces the final frame that clears a finished effect.
    if (before->has_active_shaders != after->has_active_shaders) return true;
    // A duration bounded animation expires on this tick.
    if (now >= before->next_end_at) return true;
    // Something was animating last tick, so it either wants its next frame or
    // has just stopped and wants one last one.
    if (before->min_step < MONOTONIC_T_MAX) return true;
    if (!after->has_active_shaders) return false;
    // Animated groups need periodic frames. Static groups (animation_step 0)
    // only need one when an event may have changed what they draw; everything
    // else that changes the screen requests a redraw through other paths.
    return had_events || after->min_step < MONOTONIC_T_MAX;
}

void
bind_shader_globals_to_current_context(void) {
    if (shader_globals_vao_idx == -1) return;
    bind_vao_uniform_buffer(shader_globals_vao_idx, BORDER_COLORS_GLOBAL_BUFFER, BORDER_COLORS_BINDING_POINT);
}

#define CELL_BUFFERS enum { cell_data_buffer, selection_buffer, uniform_buffer, color_table_buffer, padding_cell_data_buffer, padding_selection_buffer };

// The VAO of the cells of a screen, or -1 when there is no memory for it, which leaves the screen undrawn until there
// is, see ensure_cell_vao()
ssize_t
create_cell_vao(void) {
    ssize_t vao_idx = create_vao();
    if (vao_idx < 0) return -1;
#define A(name, size, dtype, offset, stride) \
    add_attribute_to_vao(                    \
        vao_idx, program_attribute_location(CELL_PROGRAM, name), /*size=*/size, /*dtype=*/dtype, /*stride=*/stride, /*offset=*/offset, /*divisor=*/1);
#define A1(name, size, dtype, offset) A(name, size, dtype, (void *)(offsetof(GPUCell, offset)), sizeof(GPUCell))
#define B(usage, var)                    \
    var = add_buffer_to_vao(vao_idx, usage); \
    if (var < 0) goto fail;

    ssize_t bufnum, ctbufnum, other;
    B(GL_ARRAY_BUFFER, other);
    A1("sprite_idx", 2, GL_UNSIGNED_INT, sprite_idx);
    A1("colors", 3, GL_UNSIGNED_INT, fg);
    A1("depth", 1, GL_INT, depth);
    A1("depth_pixels", 1, GL_FLOAT, depth_pixels);
    A1("phase", 1, GL_FLOAT, phase);

    B(GL_ARRAY_BUFFER, other);
    A("is_selected", 1, GL_UNSIGNED_BYTE, NULL, 0);

    B(GL_UNIFORM_BUFFER, bufnum);
    alloc_vao_buffer(vao_idx, program_uniform_block(CELL_PROGRAM, "CellRenderData").size, (size_t)bufnum, GL_STREAM_DRAW);

    B(GL_UNIFORM_BUFFER, ctbufnum);
    alloc_vao_buffer(vao_idx, program_uniform_block(CELL_PROGRAM, "ColorTable").size, (size_t)ctbufnum, GL_STATIC_DRAW);

    B(GL_ARRAY_BUFFER, other); // padding_cell_data_buffer (slot 4)
    B(GL_ARRAY_BUFFER, other); // padding_selection_buffer  (slot 5)

    return vao_idx;
fail:
    remove_vao(vao_idx);
    return -1;
#undef A
#undef A1
#undef B
}

#define IS_SPECIAL_COLOR(name)                                          \
    (screen->color_profile->overridden.name.type == COLOR_IS_SPECIAL || \
     (screen->color_profile->overridden.name.type == COLOR_NOT_SET && screen->color_profile->configured.name.type == COLOR_IS_SPECIAL))

static float
cursor_color_luminance(color_type color) {
    // Match color_to_vec() and contrast_ratio() in background.slang.
    return 0.2126f * srgb_lut[(color >> 16) & 0xff] + 0.7152f * srgb_lut[(color >> 8) & 0xff] + 0.0722f * srgb_lut[color & 0xff];
}

static float
cursor_color_contrast(color_type a, color_type b) {
    const float la = cursor_color_luminance(a), lb = cursor_color_luminance(b);
    return (MAX(la, lb) + 0.05f) / (MIN(la, lb) + 0.05f);
}

static void
pick_cursor_color(color_type cell_fg, color_type cell_bg, color_type *cursor_fg, color_type *cursor_bg, color_type default_fg, color_type default_bg) {
    *cursor_fg = cell_bg;
    *cursor_bg = cell_fg;
    const float cell_contrast = cursor_color_contrast(cell_fg, cell_bg);
    if (cell_contrast < 2.5) {
        if (cursor_color_contrast(default_fg, default_bg) > cell_contrast) {
            *cursor_fg = default_bg;
            *cursor_bg = default_fg;
        }
    }
}

static uint8_t
dimmed_color_component(uint8_t fg, uint8_t bg, float opacity) {
    // DIM blends text with the cell background in linear color space.
    const float linear = (1.f - opacity) * srgb_lut[bg] + opacity * srgb_lut[fg];
    const float srgb = linear <= 0.0031308f ? 12.92f * linear : 1.055f * powf(linear, 1.f / 2.4f) - 0.055f;
    return (uint8_t)lroundf(255.f * srgb);
}

static color_type
dimmed_cursor_color(color_type fg, color_type bg, float opacity) {
    if (opacity >= 1.f) return fg;
    return ((color_type)dimmed_color_component((fg >> 16) & 0xff, (bg >> 16) & 0xff, opacity) << 16) |
           ((color_type)dimmed_color_component((fg >> 8) & 0xff, (bg >> 8) & 0xff, opacity) << 8) |
           (color_type)dimmed_color_component(fg & 0xff, bg & 0xff, opacity);
}

static bool
has_bgimage(OSWindow *w) {
    return background_image_for_os_window(w) != NULL;
}

// The value resolve_dynamic_color() in background.slang needs for the color
// types that are an actual color. The other two types resolve to colors that
// are only known per-cell, so the shader ignores this value for them.
static color_type
dynamic_color_for_gpu(const ColorProfile *cp, color_type val) {
    const DynamicColor dc = {.val = val}, zero = {0};
    if (dc.type != COLOR_IS_RGB && dc.type != COLOR_IS_INDEX) return 0;
    return colorprofile_to_color(cp, dc, zero).rgb;
}

static color_type
cell_update_uniform_block(
    ssize_t vao_idx,
    Screen *screen,
    int uniform_buffer,
    int color_table_buf,
    const CursorRenderInfo *cursor,
    OSWindow *os_window,
    float inactive_text_alpha,
    float bg_alpha) {
    struct GPUCellRenderData {
        // Colors that are constant for the whole draw, pre-converted to linear
        // space so the vertex shader does not do a gamma LUT lookup for them
        // once per vertex. These must come first and the fourth component of
        // each is unused padding, see CellRenderData in background.slang.
        GLfloat default_fg_lin[4], default_bg_lin[4], highlight_fg_lin[4], highlight_bg_lin[4];
        GLfloat main_cursor_fg_lin[4], main_cursor_bg_lin[4], url_color_lin[4];
        GLfloat extra_cursor_fg_lin[4], extra_cursor_bg_lin[4];

        GLfloat use_cell_bg_for_selection_fg, use_cell_fg_for_selection_color, use_cell_for_selection_bg;

        GLuint default_fg, url_style, inverted, main_cursor_is_special, extra_cursor_fg, extra_cursor_bg;

        GLuint columns, lines, sprites_xnum, sprites_ynum, cursor_shape, cell_width, cell_height;
        GLuint cursor_x1, cursor_x2, cursor_y1, cursor_y2;
        GLfloat cursor_opacity, inactive_text_alpha, fg_override_threshold, row_offset, dim_opacity, blink_opacity, hdr_headroom;
        GLuint output_gamut;

        GLuint bg_colors0, bg_colors1, bg_colors2, bg_colors3, bg_colors4, bg_colors5, bg_colors6, bg_colors7;
        GLfloat bg_opacities0, bg_opacities1, bg_opacities2, bg_opacities3, bg_opacities4, bg_opacities5, bg_opacities6, bg_opacities7;
    };
    // Send the uniform data
    ColorProfile *cp = screen->paused_rendering.expires_at ? &screen->paused_rendering.color_profile : screen->color_profile;
    if (cp->dirty || screen->reload_all_gpu_data) {
        const ArrayInformation ai = program_uniform_array(CELL_PROGRAM, "color_table");
        uint8_t *base = (uint8_t *)map_vao_buffer_for_write_only(vao_idx, color_table_buf, 0, program_uniform_block(CELL_PROGRAM, "ColorTable").size);
        GLuint *ct_buf = (GLuint *)(base + ai.offset);
        copy_color_table_to_buffer(cp, ct_buf, 0, ai.stride / sizeof(GLuint));
        unmap_vao_buffer(vao_idx, color_table_buf);
    }
    // This struct is a hand written mirror of the std140 layout of
    // CellRenderData in background.slang, a mismatch silently corrupts every
    // color, so check it once.
    const UniformBlock crd_block = program_uniform_block(CELL_PROGRAM, "CellRenderData");
    static bool crd_block_size_checked = false;
    if (!crd_block_size_checked) {
        crd_block_size_checked = true;
        if ((size_t)crd_block.size < sizeof(struct GPUCellRenderData))
            fatal("The CellRenderData uniform block is %d bytes but the C struct mirroring it is %zu bytes", crd_block.size, sizeof(struct GPUCellRenderData));
    }
    struct GPUCellRenderData *rd = (struct GPUCellRenderData *)map_vao_buffer_for_write_only(vao_idx, uniform_buffer, 0, crd_block.size);
#define COLOR(name) colorprofile_to_color(cp, cp->overridden.name, cp->configured.name).rgb
    // The shaders need these only in linear space, they are converted and
    // written into rd just before the buffer is unmapped. The cursor colors
    // are left as is when there is no cursor to render.
    const color_type highlight_fg = COLOR(highlight_fg), highlight_bg = COLOR(highlight_bg);
    color_type main_cursor_fg = 0, main_cursor_bg = 0;
    rd->default_fg = COLOR(default_fg);
    rd->extra_cursor_fg = screen->extra_cursors.color.text.val;
    rd->extra_cursor_bg = screen->extra_cursors.color.cursor.val;
    rd->main_cursor_is_special = IS_SPECIAL_COLOR(cursor_color);
    rd->bg_colors0 = COLOR(default_bg);
    rd->bg_opacities0 = bg_alpha;
    rd->fg_override_threshold = OPT(text_fg_override_threshold);
    rd->row_offset = row_offset_for_screen(screen);
#define SETBG(which) \
    { colorprofile_to_transparent_color(cp, which - 1, &rd->bg_colors##which, &rd->bg_opacities##which); }
    SETBG(1);
    SETBG(2);
    SETBG(3);
    SETBG(4);
    SETBG(5);
    SETBG(6);
    SETBG(7);
#undef SETBG
    // selection
    if (IS_SPECIAL_COLOR(highlight_fg)) {
        if (IS_SPECIAL_COLOR(highlight_bg)) {
            rd->use_cell_bg_for_selection_fg = 1.f;
            rd->use_cell_fg_for_selection_color = 0.f;
        } else {
            rd->use_cell_bg_for_selection_fg = 0.f;
            rd->use_cell_fg_for_selection_color = 1.f;
        }
    } else {
        rd->use_cell_bg_for_selection_fg = 0.f;
        rd->use_cell_fg_for_selection_color = 0.f;
    }
    rd->use_cell_for_selection_bg = IS_SPECIAL_COLOR(highlight_bg) ? 1. : 0.;
    // Cursor position
    Line *line_for_cursor = NULL, overlay_line_for_cursor;
    rd->cursor_opacity = MAX(0, MIN(cursor->cursor_opacity, 1));
    rd->blink_opacity = MAX(0, MIN(cursor->text_blink_opacity, 1));
    // The extra cursors inherit the colors of the main cursor and, optionally,
    // its shape, so the main cursor render data must be computed even when the
    // main cursor itself is hidden by DECTCEM.
    if (rd->cursor_opacity != 0 && (cursor->is_visible || cursor->multicursor_count)) {
        rd->cursor_x1 = cursor->x, rd->cursor_y1 = cursor->y;
        rd->cursor_x2 = cursor->x, rd->cursor_y2 = cursor->y;
        if (pixel_scroll_enabled(screen)) {
            rd->cursor_y1 += 1;
            rd->cursor_y2 += 1;
        }
        CursorShape cs = (cursor->is_focused || OPT(cursor_shape_unfocused) == NO_CURSOR_SHAPE) ? cursor->shape : OPT(cursor_shape_unfocused);
        rd->cursor_shape = cs;
        color_type cell_fg = rd->default_fg, cell_bg = rd->bg_colors0;
        index_type cell_color_x = cursor->x;
        bool reversed = false;
        if (cursor->x < screen->columns && cursor->y < screen->lines) {
            if (screen->paused_rendering.expires_at) {
                linebuf_init_line(screen->paused_rendering.linebuf, cursor->y);
                line_for_cursor = screen->paused_rendering.linebuf->line;
            } else {
                linebuf_init_line(screen->linebuf, cursor->y);
                line_for_cursor = screen->linebuf->line;
                if (screen_is_overlay_active(screen) && cursor->y == screen->overlay_line.ynum) {
                    // The linebuf holds the original line, the pre-edit text is
                    // drawn over it only in the overlay cells, so use those to size
                    // and color the cursor, for example when it is over a wide character.
                    overlay_line_for_cursor = *line_for_cursor;
                    overlay_line_for_cursor.cpu_cells = screen->overlay_line.cpu_cells;
                    overlay_line_for_cursor.gpu_cells = screen->overlay_line.gpu_cells;
                    line_for_cursor = &overlay_line_for_cursor;
                }
            }
        }
        if (line_for_cursor) {
            colors_for_cell(line_for_cursor, cp, &cell_color_x, &cell_fg, &cell_bg, &reversed);
            const CPUCell *cursor_cell;
            const bool large_cursor = ((cursor_cell = &line_for_cursor->cpu_cells[cursor->x])->is_multicell) && cursor_cell->x == 0 && cursor_cell->y == 0;
            if (large_cursor) {
                switch (cs) {
                    case CURSOR_BEAM: rd->cursor_y2 += cursor_cell->scale - 1; break;
                    case CURSOR_UNDERLINE:
                        rd->cursor_y1 += cursor_cell->scale - 1;
                        rd->cursor_y2 = rd->cursor_y1;
                        rd->cursor_x2 += mcd_x_limit(cursor_cell) - 1;
                        break;
                    case CURSOR_BLOCK:
                        rd->cursor_y2 += cursor_cell->scale - 1;
                        rd->cursor_x2 += mcd_x_limit(cursor_cell) - 1;
                        break;
                    case CURSOR_HOLLOW:
                    case NUM_OF_CURSOR_SHAPES:
                    case NO_CURSOR_SHAPE: break;
                };
            }
        }
        // If you change the following algorithm remember to change it in the cell shader for extra cursors too
        if (rd->main_cursor_is_special) {
            if (line_for_cursor && line_for_cursor->gpu_cells[cell_color_x].attrs.dim) {
                cell_fg = dimmed_cursor_color(cell_fg, cell_bg, OPT(dim_opacity));
                pick_cursor_color(cell_fg, cell_bg, &main_cursor_fg, &main_cursor_bg, rd->default_fg, rd->bg_colors0);
            } else if (cell_bg == cell_fg) {
                main_cursor_fg = rd->bg_colors0;
                main_cursor_bg = rd->default_fg;
            } else {
                main_cursor_fg = cell_bg;
                main_cursor_bg = cell_fg;
            }
        } else {
            main_cursor_bg = COLOR(cursor_color);
            if (IS_SPECIAL_COLOR(cursor_text_color)) main_cursor_fg = cell_bg;
            else main_cursor_fg = COLOR(cursor_text_color);
        }
        // store last rendered cursor color for trail rendering
        screen->last_rendered.cursor_bg = main_cursor_bg;
        if (!cursor->is_visible) {
            // Move the main cursor off screen so that only it is hidden, the
            // extra cursors keep their shape and colors.
            rd->cursor_x1 = screen->columns + 1;
            rd->cursor_x2 = screen->columns;
            rd->cursor_y1 = screen->lines + 1;
            rd->cursor_y2 = screen->lines;
        }
    } else {
        rd->cursor_shape = 0;
        rd->cursor_x1 = screen->columns + 1;
        rd->cursor_x2 = screen->columns;
        rd->cursor_y1 = screen->lines + 1;
        rd->cursor_y2 = screen->lines;
    }

    rd->columns = screen->columns;
    rd->lines = screen->lines;

    unsigned int x, y, z;
    sprite_tracker_current_layout(os_window->fonts_data, &x, &y, &z);
    rd->sprites_xnum = x;
    rd->sprites_ynum = y;
    rd->inverted = screen_invert_colors(screen) ? 1 : 0;
    rd->cell_width = os_window->fonts_data->fcm.cell_width;
    rd->cell_height = os_window->fonts_data->fcm.cell_height;
    rd->inactive_text_alpha = inactive_text_alpha;
    rd->dim_opacity = OPT(dim_opacity);
    rd->hdr_headroom = effective_hdr_headroom(os_window);
    rd->output_gamut = output_gamut(os_window);

#undef COLOR
    rd->url_style = OPT(url_style);
    linear_color_vec4(rd->default_fg_lin, rd->default_fg);
    linear_color_vec4(rd->default_bg_lin, rd->bg_colors0);
    linear_color_vec4(rd->highlight_fg_lin, highlight_fg);
    linear_color_vec4(rd->highlight_bg_lin, highlight_bg);
    linear_color_vec4(rd->main_cursor_fg_lin, main_cursor_fg);
    linear_color_vec4(rd->main_cursor_bg_lin, main_cursor_bg);
    const float cursor_gain = MIN(OPT(cursor_brightness), rd->hdr_headroom);
    for (unsigned i = 0; i < 3; i++) rd->main_cursor_bg_lin[i] *= cursor_gain;
    linear_color_vec4(rd->url_color_lin, OPT(url_color));
    linear_color_vec4(rd->extra_cursor_fg_lin, dynamic_color_for_gpu(cp, rd->extra_cursor_fg));
    linear_color_vec4(rd->extra_cursor_bg_lin, dynamic_color_for_gpu(cp, rd->extra_cursor_bg));
    color_type default_bg = rd->bg_colors0;
    unmap_vao_buffer(vao_idx, uniform_buffer);
    rd = NULL;
    return default_bg;
}

static bool
cell_prepare_to_render(ssize_t vao_idx, Screen *screen, FONTS_DATA_HANDLE fonts_data) {
    size_t sz;
    CELL_BUFFERS;
    void *address;
    bool changed = false;

    ensure_sprite_map(fonts_data);
    const Cursor *cursor = screen->paused_rendering.expires_at ? &screen->paused_rendering.cursor : screen->cursor;

    bool cursor_pos_changed = cursor->x != screen->last_rendered.cursor.x || cursor->y != screen->last_rendered.cursor.y;
    bool disable_ligatures = screen->disable_ligatures == DISABLE_LIGATURES_CURSOR;
    bool screen_resized = screen->last_rendered.columns != screen->columns || screen->last_rendered.lines != screen->lines;

#define update_cell_data                                                                               \
    {                                                                                                  \
        const unsigned int render_lines = render_lines_for_screen(screen);                             \
        sz = sizeof(GPUCell) * render_lines * screen->columns;                                         \
        address = alloc_and_map_vao_buffer_for_full_rewrite(vao_idx, sz, cell_data_buffer);           \
        screen_update_cell_data(screen, address, fonts_data, disable_ligatures && cursor_pos_changed); \
        unmap_vao_buffer(vao_idx, cell_data_buffer);                                                   \
        address = NULL;                                                                                \
        changed = true;                                                                                \
    }

    send_deep_colors_to_gpu(screen);
    if (screen->paused_rendering.expires_at) {
        if (!screen->paused_rendering.cell_data_updated) update_cell_data;
    } else if (screen->reload_all_gpu_data || screen->scroll_changed || screen->is_dirty || screen_resized || (disable_ligatures && cursor_pos_changed))
        update_cell_data;

#define update_selection_data                                                    \
    {                                                                            \
        const unsigned int render_lines = render_lines_for_screen(screen);       \
        sz = (size_t)render_lines * screen->columns;                             \
        address = alloc_and_map_vao_buffer_for_full_rewrite(vao_idx, sz, selection_buffer); \
        screen_apply_selection(screen, address, sz);                             \
        unmap_vao_buffer(vao_idx, selection_buffer);                             \
        address = NULL;                                                          \
        changed = true;                                                          \
    }

    if (screen->paused_rendering.expires_at) {
        if (!screen->paused_rendering.cell_data_updated) {
            update_selection_data;
            screen_update_graphics_layers(screen);
        }
        screen->paused_rendering.cell_data_updated = true;
    } else {
        if (screen->reload_all_gpu_data || screen_resized || screen_is_selection_dirty(screen)) update_selection_data;
        if (screen_update_graphics_layers(screen)) changed = true;
    }
    screen->last_rendered.scrolled_by = rendered_scrolled_by(screen);
#undef update_selection_data
#undef update_cell_data
    screen->last_rendered.columns = screen->columns;
    screen->last_rendered.lines = screen->lines;
    screen->last_rendered.cursor = screen->cursor_render_info;

    return changed;
}

static void
draw_graphics(int program, ImageRenderData *data, GLuint start, GLuint count, float extra_alpha, float hdr_headroom) {
    bind_program(program);
    // The colors of indexed color images are those of the terminal, they are never HDR, so their programs have no
    // headroom or tone mapping.
    const bool has_texture_uniforms = program == GRAPHICS_PROGRAM || program == GRAPHICS_PREMULT_PROGRAM;
    if (program != GRAPHICS_ALPHA_MASK_PROGRAM) glUniform1f(program_uniform_location(program, "extra_alpha"), extra_alpha);
    if (has_texture_uniforms) glUniform1f(program_uniform_location(program, "hdr_headroom"), hdr_headroom);
    glActiveTexture(GL_TEXTURE0 + GRAPHICS_UNIT);
    GLint src_rect_loc = program_uniform_location(program, "src_rect");
    GLint dest_rect_loc = program_uniform_location(program, "dest_rect");
    // Images tagged with a gamut are converted to that of the window, the alpha mask program draws none. Indexed
    // color images are untagged, their colors are those of the terminal.
    const GLint image_gamut_loc = try_program_uniform_location(program, "image_gamut");
    const GLint output_gamut_loc = try_program_uniform_location(program, "output_gamut");
    // High precision images and those with a transfer function are decoded by the shader, see docs/silkie/precision-image-protocol.rst
    const GLint image_decode_loc = has_texture_uniforms ? try_program_uniform_location(program, "image_decode") : -1;
    const GLint image_is_gray_loc = has_texture_uniforms ? try_program_uniform_location(program, "image_is_gray") : -1;
    const GLint filter_decoded_loc = has_texture_uniforms ? try_program_uniform_location(program, "filter_decoded") : -1;
    const GLint linear_texture_loc = has_texture_uniforms ? try_program_uniform_location(program, "linear_texture") : -1;
    const unsigned viewport_width = get_gpu_viewport().width;
    if (output_gamut_loc >= 0) glUniform1ui(output_gamut_loc, current_output_gamut);
    // Indexed color images have alpha or not, palette images have it in their palettes
    const bool is_indexed = program == GRAPHICS_INDEXED_PROGRAM || program == GRAPHICS_INDEXED_UNSCALED_PROGRAM;
    const bool is_palette = program == GRAPHICS_PALETTE_PROGRAM || program == GRAPHICS_PALETTE_UNSCALED_PROGRAM;
    const GLint has_alpha_loc = is_indexed ? program_uniform_location(program, "has_alpha") : -1;
    // the scaled variants pick a mipmap level of an integer texture themselves, when it has any
    const GLint max_lod_loc = program == GRAPHICS_INDEXED_PROGRAM || program == GRAPHICS_PALETTE_PROGRAM ? try_program_uniform_location(program, "max_lod") : -1;
    for (GLuint i = 0; i < count;) {
        ImageRenderData *group = data + start + i;
        if (is_palette) {
            glActiveTexture(GL_TEXTURE0 + GRAPHICS_PALETTE_UNIT);
            glBindTexture(GL_TEXTURE_2D, group->palette_texture_id);
            glActiveTexture(GL_TEXTURE0 + GRAPHICS_UNIT);
        }
        glBindTexture(GL_TEXTURE_2D, spatial_window && spatial_window->spatial.eye > 0 && group->right_texture_id ? group->right_texture_id : group->texture_id);
        if (has_texture_uniforms) glUniform1f(program_uniform_location(program, "tone_peak"), group->tone_map ? group->peak : 0.f);
        if (image_gamut_loc >= 0) glUniform1ui(image_gamut_loc, group->gamut);
        if (image_decode_loc >= 0) glUniform1ui(image_decode_loc, group->decode);
        if (image_is_gray_loc >= 0) glUniform1ui(image_is_gray_loc, group->is_gray);
        // the same for the whole group, as refs drawn at their natural size are grouped apart from scaled ones
        if (filter_decoded_loc >= 0) glUniform1ui(filter_decoded_loc, group->filter_decoded);
        // the decoded pixels of an image, or of a tile of it, see "Mipmaps for decoded images" in graphics.c
        if (linear_texture_loc >= 0) glUniform1ui(linear_texture_loc, group->linear_texture);
        if (has_alpha_loc > -1) glUniform1ui(has_alpha_loc, group->indexed_px_sz == 2);
        if (max_lod_loc > -1) glUniform1ui(max_lod_loc, group->mip_levels);
        if (group->group_count == 0) {
            i++;
            continue;
        }
        if (!group->texture_id) {
            // a tile of an image that could not be uploaded, there is nothing to draw
            i += group->group_count;
            continue;
        }
        for (GLuint k = 0; k < group->group_count; k++, i++) {
            ImageRenderData *rd = data + start + i;
            glUniform4f(src_rect_loc, rd->src_rect.left, rd->src_rect.top, rd->src_rect.right, rd->src_rect.bottom);
            const float shift = depth_shift(rd->depth, rd->depth_pixels, viewport_width);
            float phase = 0.f;
            if (spatial_window && spatial_window->spatial.mode) {
                const float limit = spatial_window->spatial.phase_limit;
                phase = -(float)spatial_window->spatial.eye * fmaxf(-limit, fminf(limit, (float)rd->phase)) / get_gpu_viewport().height;
            }
            glUniform4f(dest_rect_loc, rd->dest_rect.left + shift, rd->dest_rect.top + phase, rd->dest_rect.right + shift, rd->dest_rect.bottom + phase);
            draw_quad(true, 0);
        }
    }
}

static ImageRenderData *
load_alpha_mask_texture(size_t width, size_t height, uint8_t *canvas) {
    static ImageRenderData data = {.group_count = 1};
    if (!data.texture_id) { glGenTextures(1, &data.texture_id); }
    glBindTexture(GL_TEXTURE_2D, data.texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, canvas);
    return &data;
}

static void
setup_texture_as_render_target(unsigned width, unsigned height, GLuint *texture_id, GLuint *framebuffer_id, bool is_float) {
    glGenTextures(1, texture_id);
    glGenFramebuffers(1, framebuffer_id);
    glBindTexture(GL_TEXTURE_2D, *texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // We use GL_RGBA16 to avoid incorrect colors due to quantization loss when
    // blending, see https://github.com/kovidgoyal/kitty/issues/8953
    static struct {
        bool ok;
        int fmt;
    } status = {false, GL_RGBA16};
    // An HDR window renders in linear light above 1.0, and a window in a wide gamut with values below zero as
    // well, so their layers must be float
    glTexImage2D(GL_TEXTURE_2D, 0, is_float ? GL_RGBA16F : status.fmt, width, height, 0, GL_RGBA, is_float ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
    bind_framebuffer_for_output(*framebuffer_id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture_id, 0);
    if (!status.ok && !is_float) {
        if (check_framebuffer_status() == NULL) {
            status.ok = true;
        } else {
            if (status.fmt == GL_RGBA16) {
                // Driver does not support 16 bit FBO so let it choose the
                // format. It will probably end up choosing 8 bit but
                // inaccurate colors are better than completely broken rendering.
                // See https://github.com/kovidgoyal/kitty/issues/9068
                status.fmt = GL_RGBA;
                free_framebuffer(framebuffer_id);
                free_texture(texture_id);
                setup_texture_as_render_target(width, height, texture_id, framebuffer_id, is_float);
                log_error("WARNING: Your GPU driver does not support 16bit textures as framebuffer targets, some colors may be slightly inaccurate.");
            } else {
                fatal("Your GPU driver does not support indirect rendering to a GL_RGBA texture via a framebuffer");
            }
        }
    }
}

static void
set_cell_uniforms(bool force) {
    static bool constants_set = false;
    if (!constants_set || force) {
        float text_contrast = 1.0f + OPT(text_contrast) * 0.01f;
        float text_gamma_adjustment = OPT(text_gamma_adjustment) < 0.01f ? 1.0f : 1.0f / OPT(text_gamma_adjustment);
        for (int i = GRAPHICS_PROGRAM; i <= GRAPHICS_PALETTE_UNSCALED_PROGRAM; i++) {
            bind_program(i);
            glUniform1i(program_uniform_location(i, "image"), GRAPHICS_UNIT);
        }
        for (int i = GRAPHICS_PALETTE_PROGRAM; i <= GRAPHICS_PALETTE_UNSCALED_PROGRAM; i++) {
            bind_program(i);
            glUniform1i(program_uniform_location(i, "palette"), GRAPHICS_PALETTE_UNIT);
        }
        // These are set for every cell program, but the background program draws no
        // text and so does not have them.
#define set_optional(setter, name, value)                  \
    {                                                      \
        GLint loc = try_program_uniform_location(i, name); \
        if (loc >= 0) setter(loc, value);                  \
    }
        for (int i = CELL_PROGRAM; i < CELL_PROGRAM_SENTINEL; i++) {
            bind_program(i);
            set_optional(glUniform1i, "sprites", SPRITE_MAP_UNIT);
            set_optional(glUniform1i, "sprite_decorations_map", SPRITE_DECORATIONS_MAP_UNIT);
            set_optional(glUniform1i, "deep_colors", DEEP_COLORS_UNIT);
            set_optional(glUniform1f, "text_contrast", text_contrast);
            set_optional(glUniform1f, "text_gamma_adjustment", text_gamma_adjustment);
        }
#undef set_optional
        bind_program(BLIT_PROGRAM);
        glUniform1i(program_uniform_location(BLIT_PROGRAM, "image"), GRAPHICS_UNIT);
        bind_program(SCREENSHOT_PROGRAM);
        glUniform1i(program_uniform_location(SCREENSHOT_PROGRAM, "image"), GRAPHICS_UNIT);
        constants_set = true;
    }
}


// UI Layer {{{
static Animation *default_visual_bell_animation = NULL;

static float
get_visual_bell_intensity(Screen *screen) {
    if (screen->start_visual_bell_at > 0) {
        if (!default_visual_bell_animation) {
            default_visual_bell_animation = alloc_animation();
            if (!default_visual_bell_animation) fatal("Out of memory");
            add_cubic_bezier_animation(default_visual_bell_animation, 0, 1, EASE_IN_OUT);
            add_cubic_bezier_animation(default_visual_bell_animation, 1, 0, EASE_IN_OUT);
        }
        const monotonic_t progress = monotonic() - screen->start_visual_bell_at;
        const monotonic_t duration = OPT(visual_bell_duration) / 2;
        if (progress <= duration) {
            Animation *a = animation_is_valid(OPT(animation.visual_bell)) ? OPT(animation.visual_bell) : default_visual_bell_animation;
            return (float)apply_easing_curve(a, progress / (double)duration, duration);
        }
        screen->start_visual_bell_at = 0;
    }
    return 0.0f;
}

static void
draw_visual_bell_flash(GLfloat intensity, const color_type flash) {
    bind_program(TINT_PROGRAM);
    GLfloat attenuation = 0.4f;
#define C(shift) srgb_color((flash >> shift) & 0xFF)
    const GLfloat r = C(16), g = C(8), b = C(0);
    const GLfloat max_channel = r > g ? (r > b ? r : b) : (g > b ? g : b);
#undef C
#define C(x) (x * intensity * attenuation)
    if (max_channel > 0.45) attenuation = 0.6f; // light color
    glUniform4f(program_uniform_location(TINT_PROGRAM, "tint_color"), C(r), C(g), C(b), C(1));
#undef C
    glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), -1, 1, 1, -1);
    draw_quad(true, 0);
}

static color_type
get_flash_color(const Screen *screen) {
#define COLOR(name, fallback)                       \
    colorprofile_to_color_with_fallback(            \
        screen->color_profile,                      \
        screen->color_profile->overridden.name,     \
        screen->color_profile->configured.name,     \
        screen->color_profile->overridden.fallback, \
        screen->color_profile->configured.fallback)
    return !IS_SPECIAL_COLOR(highlight_bg) ? COLOR(visual_bell_color, highlight_bg) : COLOR(visual_bell_color, default_fg);
#undef COLOR
}

static void
draw_visual_bell(const UIRenderData *ui) {
    if (!ui->screen->start_visual_bell_at) return;
    Screen *screen = ui->screen;
    float intensity = get_visual_bell_intensity(screen);
    if (intensity <= 0) return;
    draw_visual_bell_flash(intensity, get_flash_color(screen));
}

static void
draw_drag_preview_overlay(const UIRenderData *ui) {
    const Screen *screen = ui->screen;
    if (!screen->start_drag_overlay_at || !screen->drag_overlay_quadrant) return;
    const monotonic_t elapsed = monotonic() - screen->start_drag_overlay_at;
    const monotonic_t fade_ms = ms_to_monotonic_t(150ll);
    float intensity = elapsed >= fade_ms ? 1.0f : (float)elapsed / (float)fade_ms;
    GLfloat left = -1.f, top = 1.f, right = 1.f, bottom = -1.f;
    switch (screen->drag_overlay_quadrant) {
        case 1: right = 0.f; break;  // left half
        case 2: left = 0.f; break;   // right half
        case 3: bottom = 0.f; break; // top half
        case 4: top = 0.f; break;    // bottom half
        case 5: break;               // full window + title bar highlight (title bar hover)
        case 6: break;               // full window, no title bar highlight (body hover)
        default: return;
    }
    bind_program(TINT_PROGRAM);
    float a = intensity * 0.25f;
    color_type hint = get_flash_color(screen);
#define C(shift) (srgb_color((hint >> shift) & 0xFF) * a)
    glUniform4f(program_uniform_location(TINT_PROGRAM, "tint_color"), C(16), C(8), C(0), a);
#undef C
    glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), left, top, right, bottom);
    draw_quad(true, 0);
}

static bool
has_scrollbar(Window *w, Screen *screen) {
    return screen_has_scrollbar(screen, w->scrollbar.is_hovering);
}

static unsigned
bar_border_width(const UIRenderData *ui) {
    return MAX(1u, (unsigned)ceil(thickness_as_float(ui->os_window, 1)));
}

// The rect, including borders, of a full width bar along the top of the window
static Viewport
window_bar_rect(const UIRenderData *ui) {
    return (Viewport){.left = ui->screen_left, .top = ui->screen_top, .width = ui->screen_width, .height = ui->cell_height + 2 + 2 * bar_border_width(ui)};
}

static void
bar_colors(const UIRenderData *ui, color_type *fg, color_type *bg) {
#define RGBCOL(which, fallback)                                       \
    (0xff000000 | colorprofile_to_color_with_fallback(                \
                      ui->screen->color_profile,                      \
                      ui->screen->color_profile->overridden.which,    \
                      ui->screen->color_profile->configured.which,    \
                      ui->screen->color_profile->overridden.fallback, \
                      ui->screen->color_profile->configured.fallback))
    *fg = RGBCOL(default_fg, default_fg);
    *bg = RGBCOL(default_bg, default_bg);
#undef RGBCOL
}

// Render title into bar->buf, if needed. The rendered text is max_width x
// height pixels, unless fit_to_text is true, in which case its width is reduced
// to the width of the text. The actual width is stored in bar->width.
static bool
render_bar_text(
    const UIRenderData *ui, WindowBarData *bar, PyObject *title, unsigned max_width, unsigned height, bool fit_to_text, color_type fg, color_type bg) {
    if (!title) return false;
    if (!bar->buf || bar->requested_width != max_width || bar->height != height || bar->fit_to_text != fit_to_text) {
        free(bar->buf);
        bar->buf = malloc((size_t)4 * max_width * height);
        if (!bar->buf) return false;
        bar->requested_width = max_width;
        bar->width = max_width;
        bar->height = height;
        bar->fit_to_text = fit_to_text;
        bar->needs_render = true;
    }
    if (bar->last_drawn_title_object_id != title || bar->needs_render) {
        // The whole title is passed on, draw_window_title() clips it to the width of the bar
        Py_ssize_t title_len;
        const char *utf8 = PyUnicode_AsUTF8AndSize(title, &title_len);
        if (!utf8) {
            PyErr_Print();
            return false;
        }
        // title_len is at most PY_SSIZE_T_MAX, so adding the padding cannot overflow
        const size_t titlebuf_sz = (size_t)title_len + sizeof("  ");
        RAII_ALLOC(char, titlebuf, malloc(titlebuf_sz));
        if (!titlebuf) return false;
        snprintf(titlebuf, titlebuf_sz, fit_to_text ? " %s " : " %s", utf8);
        size_t actual_width = max_width;
        if (!draw_window_title(
                ui->os_window->fonts_data->font_sz_in_pts,
                ui->os_window->fonts_data->logical_dpi_y,
                titlebuf,
                fg,
                bg,
                bar->buf,
                max_width,
                height,
                fit_to_text ? &actual_width : NULL))
            return false;
        bar->width = MAX(1u, (unsigned)actual_width);
        Py_CLEAR(bar->last_drawn_title_object_id);
        bar->last_drawn_title_object_id = Py_NewRef(title);
    }
    return true;
}

// Draw the text previously rendered by render_bar_text() surrounded by a
// border. border_rect must be the size of the rendered text plus the borders.
static void
draw_bar(const UIRenderData *ui, const WindowBarData *bar, Viewport border_rect, color_type fg, color_type bg) {
    const double thickness = thickness_as_float(ui->os_window, 1);
    const unsigned border_width = bar_border_width(ui);
    // The rounded rect shader anti-aliases each edge over a band extending
    // one pixel on either side of it, so keep the drawn line at least half a
    // pixel inside the border_width pixels reserved for it, otherwise it would
    // bleed onto the rendered title.
    const float drawn_thickness = (float)MIN(thickness, border_width - 0.5);
    static ImageRenderData data = {.group_count = 1};
    gpu_data_for_image(&data, -1, 1, 1, -1);
    glGenTextures(1, &data.texture_id);
    glBindTexture(GL_TEXTURE_2D, data.texture_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB_ALPHA, bar->width, bar->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, bar->buf);
    bind_program(GRAPHICS_PROGRAM);
    const unsigned sh = ui->full_framebuffer_height;
    // first blank the area to be drawn to background
    enable_scissor_using_top_left_origin(border_rect, sh);
    blank_canvas(ui->bg_alpha, bg, false);
    disable_scissor();
    // then draw the rendered text
    save_viewport_using_top_left_origin(border_rect.left + border_width, border_rect.top + border_width, bar->width, bar->height, sh);
    draw_graphics(GRAPHICS_PROGRAM, &data, 0, 1, 1.f, 1.f);
    restore_viewport();
    free_texture(&data.texture_id);
    // finally draw border with transparent bg
    draw_rounded_rect(border_rect, sh, drawn_thickness, ui->cell_width, fg, bg, 0.f, 0, 0.f);
}

static unsigned
render_a_bar(const UIRenderData *ui, WindowBarData *bar, PyObject *title, Viewport border_rect) {
    const unsigned border_width = bar_border_width(ui);
    if (border_rect.width <= 2 * border_width || border_rect.height <= 2 * border_width) return 0;
    color_type fg, bg;
    bar_colors(ui, &fg, &bg);
    if (!render_bar_text(ui, bar, title, border_rect.width - 2 * border_width, border_rect.height - 2 * border_width, false, fg, bg)) return 0;
    draw_bar(ui, bar, border_rect, fg, bg);
    return border_rect.height;
}

static bool
show_hyperlink_targets_with_modifiers(int mods) {
    switch (OPT(show_hyperlink_targets)) {
        case SHOW_HYPERLINK_TARGETS_ALWAYS: return true;
        case SHOW_HYPERLINK_TARGETS_CTRL: return (mods & GLFW_MOD_CONTROL) != 0;
        case SHOW_HYPERLINK_TARGETS_SHIFT: return (mods & GLFW_MOD_SHIFT) != 0;
        case SHOW_HYPERLINK_TARGETS_SUPER: return (mods & GLFW_MOD_SUPER) != 0;
        case SHOW_HYPERLINK_TARGETS_ALT: return (mods & GLFW_MOD_ALT) != 0;
        default: return false;
    }
}

static bool
has_hyperlink_target(OSWindow *os_window, Window *w, Screen *screen) {
    return show_hyperlink_targets_with_modifiers(global_state.mods_at_last_key_or_button_event) && screen->current_hyperlink_under_mouse.id && w &&
           !is_mouse_hidden(os_window) && global_state.mouse_hover_in_window == w->id;
}

// Return the offset from the top of the screen at which to draw a bar of the
// specified height so that it does not obscure the hovered line
static unsigned
hyperlink_target_bar_top(const UIRenderData *ui, unsigned bar_height) {
    const double max_top = ui->screen_height - bar_height;
    const double row_top = (double)ui->screen->current_hyperlink_under_mouse.y * ui->cell_height + ui->screen->pixel_scroll_offset_y;
    const double row_bottom = row_top + ui->cell_height;
    double top = 0;
    switch (OPT(hyperlink_target_position)) {
        case HYPERLINK_TARGET_TOP: top = row_top < bar_height ? max_top : 0; break;
        case HYPERLINK_TARGET_BOTTOM: top = row_bottom > max_top ? 0 : max_top; break;
        case HYPERLINK_TARGET_BELOW:
        case HYPERLINK_TARGET_TOOLTIP: top = row_bottom > max_top ? row_top - bar_height : row_bottom; break;
    }
    return (unsigned)MAX(0., MIN(top, max_top));
}

// Draw the URL in a box sized to fit it, starting at the hovered cell, moved
// left as needed to fit in the window. rect is the full width bar it replaces.
static void
draw_hyperlink_target_tooltip(const UIRenderData *ui, Viewport rect, PyObject *url) {
    const unsigned border_width = bar_border_width(ui);
    if (rect.width <= 2 * border_width || rect.height <= 2 * border_width) return;
    WindowBarData *bar = &ui->window->title_bar_data;
    color_type fg, bg;
    bar_colors(ui, &fg, &bg);
    if (!render_bar_text(ui, bar, url, rect.width - 2 * border_width, rect.height - 2 * border_width, true, fg, bg)) return;
    const unsigned width = bar->width + 2 * border_width;
    const unsigned left = ui->screen->current_hyperlink_under_mouse.x * ui->cell_width;
    rect.left += MIN(left, rect.width - width);
    rect.width = width;
    draw_bar(ui, bar, rect, fg, bg);
}

static void
draw_hyperlink_target(const UIRenderData *ui) {
    if (!has_hyperlink_target(ui->os_window, ui->window, ui->screen)) return;
    Screen *screen = ui->screen;
    Window *window = ui->window;
    Viewport rect = window_bar_rect(ui);
    if (rect.height > ui->screen_height) return;
    WindowBarData *bd = &window->url_target_bar_data;
    hyperlink_id_type hid = screen->current_hyperlink_under_mouse.id;
    if (bd->hyperlink_id_for_title_object != hid) {
        bd->hyperlink_id_for_title_object = hid;
        Py_CLEAR(bd->last_drawn_title_object_id);
        const char *url = get_hyperlink_for_id(screen->hyperlink_pool, bd->hyperlink_id_for_title_object, true);
        if (url == NULL) url = "";
        bd->last_drawn_title_object_id = PyObject_CallMethod(global_state.boss, "sanitize_url_for_display_to_user", "s", url);
        if (bd->last_drawn_title_object_id == NULL) {
            PyErr_Print();
            return;
        }
        bd->needs_render = true;
    }
    if (bd->last_drawn_title_object_id == NULL) return;
    PyObject *ref = Py_NewRef(bd->last_drawn_title_object_id); // render_bar_text() clears bd->last_drawn_title_object_id
    rect.top += hyperlink_target_bar_top(ui, rect.height);
    if (OPT(hyperlink_target_position) == HYPERLINK_TARGET_TOOLTIP) draw_hyperlink_target_tooltip(ui, rect, bd->last_drawn_title_object_id);
    else render_a_bar(ui, &window->title_bar_data, bd->last_drawn_title_object_id, rect);
    Py_DECREF(ref);
}

static bool
has_window_number(Window *w, Screen *screen) {
    return w != NULL && screen->display_window_char != 0;
}

static void
draw_window_number(const UIRenderData *ui) {
    if (!has_window_number(ui->window, ui->screen)) return;
    unsigned title_bar_height = 0, requested_height = ui->screen_height;
    if (ui->window->title && PyUnicode_Check(ui->window->title) && (requested_height > (ui->cell_height + 1) * 2)) {
        title_bar_height = render_a_bar(ui, &ui->window->title_bar_data, ui->window->title, window_bar_rect(ui));
    }
    unsigned height_for_letter = ui->screen_height - title_bar_height - ui->cell_height;
    unsigned width_for_letter = ui->screen_width - ui->cell_width;
    requested_height = MIN(12 * ui->cell_height, MIN(height_for_letter, width_for_letter));
    if (requested_height < 4) return;
#define lr ui->screen->last_rendered_window_char
    if (!lr.canvas || lr.ch != ui->screen->display_window_char || lr.requested_height != requested_height) {
        free(lr.canvas);
        lr.canvas = NULL;
        lr.requested_height = requested_height;
        lr.height_px = requested_height;
        lr.ch = 0;
        lr.canvas = draw_single_ascii_char(ui->screen->display_window_char, &lr.width_px, &lr.height_px);
        if (lr.height_px < 4 || lr.width_px < 4 || !lr.canvas) return;
        lr.ch = ui->screen->display_window_char;
    }
    unsigned letter_x = 0, letter_y = title_bar_height;
    if (lr.width_px < ui->screen_width) letter_x = (ui->screen_width - lr.width_px) / 2;
    if (lr.height_px + title_bar_height < ui->screen_height) letter_y += (ui->screen_height - lr.height_px - title_bar_height) / 2;
    bind_program(GRAPHICS_ALPHA_MASK_PROGRAM);
    ImageRenderData *ird = load_alpha_mask_texture(lr.width_px, lr.height_px, lr.canvas);
    gpu_data_for_image(ird, -1, 1, 1, -1);
    glUniform1i(program_uniform_location(GRAPHICS_ALPHA_MASK_PROGRAM, "image"), GRAPHICS_UNIT);
    color_type digit_color = colorprofile_to_color_with_fallback(
        ui->screen->color_profile,
        ui->screen->color_profile->overridden.highlight_bg,
        ui->screen->color_profile->configured.highlight_bg,
        ui->screen->color_profile->overridden.default_fg,
        ui->screen->color_profile->configured.default_fg);
    color_vec3(program_uniform_location(GRAPHICS_ALPHA_MASK_PROGRAM, "amask_fg"), digit_color);
    glUniform4f(program_uniform_location(GRAPHICS_ALPHA_MASK_PROGRAM, "amask_bg_premult"), 0.f, 0.f, 0.f, 0.f);
    save_viewport_using_top_left_origin(ui->screen_left + letter_x, ui->screen_top + letter_y, lr.width_px, lr.height_px, ui->full_framebuffer_height);
    draw_graphics(GRAPHICS_ALPHA_MASK_PROGRAM, ird, 0, 1, 1.f, 1.f);
    restore_viewport();
#undef lr
}

// Helper function to extract and apply opacity to color components
static void
set_color_uniform_with_opacity(color_type color, float opacity) {
    float r = srgb_color((color >> 16) & 0xFF) * opacity;
    float g = srgb_color((color >> 8) & 0xFF) * opacity;
    float b = srgb_color(color & 0xFF) * opacity;
    glUniform4f(program_uniform_location(TINT_PROGRAM, "tint_color"), r, g, b, opacity);
}

static color_type
scrollbar_color(Screen *screen, unsigned val) {
    switch (val & 0xff) {
#define C(which) colorprofile_to_color(screen->color_profile, screen->color_profile->overridden.which, screen->color_profile->configured.which).rgb
        case 0: return C(default_fg);
        case 1: return C(highlight_bg);
#undef C
        default: return val >> 8;
    }
}

static void
draw_scrollbar(const UIRenderData *ui) {
    Screen *screen = ui->screen;
    Window *window = ui->window;
    if (!window || !screen || !has_scrollbar(window, screen)) return;

    color_type bar_color = scrollbar_color(screen, OPT(scrollbar_handle_color)), track_color = scrollbar_color(screen, OPT(scrollbar_track_color));
    float bar_frac, visible_fraction;
    screen_scrollbar_geometry(screen, &bar_frac, &visible_fraction);
    float opacity = OPT(scrollbar_handle_opacity);
    float track_opacity = window->scrollbar.is_hovering ? OPT(scrollbar_track_hover_opacity) : OPT(scrollbar_track_opacity);
    GLsizei scrollbar_width_px = (GLsizei)(OPT(scrollbar_width) * ui->cell_width);
    if (window->scrollbar.is_hovering) scrollbar_width_px = (GLsizei)(OPT(scrollbar_hover_width) * ui->cell_width);
    GLsizei scrollbar_gap_px = (GLsizei)(OPT(scrollbar_gap) * ui->cell_width);
    unsigned scrollbar_radius = (unsigned)(OPT(scrollbar_radius) * ui->cell_width);

    // Calculate window boundaries including padding
    GLsizei window_right_edge = ui->screen_left + ui->screen_width + window->render_data.geometry.spaces.right;
    GLsizei window_top_edge = ui->screen_top - window->render_data.geometry.spaces.top;
    GLsizei window_height = ui->screen_height + window->render_data.geometry.spaces.top + window->render_data.geometry.spaces.bottom;

    // Position scrollbar on right side with gap
    GLsizei scrollbar_left = window_right_edge - scrollbar_width_px - scrollbar_gap_px;
    GLsizei scrollbar_top = window_top_edge + scrollbar_gap_px;
    GLsizei scrollbar_height = window_height - 2 * scrollbar_gap_px;

    // Calculate thumb size and position
    float min_thumb_height_fraction = (OPT(scrollbar_min_handle_height) * ui->cell_height) / (float)window_height;
    float thumb_height_fraction = MAX(min_thumb_height_fraction, visible_fraction);

    // Convert to OpenGL coordinates (range -1.0 to 1.0, total span = 2.0)
    const float GL_COORD_SPAN = 2.0f;
    float thumb_height_gl = thumb_height_fraction * GL_COORD_SPAN;
    float available_space = GL_COORD_SPAN - thumb_height_gl;
    float thumb_bottom_gl = -1.0f + available_space * bar_frac;
    float thumb_top_gl = thumb_bottom_gl + thumb_height_gl;

    // Store thumb position for mouse interaction (normalized window coordinates)
    float scrollbar_top_in_window = (float)(window_top_edge + scrollbar_gap_px) / (float)ui->full_framebuffer_height;
    float scrollbar_height_in_window = (float)(window_height - 2 * scrollbar_gap_px) / (float)ui->full_framebuffer_height;
    float thumb_top_fraction = (1.0f - thumb_top_gl) / 2.0f;
    float thumb_bottom_fraction = (1.0f - thumb_bottom_gl) / 2.0f;
    window->scrollbar.thumb_top = scrollbar_top_in_window + thumb_top_fraction * scrollbar_height_in_window;
    window->scrollbar.thumb_bottom = scrollbar_top_in_window + thumb_bottom_fraction * scrollbar_height_in_window;

    // Set viewport for scrollbar area
    save_viewport_using_top_left_origin(scrollbar_left, scrollbar_top, scrollbar_width_px, scrollbar_height, ui->full_framebuffer_height);

    // Draw scrollbar track (background)
    if (track_opacity > 0) {
        bind_program(TINT_PROGRAM);
        set_color_uniform_with_opacity(track_color, track_opacity);
        glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), -1.f, 1.f, 1.f, -1.f);
        draw_quad(true, 0);
    }

    // Draw scrollbar thumb (handle)
    if (scrollbar_radius > 0) {
        // Rounded thumb - use separate viewport and rounded rect program
        GLsizei thumb_height_px = (GLsizei)(thumb_height_fraction * scrollbar_height);
        GLsizei thumb_top_px = scrollbar_top + (GLsizei)(thumb_top_fraction * scrollbar_height);

        restore_viewport();

        bind_program(ROUNDED_RECT_PROGRAM);
        color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "color"), bar_color, opacity);
        color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "background_color"), 0, 0.0f);
        color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "outer_color"), 0, 0.0f);

        float y = (float)ui->full_framebuffer_height - (float)(thumb_top_px + thumb_height_px);
        glUniform4f(program_uniform_location(ROUNDED_RECT_PROGRAM, "rect"), (float)scrollbar_left, y, (float)scrollbar_width_px, (float)thumb_height_px);

        float thickness = (float)MAX(scrollbar_width_px, thumb_height_px);
        glUniform2f(program_uniform_location(ROUNDED_RECT_PROGRAM, "params"), thickness, (float)scrollbar_radius);

        save_viewport_using_top_left_origin(scrollbar_left, thumb_top_px, scrollbar_width_px, thumb_height_px, ui->full_framebuffer_height);
        draw_quad(true, 0);
        restore_viewport();
    } else {
        set_color_uniform_with_opacity(bar_color, opacity);
        glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), -1.f, thumb_top_gl, 1.f, thumb_bottom_gl);
        draw_quad(true, 0);
        restore_viewport();
    }
}

static bool
has_progress_bar(Screen *screen) {
    if (OPT(progress_bar) == PROGRESS_BAR_HIDDEN) return false;
    return screen && screen->progress_state != PROGRESS_STATE_UNSET;
}

static void
draw_progress_handle(
    const UIRenderData *ui,
    color_type bar_color,
    float opacity,
    unsigned bar_radius,
    GLsizei track_left,
    GLsizei track_top,
    GLsizei track_width,
    GLsizei track_height,
    float handle_start,
    float handle_size,
    bool is_horizontal) {
    // handle_start and handle_size are fractions of the track length (0..1)
    // For horizontal: handle moves left-to-right; For vertical: handle moves top-to-bottom
    // Use lroundf to avoid sub-pixel jitter at the leading edge
    GLsizei handle_left, handle_top;
    GLsizei handle_w, handle_h;
    if (is_horizontal) {
        GLsizei handle_start_px = (GLsizei)lroundf(handle_start * track_width);
        GLsizei handle_end_px = (GLsizei)lroundf((handle_start + handle_size) * track_width);
        GLsizei handle_w_px = handle_end_px - handle_start_px;
        if (handle_w_px < 1) handle_w_px = 1;
        handle_left = track_left + handle_start_px;
        handle_top = track_top;
        handle_w = handle_w_px;
        handle_h = track_height;
    } else {
        GLsizei handle_start_px = (GLsizei)lroundf(handle_start * track_height);
        GLsizei handle_end_px = (GLsizei)lroundf((handle_start + handle_size) * track_height);
        GLsizei handle_h_px = handle_end_px - handle_start_px;
        if (handle_h_px < 1) handle_h_px = 1;
        handle_left = track_left;
        handle_top = track_top + handle_start_px;
        handle_w = track_width;
        handle_h = handle_h_px;
    }

    if (bar_radius > 0) {
        bind_program(ROUNDED_RECT_PROGRAM);
        color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "color"), bar_color, opacity);
        color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "background_color"), 0, 0.0f);
        color_vec4(program_uniform_location(ROUNDED_RECT_PROGRAM, "outer_color"), 0, 0.0f);

        float y = (float)ui->full_framebuffer_height - (float)(handle_top + handle_h);
        glUniform4f(program_uniform_location(ROUNDED_RECT_PROGRAM, "rect"), (float)handle_left, y, (float)handle_w, (float)handle_h);

        float thickness = (float)(is_horizontal ? handle_h : handle_w);
        glUniform2f(program_uniform_location(ROUNDED_RECT_PROGRAM, "params"), thickness, (float)bar_radius);

        save_viewport_using_top_left_origin(handle_left, handle_top, handle_w, handle_h, ui->full_framebuffer_height);
        draw_quad(true, 0);
        restore_viewport();
    } else {
        // Use GL coordinates within the track viewport, snapped to pixel boundaries
        GLsizei track_len = is_horizontal ? track_width : track_height;
        float start_snapped = (float)lroundf(handle_start * track_len) / (float)track_len;
        float end_snapped = (float)lroundf((handle_start + handle_size) * track_len) / (float)track_len;
        float start_gl = -1.0f + 2.0f * start_snapped;
        float end_gl = -1.0f + 2.0f * end_snapped;
        bind_program(TINT_PROGRAM);
        set_color_uniform_with_opacity(bar_color, opacity);
        if (is_horizontal) {
            // edges: left, top, right, bottom
            save_viewport_using_top_left_origin(track_left, track_top, track_width, track_height, ui->full_framebuffer_height);
            glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), start_gl, 1.f, end_gl, -1.f);
        } else {
            save_viewport_using_top_left_origin(track_left, track_top, track_width, track_height, ui->full_framebuffer_height);
            // For vertical: start_gl is top in GL coords (inverted y), so bottom_gl = -end_gl, top_gl = -start_gl
            glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), -1.f, -start_gl, 1.f, -end_gl);
        }
        draw_quad(true, 0);
        restore_viewport();
    }
}

static void
draw_progress_bar(const UIRenderData *ui) {
    Screen *screen = ui->screen;
    Window *window = ui->window;
    if (!window || !has_progress_bar(screen)) return;

    const ProgressBarPosition pos = OPT(progress_bar);
    const bool is_horizontal = (pos == PROGRESS_BAR_TOP || pos == PROGRESS_BAR_BOTTOM);
    color_type bar_color = scrollbar_color(screen, OPT(scrollbar_handle_color));
    color_type track_color = scrollbar_color(screen, OPT(scrollbar_track_color));
    float opacity = OPT(scrollbar_handle_opacity);
    float track_opacity = OPT(scrollbar_track_hover_opacity);
    GLsizei bar_thickness_px = (GLsizei)(OPT(scrollbar_width) * ui->cell_width);
    GLsizei gap_px = (GLsizei)(OPT(scrollbar_gap) * ui->cell_width);
    unsigned bar_radius = (unsigned)(OPT(scrollbar_radius) * ui->cell_width);
    if (bar_thickness_px < 1) return;

    // Calculate window boundaries including padding
    GLsizei window_left_edge = ui->screen_left - (GLsizei)window->render_data.geometry.spaces.left;
    GLsizei window_top_edge = ui->screen_top - (GLsizei)window->render_data.geometry.spaces.top;
    GLsizei window_width = ui->screen_width + (GLsizei)(window->render_data.geometry.spaces.left + window->render_data.geometry.spaces.right);
    GLsizei window_height = ui->screen_height + (GLsizei)(window->render_data.geometry.spaces.top + window->render_data.geometry.spaces.bottom);

    // Position the track depending on the chosen edge
    GLsizei track_left, track_top, track_width, track_height;
    switch (pos) {
        case PROGRESS_BAR_BOTTOM:
            track_left = window_left_edge + gap_px;
            track_width = window_width - 2 * gap_px;
            track_height = bar_thickness_px;
            track_top = window_top_edge + window_height - bar_thickness_px - gap_px;
            break;
        case PROGRESS_BAR_TOP:
            track_left = window_left_edge + gap_px;
            track_width = window_width - 2 * gap_px;
            track_height = bar_thickness_px;
            track_top = window_top_edge + gap_px;
            break;
        case PROGRESS_BAR_LEFT:
            track_left = window_left_edge + gap_px;
            track_width = bar_thickness_px;
            track_top = window_top_edge + gap_px;
            track_height = window_height - 2 * gap_px;
            break;
        case PROGRESS_BAR_RIGHT:
            track_left = window_left_edge + window_width - bar_thickness_px - gap_px;
            track_width = bar_thickness_px;
            track_top = window_top_edge + gap_px;
            track_height = window_height - 2 * gap_px;
            break;
        default: return;
    }
    if (track_width <= 0 || track_height <= 0) return;

    // Calculate fill fraction and indeterminate animation
    float fill_fraction = 0.0f;
    bool is_indeterminate = false;
    switch (screen->progress_state) {
        case PROGRESS_STATE_SET:
        case PROGRESS_STATE_PAUSED: fill_fraction = screen->progress_percent / 100.0f; break;
        case PROGRESS_STATE_ERROR: fill_fraction = 1.0f; break;
        case PROGRESS_STATE_INDETERMINATE: is_indeterminate = true; break;
        default: return;
    }

    // Draw track (background)
    save_viewport_using_top_left_origin(track_left, track_top, track_width, track_height, ui->full_framebuffer_height);
    if (track_opacity > 0) {
        bind_program(TINT_PROGRAM);
        set_color_uniform_with_opacity(track_color, track_opacity);
        glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), -1.f, 1.f, 1.f, -1.f);
        draw_quad(true, 0);
    }
    restore_viewport();

    // Draw fill or indeterminate handle
    // For vertical bars, progress grows from bottom to top, so invert the fraction
    if (is_indeterminate) {
        // Animate a handle bouncing back and forth like a scrollbar thumb
        const float handle_size = 0.15f;                        // 15% of track length
        const monotonic_t cycle = s_double_to_monotonic_t(2.0); // 2 second full cycle (forth and back)
        monotonic_t elapsed = screen->progress_indeterminate_anim_at > 0 ? monotonic() - screen->progress_indeterminate_anim_at : 0;
        double t = (double)(elapsed % cycle) / (double)cycle; // 0..1 over one cycle
        // Triangle wave: goes 0→1→0 over one cycle
        float pos_frac = (float)(t < 0.5 ? t * 2.0 : 2.0 - t * 2.0);
        if (!is_horizontal) pos_frac = 1.0f - pos_frac; // vertical: bounce bottom-to-top first
        float handle_start = pos_frac * (1.0f - handle_size);
        if (opacity > 0.0f) {
            draw_progress_handle(
                ui, bar_color, opacity, bar_radius, track_left, track_top, track_width, track_height, handle_start, handle_size, is_horizontal);
        }
    } else if (fill_fraction > 0.0f && opacity > 0.0f) {
        float handle_start = is_horizontal ? 0.0f : 1.0f - fill_fraction;
        draw_progress_handle(ui, bar_color, opacity, bar_radius, track_left, track_top, track_width, track_height, handle_start, fill_fraction, is_horizontal);
    }
}


static void
draw_window_logo(const UIRenderData *ui) {
    struct {
        unsigned width, height;
        int left, top;
    } w;
    WindowLogoRenderData *wl = ui->window_logo;
    w.height = wl->instance->height;
    w.width = wl->instance->width;
    if (OPT(window_logo_scale.width) > 0 || OPT(window_logo_scale.height) > 0) {
        unsigned scaled_wl_width = ui->screen_width, scaled_wl_height = ui->screen_height;

        // [sx] Scales logo to sx % of the viewports shortest dimension, preserving aspect ratio
        if (OPT(window_logo_scale.height) < 0) {
            if (ui->screen_height < ui->screen_width) {
                scaled_wl_height = (int)(ui->screen_height * OPT(window_logo_scale.width) / 100);
                scaled_wl_width = wl->instance->width * scaled_wl_height / wl->instance->height;
            } else {
                scaled_wl_width = (int)(ui->screen_width * OPT(window_logo_scale.width) / 100);
                scaled_wl_height = wl->instance->height * scaled_wl_width / wl->instance->width;
            }
        }
        // [0 sy] Scales logo's y dimension to sy % of viewporty keeping original x dimension
        else if (OPT(window_logo_scale.width) == 0.0) {
            scaled_wl_height = (int)(scaled_wl_height * OPT(window_logo_scale.height) / 100);
            scaled_wl_width = wl->instance->width;
        }
        // [sx 0] Scales logo's x dimension to sx % of viewportx keeping original y dimension
        else if (OPT(window_logo_scale.height) == 0.0) {
            scaled_wl_width = (int)(scaled_wl_width * OPT(window_logo_scale.width) / 100);
            scaled_wl_height = wl->instance->height;
        }
        // [sx sy] Scales logo's x and y dimension to sx and sy % of viewportx and viewporty respectively
        else {
            scaled_wl_height = (int)(scaled_wl_height * OPT(window_logo_scale.height) / 100);
            scaled_wl_width = (int)(scaled_wl_width * OPT(window_logo_scale.width) / 100);
        }
        w.width = scaled_wl_width;
        w.height = scaled_wl_height;
    }
    w.left = (int)(ui->screen_width * wl->position.canvas_x - w.width * wl->position.image_x);
    w.top = (int)(ui->screen_height * wl->position.canvas_y - w.height * wl->position.image_y);
    float left = gl_pos_x(w.left, ui->screen_width), top = gl_pos_y(w.top, ui->screen_height);
    ImageRenderData d = {.texture_id = wl->instance->texture_id};
    gpu_data_for_image(&d, left, top, left + gl_size(w.width, ui->screen_width), top - gl_size(w.height, ui->screen_height));
    draw_graphics(GRAPHICS_PROGRAM, &d, 0, 1, ui->inactive_text_alpha * OPT(window_logo_alpha), 1.f);
}

bool
screen_needs_rendering_in_layers(OSWindow *os_window, Window *w, Screen *screen) {
    const bool has_ui = w && ((screen->start_visual_bell_at | screen->start_drag_overlay_at) || has_scrollbar(w, screen) || has_progress_bar(screen) ||
                              has_hyperlink_target(os_window, w, screen) || has_window_number(w, screen) || w->window_logo.id);
    GraphicsManager *grman = screen->paused_rendering.expires_at && screen->paused_rendering.grman ? screen->paused_rendering.grman : screen->grman;
    return has_ui || grman_has_images(grman) || screen->has_translucent_cells;
}

bool
current_framebuffer_is_ok(void) {
    return check_framebuffer_status() == NULL;
}

// }}}

enum { DRAW_NEITHER_BG = 0, DRAW_DEFAULT_BG = 1, DRAW_NON_DEFAULT_BG = 2, DRAW_BOTH_BG = 3 };

static void
call_cell_program(int program, const UIRenderData *ui, ssize_t vao_idx, bool for_final_output, unsigned draw_bg_bitfield) {
    bind_program(program);
    CELL_BUFFERS;
    bind_vao_uniform_buffer(vao_idx, uniform_buffer, CELL_RENDER_DATA_BINDING_POINT);
    bind_vao_uniform_buffer(vao_idx, color_table_buffer, COLOR_TABLE_BINDING_POINT);
    const GLint background_location = try_program_uniform_location(program, "draw_bg_bitfield");
    if (background_location >= 0) glUniform1ui(background_location, draw_bg_bitfield);
    const OSWindow *w = ui->os_window;
    const float scale = w->spatial.mode == 2 ? w->spatial.scale : 1.f;
    glUniform4f(program_uniform_location(program, "spatial_depth"),
        w->spatial.mode ? (float)w->spatial.eye * w->viewport_width / (scale * ui->screen_width) : 0.f,
        w->spatial.mode ? tanf(w->spatial.fov * (float)(M_PI / 360.)) : 1.f,
        w->spatial.positive, w->spatial.negative);
    glUniform2f(program_uniform_location(program, "spatial_phase"),
        w->spatial.mode ? -(float)w->spatial.eye / ui->screen_height : 0.f, w->spatial.phase_limit);
    glUniform1f(program_uniform_location(program, "spatial_multiplier"), w->spatial.mode ? w->spatial.multiplier : 1.f);
    glUniform2f(program_uniform_location(program, "spatial_pixel_size"),
        w->spatial.mode ? 2.f / ui->screen_width : 0.f, w->spatial.mode ? 2.f / ui->screen_height : 0.f);
    if (for_final_output) glEnable(GL_FRAMEBUFFER_SRGB);
    draw_quad(!for_final_output, render_lines_for_screen(ui->screen) * ui->screen->columns);
    if (for_final_output) glDisable(GL_FRAMEBUFFER_SRGB);
}

static void
draw_cells_without_layers(const UIRenderData *ui, ssize_t vao_idx) {
    call_cell_program(CELL_PROGRAM, ui, vao_idx, true, DRAW_BOTH_BG);
}

static void
configure_cell_vao_attributes(ssize_t vao_idx, unsigned int base_cell, unsigned int cell_step) {
    // (Re)point the instanced cell attributes so that instance i corresponds to
    // the cell at (base_cell + i*cell_step). Used to restore the canonical
    // layout after padding draws (base_cell=0, cell_step=1). The VAO must be
    // bound before calling this.
    CELL_BUFFERS;
    const GLsizei cell_stride = (GLsizei)(cell_step * sizeof(GPUCell));
    const uintptr_t cell_base = (uintptr_t)base_cell * sizeof(GPUCell);
    set_vao_attribute(vao_idx, cell_data_buffer, program_attribute_location(CELL_PROGRAM, "depth"), 1, GL_INT,
        cell_stride, (void *)(cell_base + offsetof(GPUCell, depth)), 1);
    set_vao_attribute(vao_idx, cell_data_buffer, program_attribute_location(CELL_PROGRAM, "depth_pixels"), 1, GL_FLOAT,
        cell_stride, (void *)(cell_base + offsetof(GPUCell, depth_pixels)), 1);
    set_vao_attribute(vao_idx, cell_data_buffer, program_attribute_location(CELL_PROGRAM, "phase"), 1, GL_FLOAT,
        cell_stride, (void *)(cell_base + offsetof(GPUCell, phase)), 1);
    set_vao_attribute(
        vao_idx,
        cell_data_buffer,
        program_attribute_location(CELL_PROGRAM, "sprite_idx"),
        2,
        GL_UNSIGNED_INT,
        cell_stride,
        (void *)(cell_base + offsetof(GPUCell, sprite_idx)),
        1);
    set_vao_attribute(
        vao_idx,
        cell_data_buffer,
        program_attribute_location(CELL_PROGRAM, "colors"),
        3,
        GL_UNSIGNED_INT,
        cell_stride,
        (void *)(cell_base + offsetof(GPUCell, fg)),
        1);
    set_vao_attribute(
        vao_idx,
        selection_buffer,
        program_attribute_location(CELL_PROGRAM, "is_selected"),
        1,
        GL_UNSIGNED_BYTE,
        (GLsizei)(cell_step * sizeof(GLubyte)),
        (void *)((uintptr_t)base_cell * sizeof(GLubyte)),
        1);
}

static void
configure_padding_vao_attributes(ssize_t vao_idx) {
    // Point the instanced cell attributes at the dedicated padding buffers
    // (base=0, stride=sizeof(GPUCell)), which are pre-packed before each
    // combined draw. The VAO must be bound before calling this.
    CELL_BUFFERS;
    set_vao_attribute(
        vao_idx,
        padding_cell_data_buffer,
        program_attribute_location(CELL_PROGRAM, "sprite_idx"),
        2,
        GL_UNSIGNED_INT,
        sizeof(GPUCell),
        (void *)offsetof(GPUCell, sprite_idx),
        1);
    set_vao_attribute(
        vao_idx,
        padding_cell_data_buffer,
        program_attribute_location(CELL_PROGRAM, "colors"),
        3,
        GL_UNSIGNED_INT,
        sizeof(GPUCell),
        (void *)offsetof(GPUCell, fg),
        1);
    set_vao_attribute(
        vao_idx, padding_selection_buffer, program_attribute_location(CELL_PROGRAM, "is_selected"), 1, GL_UNSIGNED_BYTE, sizeof(GLubyte), NULL, 1);
}

static void
draw_window_padding(const UIRenderData *ui, unsigned int cl, unsigned int ct, unsigned int cr, unsigned int cb, ssize_t vao_idx, bool for_final_output) {
    // Color the compensatory padding strips (the innermost slice of the window
    // padding, arising from the window size not being an exact multiple of the
    // cell size) to match their neighboring cell. The strips lie outside the
    // per-window cell viewport, so this runs with the full framebuffer viewport.
    //
    // Two instanced draws are issued: one covers the horizontal pair (top+bottom)
    // and one the vertical pair (left+right). Each draw packs its strip cells into
    // a dedicated buffer so the VAO needs no per-strip reconfiguration. The shader
    // selects per-strip geometry and cell indices branch-free via lerp.
    if (OPT(padding_fill_strategy) != PADDING_FILL_NEIGHBORING_CELL || !(cl | ct | cr | cb)) return;
    Screen *screen = ui->screen;
    const unsigned int columns = screen->columns, lines = screen->lines;
    if (!columns || !lines) return;
    const unsigned int render_offset = pixel_scroll_enabled(screen) ? 1u : 0u;
    const unsigned int top_row = render_offset, bottom_row = render_offset + lines - 1u;

    const float fbw = (float)ui->full_framebuffer_width, fbh = (float)ui->full_framebuffer_height;
    const float cw = (float)ui->cell_width, ch = (float)ui->cell_height;
    const float fL = (float)ui->screen_left, T = (float)ui->screen_top;
    const float R = fL + (float)ui->screen_width, B = T + (float)ui->screen_height;
#define NX(px) (2.f * (px) / fbw - 1.f)
#define NY(px) (1.f - 2.f * (px) / fbh)
    const float dx = 2.f * cw / fbw, dy = 2.f * ch / fbh;

    bind_program(PADDING_PROGRAM);
    bind_vertex_array(vao_idx);
    CELL_BUFFERS;
    bind_vao_uniform_buffer(vao_idx, uniform_buffer, CELL_RENDER_DATA_BINDING_POINT);
    bind_vao_uniform_buffer(vao_idx, color_table_buffer, COLOR_TABLE_BINDING_POINT);
    if (for_final_output) glEnable(GL_FRAMEBUFFER_SRGB);

    // Point instanced attributes at the padding-specific buffers once for both draws.
    configure_padding_vao_attributes(vao_idx);

#define PL(x) program_uniform_location(PADDING_PROGRAM, #x)

    // Horizontal combined draw: top strip (strip 0) + bottom strip (strip 1).
    // Cell data is packed contiguously via GPU-side copies (no CPU round-trip).
    if (ct || cb) {
        const unsigned int nH = (ct ? 1u : 0u) + (cb ? 1u : 0u);
        alloc_vao_buffer(vao_idx, (GLsizeiptr)(sizeof(GPUCell) * nH * columns), padding_cell_data_buffer, GL_DYNAMIC_DRAW);
        alloc_vao_buffer(vao_idx, (GLsizeiptr)(sizeof(GLubyte) * nH * columns), padding_selection_buffer, GL_DYNAMIC_DRAW);

        unsigned int sidx = 0u;
        if (ct) {
            copy_vao_buffer_region(
                vao_idx,
                cell_data_buffer,
                (GLintptr)(sizeof(GPUCell) * top_row * columns),
                padding_cell_data_buffer,
                (GLintptr)(sizeof(GPUCell) * sidx * columns),
                (GLsizeiptr)(columns * sizeof(GPUCell)));
            copy_vao_buffer_region(
                vao_idx,
                selection_buffer,
                (GLintptr)(sizeof(GLubyte) * top_row * columns),
                padding_selection_buffer,
                (GLintptr)(sizeof(GLubyte) * sidx * columns),
                (GLsizeiptr)(columns * sizeof(GLubyte)));
            sidx++;
        }
        if (cb) {
            copy_vao_buffer_region(
                vao_idx,
                cell_data_buffer,
                (GLintptr)(sizeof(GPUCell) * bottom_row * columns),
                padding_cell_data_buffer,
                (GLintptr)(sizeof(GPUCell) * sidx * columns),
                (GLsizeiptr)(sizeof(GPUCell) * columns));
            copy_vao_buffer_region(
                vao_idx,
                selection_buffer,
                (GLintptr)(sizeof(GLubyte) * bottom_row * columns),
                padding_selection_buffer,
                (GLintptr)(sizeof(GLubyte) * sidx * columns),
                (GLsizeiptr)(columns * sizeof(GLubyte)));
        }

        // Strip-0 is top (or bottom when only bottom exists); strip-1 is bottom.
        // For a single-strip draw base_instance2/across2 equal strip-0 values so
        // the shader lerp is a no-op for all instances (strip_f is always 0.0).
        const unsigned int base0 = (ct ? top_row : bottom_row) * columns;
        const unsigned int base1 = bottom_row * columns;
        glUniform1ui(PL(is_horizontal), 1u);
        glUniform1ui(PL(along_count), columns);
        glUniform1ui(PL(instance_step), 1u);
        glUniform1ui(PL(base_instance), base0);
        glUniform1ui(PL(base_instance2), base1);
        glUniform2f(PL(across), ct ? NY(T - ct) : NY(B), ct ? NY(T) : NY(B + cb));
        glUniform2f(PL(across2), NY(B), NY(B + cb));
        glUniform1f(PL(along_start), NX(fL));
        glUniform1f(PL(along_step), dx);
        glUniform2f(PL(along_clamp), NX(fL), NX(R));
        draw_quad(!for_final_output, nH * columns);
    }

    // Vertical combined draw: left strip (strip 0) + right strip (strip 1).
    // Column cells are non-contiguous in the cell buffer so we gather them
    // CPU-side by mapping the buffer for reading, then upload in one shot.
    if (cl || cr) {
        const unsigned int nV = (cl ? 1u : 0u) + (cr ? 1u : 0u);
        // VLA sizes: lines is bounded by the window height / cell height (~<500).
        GPUCell gathered_cells[2 * lines];
        GLubyte gathered_sel[2 * lines];

        const unsigned int strip0_col = cl ? 0u : (columns - 1u);
        const unsigned int strip1_col = columns - 1u;

        const GPUCell *cells = (const GPUCell *)map_vao_buffer_for_reading(vao_idx, cell_data_buffer);
        for (unsigned int i = 0; i < lines; i++) {
            gathered_cells[i] = cells[(top_row + i) * columns + strip0_col];
            if (nV == 2u) gathered_cells[lines + i] = cells[(top_row + i) * columns + strip1_col];
        }
        unmap_vao_buffer(vao_idx, cell_data_buffer);

        const GLubyte *sel = (const GLubyte *)map_vao_buffer_for_reading(vao_idx, selection_buffer);
        for (unsigned int i = 0; i < lines; i++) {
            gathered_sel[i] = sel[(top_row + i) * columns + strip0_col];
            if (nV == 2u) gathered_sel[lines + i] = sel[(top_row + i) * columns + strip1_col];
        }
        unmap_vao_buffer(vao_idx, selection_buffer);

        void *dst = alloc_and_map_vao_buffer(vao_idx, (GLsizeiptr)(sizeof(GPUCell) * nV * lines), padding_cell_data_buffer);
        memcpy(dst, gathered_cells, sizeof(GPUCell) * nV * lines);
        unmap_vao_buffer(vao_idx, padding_cell_data_buffer);

        dst = alloc_and_map_vao_buffer(vao_idx, (GLsizeiptr)(sizeof(GLubyte) * nV * lines), padding_selection_buffer);
        memcpy(dst, gathered_sel, sizeof(GLubyte) * nV * lines);
        unmap_vao_buffer(vao_idx, padding_selection_buffer);

        const unsigned int base0 = top_row * columns + (cl ? 0u : (columns - 1u));
        const unsigned int base1 = top_row * columns + (columns - 1u);
        glUniform1ui(PL(is_horizontal), 0u);
        glUniform1ui(PL(along_count), lines);
        glUniform1ui(PL(instance_step), columns);
        glUniform1ui(PL(base_instance), base0);
        glUniform1ui(PL(base_instance2), base1);
        glUniform2f(PL(across), cl ? NX(fL - cl) : NX(R), cl ? NX(fL) : NX(R + cr));
        glUniform2f(PL(across2), NX(R), NX(R + cr));
        glUniform1f(PL(along_start), NY(T));
        glUniform1f(PL(along_step), -dy);
        glUniform2f(PL(along_clamp), NY(T - ct), NY(B + cb));
        draw_quad(!for_final_output, nV * lines);
    }

#undef PL
    if (for_final_output) glDisable(GL_FRAMEBUFFER_SRGB);
    // Restore the canonical cell attribute layout so subsequent cell rendering is unaffected.
    configure_cell_vao_attributes(vao_idx, 0u, 1u);
    unbind_program();
#undef NX
#undef NY
}

static void
draw_tint(const UIRenderData *ui) {
    bind_program(TINT_PROGRAM);
    color_vec4_premult(program_uniform_location(TINT_PROGRAM, "tint_color"), ui->background_color, OPT(background_tint));
    glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), -1, 1, 1, -1);
    draw_quad(true, 0);
}

// Draw a range of the image render data. Indexed color and palette images need
// different programs, so when any are present, switch programs at the
// boundaries between runs of images that need different programs, preserving
// draw order.
static int
program_for_image(const ImageRenderData *rd) {
    if (!rd->indexed_px_sz) return GRAPHICS_PROGRAM;
    if (rd->palette_texture_id) return rd->is_unscaled ? GRAPHICS_PALETTE_UNSCALED_PROGRAM : GRAPHICS_PALETTE_PROGRAM;
    return rd->is_unscaled ? GRAPHICS_INDEXED_UNSCALED_PROGRAM : GRAPHICS_INDEXED_PROGRAM;
}

static void
draw_image_layer(const UIRenderData *ui, ssize_t vao_idx, float hdr_headroom, GLuint start, GLuint count) {
    if (!ui->grd.num_of_indexed_refs) {
        draw_graphics(GRAPHICS_PROGRAM, ui->grd.images, start, count, ui->inactive_text_alpha, hdr_headroom);
        return;
    }
    CELL_BUFFERS;
    // The color table may not be bound yet, for example, when the window has a background image
    bind_vao_uniform_buffer(vao_idx, color_table_buffer, COLOR_TABLE_BINDING_POINT);
    const ImageRenderData *data = ui->grd.images;
    for (GLuint run_start = start, end = start + count; run_start < end;) {
        const int program = program_for_image(data + run_start);
        GLuint run_end = run_start + 1;
        while (run_end < end && program_for_image(data + run_end) == program) run_end++;
        draw_graphics(program, ui->grd.images, run_start, run_end - run_start, ui->inactive_text_alpha, hdr_headroom);
        run_start = run_end;
    }
}

static void
draw_cells_with_layers(const UIRenderData *ui, ssize_t vao_idx) {
    if (ui->has_background_image && OPT(background_tint) > 0) draw_tint(ui);
    const bool has_content_between_background_and_foreground = ui->os_window->spatial.mode || ui->window_logo != NULL || ui->grd.num_of_below_refs > 0 || ui->grd.num_of_negative_refs > 0;
    const float hdr_headroom = effective_hdr_headroom(ui->os_window);
    if (has_content_between_background_and_foreground) {
        if (!ui->has_background_image) call_cell_program(CELL_BG_PROGRAM, ui, vao_idx, false, DRAW_DEFAULT_BG);
        if (ui->window_logo != NULL) draw_window_logo(ui);
        if (ui->grd.num_of_below_refs > 0) draw_image_layer(ui, vao_idx, hdr_headroom, 0, ui->grd.num_of_below_refs);
        call_cell_program(CELL_BG_PROGRAM, ui, vao_idx, false, DRAW_NON_DEFAULT_BG);
        if (ui->grd.num_of_negative_refs) draw_image_layer(ui, vao_idx, hdr_headroom, ui->grd.num_of_below_refs, ui->grd.num_of_negative_refs);
        call_cell_program(CELL_FG_PROGRAM, ui, vao_idx, false, DRAW_NEITHER_BG);
    } else call_cell_program(CELL_PROGRAM, ui, vao_idx, false, ui->has_background_image ? DRAW_NON_DEFAULT_BG : DRAW_BOTH_BG);

    if (ui->grd.num_of_positive_refs > 0) draw_image_layer(ui, vao_idx, hdr_headroom, ui->grd.num_of_below_refs + ui->grd.num_of_negative_refs, ui->grd.num_of_positive_refs);

    draw_visual_bell(ui);
    draw_drag_preview_overlay(ui);
    draw_progress_bar(ui);
    draw_scrollbar(ui);
    draw_hyperlink_target(ui);
    draw_window_number(ui);
}

void
blank_canvas(float background_opacity, color_type color_in_srgb, bool for_final_output) {
    if (for_final_output) {
        // we need to write pre-multiplied sRGB color to framebuffer
#define C(shift) ((((color_in_srgb >> shift) & 0xFF) / 255.f) * background_opacity)
        glClearColor(C(16), C(8), C(0), background_opacity);
#undef C
    } else {
        // we need to write pre-multiplied linear color to framebuffer
#define C(shift) srgb_color((color_in_srgb >> shift) & 0xFF) * background_opacity
        glClearColor(C(16), C(8), C(0), background_opacity);
#undef C
    }
    glClear(GL_COLOR_BUFFER_BIT);
}

bool
send_cell_data_to_gpu(ssize_t vao_idx, Screen *screen, OSWindow *os_window) {
    bool changed = false;
    if (vao_idx < 0) return false;  // see ensure_cell_vao()
    if (os_window->fonts_data) {
        if (cell_prepare_to_render(vao_idx, screen, os_window->fonts_data)) changed = true;
    }
    return changed;
}

void
draw_cells(
    const WindowRenderData *srd,
    OSWindow *os_window,
    bool is_active_window,
    bool is_tab_bar,
    bool is_single_window,
    Window *window,
    const Window *title_bar_of,
    monotonic_t now) {
    Screen *screen = srd->screen;
    CELL_BUFFERS;
    if (srd->vao_idx < 0) return;  // see ensure_cell_vao()
    bind_vertex_array(srd->vao_idx);
    // When inactive_text_alpha is negative, its absolute value is used as the
    // opacity and fading is based only on whether the current window is active.
    const float inactive_text_alpha = fabsf(OPT(inactive_text_alpha));
    const bool use_active_window_only = OPT(inactive_text_alpha) < 0.f;
    float current_inactive_text_alpha =
        use_active_window_only
            ? (is_tab_bar || is_active_window ? 1.0f : inactive_text_alpha)
            : (is_tab_bar || (!is_single_window && is_active_window) || (is_single_window && screen->cursor_render_info.is_focused) ? 1.0f
                                                                                                                                    : inactive_text_alpha);
    float bg_alpha = effective_os_window_alpha(os_window);

    color_type default_bg = cell_update_uniform_block(
        srd->vao_idx, screen, uniform_buffer, color_table_buffer, &screen->cursor_render_info, os_window, current_inactive_text_alpha, bg_alpha);
    set_cell_uniforms(screen->reload_all_gpu_data);
    bind_deep_colors(screen);
    WindowLogoRenderData *wl;
    if (window && (wl = &window->window_logo) && wl->id && (wl->instance = find_window_logo(global_state.all_window_logos, wl->id)) &&
        wl->instance->load_from_disk_ok) {
        if (!window->window_logo.instance->texture_id) { set_on_gpu_state(window->window_logo.instance, true); }
    } else wl = NULL;
    GraphicsManager *grman = screen->paused_rendering.expires_at && screen->paused_rendering.grman ? screen->paused_rendering.grman : screen->grman;
    UIRenderData ui = {
        .screen_width = srd->geometry.right - srd->geometry.left,
        .screen_height = srd->geometry.bottom - srd->geometry.top,
        .cell_width = os_window->fonts_data->fcm.cell_width,
        .cell_height = os_window->fonts_data->fcm.cell_height,
        .screen_left = srd->geometry.left,
        .screen_top = srd->geometry.top,
        .full_framebuffer_width = os_window->viewport_width,
        .full_framebuffer_height = os_window->viewport_height,
        .window = window,
        .screen = screen,
        .os_window = os_window,
        .grd = grman_render_data(grman),
        .window_logo = wl,
        .inactive_text_alpha = current_inactive_text_alpha,
        .has_background_image = has_bgimage(os_window),
        .background_color = default_bg,
        .bg_alpha = effective_os_window_alpha(os_window),
        .now = now,
    };
    screen->reload_all_gpu_data = false;
    save_viewport_using_top_left_origin(ui.screen_left, ui.screen_top, ui.screen_width, ui.screen_height, ui.full_framebuffer_height);
    if (ui.os_window->needs_layers) draw_cells_with_layers(&ui, srd->vao_idx);
    else draw_cells_without_layers(&ui, srd->vao_idx);
    restore_viewport();
    // The compensatory padding lies outside the per-window cell viewport, so it
    // is drawn after restoring the full framebuffer viewport.
    if (window) {
        draw_window_padding(
            &ui,
            window->size_mismatch_padding.left,
            window->size_mismatch_padding.top,
            window->size_mismatch_padding.right,
            window->size_mismatch_padding.bottom,
            srd->vao_idx,
            !ui.os_window->needs_layers);
    } else if (title_bar_of) {
        // The left and right compensatory padding of a window extends alongside
        // its title bar. The layout ensures there is no compensatory padding
        // between the title bar and the window border.
        draw_window_padding(
            &ui, title_bar_of->size_mismatch_padding.left, 0, title_bar_of->size_mismatch_padding.right, 0, srd->vao_idx, !ui.os_window->needs_layers);
    }
}
// }}}

// Borders {{{

static void
populate_border_colors(GLuint colors[9], color_type active_window_bg, unsigned int num_visible_windows, bool all_windows_have_same_bg, const OSWindow *w) {
    colors[0] = (num_visible_windows > 1 && !all_windows_have_same_bg) ? os_window_background(w) : active_window_bg;
    colors[1] = OPT(active_border_color);
    colors[2] = OPT(inactive_border_color);
    colors[3] = 0;
    colors[4] = OPT(bell_border_color);
    colors[5] = os_window_tab_bar_background(w);
    colors[6] = os_window_tab_bar_margin_color(w);
    colors[7] = w->tab_bar_edge_color.left;
    colors[8] = w->tab_bar_edge_color.right;
}

ssize_t
create_border_vao(void) {
    ssize_t vao_idx = create_vao();
    if (vao_idx < 0) return -1;
    if (add_buffer_to_vao(vao_idx, GL_ARRAY_BUFFER) < 0) {
        remove_vao(vao_idx);
        return -1;
    }
    add_attribute_to_vao(
        vao_idx,
        program_attribute_location(BORDERS_PROGRAM, "rect"),
        /*size=*/4,
        /*dtype=*/GL_FLOAT,
        /*stride=*/sizeof(BorderRect),
        /*offset=*/(void *)offsetof(BorderRect, left),
        /*divisor=*/1);
    add_attribute_to_vao(
        vao_idx,
        program_attribute_location(BORDERS_PROGRAM, "rect_color"),
        /*size=*/1,
        /*dtype=*/GL_UNSIGNED_INT,
        /*stride=*/sizeof(BorderRect),
        /*offset=*/(void *)(offsetof(BorderRect, color)),
        /*divisor=*/1);

    return vao_idx;
}

void
draw_borders(
    ssize_t vao_idx,
    unsigned int num_border_rects,
    BorderRect *rect_buf,
    bool rect_data_is_dirty,
    color_type active_window_bg,
    unsigned int num_visible_windows,
    bool all_windows_have_same_bg,
    OSWindow *w) {
    float background_opacity = effective_os_window_alpha(w);
    // Without memory for their VAOs the borders are not drawn this time
    if (!num_border_rects || vao_idx < 0 || !ensure_shader_globals_vao()) return;
    bind_program(BORDERS_PROGRAM);
    bind_vertex_array(vao_idx);
    const bool has_background_image = has_bgimage(w);
    if (has_background_image) background_opacity = OPT(background_tint) * OPT(background_tint_gaps);
    if (rect_data_is_dirty) {
        const size_t sz = sizeof(BorderRect) * num_border_rects;
        void *borders_buf_address = alloc_and_map_vao_buffer(vao_idx, sz, 0);
        if (borders_buf_address) memcpy(borders_buf_address, rect_buf, sz);
        unmap_vao_buffer(vao_idx, 0);
    }
    GLuint colors[9];
    populate_border_colors(colors, active_window_bg, num_visible_windows, all_windows_have_same_bg, w);
    void *colors_buf =
        map_vao_buffer_for_write_only(shader_globals_vao_idx, BORDER_COLORS_GLOBAL_BUFFER, 0, program_uniform_block(BORDERS_PROGRAM, "Colors").size);
    const ArrayInformation border_colors_array = program_uniform_array(BORDERS_PROGRAM, "colors");
    write_uint_array_to_ubo(colors_buf, colors, arraysz(colors), &border_colors_array);
    unmap_vao_buffer(shader_globals_vao_idx, BORDER_COLORS_GLOBAL_BUFFER);
    glUniform1f(program_uniform_location(BORDERS_PROGRAM, "background_opacity"), background_opacity);
    if (!w->needs_layers) glEnable(GL_FRAMEBUFFER_SRGB);
    draw_quad(has_background_image, num_border_rects);
    if (!w->needs_layers) glDisable(GL_FRAMEBUFFER_SRGB);
    unbind_program();
    unbind_vertex_array();
}

void
draw_rounded_borders(BorderRects *br, color_type active_window_bg, unsigned int num_visible_windows, bool all_windows_have_same_bg, OSWindow *w) {
    if (!br->num_rounded_rects || br->vao_idx < 0 || !ensure_shader_globals_vao()) return;
    GLuint colors[9];
    populate_border_colors(colors, active_window_bg, num_visible_windows, all_windows_have_same_bg, w);
    // The corner regions between the rounded outline and its bounding rect
    // must be painted every frame: when not rendering in layers the
    // framebuffer is never cleared, kitty relies on full pixel coverage.
    // Fill them with the default background at the same opacity draw_borders()
    // uses for non-border rects, so corners match the surrounding gaps.
    float corner_bg_alpha = has_bgimage(w) ? OPT(background_tint) * OPT(background_tint_gaps) : effective_os_window_alpha(w);
    bind_vertex_array(br->vao_idx);
    if (!w->needs_layers) glEnable(GL_FRAMEBUFFER_SRGB);
    for (unsigned i = 0; i < br->num_border_rects; i++) {
        const BorderRect *r = br->rect_buf + i;
        if (!r->radius || !r->thickness) continue;
        Viewport rect = {.left = r->px.left, .top = r->px.top, .width = r->px.right - r->px.left, .height = r->px.bottom - r->px.top};
        unsigned radius = MIN(r->radius, MIN(rect.width, rect.height) / 2);
        unsigned color_index = r->color & 0xff;
        color_type color = color_index == 3 ? r->color >> 8 : colors[MIN(color_index, 8u)];
        draw_rounded_rect(rect, w->viewport_height, (float)r->thickness, radius, color, 0, 0.f, colors[0], corner_bg_alpha);
    }
    if (!w->needs_layers) glDisable(GL_FRAMEBUFFER_SRGB);
    unbind_program();
    // Deliberately leave the VAO bound: this runs after all cell drawing and
    // subsequent draw_quad() calls (e.g. the fullscreen blit in
    // stop_os_window_rendering()) need some VAO bound in a core profile context.
}

// }}}

// Cursor Trail {{{
void
draw_cursor_trail(CursorTrail *trail, Window *active_window) {
    bind_program(TRAIL_PROGRAM);

    glUniform4fv(program_uniform_location(TRAIL_PROGRAM, "x_coords"), 1, trail->corner_x);
    glUniform4fv(program_uniform_location(TRAIL_PROGRAM, "y_coords"), 1, trail->corner_y);

    glUniform2fv(program_uniform_location(TRAIL_PROGRAM, "cursor_edge_x"), 1, trail->cursor_edge_x);
    glUniform2fv(program_uniform_location(TRAIL_PROGRAM, "cursor_edge_y"), 1, trail->cursor_edge_y);

    color_type trail_color = OPT(cursor_trail_color);
    if (trail_color == 0) { // 0 means "none" was specified
        trail_color = active_window ? active_window->render_data.screen->last_rendered.cursor_bg : OPT(foreground);
    }
    color_vec3(program_uniform_location(TRAIL_PROGRAM, "trail_color"), trail_color);

    glUniform1f(program_uniform_location(TRAIL_PROGRAM, "trail_opacity"), trail->opacity);

    draw_quad(true, 0);
    unbind_program();
}

// }}}

// OSWindow {{{
static void
draw_bg_image(OSWindow *os_window, Tab *tab) {
    BackgroundImage *bg = background_image_for_os_window(os_window);
    // the VAO of the borders is what is bound for drawing it, see below; without one it is not drawn this time
    if (!bg || tab->border_rects.vao_idx < 0) return;
    BackgroundImageLayout layout = os_window->background_image.has_layout ? os_window->background_image.layout : OPT(background_image_layout);
    BackgroundImageRenderSettings s = {
        .os_window.width = os_window->viewport_width,
        .os_window.height = os_window->viewport_height,
        .instance_id = bg->id,
        .layout = layout,
        .linear = OPT(background_image_linear),
        .bgcolor = OPT(background),
        .opacity = effective_os_window_alpha(os_window),
    };
    GLfloat iwidth = bg->width, iheight = bg->height;
    GLfloat vwidth = s.os_window.width, vheight = s.os_window.height;
    if (CENTER_SCALED == layout) {
        GLfloat ifrac = iwidth / iheight;
        if (ifrac > (vwidth / vheight)) {
            iheight = vheight;
            iwidth = iheight * ifrac;
        } else {
            iwidth = vwidth;
            iheight = iwidth / ifrac;
        }
    }
    GLfloat tiled = 0.f;
    ;
    GLfloat left = -1.0, top = 1.0, right = 1.0, bottom = -1.0;
    switch (layout) {
        case TILING:
        case MIRRORED:
        case CLAMPED: tiled = 1.f; break;
        case SCALED: break;
        case CENTER_CLAMPED:
        case CENTER_SCALED: {
            GLfloat wfrac = (vwidth - iwidth) / vwidth;
            GLfloat hfrac = (vheight - iheight) / vheight;
            left += wfrac;
            right -= wfrac;
            top -= hfrac;
            bottom += hfrac;
        } break;
    }
    bind_program(BGIMAGE_PROGRAM);
    // altough we dont use this VO we need to ensure *some* VAO is bound at this point.
    bind_vertex_array(tab->border_rects.vao_idx);
    glUniform4f(program_uniform_location(BGIMAGE_PROGRAM, "sizes"), vwidth, vheight, iwidth, iheight);
    glUniform1f(program_uniform_location(BGIMAGE_PROGRAM, "tiled"), tiled);
    glUniform4f(program_uniform_location(BGIMAGE_PROGRAM, "positions"), left, top, right, bottom);
    glUniform1i(program_uniform_location(BGIMAGE_PROGRAM, "image"), GRAPHICS_UNIT);
    color_vec4(program_uniform_location(BGIMAGE_PROGRAM, "background"), s.bgcolor, s.opacity);
    glActiveTexture(GL_TEXTURE0 + GRAPHICS_UNIT);
    glBindTexture(GL_TEXTURE_2D, bg->texture_id);
    draw_quad(false, 0);
    unbind_program();
}

static void
gpu_data_for_centered_image(ImageRenderData *ans, unsigned int screen_width_px, unsigned int screen_height_px, unsigned int width, unsigned int height) {
    float width_frac = 2 * MIN(1, width / (float)screen_width_px), height_frac = 2 * MIN(1, height / (float)screen_height_px);
    float hmargin = (2 - width_frac) / 2;
    float vmargin = (2 - height_frac) / 2;
    gpu_data_for_image(ans, -1 + hmargin, 1 - vmargin, -1 + hmargin + width_frac, 1 - vmargin - height_frac);
}

static void
draw_centered_alpha_mask(size_t screen_width, size_t screen_height, size_t width, size_t height, uint8_t *canvas, float background_opacity) {
    ImageRenderData *data = load_alpha_mask_texture(width, height, canvas);
    gpu_data_for_centered_image(data, screen_width, screen_height, width, height);
    bind_program(GRAPHICS_ALPHA_MASK_PROGRAM);
    glUniform1i(program_uniform_location(GRAPHICS_ALPHA_MASK_PROGRAM, "image"), GRAPHICS_UNIT);
    color_vec3(program_uniform_location(GRAPHICS_ALPHA_MASK_PROGRAM, "amask_fg"), OPT(foreground));
    color_vec4_premult(program_uniform_location(GRAPHICS_ALPHA_MASK_PROGRAM, "amask_bg_premult"), OPT(background), background_opacity);
    draw_graphics(GRAPHICS_ALPHA_MASK_PROGRAM, data, 0, 1, 1.0, 1.0);
}

static void
draw_resizing_text(OSWindow *w) {
    if (monotonic() - w->created_at > ms_to_monotonic_t(1000) && w->live_resize.num_of_resize_events > 1) {
        char text[32] = {0};
        unsigned int width = w->live_resize.width, height = w->live_resize.height;
        snprintf(text, sizeof(text), "%u by %u cells", width / w->fonts_data->fcm.cell_width, height / w->fonts_data->fcm.cell_height);
        StringCanvas rendered = render_simple_text(w->fonts_data, text);
        if (rendered.canvas) {
            draw_centered_alpha_mask(width, height, rendered.width, rendered.height, rendered.canvas, MAX(0.2f, MIN(OPT(background_opacity), 0.8f)));
            free(rendered.canvas);
        }
    }
}

// An sRGB color as the values that show the same color in the window, which
// are other values when the window is in a wide gamut
static color_type
color_in_gamut_of_window(const OSWindow *w, color_type color) {
    static const float from_srgb[][9] = {
        [GAMUT_DISPLAY_P3] = {0.8224621f, 0.1775380f, 0.0000000f, 0.0331941f, 0.9668058f, 0.0000000f, 0.0170827f, 0.0723974f, 0.9105199f},
        [GAMUT_REC2020] = {0.6274039f, 0.3292830f, 0.0433131f, 0.0690973f, 0.9195404f, 0.0113623f, 0.0163914f, 0.0880133f, 0.8955953f},
    };
    if (!w->wide_gamut || w->wide_gamut >= arraysz(from_srgb)) return color;
    const float *m = from_srgb[w->wide_gamut], lin[3] = {srgb_lut[(color >> 16) & 0xFF], srgb_lut[(color >> 8) & 0xFF], srgb_lut[color & 0xFF]};
    color_type ans = 0;
    for (int i = 0; i < 3; i++) {
        const float c = MAX(0.f, MIN(m[3 * i] * lin[0] + m[3 * i + 1] * lin[1] + m[3 * i + 2] * lin[2], 1.f));
        const float encoded = c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
        ans = (ans << 8) | (color_type)lrintf(encoded * 255.f);
    }
    return ans;
}

void
scale_output_to_hdr_white_level(const OSWindow *w) {
    // Everything is drawn with SDR white at 1.0. On a system that wants it elsewhere in the framebuffer of an HDR
    // window, see glfwSetWindowHDRRange(), the finished frame is scaled just before it is presented, by
    // multiplying what is in the framebuffer with the color of a quad that covers it
    if (!w->linear_output || !(w->hdr_white_level > 0.f) || w->hdr_white_level == 1.f) return;
    set_framebuffer_to_use_for_output(0);
    bind_framebuffer_for_output(0);
    disable_scissor();
    if (shader_globals_vao_idx != -1) bind_vertex_array(shader_globals_vao_idx);
    bind_program(TINT_PROGRAM);
    glUniform4f(program_uniform_location(TINT_PROGRAM, "tint_color"), w->hdr_white_level, w->hdr_white_level, w->hdr_white_level, 1.f);
    glUniform4f(program_uniform_location(TINT_PROGRAM, "edges"), -1, 1, 1, -1);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void
blank_os_window(OSWindow *osw) {
    color_type color = os_window_background(osw);
    if (osw->num_tabs > 0) {
        Tab *t = osw->tabs + osw->active_tab;
        if (t->num_windows == 1) {
            Window *w = t->windows + t->active_window;
            Screen *s = w->render_data.screen;
            if (s) { color = colorprofile_to_color(s->color_profile, s->color_profile->overridden.default_bg, s->color_profile->configured.default_bg).rgb; }
        }
    }
    blank_canvas(effective_os_window_alpha(osw), color_in_gamut_of_window(osw, color), !osw->linear_output);
}

// An HDR window is drawn in linear light that reaches above one, and a window
// in a wide gamut in linear sRGB that reaches below zero and above one
static bool
layers_are_float(const OSWindow *os_window) {
    return os_window->linear_output || os_window->wide_gamut != 0;
}

static void
start_os_window_rendering(OSWindow *os_window, Tab *tab) {
    spatial_window = os_window;
    current_output_gamut = output_gamut(os_window);
    // note that during live resize rendering is done in layers
    if (os_window->live_resize.in_progress && os_window->spatial.eye != 1) blank_os_window(os_window);
    if (os_window->needs_layers) {
        // Ensure the global shared texture is large enough for this window
        if (global_state.layers_render_texture.width < os_window->viewport_width || global_state.layers_render_texture.height < os_window->viewport_height ||
            global_state.layers_render_texture.is_float != layers_are_float(os_window)) {
            if (global_state.layers_render_texture.texture_id) free_texture(&global_state.layers_render_texture.texture_id);
            if (global_state.layers_render_texture.framebuffer_id) free_framebuffer(&global_state.layers_render_texture.framebuffer_id);
            if (global_state.layers_render_texture.extra_texture_id) free_texture(&global_state.layers_render_texture.extra_texture_id);
            if (global_state.layers_render_texture.extra_texture_setup_fbo_id) free_framebuffer(&global_state.layers_render_texture.extra_texture_setup_fbo_id);
            if (global_state.layers_render_texture.texture_a_id) free_texture(&global_state.layers_render_texture.texture_a_id);
            if (global_state.layers_render_texture.texture_a_fbo_id) free_framebuffer(&global_state.layers_render_texture.texture_a_fbo_id);
            if (global_state.layers_render_texture.texture_b_id) free_texture(&global_state.layers_render_texture.texture_b_id);
            if (global_state.layers_render_texture.texture_b_fbo_id) free_framebuffer(&global_state.layers_render_texture.texture_b_fbo_id);
            unsigned new_w = (unsigned)MAX(global_state.layers_render_texture.width, os_window->viewport_width);
            unsigned new_h = (unsigned)MAX(global_state.layers_render_texture.height, os_window->viewport_height);
            setup_texture_as_render_target(
                new_w, new_h, &global_state.layers_render_texture.texture_id, &global_state.layers_render_texture.framebuffer_id, layers_are_float(os_window));
            global_state.layers_render_texture.is_float = layers_are_float(os_window);
            global_state.layers_render_texture.width = (int)new_w;
            global_state.layers_render_texture.height = (int)new_h;
            global_state.layers_render_texture.texture_generation++;
        }
        // Create per-window framebuffer if needed and attach the global texture to it
        if (!os_window->indirect_output.framebuffer_id) glGenFramebuffers(1, &os_window->indirect_output.framebuffer_id);
        if (os_window->indirect_output.attached_texture_generation != global_state.layers_render_texture.texture_generation) {
            bind_framebuffer_for_output(os_window->indirect_output.framebuffer_id);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, global_state.layers_render_texture.texture_id, 0);
            os_window->indirect_output.attached_texture_generation = global_state.layers_render_texture.texture_generation;
        }
        if (custom_shaders.end.num_groups > 1) {
            // Lazily create the global shared extra texture (ping-pong target for multi-group end shaders)
            if (!global_state.layers_render_texture.extra_texture_id) {
                setup_texture_as_render_target(
                    (unsigned)global_state.layers_render_texture.width,
                    (unsigned)global_state.layers_render_texture.height,
                    &global_state.layers_render_texture.extra_texture_id,
                    &global_state.layers_render_texture.extra_texture_setup_fbo_id,
                    layers_are_float(os_window));
            }
            // Create/re-attach per-window extra FBO when generation changes
            if (os_window->indirect_output.extra_fbo_generation != global_state.layers_render_texture.texture_generation) {
                if (os_window->indirect_output.extra_fbo_id) free_framebuffer(&os_window->indirect_output.extra_fbo_id);
                glGenFramebuffers(1, &os_window->indirect_output.extra_fbo_id);
                bind_framebuffer_for_output(os_window->indirect_output.extra_fbo_id);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, global_state.layers_render_texture.extra_texture_id, 0);
                os_window->indirect_output.extra_fbo_generation = global_state.layers_render_texture.texture_generation;
            }
        }
        // Named texture A: global shared, per-window FBO
        if (custom_shaders.end.textures & (1u << CUSTOM_SHADER_TEXTURE_A)) {
            if (!global_state.layers_render_texture.texture_a_id) {
                setup_texture_as_render_target(
                    (unsigned)global_state.layers_render_texture.width,
                    (unsigned)global_state.layers_render_texture.height,
                    &global_state.layers_render_texture.texture_a_id,
                    &global_state.layers_render_texture.texture_a_fbo_id,
                    layers_are_float(os_window));
            }
            if (os_window->indirect_output.fbo_a_generation != global_state.layers_render_texture.texture_generation) {
                if (os_window->indirect_output.fbo_a_id) free_framebuffer(&os_window->indirect_output.fbo_a_id);
                glGenFramebuffers(1, &os_window->indirect_output.fbo_a_id);
                bind_framebuffer_for_output(os_window->indirect_output.fbo_a_id);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, global_state.layers_render_texture.texture_a_id, 0);
                os_window->indirect_output.fbo_a_generation = global_state.layers_render_texture.texture_generation;
            }
        } else {
            if (os_window->indirect_output.fbo_a_id) free_framebuffer(&os_window->indirect_output.fbo_a_id);
        }
        // Named texture B: global shared, per-window FBO
        if (custom_shaders.end.textures & (1u << CUSTOM_SHADER_TEXTURE_B)) {
            if (!global_state.layers_render_texture.texture_b_id) {
                setup_texture_as_render_target(
                    (unsigned)global_state.layers_render_texture.width,
                    (unsigned)global_state.layers_render_texture.height,
                    &global_state.layers_render_texture.texture_b_id,
                    &global_state.layers_render_texture.texture_b_fbo_id,
                    layers_are_float(os_window));
            }
            if (os_window->indirect_output.fbo_b_generation != global_state.layers_render_texture.texture_generation) {
                if (os_window->indirect_output.fbo_b_id) free_framebuffer(&os_window->indirect_output.fbo_b_id);
                glGenFramebuffers(1, &os_window->indirect_output.fbo_b_id);
                bind_framebuffer_for_output(os_window->indirect_output.fbo_b_id);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, global_state.layers_render_texture.texture_b_id, 0);
                os_window->indirect_output.fbo_b_generation = global_state.layers_render_texture.texture_generation;
            }
        } else {
            if (os_window->indirect_output.fbo_b_id) free_framebuffer(&os_window->indirect_output.fbo_b_id);
        }
        // Named texture persist: per-window, recreated when global texture dimensions change
        if (custom_shaders.end.textures & (1u << CUSTOM_SHADER_TEXTURE_PERSIST)) {
            if (os_window->persist_texture_generation != global_state.layers_render_texture.texture_generation) {
                if (os_window->persist_texture_id) free_texture(&os_window->persist_texture_id);
                if (os_window->persist_fbo_id) free_framebuffer(&os_window->persist_fbo_id);
                setup_texture_as_render_target(
                    (unsigned)global_state.layers_render_texture.width,
                    (unsigned)global_state.layers_render_texture.height,
                    &os_window->persist_texture_id,
                    &os_window->persist_fbo_id,
                    layers_are_float(os_window));
                os_window->persist_texture_generation = global_state.layers_render_texture.texture_generation;
            }
        } else {
            if (os_window->persist_texture_id) {
                free_texture(&os_window->persist_texture_id);
                os_window->persist_texture_generation = 0;
            }
            if (os_window->persist_fbo_id) free_framebuffer(&os_window->persist_fbo_id);
        }
        set_framebuffer_to_use_for_output(os_window->indirect_output.framebuffer_id);
        bind_framebuffer_for_output(0);
        save_viewport_using_bottom_left_origin(0, 0, os_window->viewport_width, os_window->viewport_height);
        clear_current_framebuffer();
        draw_bg_image(os_window, tab);
    }
}

// Bilinear lookup of the position u, v (viewport UV, clamped to [0, 1]) in a
// pointer map, replacing it with the position of the content shown there
static void
sample_pointer_map(const float *data, double *u, double *v) {
    const double last = POINTER_MAP_SIZE - 1;
    const double fx = MAX(0., MIN(*u, 1.)) * last, fy = MAX(0., MIN(*v, 1.)) * last;
    const unsigned x0 = MIN((unsigned)fx, POINTER_MAP_SIZE - 2), y0 = MIN((unsigned)fy, POINTER_MAP_SIZE - 2);
    const double tx = fx - x0, ty = fy - y0;
#define P(x, y, c) ((double)data[2 * ((y) * POINTER_MAP_SIZE + (x)) + (c)])
#define L(c) ((P(x0, y0, c) * (1 - tx) + P(x0 + 1, y0, c) * tx) * (1 - ty) + (P(x0, y0 + 1, c) * (1 - tx) + P(x0 + 1, y0 + 1, c) * tx) * ty)
    *u = L(0);
    *v = L(1);
#undef L
#undef P
}

// The groups whose pointer_map() functions currently apply: those that are
// always active and those turned on by events that are active right now. They
// are written to the group_words words of the pipeline's group_scratch, which
// is returned. *any is set to whether there are any.
static const uint32_t *
active_pointer_map_groups(const OSWindow *os_window, bool *any) {
    const CustomShaderPipeline *p = &custom_shaders.end;
    uint32_t *ans = p->group_scratch;
    *any = false;
    if (!ans) return NULL;
    zero_at_ptr_count(ans, p->group_words);
    if (!has_group_anim_state(os_window)) return ans;
    for (size_t g = 0; g < p->num_groups; g++) {
        if (!p->groups[g].uses_pointer_map) continue;
        if (!group_is_animated(p->groups + g) || os_window->shader_group_anim[g].active) {
            ans[g / 32] |= 1u << (g % 32);
            *any = true;
        }
    }
    return ans;
}

// Whether the pointer map of the OS window was computed from these groups
static bool
pointer_map_is_for_groups(const OSWindow *os_window, const uint32_t *groups, bool any) {
    if (!any || !os_window->pointer_map.has_groups) return any == os_window->pointer_map.has_groups;
    return os_window->pointer_map.groups.num_words >= custom_shaders.end.group_words &&
           memcmp(os_window->pointer_map.groups.words, groups, custom_shaders.end.group_words * sizeof(uint32_t)) == 0;
}

// Moves a pointer position, in viewport pixels with the origin at the top left,
// to the position of the content that custom shaders such as crt actually show
// there, so that clicks and selections land on what the user sees.
void
map_pointer_position(const OSWindow *os_window, double *x, double *y) {
    const float *data = os_window->pointer_map.data;
    if (!data || !custom_shaders.end.has_pointer_map || os_window->pointer_map.pipeline_generation != custom_shaders.generation) return;
    // The map is updated only when the custom shaders are drawn, which they
    // are not once every group has been turned off, so check that it is
    // still for the groups that are active
    bool any;
    const uint32_t *groups = active_pointer_map_groups(os_window, &any);
    if (!any || !pointer_map_is_for_groups(os_window, groups, any)) return;
    const double width = os_window->viewport_width, height = os_window->viewport_height;
    if (width <= 0 || height <= 0) return;
    const double u = *x / width, v = 1. - *y / height;
    const double cu = MAX(0., MIN(u, 1.)), cv = MAX(0., MIN(v, 1.));
    double mu = cu, mv = cv;
    sample_pointer_map(data, &mu, &mv);
    // Positions outside the viewport, such as when drag selecting past its
    // edge, move along with the nearest edge
    *x = (mu + u - cu) * width;
    *y = (1. - (mv + v - cv)) * height;
}

// Renders the pointer_map() functions of the pipeline into a small texture
// and reads it back so that pointer events can be mapped without the GPU. A
// pointer map can depend only on the pipeline, the viewport size and which
// groups are active, so this is done only when those change.
static void
update_pointer_map(OSWindow *os_window, GLint group_loc) {
    bool any;
    const uint32_t *groups = active_pointer_map_groups(os_window, &any);
    if (!groups) return;
    if (os_window->pointer_map.pipeline_generation == custom_shaders.generation && pointer_map_is_for_groups(os_window, groups, any) &&
        (os_window->pointer_map.data || !any)) {
        if (os_window->live_resize.in_progress) return; // avoid a GPU sync on every frame of a live resize
        if (os_window->pointer_map.viewport_width == os_window->viewport_width && os_window->pointer_map.viewport_height == os_window->viewport_height) return;
    }
    os_window->pointer_map.viewport_width = os_window->viewport_width;
    os_window->pointer_map.viewport_height = os_window->viewport_height;
    os_window->pointer_map.pipeline_generation = custom_shaders.generation;
    os_window->pointer_map.has_groups = false;
    // No pointer_map() applies, such as when content moving groups are toggled off, so pointer events are not moved
    if (!any) return;
    if (!os_window->pointer_map.data && !(os_window->pointer_map.data = malloc(sizeof(float) * 2 * POINTER_MAP_SIZE * POINTER_MAP_SIZE))) return;
    if (!ensure_bitset_holds(&os_window->pointer_map.groups, custom_shaders.end.group_words * 32 - 1)) return;
    if (!pointer_map_target.framebuffer_id)
        setup_texture_as_render_target(POINTER_MAP_SIZE, POINTER_MAP_SIZE, &pointer_map_target.texture_id, &pointer_map_target.framebuffer_id, false);
    // The pointer map does not sample any textures, but every sampler must be bound to a valid one
    static const int units[] = {GRAPHICS_UNIT, CUSTOM_END_TEXTURE_A_UNIT, CUSTOM_END_TEXTURE_B_UNIT, CUSTOM_END_TEXTURE_PERSIST_UNIT};
    for (size_t i = 0; i < arraysz(units); i++) {
        glActiveTexture(GL_TEXTURE0 + units[i]);
        glBindTexture(GL_TEXTURE_2D, global_state.layers_render_texture.texture_id);
    }
    bind_framebuffer_for_output(pointer_map_target.framebuffer_id);
    glViewport(0, 0, POINTER_MAP_SIZE, POINTER_MAP_SIZE);
    glUniform1i(group_loc, -1);
    // Absent when no group with a pointer_map() is turned on and off by events
    GLint active_groups_loc = try_program_uniform_location(CUSTOM_END_PROGRAM, "active_groups");
    if (active_groups_loc >= 0) glUniform1uiv(active_groups_loc, (GLsizei)custom_shaders.end.group_words, groups);
    draw_quad(false, 0);
    static uint8_t pixels[POINTER_MAP_SIZE * POINTER_MAP_SIZE * 4];
    glReadPixels(0, 0, POINTER_MAP_SIZE, POINTER_MAP_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    // See encode_pointer_map() in pipeline.slang
    for (size_t i = 0; i < POINTER_MAP_SIZE * POINTER_MAP_SIZE; i++) {
        const uint8_t *p = pixels + 4 * i;
        os_window->pointer_map.data[2 * i] = (float)(p[0] * 256 + p[2]) / 65535.f;
        os_window->pointer_map.data[2 * i + 1] = (float)(p[1] * 256 + p[3]) / 65535.f;
    }
    memcpy(os_window->pointer_map.groups.words, groups, custom_shaders.end.group_words * sizeof(uint32_t));
    os_window->pointer_map.has_groups = true;
}

static void
free_pointer_map_target(void) {
    if (pointer_map_target.texture_id) free_texture(&pointer_map_target.texture_id);
    if (pointer_map_target.framebuffer_id) free_framebuffer(&pointer_map_target.framebuffer_id);
}

static void
run_custom_end_shader(OSWindow *os_window, float sx, float sy, monotonic_t now) {
    bind_program(CUSTOM_END_PROGRAM);
    struct GPUCustomEndData {
        float src_rect[4];
        float dest_rect[4];
        float background[4];
        float foreground[4];
        float active_window_background[4];
        float mouse_pos[4];
        float mouse_button_pressed[4];
        float active_window_geometry[4];
        float active_window_padding[4];
        float bell_window_geometry[4];
        float central_area[4];
        float cursor_trail_corners_x[4];
        float cursor_trail_corners_y[4];
        float cursor_trail_prev_corners_x[4];
        float cursor_trail_prev_corners_y[4];
        float cursor_trail_edge[4];
        float cursor_trail_prev_edge[4];
        float cursor_color[4];
        float cursor_trail_color[4];
        float cursor_trail_history_from[CURSOR_TRAIL_HISTORY_SIZE][4];
        float cursor_trail_history_to[CURSOR_TRAIL_HISTORY_SIZE][4];
        float cursor_trail_history_age[CURSOR_TRAIL_HISTORY_SIZE / 4][4];
        uint32_t viewport_size_pixels[2];
        float mouse_pointer_hidden;
        float cursor_trail_state;
        float timestamp, last_rendered_at;
        uint32_t frame_counter;
        float cursor_trail_change_time;
        uint32_t timestamp_seconds;
        float timestamp_fraction;
        float time_since_last_render;
        float cursor_trail_age;
        float active_window_alternate_screen;
        float active_window_command_running;
        float active_window_alternate_screen_age;
        float active_window_command_state_age;
        float pixels_per_pt;
    };
    // This struct is a hand written mirror of the std140 layout of
    // KittyCustomShaderData in types.slang, so check that they match once.
    const UniformBlock data_block = program_uniform_block(CUSTOM_END_PROGRAM, "KittyCustomShaderData");
    static bool data_block_size_checked = false;
    if (!data_block_size_checked) {
        data_block_size_checked = true;
        if ((size_t)data_block.size < sizeof(struct GPUCustomEndData))
            fatal(
                "The KittyCustomShaderData uniform block is %d bytes but the C struct mirroring it is %zu bytes",
                data_block.size,
                sizeof(struct GPUCustomEndData));
    }
    struct GPUCustomEndData *d = (struct GPUCustomEndData *)map_vao_buffer_for_write_only(custom_end_vao_idx, 0, 0, data_block.size);
    memset(d, 0, sizeof(struct GPUCustomEndData));
    d->src_rect[1] = sy;
    d->src_rect[2] = sx;
    d->dest_rect[0] = -1;
    d->dest_rect[1] = 1;
    d->dest_rect[2] = 1;
    d->dest_rect[3] = -1;
#define FILL_COLOR(dst, c)                         \
    do {                                           \
        (dst)[0] = srgb_color(((c) >> 16) & 0xFF); \
        (dst)[1] = srgb_color(((c) >> 8) & 0xFF);  \
        (dst)[2] = srgb_color((c) & 0xFF);         \
        (dst)[3] = 1.f;                            \
    } while (0)
    FILL_COLOR(d->background, os_window_background(os_window));
    FILL_COLOR(d->foreground, os_window_foreground(os_window));
    color_type active_bg = os_window_background(os_window);
    color_type cursor_color = os_window_foreground(os_window);
    float cursor_opacity = 0.f;
    WindowGeometry active_win_geom = {0};
    d->active_window_alternate_screen_age = -1.f;
    d->active_window_command_state_age = -1.f;
    // The same conversion as for kitty's options in pts, such as font_size
    d->pixels_per_pt = (float)(dpi_for_os_window(os_window) / 72.0);
    if (os_window->num_tabs > 0) {
        Tab *t = os_window->tabs + os_window->active_tab;
        if (t->num_windows) {
            Window *w = t->windows + t->active_window;
            active_win_geom = w->render_data.geometry;
            if (os_window->viewport_width && os_window->viewport_height) {
                const float width = (float)os_window->viewport_width, height = (float)os_window->viewport_height;
                // spaces is all the space the layout reserved around the window:
                // margin, border (only if the layout reserves it), padding and
                // cell-size compensation.
                const WindowGeometry *sg = &w->render_data.geometry;
                d->active_window_padding[0] = (float)sg->spaces.left / width;
                d->active_window_padding[1] = (float)sg->spaces.bottom / height;
                d->active_window_padding[2] = (float)sg->spaces.right / width;
                d->active_window_padding[3] = (float)sg->spaces.top / height;
                // The content geometry excludes the title bar. Include it before
                // extending to the outer padding, so the effect boundary cannot
                // cut through a title bar at either edge of the window.
                const WindowRenderData *trd = &w->window_title_render_data;
                if (trd->screen && trd->geometry.right > trd->geometry.left && trd->geometry.bottom > trd->geometry.top) {
                    if (trd->geometry.top < active_win_geom.top) d->active_window_padding[3] += (float)(active_win_geom.top - trd->geometry.top) / height;
                    if (trd->geometry.bottom > active_win_geom.bottom)
                        d->active_window_padding[1] += (float)(trd->geometry.bottom - active_win_geom.bottom) / height;
                }
            }
            Screen *s = w->render_data.screen;
            if (s) {
                active_bg = colorprofile_to_color(s->color_profile, s->color_profile->overridden.default_bg, s->color_profile->configured.default_bg).rgb;
                cursor_color = s->last_rendered.cursor_bg;
                cursor_opacity = MAX(0.f, MIN(s->cursor_render_info.cursor_opacity, 1.f));
                d->active_window_alternate_screen = s->linebuf == s->alt_linebuf ? 1.f : 0.f;
                d->active_window_command_running = s->command_state.running ? 1.f : 0.f;
                const monotonic_t alt_changed_at = MAX(s->alternate_screen.entered_at, s->alternate_screen.exited_at);
                if (alt_changed_at) d->active_window_alternate_screen_age = (float)monotonic_t_to_s_double(now - alt_changed_at);
                const monotonic_t cmd_changed_at = MAX(s->command_state.started_at, s->command_state.finished_at);
                if (cmd_changed_at) d->active_window_command_state_age = (float)monotonic_t_to_s_double(now - cmd_changed_at);
            }
        }
    }
    FILL_COLOR(d->active_window_background, active_bg);
#undef FILL_COLOR
#define NDC_TO_UV(v) (((v) + 1.0f) * 0.5f)
    // Convert to seconds via double, as float32 cannot represent times since
    // kitty started precisely once kitty has been running for a few hours.
#define SECONDS(t) ((float)monotonic_t_to_s_double(t))
    {
        CursorTrail *ct = NULL;
        d->cursor_trail_age = -1.f;
        for (int i = 0; i < CURSOR_TRAIL_HISTORY_SIZE; i++) d->cursor_trail_history_age[i / 4][i % 4] = -1.f;
        if (os_window_has_cursor_trail(os_window) && os_window->num_tabs > 0) {
            CursorTrail *candidate = &os_window->tabs[os_window->active_tab].cursor_trail;
            if (candidate->needs_render) ct = candidate;
            // The history outlives the trail so that effects started by a move can finish
            for (int i = 0; i < CURSOR_TRAIL_HISTORY_SIZE; i++) {
                const CursorMove *m = candidate->history + i;
                if (!m->at) break;
                for (int k = 0; k < 2; k++) {
                    d->cursor_trail_history_from[i][k] = NDC_TO_UV(m->from_x[k]);
                    d->cursor_trail_history_from[i][k + 2] = NDC_TO_UV(m->from_y[k]);
                    d->cursor_trail_history_to[i][k] = NDC_TO_UV(m->to_x[k]);
                    d->cursor_trail_history_to[i][k + 2] = NDC_TO_UV(m->to_y[k]);
                }
                d->cursor_trail_history_age[i / 4][i % 4] = SECONDS(now - m->at);
            }
        }
        if (ct) {
            for (int i = 0; i < 4; i++) {
                d->cursor_trail_corners_x[i] = NDC_TO_UV(ct->corner_x[i]);
                d->cursor_trail_corners_y[i] = NDC_TO_UV(ct->corner_y[i]);
                d->cursor_trail_prev_corners_x[i] = NDC_TO_UV(ct->previous_corner_x[i]);
                d->cursor_trail_prev_corners_y[i] = NDC_TO_UV(ct->previous_corner_y[i]);
            }
            d->cursor_trail_edge[0] = NDC_TO_UV(ct->cursor_edge_x[0]); // left
            d->cursor_trail_edge[1] = NDC_TO_UV(ct->cursor_edge_x[1]); // right
            d->cursor_trail_edge[2] = NDC_TO_UV(ct->cursor_edge_y[0]); // top
            d->cursor_trail_edge[3] = NDC_TO_UV(ct->cursor_edge_y[1]); // bottom
            if (ct->prev_edge_valid) {
                d->cursor_trail_prev_edge[0] = NDC_TO_UV(ct->prev_cursor_edge_x[0]); // left
                d->cursor_trail_prev_edge[1] = NDC_TO_UV(ct->prev_cursor_edge_x[1]); // right
                d->cursor_trail_prev_edge[2] = NDC_TO_UV(ct->prev_cursor_edge_y[0]); // top
                d->cursor_trail_prev_edge[3] = NDC_TO_UV(ct->prev_cursor_edge_y[1]); // bottom
            }
            d->cursor_trail_color[3] = ct->opacity;
            d->cursor_trail_state = 1.0f;
            if (ct->cursor_changed_at) {
                d->cursor_trail_change_time = SECONDS(ct->cursor_changed_at);
                d->cursor_trail_age = SECONDS(now - ct->cursor_changed_at);
            }
        }
    }
#undef NDC_TO_UV
#define FILL_COLOR3(dst, c)                        \
    do {                                           \
        (dst)[0] = srgb_color(((c) >> 16) & 0xFF); \
        (dst)[1] = srgb_color(((c) >> 8) & 0xFF);  \
        (dst)[2] = srgb_color((c) & 0xFF);         \
    } while (0)
    FILL_COLOR3(d->cursor_color, cursor_color);
    d->cursor_color[3] = cursor_opacity;
    {
        color_type trail_color = OPT(cursor_trail_color);
        if (trail_color == 0) trail_color = cursor_color;
        FILL_COLOR3(d->cursor_trail_color, trail_color);
    }
#undef FILL_COLOR3
    d->viewport_size_pixels[0] = (uint32_t)os_window->viewport_width;
    d->viewport_size_pixels[1] = (uint32_t)os_window->viewport_height;
    const float vpw = (float)os_window->viewport_width, vph = (float)os_window->viewport_height;
    if (vpw > 0 && vph > 0) {
        d->mouse_pos[0] = (float)os_window->mouse_x / vpw;
        d->mouse_pos[1] = 1.f - (float)os_window->mouse_y / vph;
        d->mouse_pos[2] = (float)os_window->mouse_left_press_x / vpw;
        d->mouse_pos[3] = 1.f - (float)os_window->mouse_left_press_y / vph;
    }
    d->mouse_button_pressed[0] = os_window->mouse_button_pressed[GLFW_MOUSE_BUTTON_LEFT];
    d->mouse_button_pressed[1] = os_window->mouse_button_pressed[GLFW_MOUSE_BUTTON_RIGHT];
    d->mouse_button_pressed[2] = os_window->mouse_button_pressed[GLFW_MOUSE_BUTTON_MIDDLE];
    d->mouse_button_pressed[3] = os_window->mouse_button_pressed[3];
    d->mouse_pointer_hidden = is_mouse_hidden(os_window) ? 1.f : 0.f;
    d->timestamp = SECONDS(now);
    d->last_rendered_at = SECONDS(os_window->last_rendered_at);
    d->timestamp_seconds = (uint32_t)(now / MONOTONIC_T_1e9);
    d->timestamp_fraction = (float)monotonic_t_to_s_double(now % MONOTONIC_T_1e9);
    if (os_window->last_rendered_at) d->time_since_last_render = SECONDS(now - os_window->last_rendered_at);
#undef SECONDS
    d->frame_counter = os_window->frame_counter;
    if (vpw > 0 && vph > 0 && active_win_geom.right > active_win_geom.left) {
        d->active_window_geometry[0] = (float)active_win_geom.left / vpw;
        d->active_window_geometry[1] = 1.f - (float)active_win_geom.bottom / vph;
        d->active_window_geometry[2] = (float)(active_win_geom.right - active_win_geom.left) / vpw;
        d->active_window_geometry[3] = (float)(active_win_geom.bottom - active_win_geom.top) / vph;
    }
    if (vpw > 0 && vph > 0) {
        Region central_rgn = {0}, tab_bar_rgn = {0};
        os_window_regions(os_window, &central_rgn, &tab_bar_rgn);
        d->central_area[0] = (float)central_rgn.left / vpw;
        d->central_area[1] = 1.f - (float)central_rgn.bottom / vph;
        d->central_area[2] = (float)(central_rgn.right - central_rgn.left) / vpw;
        d->central_area[3] = (float)(central_rgn.bottom - central_rgn.top) / vph;
    } else {
        d->central_area[2] = 1.f;
        d->central_area[3] = 1.f;
    }
    if (os_window->last_bell_window_id && os_window->num_tabs > 0 && vpw > 0 && vph > 0) {
        WindowGeometry bell_win_geom = {0};
        bool bell_win_found = false;
        Tab *bt = os_window->tabs + os_window->active_tab;
        for (unsigned i = 0; i < bt->num_windows; i++) {
            Window *bw = bt->windows + i;
            if (bw->id == os_window->last_bell_window_id && bw->visible) {
                bell_win_geom = bw->render_data.geometry;
                bell_win_found = true;
                break;
            }
        }
        if (bell_win_found && bell_win_geom.right > bell_win_geom.left) {
            d->bell_window_geometry[0] = (float)bell_win_geom.left / vpw;
            d->bell_window_geometry[1] = 1.f - (float)bell_win_geom.bottom / vph;
            d->bell_window_geometry[2] = (float)(bell_win_geom.right - bell_win_geom.left) / vpw;
            d->bell_window_geometry[3] = (float)(bell_win_geom.bottom - bell_win_geom.top) / vph;
        } else {
            memcpy(d->bell_window_geometry, d->central_area, sizeof(d->bell_window_geometry));
        }
    }
    unmap_vao_buffer(custom_end_vao_idx, 0);
    bind_vao_uniform_buffer(custom_end_vao_idx, 0, CUSTOM_END_DATA_BINDING_POINT);

    // A custom shader need not reference any of these, in which case the driver
    // drops them and glUniform*() on the resulting -1 location is a no-op.
    GLint group_loc = try_program_uniform_location(CUSTOM_END_PROGRAM, "group");
    GLint viewport_loc = try_program_uniform_location(CUSTOM_END_PROGRAM, "viewport");
    GLint anim_progress_loc = try_program_uniform_location(CUSTOM_END_PROGRAM, "animation_progress");
    GLint convert_to_srgb_loc = try_program_uniform_location(CUSTOM_END_PROGRAM, "convert_to_srgb");
    GLint wide_gamut_loc = try_program_uniform_location(CUSTOM_END_PROGRAM, "wide_gamut");
    if (custom_shaders.end.has_pointer_map && group_loc >= 0) update_pointer_map(os_window, group_loc);
    GLint dither_levels_loc = try_program_uniform_location(CUSTOM_END_PROGRAM, "dither_levels");
    float dither_levels[3] = {0};
    output_dither_levels(os_window, dither_levels);
    static const float no_dither[3] = {0};
    const unsigned num_groups = (unsigned)custom_shaders.end.num_groups;
    const unsigned textures_mask = custom_shaders.end.textures;
    const int vw = os_window->viewport_width, vh = os_window->viewport_height;

    // Named texture IDs and their render-target FBOs, indexed by NamedTexture enum value
    const GLuint named_tex_ids[4] = {
        0, // DEFAULT — unused
        global_state.layers_render_texture.texture_a_id,
        global_state.layers_render_texture.texture_b_id,
        os_window->persist_texture_id,
    };
    const GLuint named_tex_fbos[4] = {
        0, // DEFAULT — unused
        os_window->indirect_output.fbo_a_id,
        os_window->indirect_output.fbo_b_id,
        os_window->persist_fbo_id,
    };
    static const int named_tex_units[4] = {
        GRAPHICS_UNIT, // DEFAULT maps to backbuffer slot
        CUSTOM_END_TEXTURE_A_UNIT,
        CUSTOM_END_TEXTURE_B_UNIT,
        CUSTOM_END_TEXTURE_PERSIST_UNIT,
    };

    // Pre-scan to find the last group that will actually run this frame.
    // That group gets convert_to_srgb=true so its draw call replaces the
    // blit pass; all other groups write premultiplied linear RGB and skip it.
    int last_active_g = -1;
    for (unsigned g = 0; g < num_groups; g++) {
        // Non-attached always-active groups always run; attached and animated groups run only when active.
        if (group_is_running(os_window, g)) last_active_g = (int)g;
    }

    // Ping-pong state: when true, backbuffer = texture_id and the ping-pong
    // target is extra_texture_id; toggled each time a group outputs to DEFAULT.
    bool backbuffer_is_main = true;

    float *anim_progress = custom_shaders.end.group_progress;
    custom_shader_group_progress(os_window, now, anim_progress);

    for (unsigned g = 0; g < num_groups; g++) {
        const CustomShaderGroup *cg = &custom_shaders.end.groups[g];
        const bool is_last = (last_active_g >= 0 && g == (unsigned)last_active_g);

        // Skip inactive groups. Non-attached, non-animated groups always run; all others require
        // shader_group_anim[g].active (set by update_custom_shader_animations, including attach logic).
        if (!group_is_running(os_window, g)) continue;
        const GLuint backbuffer = backbuffer_is_main ? global_state.layers_render_texture.texture_id : global_state.layers_render_texture.extra_texture_id;

        // Bind backbuffer to the backbuffer texture unit
        glActiveTexture(GL_TEXTURE0 + GRAPHICS_UNIT);
        glBindTexture(GL_TEXTURE_2D, backbuffer);

        // Bind each named texture to its own unit.
        // If the texture is the output for this group, or is not declared in the
        // pipeline, fall back to backbuffer so the sampler is always valid.
        for (int t = CUSTOM_SHADER_TEXTURE_A; t <= CUSTOM_SHADER_TEXTURE_PERSIST; t++) {
            GLuint tex;
            if (!is_last && cg->output_texture == (NamedTexture)t) {
                tex = backbuffer; // can't read from the render target we're writing to
            } else if (textures_mask & (1u << t)) {
                tex = named_tex_ids[t];
            } else {
                tex = backbuffer; // not present — fall back to backbuffer
            }
            glActiveTexture(GL_TEXTURE0 + named_tex_units[t]);
            glBindTexture(GL_TEXTURE_2D, tex);
        }

        // Set the output framebuffer
        if (is_last) {
            set_framebuffer_to_use_for_output(0);
            bind_framebuffer_for_output(0);
        } else if (cg->output_texture != CUSTOM_SHADER_TEXTURE_DEFAULT) {
            // Output to named texture; backbuffer is unchanged for the next group
            bind_framebuffer_for_output(named_tex_fbos[cg->output_texture]);
        } else {
            // Ping-pong: output to the other texture, which becomes the new backbuffer
            bind_framebuffer_for_output(backbuffer_is_main ? os_window->indirect_output.extra_fbo_id : os_window->indirect_output.framebuffer_id);
            backbuffer_is_main = !backbuffer_is_main;
        }

        // Set the viewport
        float vp_x, vp_y, vp_w, vp_h;
        if (is_last) {
            // Last group always covers the full screen region
            if (os_window->live_resize.in_progress) {
                glViewport(0, os_window->live_resize.height - vh, vw, vh);
            } else {
                glViewport(0, 0, vw, vh);
            }
            vp_x = 0.f;
            vp_y = 0.f;
            vp_w = 1.f;
            vp_h = 1.f;
        } else {
            float px = cg->viewport_pos.x, py = cg->viewport_pos.y;
            float sw = cg->viewport_size.w, sh = cg->viewport_size.h;
            // viewport_pos/size are standard UV (origin at bottom-left, y increasing
            // upward), which matches GL pixel coords directly — no y-flip needed.
            GLint x0 = MAX(0, (GLint)(px * vw));
            GLint y0 = MAX(0, (GLint)(py * vh));
            GLint x1 = MIN(vw, (GLint)((px + sw) * vw));
            GLint y1 = MIN(vh, (GLint)((py + sh) * vh));
            glViewport(x0, y0, MAX(0, x1 - x0), MAX(0, y1 - y0));
            vp_x = px;
            vp_y = py;
            vp_w = sw;
            vp_h = sh;
        }

        glUniform1i(group_loc, (GLint)g);
        glUniform4f(viewport_loc, vp_x, vp_y, vp_w, vp_h);
        glUniform1f(anim_progress_loc, anim_progress[g]);
        // The last group writes what the surface of the window holds, as the blit does for a window without custom
        // shaders: encoded, unless the window is an HDR one, and in the primaries of the window
        glUniform1i(convert_to_srgb_loc, is_last && !os_window->linear_output ? 1 : 0);
        glUniform1ui(wide_gamut_loc, is_last ? os_window->wide_gamut : 0);
        // and dithered to its steps, as the blit does
        if (dither_levels_loc >= 0) glUniform3fv(dither_levels_loc, 1, is_last ? dither_levels : no_dither);
        // debug_rendering("Custom shader draw group %u: anim_progress=%.4f viewport=(%.2f,%.2f,%.2f,%.2f)\n", g, anim_progress[g], vp_x, vp_y, vp_w, vp_h);
        draw_quad(false, 0);
    }
}

static void
stop_os_window_rendering(OSWindow *os_window, Tab *tab, Window *active_window, monotonic_t now) {
    if (os_window_has_cursor_trail(os_window) && tab->cursor_trail.needs_render && !(custom_shaders.end.active && custom_shaders.end.bypass_builtin_trail))
        draw_cursor_trail(&tab->cursor_trail, active_window);
    if (os_window->needs_layers) {
        float sx = global_state.layers_render_texture.width > 0 ? (float)os_window->viewport_width / (float)global_state.layers_render_texture.width : 1.f;
        float sy = global_state.layers_render_texture.height > 0 ? (float)os_window->viewport_height / (float)global_state.layers_render_texture.height : 1.f;
        if (!os_window->spatial.mode && custom_shaders.end.active && os_window->shader_anim.has_active_shaders && custom_end_vao_idx >= 0) {
            // debug_rendering("Custom end shader running (groups=%zu)\n", custom_shaders.end.num_groups);
            restore_viewport();
            if (os_window->live_resize.in_progress)
                save_viewport_using_top_left_origin(0, 0, os_window->viewport_width, os_window->viewport_height, os_window->live_resize.height);
            run_custom_end_shader(os_window, sx, sy, now);
        } else {
            // debug_rendering("Custom shaders inactive, falling back to blit\n");
            set_framebuffer_to_use_for_output(0);
            bind_framebuffer_for_output(0);
            bind_program(BLIT_PROGRAM);
            glUniform1ui(program_uniform_location(BLIT_PROGRAM, "output_gamut"), output_gamut(os_window));
            glActiveTexture(GL_TEXTURE0 + GRAPHICS_UNIT);
            glBindTexture(GL_TEXTURE_2D, global_state.layers_render_texture.texture_id);
            glUniform4f(program_uniform_location(BLIT_PROGRAM, "src_rect"), 0, sy, sx, 0);
            glUniform4f(program_uniform_location(BLIT_PROGRAM, "dest_rect"), -1, 1, 1, -1);
            glUniform1f(program_uniform_location(BLIT_PROGRAM, "encode_srgb"), os_window->linear_output ? 0.f : 1.f);
            float dither_levels[3] = {0};
            output_dither_levels(os_window, dither_levels);
            glUniform3fv(program_uniform_location(BLIT_PROGRAM, "dither_levels"), 1, dither_levels);
            restore_viewport();
            if (os_window->spatial.mode) {
                const bool red_blue = os_window->spatial.red_blue;
                const int width = red_blue ? os_window->spatial.output_width : os_window->spatial.output_width / 2;
                glViewport(!red_blue && os_window->spatial.eye > 0 ? width : 0, 0, width, os_window->spatial.output_height);
                glUniform1f(program_uniform_location(BLIT_PROGRAM, "spatial_anaglyph"), red_blue ? (os_window->spatial.eye < 0 ? 1.f : 2.f) : 0.f);
                if (red_blue) glColorMask(os_window->spatial.eye < 0, GL_FALSE, os_window->spatial.eye > 0, GL_TRUE);
                glUniform4f(program_uniform_location(BLIT_PROGRAM, "spatial_wall"), 1.f,
                    os_window->spatial.mode == 2 ? os_window->spatial.scale : 1.f,
                    tanf(os_window->spatial.fov * (float)(M_PI / 360.)), (float)width / os_window->spatial.output_height);
                glUniformMatrix3fv(program_uniform_location(BLIT_PROGRAM, "spatial_rotation"), 1, GL_FALSE, os_window->spatial.rotation);
            } else {
                glUniform4f(program_uniform_location(BLIT_PROGRAM, "spatial_wall"), 0, 1, 1, 1);
                glUniform1f(program_uniform_location(BLIT_PROGRAM, "spatial_anaglyph"), 0.f);
            }
            if (os_window->live_resize.in_progress)
                save_viewport_using_top_left_origin(0, 0, os_window->viewport_width, os_window->viewport_height, os_window->live_resize.height);
            draw_quad(false, 0);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glUniform4f(program_uniform_location(BLIT_PROGRAM, "spatial_wall"), 0, 1, 1, 1);
            glUniform1f(program_uniform_location(BLIT_PROGRAM, "spatial_anaglyph"), 0.f);
            if (os_window->spatial.mode) set_gpu_viewport(os_window->viewport_width, os_window->viewport_height);
        }
        if (os_window->live_resize.in_progress) {
            restore_viewport();
            draw_resizing_text(os_window);
        }
    }
    os_window->last_rendered_at = now;
    if (os_window->spatial.eye != -1) os_window->frame_counter++;
}

void
setup_os_window_for_rendering(OSWindow *os_window, Tab *tab, Window *active_window, bool start, monotonic_t now) {
    if (start) start_os_window_rendering(os_window, tab);
    else {
        stop_os_window_rendering(os_window, tab, active_window, now);
        spatial_window = NULL;
    }
}

// Take a screenshot of the OS Window, must be called immediately after
// the OSWindow is rendered into the back buffer and before the buffers
// are swapped. If thumb_w or thumb_h are zero the are set to the corresponding
// dimension of the source region (viewport or central region without tab bar).
// Takes a screenshot of a rectangular region of the OSWindow's framebuffer.
// The region parameter specifies which part of the framebuffer to capture.
// Scaling is performed on the GPU using the SCREENSHOT_PROGRAM shader for better performance.
// The shader properly handles sRGB color space conversion and downscaling.
// Setting the thumbnail dimensions to zero disables scaling.
// If no_scaling is true, thumb_w/thumb_h are ignored and the region is read
// back directly from the framebuffer with no GPU resampling, for a pixel
// perfect capture (the output is in GL's native bottom-up row order, unlike
// the scaled path below which flips to top-down).
void
take_screenshot_of_rectangular_region(OSWindow *os_window, Region region, unsigned char *dst_buf, unsigned *thumb_w, unsigned *thumb_h, bool no_scaling) {
    unsigned vh = os_window->spatial.mode ? os_window->spatial.output_height : os_window->viewport_height;

    // Calculate the source region dimensions
    unsigned src_height = region.bottom - region.top;
    unsigned src_width = region.right - region.left;
    const bool is_float = backbuffer_is_float();

    if (global_state.debug_rendering && src_width && src_height) {
        // A screenshot holds 8 bits per channel and nothing brighter than SDR white, so say what the frame it is
        // taken of holds: how bright it gets and how many of its color values lie between the steps of 8 bits,
        // after the sRGB encoding a floating point frame does not have yet
        const bool fits = (size_t)src_width <= SIZE_MAX / (4 * sizeof(float)) / src_height;
        const size_t count = fits ? (size_t)src_width * src_height * 4 : 0;
        float *buf = fits ? malloc(count * sizeof(float)) : NULL;
        if (buf) {
            float peak = 0;
            size_t between = 0;
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels((GLint)region.left, (GLint)(vh - region.bottom), (GLsizei)src_width, (GLsizei)src_height, GL_RGBA, GL_FLOAT, buf);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            for (size_t i = 0; i < count; i++) {
                if (i % 4 == 3) continue;
                if (buf[i] > peak) peak = buf[i];
                float v = MAX(0.f, MIN(buf[i], 1.f));
                if (is_float) v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
                if (fabsf(v * 255.f - roundf(v * 255.f)) > 1.f / 16.f) between++;
            }
            free(buf);
            if (is_float) debug_rendering("Screenshot of a floating point frame, the brightest value in it is %g times SDR white\n", peak);
            debug_rendering("Screenshot: %zu of the %zu color values of the frame lie between the steps of 8 bits\n", between, count / 4 * 3);
        } else debug_rendering("Screenshot: no memory to read the frame back at full precision\n");
    }

    if (no_scaling) {
        *thumb_w = src_width;
        *thumb_h = src_height;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (is_float) {
            const size_t count = (size_t)src_width * src_height * 4;
            float *buf = malloc(count * sizeof(float));
            if (buf) {
                glReadPixels((GLint)region.left, (GLint)(vh - region.bottom), (GLsizei)src_width, (GLsizei)src_height, GL_RGBA, GL_FLOAT, buf);
                for (size_t i = 0; i < count; i++) {
                    float v = MAX(0.f, MIN(buf[i], 1.f));
                    if (i % 4 != 3) v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
                    dst_buf[i] = (unsigned char)(v * 255.f + 0.5f);
                }
                free(buf);
            }
        } else glReadPixels((GLint)region.left, (GLint)(vh - region.bottom), (GLsizei)src_width, (GLsizei)src_height, GL_RGBA, GL_UNSIGNED_BYTE, dst_buf);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        return;
    }

    unsigned vw = os_window->spatial.mode ? os_window->spatial.output_width : os_window->viewport_width;
    if (!*thumb_w) *thumb_w = src_width;
    if (!*thumb_h) *thumb_h = src_height;
    *thumb_w = MIN(src_width, *thumb_w);
    *thumb_h = MIN(src_height, *thumb_h);

    // Create a texture to hold the current framebuffer content
    GLuint src_texture = 0;
    glGenTextures(1, &src_texture);
    glBindTexture(GL_TEXTURE_2D, src_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Copy the current framebuffer to the texture
    glCopyTexImage2D(GL_TEXTURE_2D, 0, is_float ? GL_RGBA16F : GL_RGBA, 0, 0, vw, vh, 0);

    // Create a temporary framebuffer for GPU-based scaling
    GLuint temp_texture = 0, temp_framebuffer = 0;
    setup_texture_as_render_target(*thumb_w, *thumb_h, &temp_texture, &temp_framebuffer, false);

    // Save current state
    GLint current_framebuffer;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_framebuffer);

    // Bind our temporary framebuffer for rendering
    bind_framebuffer_for_output(temp_framebuffer);
    save_viewport_using_bottom_left_origin(0, 0, *thumb_w, *thumb_h);

    // Use the screenshot program to render the scaled framebuffer with proper color space handling
    bind_program(SCREENSHOT_PROGRAM);

    // Set source rectangle (normalized coordinates: 0 to 1)
    // Note: OpenGL texture origin is bottom-left, but Region uses top-left origin
    // Convert from screen coordinates (top-left origin) to OpenGL texture coordinates (bottom-left origin)
    float src_left_norm = (float)region.left / (float)vw;
    float src_right_norm = (float)region.right / (float)vw;
    float src_bottom_norm = (float)(vh - region.bottom) / (float)vh;
    float src_top_norm = (float)(vh - region.top) / (float)vh;
    glUniform4f(program_uniform_location(SCREENSHOT_PROGRAM, "src_rect"), src_left_norm, src_top_norm, src_right_norm, src_bottom_norm);

    // Set destination rectangle (NDC coordinates: -1 to 1)
    glUniform4f(program_uniform_location(SCREENSHOT_PROGRAM, "dest_rect"), -1.0f, -1.0f, 1.0f, 1.0f);

    // Set the source texture size for proper downscaling
    glUniform2f(program_uniform_location(SCREENSHOT_PROGRAM, "src_size"), (float)vw, (float)vh);
    glUniform1f(program_uniform_location(SCREENSHOT_PROGRAM, "src_is_linear"), is_float ? 1.f : 0.f);

    // Bind the source texture
    glActiveTexture(GL_TEXTURE0 + GRAPHICS_UNIT);
    glBindTexture(GL_TEXTURE_2D, src_texture);

    // Draw the scaled quad
    draw_quad(false, 0);

    // Read the scaled result
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, *thumb_w, *thumb_h, GL_RGBA, GL_UNSIGNED_BYTE, dst_buf);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    // Restore previous state
    bind_framebuffer_for_output(current_framebuffer);
    restore_viewport();

    // Clean up temporary resources
    free_texture(&src_texture);
    free_texture(&temp_texture);
    free_framebuffer(&temp_framebuffer);
}
// }}}

// Python API {{{

static PyObject *
pygpu_driver_version_string(PyObject *self UNUSED, PyObject *args UNUSED) {
    return PyUnicode_FromString(global_state.gl_version ? gl_version_string() : "");
}

static void
free_custom_shader_pipeline(CustomShaderPipeline *p) {
    for (size_t i = 0; i < p->num_groups; i++) {
        CustomShaderGroup *cg = p->groups + i;
        if (cg->animation_curve_is_shared) cg->animation_curve = NULL;
        else cg->animation_curve = free_animation(cg->animation_curve);
        free(cg->animation_start_user_events.indices);
        free(cg->animation_toggle_user_events.indices);
        free(cg->animation_end_user_events.indices);
    }
    free(p->groups);
    p->groups = NULL;
    p->num_groups = 0;
    free(p->group_progress);
    p->group_progress = NULL;
    free(p->group_scratch);
    p->group_scratch = NULL;
    for (size_t i = 0; i < p->num_user_events; i++) free(p->user_events[i]);
    free(p->user_events);
    p->user_events = NULL;
    p->num_user_events = p->user_events_capacity = 0;
}

static NamedTexture
named_texture_from_str(const char *s) {
    if (strcmp(s, "a") == 0) return CUSTOM_SHADER_TEXTURE_A;
    if (strcmp(s, "b") == 0) return CUSTOM_SHADER_TEXTURE_B;
    if (strcmp(s, "persist") == 0) return CUSTOM_SHADER_TEXTURE_PERSIST;
    return CUSTOM_SHADER_TEXTURE_DEFAULT;
}

// The bit for the builtin event named s, zero when there is no such event
static ShaderAnimEventMask
builtin_anim_event_bit(const char *s) {
    if (strcmp(s, "pointer-left-button-press") == 0) return 1u << SHADER_ANIM_EVENT_POINTER_LEFT_BUTTON_PRESS;
    if (strcmp(s, "os-window-focus-in") == 0) return 1u << SHADER_ANIM_EVENT_OS_WINDOW_FOCUS_IN;
    if (strcmp(s, "os-window-focus-out") == 0) return 1u << SHADER_ANIM_EVENT_OS_WINDOW_FOCUS_OUT;
    if (strcmp(s, "window-focus-in") == 0) return 1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_IN;
    if (strcmp(s, "window-focus-out") == 0) return 1u << SHADER_ANIM_EVENT_WINDOW_FOCUS_OUT;
    if (strcmp(s, "tab-change") == 0) return 1u << SHADER_ANIM_EVENT_TAB_CHANGE;
    if (strcmp(s, "bell-in-window") == 0) return 1u << SHADER_ANIM_EVENT_BELL_IN_WINDOW;
    if (strcmp(s, "user-activity") == 0) return 1u << SHADER_ANIM_EVENT_USER_ACTIVITY;
    if (strcmp(s, "user-idle") == 0) return 1u << SHADER_ANIM_EVENT_USER_IDLE;
    if (strcmp(s, "cursor-trail-move") == 0) return 1u << SHADER_ANIM_EVENT_CURSOR_TRAIL_MOVE;
    if (strcmp(s, "cursor-trail-stop") == 0) return 1u << SHADER_ANIM_EVENT_CURSOR_TRAIL_STOP;
    if (strcmp(s, "command-start") == 0) return 1u << SHADER_ANIM_EVENT_COMMAND_START;
    if (strcmp(s, "command-finish") == 0) return 1u << SHADER_ANIM_EVENT_COMMAND_FINISH;
    if (strcmp(s, "alternate-screen-enter") == 0) return 1u << SHADER_ANIM_EVENT_ALTERNATE_SCREEN_ENTER;
    if (strcmp(s, "alternate-screen-exit") == 0) return 1u << SHADER_ANIM_EVENT_ALTERNATE_SCREEN_EXIT;
    return 0;
}

static const char user_event_prefix[] = "user:";

// The name of the user event s, without the user: prefix, NULL if s is not a user event
static const char *
user_event_name(const char *s) {
    if (strncmp(s, user_event_prefix, sizeof(user_event_prefix) - 1) != 0 || !s[sizeof(user_event_prefix) - 1]) return NULL;
    return s + sizeof(user_event_prefix) - 1;
}

// The index of the user event with the specified name in the pipeline, adding
// it to the pipeline if not there yet. Sets a Python exception and returns
// SIZE_MAX when out of memory.
static size_t
add_user_event(CustomShaderPipeline *p, const char *name) {
    size_t i = user_event_index(p, name);
    if (i != SIZE_MAX) return i;
    if (p->num_user_events >= p->user_events_capacity) {
        const size_t cap = MAX(8u, p->user_events_capacity * 2);
        char **q = cap > p->user_events_capacity && cap <= SIZE_MAX / sizeof(char *) ? realloc(p->user_events, cap * sizeof(char *)) : NULL;
        if (!q) {
            PyErr_NoMemory();
            return SIZE_MAX;
        }
        p->user_events = q;
        p->user_events_capacity = cap;
    }
    if (!(p->user_events[p->num_user_events] = strdup(name))) {
        PyErr_NoMemory();
        return SIZE_MAX;
    }
    return p->num_user_events++;
}

static bool
add_to_user_event_list(ShaderUserEventList *l, size_t idx) {
    for (size_t i = 0; i < l->count; i++) {
        if (l->indices[i] == idx) return true;
    }
    if (l->count >= l->capacity) {
        const size_t cap = MAX(4u, l->capacity * 2);
        size_t *q = cap > l->capacity && cap <= SIZE_MAX / sizeof(size_t) ? realloc(l->indices, cap * sizeof(size_t)) : NULL;
        if (!q) {
            PyErr_NoMemory();
            return false;
        }
        l->indices = q;
        l->capacity = cap;
    }
    l->indices[l->count++] = idx;
    return true;
}

// Adds the events named in seq to the builtin events in *mask and the user
// events in *user, adding user events to the pipeline as needed. Unknown
// names are ignored. Returns false with a Python exception set on failure.
static bool
parse_anim_event_sequence(PyObject *seq, CustomShaderPipeline *p, ShaderAnimEventMask *mask, ShaderUserEventList *user) {
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyUnicode_Check(item)) continue;
        const char *s = PyUnicode_AsUTF8(item);
        if (!s) return false;
        const char *name = user_event_name(s);
        if (name) {
            const size_t idx = add_user_event(p, name);
            if (idx == SIZE_MAX || !add_to_user_event_list(user, idx)) return false;
        } else *mask |= builtin_anim_event_bit(s);
    }
    return true;
}

static bool
transfer_pipeline_to_struct(PyObject *pg, CustomShaderPipeline *p) {
#define is_seq(x) (x && (PyList_Check(x) || PyTuple_Check(x)))
    PyObject *textures = PyDict_GetItemString(pg, "textures");
    if (is_seq(textures)) {
        for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(textures); i++) {
            PyObject *t = PySequence_Fast_GET_ITEM(textures, i);
            if (!PyUnicode_Check(t)) {
                PyErr_SetString(PyExc_TypeError, "pipeline textures must be strings");
                return false;
            }
            p->textures |= (1u << named_texture_from_str(PyUnicode_AsUTF8(t)));
        }
    }
    PyObject *groups = PyDict_GetItemString(pg, "groups");
    if (!is_seq(groups)) {
        PyErr_SetString(PyExc_TypeError, "pipeline groups must be a sequence");
        return false;
    }
    Py_ssize_t num_groups = PySequence_Fast_GET_SIZE(groups);
    // The words of a set of groups are passed to the GPU with a GLsizei count
    const size_t group_words = MAX((size_t)1, ((size_t)num_groups + 31) / 32);
    if (group_words > INT32_MAX) {
        PyErr_SetString(PyExc_ValueError, "too many pipeline groups for the GPU to be told which are active");
        return false;
    }
    if (num_groups) {
        if (!(p->groups = calloc(num_groups, sizeof(CustomShaderGroup))) || !(p->group_progress = calloc(num_groups, sizeof(float)))) {
            PyErr_NoMemory();
            return false;
        }
        p->num_groups = (size_t)num_groups;
    }
    if (!(p->group_scratch = calloc(group_words, sizeof(uint32_t)))) {
        PyErr_NoMemory();
        return false;
    }
    p->group_words = group_words;
    for (Py_ssize_t i = 0; i < num_groups; i++) {
        PyObject *g = PySequence_Fast_GET_ITEM(groups, i);
        if (!PyDict_Check(g)) {
            PyErr_SetString(PyExc_TypeError, "pipeline group must be a dict");
            return false;
        }
        CustomShaderGroup *cg = &p->groups[i];
        PyObject *vp = PyDict_GetItemString(g, "viewport_pos");
        if (is_seq(vp) && PySequence_Fast_GET_SIZE(vp) == 2) {
            cg->viewport_pos.x = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(vp, 0));
            cg->viewport_pos.y = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(vp, 1));
        }
        PyObject *vs = PyDict_GetItemString(g, "viewport_size");
        if (is_seq(vs) && PySequence_Fast_GET_SIZE(vs) == 2) {
            cg->viewport_size.w = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(vs, 0));
            cg->viewport_size.h = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(vs, 1));
        }
        PyObject *ot = PyDict_GetItemString(g, "output_texture");
        if (ot && PyUnicode_Check(ot)) { cg->output_texture = named_texture_from_str(PyUnicode_AsUTF8(ot)); }

        PyObject *astart = PyDict_GetItemString(g, "animation_start");
        if (is_seq(astart) && !parse_anim_event_sequence(astart, p, &cg->animation_start_events, &cg->animation_start_user_events)) return false;
        PyObject *atoggle = PyDict_GetItemString(g, "animation_toggle");
        if (is_seq(atoggle) && !parse_anim_event_sequence(atoggle, p, &cg->animation_toggle_events, &cg->animation_toggle_user_events)) return false;

        PyObject *acurve = PyDict_GetItemString(g, "animation_curve");
        if (acurve && PyObject_IsTrue(acurve)) {
            if ((cg->animation_curve = alloc_animation()) != NULL) add_easing_function(cg->animation_curve, acurve, 0.0, 1.0);
            for (Py_ssize_t j = 0; j < i; j++) {
                if (animations_equal(p->groups[j].animation_curve, cg->animation_curve)) {
                    free_animation(cg->animation_curve);
                    cg->animation_curve = p->groups[j].animation_curve;
                    cg->animation_curve_is_shared = true;
                    break;
                }
            }
        }

        PyObject *astep = PyDict_GetItemString(g, "animation_step");
        cg->animation_step = (astep && PyLong_Check(astep)) ? (monotonic_t)PyLong_AsLong(astep) : ANIMATION_SAMPLE_WAIT;
        cg->animation_step = cg->animation_step == 0 ? MONOTONIC_T_MAX : MAX(cg->animation_step, OPT(repaint_delay));

        PyObject *ae_events = PyDict_GetItemString(g, "animation_end_events");
        if (is_seq(ae_events) && !parse_anim_event_sequence(ae_events, p, &cg->animation_end_events, &cg->animation_end_user_events)) return false;

        PyObject *ae_dur = PyDict_GetItemString(g, "animation_end_duration");
        if (ae_dur && PyLong_Check(ae_dur)) cg->animation_end_duration = (monotonic_t)PyLong_AsLong(ae_dur);

        PyObject *a_dur = PyDict_GetItemString(g, "animation_duration");
        cg->animation_duration = (a_dur && PyLong_Check(a_dur)) ? (monotonic_t)PyLong_AsLong(a_dur) : -1;
        PyObject *a_rev = PyDict_GetItemString(g, "animation_reverse");
        cg->animation_reverse = (a_rev && PyLong_Check(a_rev)) ? (monotonic_t)PyLong_AsLong(a_rev) : 0;
        PyObject *a_loops = PyDict_GetItemString(g, "animation_loops");
        cg->animation_loops = (a_loops && PyLong_Check(a_loops)) ? (unsigned)PyLong_AsUnsignedLong(a_loops) : 1;

        PyObject *attached = PyDict_GetItemString(g, "attached");
        cg->attached = attached && PyObject_IsTrue(attached);
        if (PyErr_Occurred()) return false;
    }
    p->all_animation_start_events = 0;
    for (size_t i = 0; i < p->num_groups; i++) p->all_animation_start_events |= p->groups[i].animation_start_events | p->groups[i].animation_toggle_events;
    p->bypass_builtin_trail = (p->all_animation_start_events & (1u << SHADER_ANIM_EVENT_CURSOR_TRAIL_MOVE)) != 0;
#undef is_seq
    return true;
}

static bool
attach_shaders(PyObject *sources, GLuint program_id, GLenum shader_type) {
    RAII_ALLOC(const GLchar *, c_sources, calloc(PyTuple_GET_SIZE(sources), sizeof(GLchar *)));
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(sources); i++) {
        PyObject *temp = PyTuple_GET_ITEM(sources, i);
        if (!PyUnicode_Check(temp)) {
            PyErr_SetString(PyExc_TypeError, "shaders must be strings");
            return false;
        }
        c_sources[i] = PyUnicode_AsUTF8(temp);
    }
    GLuint shader_id = compile_shaders(shader_type, PyTuple_GET_SIZE(sources), c_sources);
    if (shader_id == 0) return false;
    glAttachShader(program_id, shader_id);
    glDeleteShader(shader_id);
    return true;
}

static PyObject *
compile_program(PyObject UNUSED *self, PyObject *args) {
    PyObject *vertex_shaders, *fragment_shaders, *metadata;
    int which, allow_recompile = 0;
    if (!PyArg_ParseTuple(
            args, "iO!O!O!|p", &which, &PyTuple_Type, &vertex_shaders, &PyTuple_Type, &fragment_shaders, &PyDict_Type, &metadata, &allow_recompile))
        return NULL;
    if (which == -1) {
        init_cell_program();
        Py_RETURN_NONE;
    }
    if (which == -2) {
        init_custom_programs();
        Py_RETURN_NONE;
    }
    if (which < 0 || which >= NUM_PROGRAMS) {
        PyErr_Format(PyExc_ValueError, "Unknown program: %d", which);
        return NULL;
    }
    Program *program = program_ptr(which);
    if (!PyTuple_GET_SIZE(vertex_shaders)) {
        if (which == CUSTOM_END_PROGRAM) {
            free_custom_shader_pipeline(&custom_shaders.end);
            zero_at_ptr(&custom_shaders.end);
            custom_shaders.generation++;
        }
        if (program->id != 0) {
            glDeleteProgram(program->id);
            program->id = 0;
        }
        Py_RETURN_NONE;
    }
    if (program->id != 0) {
        if (allow_recompile) {
            glDeleteProgram(program->id);
            program->id = 0;
        } else {
            PyErr_SetString(PyExc_ValueError, "program already compiled");
            return NULL;
        }
    }
#define fail_compile()                \
    {                                 \
        glDeleteProgram(program->id); \
        return NULL;                  \
    }
    program->id = glCreateProgram();
    if (!attach_shaders(vertex_shaders, program->id, GL_VERTEX_SHADER)) fail_compile();
    if (!attach_shaders(fragment_shaders, program->id, GL_FRAGMENT_SHADER)) fail_compile();
    glLinkProgram(program->id);
    GLint ret = GL_FALSE;
    glGetProgramiv(program->id, GL_LINK_STATUS, &ret);
    if (ret != GL_TRUE) {
        GLsizei len;
        static char glbuf[4096];
        glGetProgramInfoLog(program->id, sizeof(glbuf), &len, glbuf);
        PyErr_Format(PyExc_ValueError, "Failed to link GLSL shaders:\n%s", glbuf);
        fail_compile();
    }
#undef fail_compile
    if (which == CUSTOM_END_PROGRAM) {
        free_custom_shader_pipeline(&custom_shaders.end);
        zero_at_ptr(&custom_shaders.end);
        PyObject *pg = PyDict_GetItemString(metadata, "pipeline");
        if (pg && !transfer_pipeline_to_struct(pg, &custom_shaders.end)) {
            glDeleteProgram(program->id);
            return NULL;
        }
        custom_shaders.end.active = true;
        PyObject *has_pointer_map = PyDict_GetItemString(metadata, "has_pointer_map");
        custom_shaders.end.has_pointer_map = has_pointer_map && PyObject_IsTrue(has_pointer_map) == 1;
        PyObject *pointer_map_groups = PyDict_GetItemString(metadata, "pointer_map_groups");
        if (pointer_map_groups && (PyList_Check(pointer_map_groups) || PyTuple_Check(pointer_map_groups))) {
            for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(pointer_map_groups); i++) {
                const Py_ssize_t g = PyLong_AsSsize_t(PySequence_Fast_GET_ITEM(pointer_map_groups, i));
                if (g >= 0 && (size_t)g < custom_shaders.end.num_groups) custom_shaders.end.groups[g].uses_pointer_map = true;
            }
            if (PyErr_Occurred()) {
                glDeleteProgram(program->id);
                return NULL;
            }
        }
        custom_shaders.generation++;
    }
    init_uniforms(which);
    set_program_layout(which, metadata);
    return Py_BuildValue("I", program->id);
}

#define PYWRAP0(name) static PyObject *py##name(PYNOARG)
#define PYWRAP1(name) static PyObject *py##name(PyObject UNUSED *self, PyObject *args)
#define PA(fmt, ...) \
    if (!PyArg_ParseTuple(args, fmt, __VA_ARGS__)) return NULL;
#define ONE_INT(name)                 \
    PYWRAP1(name) {                   \
        name(PyLong_AsSsize_t(args)); \
        Py_RETURN_NONE;               \
    }
#define TWO_INT(name)     \
    PYWRAP1(name) {       \
        int a, b;         \
        PA("ii", &a, &b); \
        name(a, b);       \
        Py_RETURN_NONE;   \
    }
#define NO_ARG(name)    \
    PYWRAP0(name) {     \
        name();         \
        Py_RETURN_NONE; \
    }
#define NO_ARG_INT(name) \
    PYWRAP0(name) { return PyLong_FromSsize_t(name()); }

ONE_INT(bind_program)
NO_ARG(unbind_program)

// test only, see gl_test_slot_table()
static PyObject *
pytest_vao_slot_table(PyObject *self UNUSED, PyObject *args) {
    unsigned long long count, free_every;
    if (!PyArg_ParseTuple(args, "KK", &count, &free_every)) return NULL;
    size_t capacity = 0;
    const bool ok = gl_test_slot_table((size_t)count, (size_t)free_every, &capacity);
    return Py_BuildValue("OK", ok ? Py_True : Py_False, (unsigned long long)capacity);
}

PYWRAP0(create_vao) {
    ssize_t ans = create_vao();
    if (ans < 0) return PyErr_NoMemory();
    return PyLong_FromSsize_t(ans);
}

ONE_INT(bind_vertex_array)
NO_ARG(unbind_vertex_array)
TWO_INT(unmap_vao_buffer)

static PyObject *
sprite_map_set_limits(PyObject UNUSED *self, PyObject *args) {
    unsigned int w, h;
    if (!PyArg_ParseTuple(args, "II", &w, &h)) return NULL;
    sprite_tracker_set_limits(w, h);
    max_texture_size = w;
    max_array_texture_layers = h;
    Py_RETURN_NONE;
}

// Test only. Exercise the renderer's HDR policy across display and content
// changes without a GPU context. Options use the normal configuration path.
static PyObject *
pysimulate_hdr_render_ticks(PyObject *self UNUSED, PyObject *states) {
    RAII_PyObject(ticks, PySequence_Fast(states, "expected HDR display states"));
    if (!ticks) return NULL;
    RAII_ALLOC(OSWindow, w, calloc(1, sizeof(OSWindow)));
    if (!w) return PyErr_NoMemory();
    RAII_PyObject(ans, PyList_New(0));
    if (!ans) return NULL;
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(ticks); i++) {
        int enabled_by_default, has_content;
        if (!PyArg_ParseTuple(PySequence_Fast_GET_ITEM(ticks, i), "pfp", &enabled_by_default, &w->hdr_headroom, &has_content)) return NULL;
        w->hdr_enabled_by_default = enabled_by_default;
        w->hdr_content = has_content;
        w->hdr_active = hdr_should_be_active(w);
        RAII_PyObject(state, Py_BuildValue("Of", w->hdr_active ? Py_True : Py_False, effective_hdr_headroom(w)));
        if (!state || PyList_Append(ans, state) != 0) return NULL;
    }
    return Py_NewRef(ans);
}

static PyObject *
os_window_output_state(OSWindow *w, bool switched) {
    float levels[3] = {0};
    const bool dither = output_dither_levels(w, levels);
    DisplayInfo d;
    os_window_display_info(w, &d);
    return Py_BuildValue("{sOsOsOsOs(fff)sisssisOsOsO}", "linear", w->linear_output ? Py_True : Py_False, "encode_in_shader",
        w->output.encode_in_shader ? Py_True : Py_False, "needs_layers", w->output.encode_in_shader || dither ? Py_True : Py_False, "dither",
        dither ? Py_True : Py_False, "levels", levels[0], levels[1], levels[2], "output_bits", d.output_bits, "output_type", d.output_type,
        "output_dither", d.output_dither, "display_deep", w->output.depth.acted ? Py_True : Py_False, "switched", switched ? Py_True : Py_False,
        "layers_float", layers_are_float(w) ? Py_True : Py_False);
}

// test only: what is done with, and reported about, a window surface with the given bits per channel on a display that
// is deep or not
static PyObject *
pysimulate_os_window_output(PyObject *self UNUSED, PyObject *args) {
    int r, g, b, is_float, display_deep = 0;
    if (!PyArg_ParseTuple(args, "iiip|p", &r, &g, &b, &is_float, &display_deep)) return NULL;
    RAII_ALLOC(OSWindow, w, calloc(1, sizeof(OSWindow)));
    if (!w) return PyErr_NoMemory();
    set_os_window_output(w, r, g, b, is_float);
    update_output_depth(w, display_deep, 0, NULL);
    return os_window_output_state(w, false);
}

// test only: a window moved between displays, as a sequence of (milliseconds, display is deep, HDR is enabled by
// default on it). A switchable surface, as on macOS, follows os_window_wants_float_surface() between 8 bit and floating
// point, a fixed one, as elsewhere, keeps the bits it starts with
static PyObject *
pysimulate_output_depth_changes(PyObject *self UNUSED, PyObject *args) {
    int r, g, b, is_float, switchable;
    PyObject *ticks;
    if (!PyArg_ParseTuple(args, "iiippO", &r, &g, &b, &is_float, &switchable, &ticks)) return NULL;
    RAII_PyObject(seq, PySequence_Fast(ticks, "expected a sequence of display states"));
    if (!seq) return NULL;
    RAII_ALLOC(OSWindow, w, calloc(1, sizeof(OSWindow)));
    if (!w) return PyErr_NoMemory();
    set_os_window_output(w, r, g, b, is_float);
    RAII_PyObject(ans, PyList_New(0));
    if (!ans) return NULL;
    for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(seq); i++) {
        long long ms;
        int deep, hdr_default;
        if (!PyArg_ParseTuple(PySequence_Fast_GET_ITEM(seq, i), "Lpp", &ms, &deep, &hdr_default)) return NULL;
        w->hdr_enabled_by_default = hdr_default;
        update_output_depth(w, deep, ms_to_monotonic_t(ms), NULL);
        bool switched = false;
        const bool want = os_window_wants_float_surface(w);
        if (switchable && want != w->output.is_float) {
            if (want) set_os_window_output(w, 16, 16, 16, true);
            else set_os_window_output(w, 8, 8, 8, false);
            os_window_surface_switched(w);
            switched = true;
        }
        RAII_PyObject(state, os_window_output_state(w, switched));
        if (!state || PyList_Append(ans, state) != 0) return NULL;
    }
    return Py_NewRef(ans);
}

// test only wrapper for decorations_map_new_size
static PyObject *
pydecorations_map_new_size(PyObject *self UNUSED, PyObject *args) {
    unsigned long count, current_capacity, max_size;
    if (!PyArg_ParseTuple(args, "kkk", &count, &current_capacity, &max_size)) return NULL;
    GLint w, h;
    if (!decorations_map_new_size(count, current_capacity, max_size, &w, &h)) Py_RETURN_NONE;
    return Py_BuildValue("ii", (int)w, (int)h);
}

// Test only. Wraps custom_shader_needs_render() so the redraw decision can be
// exercised from the Python test suite without a GPU context. Each state is a
// (has_active_shaders, min_step, next_end_at) tuple.
static bool
parse_shader_anim_state(PyObject *obj, ShaderAnimState *ans) {
    int has_active;
    long long min_step, next_end_at;
    if (!PyArg_ParseTuple(obj, "pLL", &has_active, &min_step, &next_end_at)) return false;
    *ans = (ShaderAnimState){.has_active_shaders = has_active, .min_step = min_step, .next_end_at = next_end_at};
    return true;
}

static PyObject *
pycustom_shader_needs_render(PyObject *self UNUSED, PyObject *args) {
    PyObject *b, *a;
    unsigned long long event_mask;
    long long now;
    ShaderAnimState before, after;
    if (!PyArg_ParseTuple(args, "O!O!KL", &PyTuple_Type, &b, &PyTuple_Type, &a, &event_mask, &now)) return NULL;
    if (!parse_shader_anim_state(b, &before) || !parse_shader_anim_state(a, &after)) return NULL;
    if (custom_shader_needs_render(&before, &after, event_mask != 0, now)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

// Test only. Runs the renderer's per-frame custom shader bookkeeping (the same
// sequence as prepare_to_render_os_window) against a freshly allocated, zero
// initialized OSWindow and returns whether each tick asked for a redraw. Used
// to assert that an idle OS window with no shader work pending stays idle.
static PyObject *
pysimulate_custom_shader_render_ticks(PyObject *self UNUSED, PyObject *args) {
    unsigned long num_ticks;
    unsigned long long event_mask = 0;
    int initialize = 0;
    if (!PyArg_ParseTuple(args, "k|Kp", &num_ticks, &event_mask, &initialize)) return NULL;
    RAII_ALLOC(OSWindow, osw, calloc(1, sizeof(OSWindow)));
    if (!osw) return PyErr_NoMemory();
    if (initialize) init_shader_animation_state(osw);
    PyObject *ans = PyList_New(0);
    monotonic_t now = monotonic();
    for (unsigned long i = 0; ans && i < num_ticks; i++, now += ms_to_monotonic_t(10ll)) {
        const ShaderAnimState before = osw->shader_anim;
        update_custom_shader_animations(event_mask, now, osw);
        PyObject *v = custom_shader_needs_render(&before, &osw->shader_anim, event_mask != 0, now) ? Py_True : Py_False;
        if (PyList_Append(ans, v) != 0) Py_CLEAR(ans);
    }
    free(osw->shader_group_anim);
    free(osw->shader_user_events.fired.words);
    return ans;
}

static PyObject *
pyfire_custom_shader_event(PyObject *self UNUSED, PyObject *args) {
    const char *name;
    unsigned long long os_window_id = 0;
    if (!PyArg_ParseTuple(args, "s|K", &name, &os_window_id)) return NULL;
    switch (fire_custom_shader_user_event(name, os_window_id)) {
        case -1: return NULL;
        case 0: Py_RETURN_FALSE;
        default: Py_RETURN_TRUE;
    }
}

// Test only. Loads pipeline, a dict as sent by compile_program(), in place of
// the current one and runs the animation state machine over an OS window, one
// tick per item of ticks, each a sequence of event names, 10ms apart. Returns
// the active state of every group after each tick, or if with_progress is
// true, the animation_progress of active groups and None for inactive ones.
static PyObject *
pysimulate_custom_shader_animation(PyObject *self UNUSED, PyObject *args) {
    PyObject *pipeline, *ticks;
    int with_progress = 0;
    if (!PyArg_ParseTuple(args, "O!O!|p", &PyDict_Type, &pipeline, &PyList_Type, &ticks, &with_progress)) return NULL;
    RAII_ALLOC(OSWindow, osw, calloc(1, sizeof(OSWindow)));
    if (!osw) return PyErr_NoMemory();
    const CustomShaderPipeline saved = custom_shaders.end;
    const size_t saved_count = custom_shaders.count;
    zero_at_ptr(&custom_shaders.end);
    PyObject *ans = NULL;
    if (transfer_pipeline_to_struct(pipeline, &custom_shaders.end)) {
        custom_shaders.count = 1;
        init_shader_animation_state(osw);
        ans = PyList_New(0);
        monotonic_t now = monotonic();
        for (Py_ssize_t i = 0; ans && i < PyList_GET_SIZE(ticks); i++, now += ms_to_monotonic_t(10ll)) {
            PyObject *names = PyList_GET_ITEM(ticks, i);
            if (!PyTuple_Check(names) && !PyList_Check(names)) {
                PyErr_SetString(PyExc_TypeError, "each tick must be a sequence of event names");
                Py_CLEAR(ans);
                break;
            }
            // focus:in and focus:out change OS window focus, as the GLFW focus
            // callback does, before the tick's events are consumed, the way
            // prepare_to_render_os_window() does. User events are queued as
            // the shader_event action does, those the pipeline does not use
            // ignored.
            ShaderAnimEventMask events = 0;
            for (Py_ssize_t n = 0; n < PySequence_Fast_GET_SIZE(names); n++) {
                PyObject *item = PySequence_Fast_GET_ITEM(names, n);
                if (!PyUnicode_Check(item)) continue;
                const char *name = PyUnicode_AsUTF8(item);
                if (!name) {
                    Py_CLEAR(ans);
                    break;
                }
                const bool focus_in = strcmp(name, "focus:in") == 0;
                const char *user_name = user_event_name(name);
                if (focus_in || strcmp(name, "focus:out") == 0) {
                    const bool focused = focus_in;
                    if (osw->is_focused != focused) record_os_window_focus_change_for_shaders(osw, focused);
                    osw->is_focused = focused;
                } else if (user_name) {
                    const size_t idx = user_event_index(&custom_shaders.end, user_name);
                    if (idx != SIZE_MAX && !queue_user_event(osw, idx)) {
                        PyErr_NoMemory();
                        Py_CLEAR(ans);
                        break;
                    }
                } else events |= builtin_anim_event_bit(name);
            }
            if (!ans) break;
            update_custom_shader_animations(events | osw->shader_anim_event_registry, now, osw);
            osw->shader_anim_event_registry = 0;
            RAII_PyObject(states, PyTuple_New(custom_shaders.end.num_groups));
            if (!states) {
                Py_CLEAR(ans);
                break;
            }
            float *progress = custom_shaders.end.group_progress;
            custom_shader_group_progress(osw, now, progress);
            for (size_t g = 0; g < custom_shaders.end.num_groups; g++) {
                const bool active = group_is_running(osw, g);
                PyObject *state =
                    with_progress && active ? PyFloat_FromDouble(progress[g]) : Py_NewRef(with_progress ? Py_None : (active ? Py_True : Py_False));
                if (!state) break;
                PyTuple_SET_ITEM(states, g, state);
            }
            if (PyErr_Occurred()) {
                Py_CLEAR(ans);
                break;
            }
            if (PyList_Append(ans, states) != 0) Py_CLEAR(ans);
        }
    }
    free_custom_shader_pipeline(&custom_shaders.end);
    custom_shaders.end = saved;
    custom_shaders.count = saved_count;
    free(osw->shader_group_anim);
    free(osw->shader_user_events.fired.words);
    return ans;
}

// Test only. Looks up a position in a pointer map given as the bytes of
// POINTER_MAP_SIZE x POINTER_MAP_SIZE (x, y) float pairs.
static PyObject *
pysample_pointer_map(PyObject *self UNUSED, PyObject *args) {
    Py_buffer data;
    double u, v;
    if (!PyArg_ParseTuple(args, "y*dd", &data, &u, &v)) return NULL;
    if ((size_t)data.len != sizeof(float) * 2 * POINTER_MAP_SIZE * POINTER_MAP_SIZE) {
        PyBuffer_Release(&data);
        PyErr_SetString(PyExc_ValueError, "pointer map has the wrong size");
        return NULL;
    }
    sample_pointer_map(data.buf, &u, &v);
    PyBuffer_Release(&data);
    return Py_BuildValue("dd", u, v);
}

#define M(name, arg_type) {#name, (PyCFunction)name, arg_type, NULL}
#define MW(name, arg_type) {#name, (PyCFunction)py##name, arg_type, NULL}
static PyObject *
pyspatial_configure(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    unsigned mode;
    int red_blue;
    float scale, fov, positive, negative, phase_limit, multiplier;
    if (!PyArg_ParseTuple(args, "KIpffffff", &id, &mode, &red_blue, &scale, &fov, &positive, &negative, &phase_limit, &multiplier)) return NULL;
    OSWindow *w = os_window_for_id(id);
    if (!w) Py_RETURN_FALSE;
    if (mode > 2 || !isfinite(scale) || scale < 1 || !isfinite(fov) || fov <= 0 || fov >= 180 ||
        !isfinite(positive) || positive < 0 || positive >= 180 || !isfinite(negative) || negative < 0 || negative >= 180 || !isfinite(phase_limit) || phase_limit < 0 ||
        !isfinite(multiplier) || multiplier < 1.f / 64.f || multiplier > 64.f) {
        PyErr_SetString(PyExc_ValueError, "Invalid spatial display parameters");
        return NULL;
    }
    make_os_window_context_current(w);
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    const double factor = mode == 2 ? scale : 1.;
    const int eye_width = red_blue ? w->spatial.output_width : w->spatial.output_width / 2;
    if (mode && (eye_width * factor > limit || w->spatial.output_height * factor > limit)) {
        PyErr_Format(PyExc_ValueError, "Spatial canvas exceeds this GPU's %d pixel texture dimension", limit);
        return NULL;
    }
    w->spatial.mode = mode;
    w->spatial.red_blue = red_blue;
    w->spatial.scale = scale;
    w->spatial.requested_scale = scale;
    w->spatial.texture_limit = limit;
    w->spatial.fov = fov;
    w->spatial.positive = positive;
    w->spatial.negative = negative;
    w->spatial.phase_limit = phase_limit;
    w->spatial.multiplier = multiplier;
    for (unsigned i = 0; i < 9; i++) w->spatial.rotation[i] = i % 4 == 0 ? 1.f : 0.f;
    w->spatial.tracking = false;
    for (unsigned t = 0; t < w->num_tabs; t++) for (unsigned i = 0; i < w->tabs[t].num_windows; i++) {
        Screen *s = w->tabs[t].windows[i].render_data.screen;
        if (s) { s->is_dirty = true; grman_mark_layers_dirty(s->grman); }
    }
    update_os_window_viewport(w, true);
    w->needs_render = true;
    Py_RETURN_TRUE;
}

static PyObject *
pyspatial_pose(PyObject *self UNUSED, PyObject *args) {
    unsigned long long id;
    PyObject *rotation;
    int tracking;
    if (!PyArg_ParseTuple(args, "KOp", &id, &rotation, &tracking)) return NULL;
    OSWindow *w = os_window_for_id(id);
    if (!w) Py_RETURN_FALSE;
    RAII_PyObject(seq, PySequence_Fast(rotation, "Expected a 3 by 3 rotation matrix"));
    if (!seq) return NULL;
    if (PySequence_Fast_GET_SIZE(seq) != 9) { PyErr_SetString(PyExc_ValueError, "Expected nine rotation components"); return NULL; }
    float values[9]; // mathematical size of a 3 by 3 matrix
    for (unsigned i = 0; i < 9; i++) {
        values[i] = (float)PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
        if (PyErr_Occurred()) return NULL;
        if (!isfinite(values[i])) { PyErr_SetString(PyExc_ValueError, "Rotation must be finite"); return NULL; }
    }
    if (memcmp(values, w->spatial.rotation, sizeof(values)) || tracking != w->spatial.tracking) w->needs_render = true;
    memcpy(w->spatial.rotation, values, sizeof(values));
    w->spatial.tracking = tracking;
    Py_RETURN_TRUE;
}

static PyMethodDef module_methods[] = {
    MW(spatial_configure, METH_VARARGS),
    MW(spatial_pose, METH_VARARGS),
    M(compile_program, METH_VARARGS),
    M(sprite_map_set_limits, METH_VARARGS),
    MW(create_vao, METH_NOARGS),
    {"test_vao_slot_table", (PyCFunction)pytest_vao_slot_table, METH_VARARGS, NULL},
    MW(gpu_driver_version_string, METH_NOARGS),
    MW(bind_vertex_array, METH_O),
    MW(unbind_vertex_array, METH_NOARGS),
    MW(unmap_vao_buffer, METH_VARARGS),
    MW(bind_program, METH_O),
    MW(unbind_program, METH_NOARGS),
    MW(custom_shader_needs_render, METH_VARARGS),
    MW(decorations_map_new_size, METH_VARARGS),
    MW(simulate_custom_shader_render_ticks, METH_VARARGS),
    MW(simulate_hdr_render_ticks, METH_O),
    MW(sample_pointer_map, METH_VARARGS),
    MW(fire_custom_shader_event, METH_VARARGS),
    MW(simulate_custom_shader_animation, METH_VARARGS),
    MW(simulate_os_window_output, METH_VARARGS),
    MW(simulate_output_depth_changes, METH_VARARGS),

    {NULL, NULL, 0, NULL} /* Sentinel */
};

void
initialize_gpu(void) {
    gl_init();
    query_gpu_texture_limits();
}

void
free_vao(ssize_t vao_idx) {
    if (vao_idx >= 0) remove_vao(vao_idx);
}

// Gives a window, tab bar or title bar whose VAO could not be made for want of memory another try, before it is drawn.
// Returns whether it has one. A window that has no GPU resources on purpose, while it is being moved between OS windows,
// has no screen to draw and is not asked for one
bool
ensure_cell_vao(ssize_t *vao_idx) {
    if (*vao_idx < 0) *vao_idx = create_cell_vao();
    return *vao_idx >= 0;
}

bool
ensure_border_vao(ssize_t *vao_idx) {
    if (*vao_idx < 0) *vao_idx = create_border_vao();
    return *vao_idx >= 0;
}

void
cleanup_shader_resources_on_terminate(void) {
    free_pointer_map_target();
    if (shader_globals_vao_idx != -1) {
        remove_vao(shader_globals_vao_idx);
        shader_globals_vao_idx = -1;
    }
}

static void
finalize(void) {
    default_visual_bell_animation = free_animation(default_visual_bell_animation);
    free_custom_shader_pipeline(&custom_shaders.end);
    free_program_layouts();
}

bool
init_shaders(PyObject *module) {
#define C(x)                                           \
    if (PyModule_AddIntConstant(module, #x, x) != 0) { \
        PyErr_NoMemory();                              \
        return false;                                  \
    }
    C(CELL_PROGRAM);
    C(CELL_FG_PROGRAM);
    C(CELL_BG_PROGRAM);
    C(BORDERS_PROGRAM);
    C(GRAPHICS_PROGRAM);
    C(GRAPHICS_PREMULT_PROGRAM);
    C(GRAPHICS_ALPHA_MASK_PROGRAM);
    C(GRAPHICS_INDEXED_PROGRAM);
    C(GRAPHICS_INDEXED_UNSCALED_PROGRAM);
    C(GRAPHICS_PALETTE_PROGRAM);
    C(GRAPHICS_PALETTE_UNSCALED_PROGRAM);
    C(BGIMAGE_PROGRAM);
    C(TINT_PROGRAM);
    C(TRAIL_PROGRAM);
    C(BLIT_PROGRAM);
    C(SCREENSHOT_PROGRAM);
    C(ROUNDED_RECT_PROGRAM);
    C(PADDING_PROGRAM);
    C(CUSTOM_END_PROGRAM);
    C(POINTER_MAP_SIZE);
    C(GLSL_VERSION);
    C(GL_VERSION);
    C(GL_VENDOR);
    C(GL_SHADING_LANGUAGE_VERSION);
    C(GL_RENDERER);
    C(GL_TRIANGLE_FAN);
    C(GL_TRIANGLE_STRIP);
    C(GL_TRIANGLES);
    C(GL_LINE_LOOP);
    C(GL_COLOR_BUFFER_BIT);
    C(GL_VERTEX_SHADER);
    C(GL_FRAGMENT_SHADER);
    C(GL_TRUE);
    C(GL_FALSE);
    C(GL_COMPILE_STATUS);
    C(GL_LINK_STATUS);
    C(GL_MAX_ARRAY_TEXTURE_LAYERS);
    C(GL_TEXTURE_BINDING_BUFFER);
    C(GL_MAX_TEXTURE_BUFFER_SIZE);
    C(GL_MAX_TEXTURE_SIZE);
    C(GL_TEXTURE_2D_ARRAY);
    C(GL_LINEAR);
    C(GL_CLAMP_TO_EDGE);
    C(GL_NEAREST);
    C(GL_TEXTURE_MIN_FILTER);
    C(GL_TEXTURE_MAG_FILTER);
    C(GL_TEXTURE_WRAP_S);
    C(GL_TEXTURE_WRAP_T);
    C(GL_UNPACK_ALIGNMENT);
    C(GL_R8);
    C(GL_RED);
    C(GL_UNSIGNED_BYTE);
    C(GL_UNSIGNED_SHORT);
    C(GL_R32UI);
    C(GL_RGB32UI);
    C(GL_RGBA);
    C(GL_TEXTURE_BUFFER);
    C(GL_STATIC_DRAW);
    C(GL_STREAM_DRAW);
    C(GL_DYNAMIC_DRAW);
    C(GL_SRC_ALPHA);
    C(GL_ONE_MINUS_SRC_ALPHA);
    C(GL_WRITE_ONLY);
    C(GL_READ_ONLY);
    C(GL_READ_WRITE);
    C(GL_BLEND);
    C(GL_FLOAT);
    C(GL_UNSIGNED_INT);
    C(GL_ARRAY_BUFFER);
    C(GL_UNIFORM_BUFFER);
    C(ANIMATION_SAMPLE_WAIT);

#undef C
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    register_at_exit_cleanup_func(SHADERS_CLEANUP_FUNC, finalize);
    return true;
}
// }}}
