/*
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once
#include "data-types.h"
#include "monotonic.h"
#include "vector.h"

// Bitmask values for GraphicsCommand.usage_hints
#define GRAPHICS_USAGE_HINT_TRANSIENT 1u

typedef struct {
    unsigned char action, transmission_type, compressed, delete_action;
    uint32_t format, more, id, image_number, data_sz, data_offset, placement_id, quiet, parent_id, parent_placement_id, usage_hints, tone_map, gamut;
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
    bool unicode_placement;
    int32_t offset_from_parent_x, offset_from_parent_y;
} GraphicsCommand;

typedef struct {
    float left, top, right, bottom;
} ImageRect;

typedef struct {
    ImageRect src_rect, dest_rect;
    uint32_t texture_id, group_count;
    int z_index;
    id_type image_id, ref_id;
    float peak; // of HDR images, see docs/hdr-protocol.rst
    bool tone_map;
    uint32_t gamut;  // the gamut of the pixels, see docs/wide-gamut-protocol.rst
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
    int32_t start_row, start_column;
    uint32_t client_id;
    ImageRect src_rect;
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
} Frame;

typedef enum { ANIMATION_STOPPED = 0, ANIMATION_LOADING = 1, ANIMATION_RUNNING = 2 } AnimationState;

typedef struct TextureRef {
    uint32_t id, refcnt;
} TextureRef;

#define NAME ref_map
#define KEY_TY id_type
#define VAL_TY ImageRef *
#include "kitty-verstable.h"

typedef struct {
    uint32_t client_id, client_number, width, height;
    TextureRef *texture;
    id_type internal_id;
    // A GPU surface shared by a local client, see t=g in the graphics protocol
    void *surface;
    uint32_t surface_texture;
    bool surface_is_bgra;

    bool root_frame_data_loaded;
    id_type ref_id_counter;
    Frame *extra_frames, root_frame;
    uint32_t current_frame_index, frame_id_counter;
    uint64_t animation_duration;
    size_t extra_framecnt;
    monotonic_t atime;
    size_t used_storage;
    bool is_drawn;
    AnimationState animation_state;
    uint32_t max_loops, current_loop;
    monotonic_t current_frame_shown_at;
    ref_map refs_by_internal_id;
    float peak; // of HDR images, see docs/hdr-protocol.rst
    bool tone_map;
    VectorDoc *vector;  // f=200 images are rasterized on demand at the size they are shown at, the document tracks what changed
    uint32_t vector_tex_width, vector_tex_height;
    uint8_t gamut;  // the gamut the pixels of every frame are in, see docs/wide-gamut-protocol.rst
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
    uint32_t width, height;
    uint8_t gamut;  // of the loaded pixels
    GraphicsCommand start_command;
    ImageAndFrame loading_for;
} LoadData;

#define NAME image_map
#define KEY_TY id_type
#define VAL_TY Image *
#include "kitty-verstable.h"

typedef struct {
    PyObject_HEAD

        size_t storage_limit;
    LoadData currently_loading;
    PyObject *vector_font_session, *vector_response;
    id_type image_id_counter;
    struct {
        size_t count, capacity;
        ImageRenderData *item;
        bool has_hdr;
    } render_data;
    bool layers_dirty;
    // The number of images below MIN_ZINDEX / 2, then the number of refs between MIN_ZINDEX / 2 and -1 inclusive, then the number of refs above 0 inclusive.
    size_t num_of_below_refs, num_of_negative_refs, num_of_positive_refs;
    unsigned int last_scrolled_by;
    float last_scroll_offset_lines;
    size_t used_storage;
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
    size_t count, capacity, num_of_below_refs, num_of_negative_refs, num_of_positive_refs;
    ImageRenderData *images;
} GraphicsRenderData;

GraphicsManager *grman_alloc(bool for_paused_rendering);
void grman_share_vector_fonts(GraphicsManager *dest, GraphicsManager *source);
void grman_clear(GraphicsManager *, bool, CellPixelSize fg);
const char *grman_handle_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, Cursor *c, bool *is_dirty, CellPixelSize fg);
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
// Terminal-side ink, see docs/ink-protocol.rst
typedef struct InkView {  // how the cells of a window are shown, which is what places an image in it
    CellPixelSize cell;
    unsigned scrolled_by;
    double scroll_offset;  // in pixels
} InkView;
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
void grman_mark_layers_dirty(GraphicsManager *self);
void grman_set_window_id(GraphicsManager *self, id_type id);
bool grman_has_images(GraphicsManager *self);
bool grman_image_version(GraphicsManager *self, uint32_t client_id, id_type *image, uint32_t *frame);
uint8_t *grman_image_rgba(GraphicsManager *self, uint32_t client_id, uint32_t *width, uint32_t *height);
GraphicsRenderData grman_render_data(GraphicsManager *self);
