/*
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once
#include "data-types.h"
#include "monotonic.h"
#include "vector.h"
#include "image-transfer.h"

// Bitmask values for GraphicsCommand.usage_hints
#define GRAPHICS_USAGE_HINT_TRANSIENT 1u

typedef struct {
    unsigned char action, transmission_type, compressed, delete_action, report_supported;
    uint32_t format, more, id, image_number, placement_id, quiet, parent_id, parent_placement_id, usage_hints, tone_map, gamut;
    // The S and O keys: 64 bits, so that data in a file or shared memory object can be larger than 4 GiB or begin past
    // it, see docs/silkie/image-tiling.rst. Every other key is 32 bits.
    uint64_t data_sz, data_offset;
    uint32_t transfer;  // the E key, an ITU-T H.273 transfer characteristic, see docs/silkie/precision-image-protocol.rst
    uint32_t width, height, x_offset, y_offset;
    union {
        uint32_t cursor_movement, compose_mode;
    };
    union {
        uint32_t cell_x_offset, blend_mode;
    };
    union {
        uint32_t cell_y_offset, bgcolor;
    };
    union {
        uint32_t data_width, animation_state;
    };
    union {
        uint32_t data_height, loop_count;
    };
    union {
        uint32_t num_lines, frame_number;
    };
    union {
        uint32_t num_cells, other_frame_number;
    };
    union {
        int32_t z_index, gap;
    };
    size_t payload_sz;
    uint32_t unicode_placement;
    int32_t offset_from_parent_x, offset_from_parent_y;
} GraphicsCommand;

typedef struct {
    float left, top, right, bottom;
} ImageRect;

// The texture formats of the tiles of an image larger than the GPU's largest texture, see graphics.c
typedef enum {
    TILE_SRGB8,  // eight bit sRGB RGBA, also for RGB
    TILE_HALF,  // half float RGBA, f=64
    TILE_LINEAR8,  // eight bit RGBA the shader decodes, with a transfer function other than sRGB
    TILE_GRAY16,
    TILE_RGB10A2,
    TILE_RGBA16,
    TILE_INDEX8,  // the indices of an indexed color or palette image
    TILE_INDEX_ALPHA8,  // an index and an alpha
} ImageTileFormat;

typedef struct {
    ImageRect src_rect, dest_rect;
    uint32_t texture_id, group_count;
    uint32_t right_texture_id;
    int z_index;
    int32_t depth;
    float depth_pixels, phase;
    // Zero for normal images, otherwise the number of bytes per pixel of an indexed color image
    uint8_t indexed_px_sz;
    // True for indexed color images, and images the shader decodes, drawn at
    // their natural size, which can use a cheaper shader. Always false for
    // other images.
    bool is_unscaled;
    // The texture holding the palette of a palette image, zero for other images
    uint32_t palette_texture_id;
    id_type image_id, ref_id;
    float peak; // of HDR images, see docs/hdr-protocol.rst
    bool tone_map;
    uint32_t gamut;  // the gamut of the pixels, see docs/wide-gamut-protocol.rst
    // What the shader does to the values it samples, an ImageDecode, and
    // whether the texture holds gray in its red channel alone
    uint8_t decode;
    bool is_gray;
    // Zero for an image in one texture, otherwise one more than the index of
    // the tile of the image, a row after another, this part of a placement is drawn from
    uint32_t tile;
    // The mipmap levels below the first of the integer texture of an indexed color or palette image, which the
    // shader picks itself, the GPU cannot filter integers
    uint8_t mip_levels;
    // Whether the shader filters the image itself, in linear light: a scaled
    // image it decodes, see graphics.slang
    bool filter_decoded;
    // Whether the texture holds the decoded values of such an image, linear in its gamut with straight alpha, which
    // the GPU filters and mipmaps itself, see graphics.c
    bool linear_texture;
} ImageRenderData;

typedef struct {
    uint32_t texture_id;
    unsigned int height, width;
    uint8_t *bitmap;
    uint32_t refcnt, id;
    size_t mmap_size;
} BackgroundImage;


#ifdef GRAPHICS_INTERNAL_APIS
typedef struct {
    float src_width, src_height, src_x, src_y;
    uint32_t cell_x_offset, cell_y_offset, num_cols, num_rows, effective_num_rows, effective_num_cols;
    int32_t z_index;
    int32_t depth;
    float depth_pixels, phase;
    int32_t start_row, start_column;
    uint32_t client_id;
    ImageRect src_rect;
    PyObject *annotation;  // immutable (purpose, name, description, details), or NULL
    // Indicates whether this reference represents a cell ref that should be
    // removed when the corresponding cells are modified.
    // The internal id of the virtual ref this cell image was created from. Is a cell ref if this is non-zero.
    id_type virtual_ref_id;
    // Virtual refs are not displayed but they can be used as prototypes for
    // refs placed using unicode placeholders.
    bool is_virtual_ref;

    struct {
        id_type img, ref;
        struct {
            int32_t x, y;
        } offset;
    } parent;

    id_type internal_id;
} ImageRef;

typedef struct {
    uint32_t gap, id, width, height, x, y, base_frame_id, bgcolor;
    bool is_opaque, is_4byte_aligned, alpha_blend, transient, is_float;
    uint8_t deep;  // a DeepStorage, the pixels of a high precision image
} Frame;

typedef enum { ANIMATION_STOPPED = 0, ANIMATION_LOADING = 1, ANIMATION_RUNNING = 2 } AnimationState;

typedef struct TextureRef {
    uint32_t id, refcnt;
    // whether the texture has mipmaps of what it holds now, and how many levels below the first an integer texture has
    bool mipmapped;
    uint8_t mip_levels;
} TextureRef;

struct ImageTiles;

// The palette of a palette image (f=401 to f=408, see
// docs/silkie/palette-image-protocol.rst). It is shared with the copies of the
// image that paused rendering makes, and copied before a change while shared.
typedef struct ImagePalette {
    uint32_t refcnt, texture, slots, version;
    uint8_t rgba[];  // slots entries of four bytes: red, green, blue and straight alpha, in sRGB
} ImagePalette;

#define NAME ref_map
#define KEY_TY id_type
#define VAL_TY ImageRef *
#include "kitty-verstable.h"

typedef struct {
    uint32_t client_id, client_number, width, height;
    TextureRef *texture;
    TextureRef *vector_right_texture;
    // The decoded pixels of an image the shader decodes, linear, with mipmaps, for when it is drawn smaller than it is
    TextureRef *decoded_texture;
    VectorStereo vector_stereo;
    id_type internal_id;
    // A GPU surface shared by a local client, see t=g in the graphics protocol
    void *surface;
    uint32_t surface_texture;
    bool surface_is_bgra;
    // Zero for normal images, otherwise the number of bytes per pixel of an
    // indexed color image. Such images cannot have more than one frame.
    // Palette images are indexed too, with one byte per pixel.
    uint8_t indexed_px_sz;
    // The palette of a palette image, NULL for other images
    ImagePalette *palette;

    bool root_frame_data_loaded;
    id_type ref_id_counter;
    Frame *extra_frames, root_frame;
    uint32_t current_frame_index, frame_id_counter, displayed_frame_id;
    uint64_t animation_duration;
    size_t extra_framecnt;
    monotonic_t atime;
    size_t used_storage;
    bool is_drawn;
    AnimationState animation_state;
    uint32_t max_loops, current_loop;
    monotonic_t current_frame_shown_at;
    ref_map refs_by_internal_id;
    ref_map refs_by_client_id;  // named placements only; client id 0 can have many refs
    float peak; // of HDR images, see docs/hdr-protocol.rst
    bool tone_map;
    VectorDoc *vector;  // f=200 images are rasterized on demand at the size they are shown at, the document tracks what changed
    VectorView vector_view;  // the part of the image, at the size it is shown at, that its texture holds
    uint8_t gamut;  // the gamut the pixels of every frame are in, see docs/wide-gamut-protocol.rst
    uint8_t transfer;  // the TransferFunction the pixels are encoded with, see docs/silkie/precision-image-protocol.rst
    // The tiles of an image larger than the GPU's largest texture, NULL for one that fits a texture, see graphics.c
    struct ImageTiles *tiles;
    bool drawn_minified;  // a placement shows it smaller than it is, so its textures need mipmaps
} Image;

typedef struct {
    id_type image_id;
    uint32_t frame_id;
} ImageAndFrame;

typedef struct {
    uint8_t *buf;
    size_t buf_capacity, buf_used;

    size_t data_sz;
    uint8_t *data;
    bool is_4byte_aligned;
    bool is_opaque, loading_completed_successfully, is_float;
    uint8_t indexed_px_sz;
    ImagePalette *palette;  // of a palette image being loaded
    uint32_t width, height;
    uint8_t gamut;  // of the loaded pixels
    uint8_t deep, transfer;  // the DeepStorage and TransferFunction of the loaded pixels
    GraphicsCommand start_command;
    ImageAndFrame loading_for;
} LoadData;

#define NAME image_map
#define KEY_TY id_type
#define VAL_TY Image *
#include "kitty-verstable.h"

typedef struct {
    PyObject_HEAD

    LoadData currently_loading;
    PyObject *vector_font_session, *vector_response;
    id_type image_id_counter;
    struct {
        size_t count, capacity;
        ImageRenderData *item;
        bool has_hdr, has_visible_raster;
    } render_data;
    bool layers_dirty;
    // The number of images below MIN_ZINDEX / 2, then the number of refs between MIN_ZINDEX / 2 and -1 inclusive, then the number of refs above 0 inclusive.
    size_t num_of_below_refs, num_of_negative_refs, num_of_positive_refs;
    // The number of visible refs to indexed color and palette images
    size_t num_of_indexed_refs;
    unsigned int last_scrolled_by;
    float last_scroll_offset_lines;
    PaneMagnification last_magnification;
    double last_raster_scale;
    size_t used_storage;  // the bytes of image data held, which tells how much evicting images frees, see image_alloc()
    // For the tests: the number of allocations of image data still to fail, as if memory had run out
    unsigned int simulated_alloc_failures;
    id_type busy_image;  // the image a command is working on, which running out of memory does not evict
    PyObject *disk_cache;
    bool has_images_needing_animation, context_made_current_for_this_command;
    id_type window_id;
    image_map images_by_internal_id;
} GraphicsManager;
#else
typedef struct {
    int x;
} *GraphicsManager;
#endif

typedef struct {
    int32_t amt, limit;
    index_type margin_top, margin_bottom;
    bool has_margins;
    // The history scrolls with the screen: lines move into it (amt < 0) or are popped back from it (amt > 0)
    bool scroll_history;
} ScrollData;


static inline float
gl_size(const unsigned int sz, const unsigned int viewport_size) {
    // convert pixel sz to OpenGL coordinate system.
    const float px = 2.f / viewport_size;
    return px * sz;
}

static inline float
gl_pos_x(const int px_from_left_margin, const unsigned int viewport_size) {
    const float px = 2.f / viewport_size;
    return -1.f + px_from_left_margin * px;
}

static inline float
tex_pos_x(const int px_from_left_margin, const unsigned texture_width) {
    return px_from_left_margin / (float)texture_width;
}

static inline float
gl_pos_y(const int px_from_top_margin, const unsigned int viewport_size) {
    const float px = 2.f / viewport_size;
    return 1.f - px_from_top_margin * px;
}

static inline float
tex_pos_y(const int px_from_top_margin, const unsigned texture_height) {
    const int px_from_bottom_margin = texture_height - px_from_top_margin;
    return px_from_bottom_margin / (float)texture_height;
}


typedef struct GraphicsRenderData {
    size_t count, capacity, num_of_below_refs, num_of_negative_refs, num_of_positive_refs, num_of_indexed_refs;
    ImageRenderData *images;
} GraphicsRenderData;

GraphicsManager *grman_alloc(bool for_paused_rendering);
void grman_share_vector_fonts(GraphicsManager *dest, GraphicsManager *source);
void grman_clear(GraphicsManager *, bool, CellPixelSize fg);
const char *grman_handle_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, Cursor *c, bool *is_dirty, CellPixelSize fg);
bool grman_set_image_annotation(GraphicsManager *self, uint32_t image, uint32_t placement, PyObject *annotation);
void grman_put_cell_image(
    GraphicsManager *self,
    uint32_t row,
    uint32_t col,
    uint32_t image_id,
    uint32_t placement_id,
    uint32_t x,
    uint32_t y,
    uint32_t w,
    uint32_t h,
    CellPixelSize cell);
bool grman_update_layers(
    GraphicsManager *self,
    unsigned int scrolled_by,
    float scroll_offset_lines,
    float screen_left,
    float screen_top,
    float dx,
    float dy,
    unsigned int num_cols,
    unsigned int num_rows,
    CellPixelSize);
bool grman_update_magnified_layers(
    GraphicsManager*, unsigned int, float, float, float, float, float,
    unsigned int, unsigned int, CellPixelSize, PaneMagnification, double);
bool grman_has_visible_raster(const GraphicsManager *self);
// Terminal-side ink, see docs/ink-protocol.rst
typedef struct InkView {  // how the cells of a window are shown, which is what places an image in it
    CellPixelSize cell;
    unsigned scrolled_by;
    double scroll_offset;  // in pixels
} InkView;
PyObject *grman_image_descriptions(GraphicsManager *self, const InkView *view, unsigned rows, unsigned columns);
typedef struct InkPoint {  // a position in pixels from the top left of the cell area and the width of the stroke there
    double x, y, width;
} InkPoint;
typedef struct InkStrokeTarget {  // the image and the placement of it a stroke began in
    id_type image, placement;
} InkStrokeTarget;
bool grman_ink_start(GraphicsManager *self, const InkView *view, uint32_t image, uint32_t group, uint32_t element, const char *fill, InkPoint p, InkStrokeTarget *target);
bool grman_ink_append(GraphicsManager *self, const InkView *view, InkStrokeTarget target, uint32_t group, uint32_t element, InkPoint p);
void grman_ink_remove(GraphicsManager *self, InkStrokeTarget target, uint32_t element);
void grman_scroll_images(GraphicsManager *self, const ScrollData *, CellPixelSize fg);
bool grman_has_any_images(GraphicsManager *self);
bool grman_has_hdr_images(GraphicsManager *self);
void grman_resize(GraphicsManager *, index_type, index_type, index_type, index_type, index_type, index_type);
void grman_rescale(GraphicsManager *self, CellPixelSize fg);
void grman_remove_cell_images(GraphicsManager *self, int32_t top, int32_t bottom);
void grman_remove_all_cell_images(GraphicsManager *self);
void gpu_data_for_image(ImageRenderData *ans, float left, float top, float right, float bottom);
bool png_from_file_pointer(FILE *fp, const char *path, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz);
bool png_path_to_bitmap(const char *path, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz);
bool
png_from_data(void *png_data, size_t png_data_sz, const char *path_for_error_messages, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz);
bool image_path_to_bitmap(const char *path, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz);
bool scan_active_animations(GraphicsManager *self, const monotonic_t now, monotonic_t *minimum_gap, bool os_window_context_set);
void grman_pause_rendering(GraphicsManager *self, GraphicsManager *dest);
PyObject *grman_render_data_as_python(GraphicsManager *self);
void grman_mark_layers_dirty(GraphicsManager *self);
void grman_set_window_id(GraphicsManager *self, id_type id);
bool grman_has_images(GraphicsManager *self);
bool grman_image_version(GraphicsManager *self, uint32_t client_id, id_type *image, uint64_t *version);
uint8_t *grman_image_rgba(GraphicsManager *self, uint32_t client_id, uint32_t *width, uint32_t *height);
GraphicsRenderData grman_render_data(GraphicsManager *self);
