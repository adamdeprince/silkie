/*
 * graphics.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#define GRAPHICS_INTERNAL_APIS
#include "graphics.h"
#include "state.h"
#include "disk-cache.h"
#include "iqsort.h"
#include "safe-wrappers.h"
#include "simd-string.h"
#ifdef __APPLE__
#include "iosurface.h"
#endif
#ifdef __linux__
#include "dmabuf.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>

#include <zlib.h>
#include <structmember.h>
#include "png-reader.h"
PyTypeObject GraphicsManager_Type;

#define MAX_IMAGE_DIMENSION 10000u
#define DEFAULT_STORAGE_LIMIT 320u * (1024u * 1024u)
#define REPORT_ERROR(...) \
    { log_error(__VA_ARGS__); }
#define MAX_DATA_SZ (4u * 100000000u)
#define RAII_CoalescedFrameData(name, initializer) __attribute__((cleanup(cfd_free))) CoalescedFrameData name = initializer

// caching {{{
#define member_size(type, member) sizeof(((type *)0)->member)
#define CACHE_KEY_BUFFER_SIZE (member_size(ImageAndFrame, image_id) + member_size(ImageAndFrame, frame_id))

static size_t
cache_key(const ImageAndFrame x, char *key) {
    memcpy(key, &x.image_id, sizeof(x.image_id));
    memcpy(key + sizeof(x.image_id), &x.frame_id, sizeof(x.frame_id));
    return CACHE_KEY_BUFFER_SIZE;
}
#define CK(x) key, cache_key(x, key)

// Transfers ownership of *data (a malloc() allocated pointer) to the cache,
// setting it to NULL, on success. This avoids copying what are often many
// megabytes of image data. Note that *data must not be used afterwards, the
// cache write thread can free it at any time.
static bool
add_to_cache(GraphicsManager *self, const ImageAndFrame x, void **data, const size_t sz, bool memory_only) {
    char key[CACHE_KEY_BUFFER_SIZE];
    return add_to_disk_cache_move(self->disk_cache, CK(x), data, sz, memory_only);
}

static bool
remove_from_cache(GraphicsManager *self, const ImageAndFrame x) {
    char key[CACHE_KEY_BUFFER_SIZE];
    return remove_from_disk_cache(self->disk_cache, CK(x));
}

static bool
read_from_cache(const GraphicsManager *self, const ImageAndFrame x, void **data, size_t *sz) {
    char key[CACHE_KEY_BUFFER_SIZE];
    return read_from_disk_cache_simple(self->disk_cache, CK(x), data, sz, false);
}

static size_t
cache_size(const GraphicsManager *self) {
    return disk_cache_total_size(self->disk_cache);
}
#undef CK
// }}}


static inline id_type
next_id(id_type *counter) {
    id_type ans = ++(*counter);
    if (UNLIKELY(ans == 0)) ans = ++(*counter);
    return ans;
}
static const unsigned PARENT_DEPTH_LIMIT = 8;

GraphicsManager *
grman_alloc(bool for_paused_rendering) {
    GraphicsManager *self = (GraphicsManager *)GraphicsManager_Type.tp_alloc(&GraphicsManager_Type, 0);
    self->render_data.capacity = 64;
    self->render_data.item = calloc(self->render_data.capacity, sizeof(self->render_data.item[0]));
    self->storage_limit = DEFAULT_STORAGE_LIMIT;
    self->vector_font_session = PyDict_New();
    if (!self->vector_font_session) { Py_CLEAR(self); return NULL; }
    if (self->render_data.item == NULL) {
        PyErr_NoMemory();
        Py_CLEAR(self);
        return NULL;
    }
    if (!for_paused_rendering) {
        self->disk_cache = create_disk_cache();
        if (!self->disk_cache) {
            Py_CLEAR(self);
            return NULL;
        }
    }
    vt_init(&self->images_by_internal_id);
    return self;
}

#define iter_refs(img) vt_create_for_loop(ref_map_itr, i, &((img)->refs_by_internal_id))

static void
free_refs_data(Image *img) {
    iter_refs(img) free(i.data->val);
    vt_cleanup(&img->refs_by_internal_id);
    vt_cleanup(&img->refs_by_client_id);
}

// Palettes of palette images, see docs/silkie/palette-image-protocol.rst {{{
static ImagePalette *
alloc_palette(uint32_t slots) {
    // a palette has at most 256 slots, so its size cannot overflow
    ImagePalette *ans = calloc(1, sizeof(ImagePalette) + (size_t)slots * 4);
    if (ans) {
        ans->refcnt = 1;
        ans->slots = slots;
    }
    return ans;
}

static void
release_palette(ImagePalette **p) {
    if (!*p) return;
    if ((*p)->refcnt < 2) {
        if ((*p)->texture) free_texture(&(*p)->texture);
        free(*p);
    } else (*p)->refcnt--;
    *p = NULL;
}

static ImagePalette *
incref_palette(ImagePalette *p) {
    if (p) p->refcnt++;
    return p;
}

// The palette of the image, made its own before it is changed, so that a copy
// of the image made for paused rendering keeps showing the old colors
static ImagePalette *
palette_for_change(Image *img) {
    ImagePalette *p = img->palette;
    if (p->refcnt < 2) return p;
    ImagePalette *copy = alloc_palette(p->slots);
    if (!copy) return NULL;
    memcpy(copy->rgba, p->rgba, (size_t)p->slots * 4);
    copy->version = p->version;
    p->refcnt--;
    img->palette = copy;
    return copy;
}
// }}}

static void
free_load_data(LoadData *ld) {
    release_palette(&ld->palette);
    free(ld->buf);
    ld->buf_used = 0;
    ld->buf_capacity = 0;
    ld->buf = NULL;
    ld->data = NULL;
    ld->loading_for = (const ImageAndFrame){0};
}

// Moves the loaded image data into the cache. ld->data always aliases ld->buf,
// so the load data is left empty on success.
static bool
add_load_data_to_cache(GraphicsManager *self, const ImageAndFrame x, bool memory_only) {
    LoadData *ld = &self->currently_loading;
    void *data = ld->data;
    if (!add_to_cache(self, x, &data, ld->data_sz, memory_only)) return false;
    ld->buf = NULL;
    ld->data = NULL;
    ld->buf_used = 0;
    ld->buf_capacity = 0;
    return true;
}

static void *
clear_texture_ref(TextureRef **x) {
    if (*x) {
        if ((*x)->refcnt < 2) {
            if ((*x)->id) free_texture(&(*x)->id);
            free(*x);
            *x = NULL;
        } else (*x)->refcnt--;
    }
    return NULL;
}

static TextureRef *
incref_texture_ref(TextureRef *ref) {
    if (ref) ref->refcnt++;
    return ref;
}

// For the tests: the allocations that can be made to fail with fail_next_graphics_allocation(), as when out of memory
typedef enum {
    ALLOCATION_NONE,
    ALLOCATION_INFLATE,
    ALLOCATION_IMAGE,
    ALLOCATION_TEXTURE,
    ALLOCATION_REF,
    ALLOCATION_SORTED,
    ALLOCATION_CLIENT_IDS,
    ALLOCATION_RENDER_DATA,
} Allocation;
static const char *allocation_names[] = {"", "inflate", "image", "texture", "ref", "sorted", "client_ids", "render_data"};
static Allocation failing_allocation = ALLOCATION_NONE;

// Whether the tests made this allocation fail, which it does once
static bool
allocation_fails(Allocation which) {
    if (failing_allocation != which) return false;
    failing_allocation = ALLOCATION_NONE;
    return true;
}

// NULL when out of memory
static TextureRef *
new_texture_ref(void) {
    TextureRef *ans = allocation_fails(ALLOCATION_TEXTURE) ? NULL : calloc(1, sizeof(TextureRef));
    if (ans) ans->refcnt = 1;
    return ans;
}

static uint32_t
texture_id_for_img(Image *img) {
    return img->texture ? img->texture->id : 0;
}

static void
free_image_resources(GraphicsManager *self, Image *img) {
    clear_texture_ref(&img->texture);
    clear_texture_ref(&img->vector_right_texture);
    release_palette(&img->palette);
    vector_doc_free(img->vector);
    img->vector = NULL;
#if defined(__APPLE__) || defined(__linux__)
    if (img->surface) {
        if (img->surface_texture) free_texture(&img->surface_texture);
#ifdef __APPLE__
        iosurface_release(img->surface);
#else
        dmabuf_release(img->surface);
#endif
        img->surface = NULL;
    }
#endif
    if (self->disk_cache) {
        ImageAndFrame key = {.image_id = img->internal_id, .frame_id = img->root_frame.id};
        if (!remove_from_cache(self, key) && PyErr_Occurred()) PyErr_Print();
        for (unsigned i = 0; i < img->extra_framecnt; i++) {
            key.frame_id = img->extra_frames[i].id;
            if (!remove_from_cache(self, key) && PyErr_Occurred()) PyErr_Print();
        }
    }
    if (img->extra_frames) {
        free(img->extra_frames);
        img->extra_frames = NULL;
    }
    free_refs_data(img);
    self->used_storage = img->used_storage <= self->used_storage ? self->used_storage - img->used_storage : 0;
}

static void
free_image(GraphicsManager *self, Image *img) {
    free_image_resources(self, img);
    free(img);
}

#define iter_images(grman) vt_create_for_loop(image_map_itr, i, &((grman)->images_by_internal_id))

static void
free_all_images(GraphicsManager *self) {
    iter_images(self) free_image(self, i.data->val);
    vt_cleanup(&self->images_by_internal_id);
}

static void
dealloc(GraphicsManager *self) {
    free_all_images(self);
    free(self->render_data.item);
    Py_CLEAR(self->disk_cache);
    free_load_data(&self->currently_loading);
    Py_CLEAR(self->vector_response);
    Py_CLEAR(self->vector_font_session);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

void
grman_share_vector_fonts(GraphicsManager *dest, GraphicsManager *source) {
    Py_SETREF(dest->vector_font_session, Py_NewRef(source->vector_font_session));
}

static Image *
img_by_internal_id(const GraphicsManager *self, id_type id) {
    image_map_itr i = vt_get((image_map *)&self->images_by_internal_id, id);
    return vt_is_end(i) ? NULL : i.data->val;
}

static Image *
img_by_client_id(const GraphicsManager *self, uint32_t id) {
    iter_images(((GraphicsManager *)self)) if (i.data->val->client_id == id) return i.data->val;
    return NULL;
}

static Image *
img_by_client_number(const GraphicsManager *self, uint32_t number) {
    // get the newest image with the specified number
    Image *ans = NULL;
    iter_images(((GraphicsManager *)self)) {
        Image *img = i.data->val;
        if (img->client_number == number && (!ans || img->internal_id > ans->internal_id)) ans = img;
    }
    return ans;
}

static ImageRef *
ref_by_internal_id(const Image *img, id_type id) {
    ref_map_itr i = vt_get(&((Image *)img)->refs_by_internal_id, id);
    return vt_is_end(i) ? NULL : i.data->val;
}


static ImageRef *
ref_by_client_id(const Image *img, uint32_t id) {
    if (id) {
        ref_map_itr i = vt_get(&((Image *)img)->refs_by_client_id, id);
        return vt_is_end(i) ? NULL : i.data->val;
    }
    iter_refs((Image *)img) if (!i.data->val->client_id) return i.data->val;
    return NULL;
}

static void
set_layers_dirty(GraphicsManager *self) {
    self->layers_dirty = true;
}

static image_map_itr
remove_image_itr(GraphicsManager *self, image_map_itr i) {
    const uint32_t id = i.data->val->client_id;
    iter_images(self) if (i.data->val->vector) vector_doc_image_changed(i.data->val->vector, id);
    free_image(self, i.data->val);
    set_layers_dirty(self);
    return vt_erase_itr(&self->images_by_internal_id, i);
}

static bool
invalidate_vector_users(GraphicsManager *self, uint32_t id) {
    bool changed = false;
    iter_images(self) if (i.data->val->vector && vector_doc_image_changed(i.data->val->vector, id)) changed = true;
    if (changed) set_layers_dirty(self);
    return changed;
}

static void
remove_image(GraphicsManager *self, Image *img) {
    image_map_itr i = vt_get(&self->images_by_internal_id, img->internal_id);
    if (!vt_is_end(i)) remove_image_itr(self, i);
}

static void
remove_images(GraphicsManager *self, bool (*predicate)(Image *), id_type skip_image_internal_id) {
    for (image_map_itr i = vt_first(&self->images_by_internal_id); !vt_is_end(i);) {
        Image *img = i.data->val;
        if (img->internal_id != skip_image_internal_id && predicate(img)) i = remove_image_itr(self, i);
        else i = vt_next(i);
    }
}

void
grman_pause_rendering(GraphicsManager *self, GraphicsManager *dest) {
    make_window_context_current(dest->window_id);
    free_all_images(dest);
    dest->render_data.count = 0;
    dest->num_of_below_refs = 0;
    dest->num_of_negative_refs = 0;
    dest->num_of_positive_refs = 0;
    dest->num_of_indexed_refs = 0;
    if (self == NULL) return;
    dest->window_id = self->window_id;
    dest->layers_dirty = true;
    // Copy the visible ref counts so that the decision on whether to render in
    // layers, which is made before the render data is rebuilt, is correct for
    // the first paused frame. See https://github.com/kovidgoyal/kitty/issues/10538
    dest->num_of_below_refs = self->num_of_below_refs;
    dest->num_of_negative_refs = self->num_of_negative_refs;
    dest->num_of_positive_refs = self->num_of_positive_refs;
    dest->last_scrolled_by = 0;
    dest->last_scroll_offset_lines = 0.0f;

    iter_images(self) {
        Image *clone = calloc(1, sizeof(Image)), *img = i.data->val;
        if (!clone) continue;
        memcpy(clone, img, sizeof(*clone));
        memset(&clone->refs_by_internal_id, 0, sizeof(clone->refs_by_internal_id));
        vt_init(&clone->refs_by_internal_id);
        memset(&clone->refs_by_client_id, 0, sizeof(clone->refs_by_client_id));
        vt_init(&clone->refs_by_client_id);
        clone->extra_frames = NULL;
        clone->vector = NULL;  // the texture is shared, the document stays with the live manager
        // The GPU surface of a t=g image stays with the image: what is drawn
        // while paused is the texture, shared by reference count below. A
        // clone holding the surface too would release it a second time when
        // it is freed, whether or not the image still exists.
        clone->surface = NULL;
        clone->surface_texture = 0;
        iter_refs(img) {
            ImageRef *cr = malloc(sizeof(ImageRef));
            if (cr) {
                memcpy(cr, i.data->val, sizeof(*cr));
                if (vt_is_end(vt_insert(&clone->refs_by_internal_id, cr->internal_id, cr))) fatal("Out of memory");
                if (cr->client_id && vt_is_end(vt_insert(&clone->refs_by_client_id, cr->client_id, cr))) fatal("Out of memory");
            }
        }
        clone->texture = incref_texture_ref(img->texture);
        clone->vector_right_texture = incref_texture_ref(img->vector_right_texture);
        clone->palette = incref_palette(img->palette);
        vt_insert(&dest->images_by_internal_id, clone->internal_id, clone);
    }
}

// Loading image data {{{

static bool
trim_predicate(Image *img) {
    return !img->root_frame_data_loaded || !vt_size(&img->refs_by_internal_id);
}

static void
apply_storage_quota(GraphicsManager *self, size_t storage_limit, id_type currently_added_image_internal_id) {
    // First remove unreferenced images, even if they have an id
    remove_images(self, trim_predicate, currently_added_image_internal_id);
    if (self->used_storage < storage_limit) return;
    size_t num_images = vt_size(&self->images_by_internal_id);
#define transient_or_older_first(a, b) \
    ((*a)->root_frame.transient != (*b)->root_frame.transient ? (*a)->root_frame.transient > (*b)->root_frame.transient : (*a)->atime < (*b)->atime)
    RAII_ALLOC(Image *, sorted, allocation_fails(ALLOCATION_SORTED) || num_images > SIZE_MAX / sizeof(Image *) ? NULL : malloc(num_images * sizeof(Image *)));
    if (!sorted) {
        // Out of memory: remove the images in the same order, finding the first that is left each time
        while (self->used_storage > storage_limit && vt_size(&self->images_by_internal_id)) {
            Image *first = NULL;
            iter_images(self) {
                Image *img = i.data->val;
                if (!first || transient_or_older_first(&img, &first)) first = img;
            }
            remove_image(self, first);
        }
        if (!vt_size(&self->images_by_internal_id)) self->used_storage = 0; // sanity check
        return;
    }
    Image **p = sorted;
    iter_images(self) { *p++ = i.data->val; }
    QSORT(Image *, sorted, num_images, transient_or_older_first);
#undef transient_or_older_first

    for (p = sorted; self->used_storage > storage_limit && num_images; p++, num_images--) remove_image(self, *p);
    if (!num_images || !vt_size(&self->images_by_internal_id)) self->used_storage = 0; // sanity check
}

static char command_response[512] = {0};

static void
set_command_failed_response(const char *code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const size_t sz = sizeof(command_response) / sizeof(command_response[0]);
    const int num = snprintf(command_response, sz, "%s:", code);
    vsnprintf(command_response + num, sz - num, fmt, args);
    va_end(args);
}

// Decode formats {{{
#define ABRT(code, ...)                                  \
    {                                                    \
        set_command_failed_response(#code, __VA_ARGS__); \
        goto err;                                        \
    }

// Reading an image file can fail for many different reasons: the file does
// not exist, it is not readable, it is of the wrong type, it is smaller than
// the client claimed, etc. The client can be a program running on a remote
// machine over SSH or a sandboxed process, so telling it which of these
// happened lets it probe the filesystem for the existence, type and size of
// files it has no business knowing anything about. So all such failures are
// answered back with one identical, information free, error and the actual
// reason is written to the kitty log, which only the local user can see.
#define IMAGE_FILE_ERROR_CODE "EBADF"
#define IMAGE_FILE_ERROR_MSG "Failed to read image file"
#define FAIL_IMAGE_FILE(...)                                                            \
    {                                                                                   \
        log_error(__VA_ARGS__);                                                         \
        set_command_failed_response(IMAGE_FILE_ERROR_CODE, "%s", IMAGE_FILE_ERROR_MSG); \
    }
// As above but for use in functions that clean up via a goto err; label.
#define FABRT(...)                    \
    {                                 \
        FAIL_IMAGE_FILE(__VA_ARGS__); \
        goto err;                     \
    }

#ifdef __APPLE__
// On macOS POSIX shared memory objects cannot be read() from, they can only be
// accessed via mmap(). Unlike with regular files, there is no risk of SIGBUS
// from a client shrinking the object while we copy from it, since macOS does
// not allow shared memory objects to be resized once created.
static bool
copy_from_shm(GraphicsManager *self, int fd, size_t sz, off_t offset) {
    LoadData *ld = &self->currently_loading;
    if (!sz) return true; // mmap() of a zero sized region fails
    const off_t page_start = offset - (offset % sysconf(_SC_PAGESIZE));
    const size_t delta = (size_t)(offset - page_start), map_sz = sz + delta;
    void *addr = mmap(0, map_sz, PROT_READ, MAP_SHARED, fd, page_start);
    if (addr == MAP_FAILED)
        FABRT("Failed to map shm object fd: %d at offset: %zd with size: %zu with error: [%d] %s", fd, (ssize_t)page_start, map_sz, errno, strerror(errno));
    memcpy(ld->buf, (uint8_t *)addr + delta, sz);
    munmap(addr, map_sz);
    ld->buf_used = sz;
    return true;
err:
    return false;
}
#endif

static bool
read_img_file(GraphicsManager *self, int fd, size_t sz, off_t offset, size_t max_to_read, bool is_shm) {
    LoadData *ld = &self->currently_loading;
    struct stat s;
    if (fstat(fd, &s) != 0) FABRT("Failed to fstat() the fd: %d file with error: [%d] %s", fd, errno, strerror(errno));
    // The graphics protocol specification mandates that only regular files be
    // read. Reading from FIFOs/devices/etc. can block forever or have
    // side-effects. POSIX shared memory fds on macOS do not report as regular
    // files via fstat(), so skip this check for them.
    if (!is_shm && !S_ISREG(s.st_mode)) FABRT("The image file with fd: %d is not a regular file", fd);
    const size_t available = offset < s.st_size ? (size_t)(s.st_size - offset) : 0;
    if (!sz) sz = available;
    if (sz > max_to_read) sz = max_to_read;
    free(ld->buf);
    // Note that for files the data is copied rather than mmap()ed as the
    // client is free to truncate the file at any time, which would cause
    // SIGBUS when reading from a mapping of it. With read() a truncated file
    // simply results in a short read, which is reported as insufficient data
    // by the caller.
    ld->buf = malloc(sz ? sz : 1);
    if (!ld->buf) FABRT("Out of memory allocating %zu bytes to read image file", sz);
    ld->buf_capacity = sz;
    ld->buf_used = 0;
#ifdef __APPLE__
    // Copying more than the shared memory object contains would read past its
    // end, a smaller object is reported as insufficient data by the caller.
    if (is_shm) {
        if (!copy_from_shm(self, fd, MIN(sz, available), offset)) goto err;
    } else
#endif
    {
        while (ld->buf_used < sz) {
            ssize_t n = pread(fd, ld->buf + ld->buf_used, sz - ld->buf_used, offset + (off_t)ld->buf_used);
            if (n < 0) {
                if (errno == EINTR) continue;
                FABRT("Failed to read from image file fd: %d with error: [%d] %s", fd, errno, strerror(errno));
            }
            if (!n) break; // EOF, the file is smaller than expected, reported as insufficient data by the caller
            ld->buf_used += n;
        }
    }
    return true;
err:
    // Discard any partially read data, the caller is not guaranteed to call
    // free_load_data() on failure.
    free(ld->buf);
    ld->buf = NULL;
    ld->buf_capacity = 0;
    ld->buf_used = 0;
    return false;
}


static const char *
zlib_strerror(int ret) {
#define Z(x) \
    case x: return #x;
    static char buf[128];
    switch (ret) {
        case Z_ERRNO: return strerror(errno);
        default:
            snprintf(buf, sizeof(buf) / sizeof(buf[0]), "Unknown error: %d", ret);
            return buf;
            Z(Z_STREAM_ERROR);
            Z(Z_DATA_ERROR);
            Z(Z_MEM_ERROR);
            Z(Z_BUF_ERROR);
            Z(Z_VERSION_ERROR);
    }
#undef Z
}

// Decompresses the image data into a buffer of exactly the expected size, data_sz. zlib counts the bytes of one call
// in a uInt, so the input and output are given to it in parts that each fit in one, for data of any size. Fails the
// command, never aborts, when out of memory.
static bool
inflate_zlib(LoadData *load_data, uint8_t *buf, size_t bufsz) {
    bool ok = false, initialized = false;
    z_stream z = {.zalloc = Z_NULL, .zfree = Z_NULL, .opaque = Z_NULL};
    const size_t expected = load_data->data_sz;
    // malloc(0) may return NULL, which is not a failure
    uint8_t *decompressed = allocation_fails(ALLOCATION_INFLATE) ? NULL : malloc(MAX(expected, (size_t)1));
    if (decompressed == NULL) ABRT(ENOMEM, "Out of memory allocating %zu bytes for the decompressed image data", expected);
    int ret;
    if ((ret = inflateInit(&z)) != Z_OK) ABRT(ENOMEM, "Failed to initialize inflate with error: %s", zlib_strerror(ret));
    initialized = true;
    size_t consumed = 0, produced = 0;
    do {
        if (!z.avail_in && consumed < bufsz) {
            const size_t part = MIN(bufsz - consumed, (size_t)UINT_MAX);
            z.next_in = (Bytef *)buf + consumed;
            z.avail_in = (uInt)part;
            consumed += part;
        }
        const uInt room = (uInt)MIN(expected - produced, (size_t)UINT_MAX);
        z.next_out = decompressed + produced;
        z.avail_out = room;
        ret = inflate(&z, Z_NO_FLUSH);
        produced += room - z.avail_out;
        // Z_BUF_ERROR: no progress is possible, either the output is full before the end of the stream or the input
        // ended before it
        if (ret == Z_BUF_ERROR && produced == expected) ABRT(EINVAL, "Image data size post inflation does not match expected size");
        if (ret == Z_MEM_ERROR) ABRT(ENOMEM, "Out of memory inflating image data");
    } while (ret == Z_OK);
    if (ret != Z_STREAM_END) ABRT(EINVAL, "Failed to inflate image data with error: %s", zlib_strerror(ret));
    if (produced != expected) ABRT(EINVAL, "Image data size post inflation does not match expected size");
    free_load_data(load_data);
    load_data->buf_capacity = expected;
    load_data->buf = decompressed;
    load_data->buf_used = expected;
    ok = true;
err:
    if (initialized) inflateEnd(&z);
    if (!ok) free(decompressed);
    return ok;
}

// Inflates data of unknown size, for text formats. The buffer grows as the
// data comes out, rather than being sized from the compressed data, and
// running out of memory fails the command, not the terminal.
static bool
inflate_zlib_unbounded(LoadData *load_data, uint8_t *buf, size_t bufsz) {
    bool ok = false;
    z_stream z = {0};
    size_t cap = 4096, used = 0, consumed = 0;
    uint8_t *decompressed = malloc(cap);
    if (decompressed == NULL) { set_command_failed_response("ENOMEM", "Out of memory allocating decompression buffer"); return false; }
    int ret;
    if ((ret = inflateInit(&z)) != Z_OK) ABRT(ENOMEM, "Failed to initialize inflate with error: %s", zlib_strerror(ret));
    do {
        // zlib's per-call counters are uInt; the complete stream need not fit.
        if (!z.avail_in && consumed < bufsz) {
            const size_t part = MIN(bufsz-consumed, (size_t)UINT_MAX);
            z.next_in = buf+consumed;
            z.avail_in = (uInt)part;
            consumed += part;
        }
        if (used == cap) {
            if (cap >= PTRDIFF_MAX) ABRT(EOVERFLOW, "Decompressed vector exceeds address space");
            cap = cap > PTRDIFF_MAX / 2 ? PTRDIFF_MAX : cap * 2;
            uint8_t *n = realloc(decompressed, cap);
            if (n == NULL) ABRT(ENOMEM, "Out of memory allocating decompression buffer");
            decompressed = n;
        }
        const uInt available = (uInt)MIN(cap-used, (size_t)UINT_MAX);
        z.avail_out = available;
        z.next_out = decompressed + used;
        ret = inflate(&z, Z_NO_FLUSH);
        used += available-z.avail_out;
    } while (ret == Z_OK);
    if (ret != Z_STREAM_END) ABRT(EINVAL, "Failed to inflate image data with error: %s", zlib_strerror(ret));
    free_load_data(load_data);
    load_data->buf_capacity = cap;
    load_data->buf = decompressed;
    load_data->buf_used = used;
    ok = true;
err:
    inflateEnd(&z);
    if (!ok) free(decompressed);
    return ok;
}

static void
png_error_handler(png_read_data *d UNUSED, const char *code, const char *msg) {
    set_command_failed_response(code, "%s", msg);
}

// The gamut of a PNG is what the file says it is, unless it is a frame of an
// image, in which case it is converted to the gamut of that image
static bool
inflate_png(LoadData *load_data, uint8_t *buf, size_t bufsz, const Image *frame_of) {
    png_read_data d = {.err_handler = png_error_handler, .keep_wide_gamut = frame_of == NULL, .convert_to_gamut = frame_of ? frame_of->gamut : 0};
    inflate_png_inner(&d, buf, bufsz, MAX_IMAGE_DIMENSION);
    if (d.ok) {
        free_load_data(load_data);
        load_data->buf = d.decompressed;
        load_data->buf_capacity = d.sz;
        load_data->buf_used = d.sz;
        load_data->data_sz = d.sz;
        load_data->width = d.width;
        load_data->height = d.height;
        load_data->gamut = d.gamut;
    } else free(d.decompressed);
    free(d.row_pointers);
    return d.ok;
}
#undef ABRT
// }}}

static bool
add_trim_predicate(Image *img) {
    return !img->root_frame_data_loaded || (!img->client_id && !vt_size(&img->refs_by_internal_id));
}

static void
print_png_read_error(png_read_data *d, const char *code, const char *msg) {
    if (d->error.used >= d->error.capacity) {
        size_t cap = MAX(2 * d->error.capacity, 1024 + d->error.used);
        char *buf = realloc(d->error.buf, cap);
        if (!buf) return;  // the message is lost, the earlier ones are kept
        d->error.buf = buf;
        d->error.capacity = cap;
    }
    d->error.used += snprintf(d->error.buf + d->error.used, d->error.capacity - d->error.used, "%s: %s ", code, msg);
}

bool
png_from_data(void *png_data, size_t png_data_sz, const char *path_for_error_messages, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz) {
    png_read_data d = {.err_handler = print_png_read_error};
    inflate_png_inner(&d, png_data, png_data_sz, MAX_IMAGE_DIMENSION);
    if (!d.ok) {
        log_error("Failed to decode PNG image at: %s with error: %s", path_for_error_messages, d.error.used > 0 ? d.error.buf : "");
        free(d.decompressed);
        free(d.row_pointers);
        free(d.error.buf);
        return false;
    }
    *data = d.decompressed;
    free(d.row_pointers);
    free(d.error.buf);
    *sz = d.sz;
    *height = d.height;
    *width = d.width;
    return true;
}

bool
png_from_file_pointer(FILE *fp, const char *path_for_error_messages, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz) {
    size_t capacity = 16 * 1024, pos = 0;
    unsigned char *buf = malloc(capacity);
    if (!buf) {
        log_error("Out of memory reading PNG file at: %s", path_for_error_messages);
        return false;
    }
    while (!feof(fp)) {
        if (capacity - pos < 1024) {
            capacity *= 2;
            unsigned char *new_buf = realloc(buf, capacity);
            if (!new_buf) {
                free(buf);
                log_error("Out of memory reading PNG file at: %s", path_for_error_messages);
                return false;
            }
            buf = new_buf;
        }
        pos += fread(buf + pos, sizeof(char), capacity - pos, fp);
        int saved_errno = errno;
        if (ferror(fp) && saved_errno != EINTR) {
            log_error("Failed while reading from file: %s with error: %s", path_for_error_messages, strerror(saved_errno));
            free(buf);
            return false;
        }
    }
    bool ret = png_from_data(buf, pos, path_for_error_messages, data, width, height, sz);
    free(buf);
    return ret;
}

bool
png_path_to_bitmap(const char *path, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        log_error("The PNG image: %s could not be opened with error: %s", path, strerror(errno));
        return false;
    }
    bool ret = png_from_file_pointer(fp, path, data, width, height, sz);
    fclose(fp);
    fp = NULL;
    return ret;
}

bool
image_path_to_bitmap(const char *path, uint8_t **data, unsigned int *width, unsigned int *height, size_t *sz) {
    *data = NULL;
    *sz = 0;
    *width = 0;
    *height = 0;
    RAII_PyObject(module, PyImport_ImportModule("kitty.render_cache"));
#define fail_on_python_error                                                           \
    {                                                                                  \
        log_error("Failed to convert image at %s to bitmap with python error:", path); \
        PyErr_Print();                                                                 \
        return false;                                                                  \
    }
    if (!module) fail_on_python_error;
    RAII_PyObject(irc, PyObject_GetAttrString(module, "default_image_render_cache"));
    if (!irc) fail_on_python_error;
    RAII_PyObject(ret, PyObject_CallFunction(irc, "s", path));
    if (!ret) fail_on_python_error;
    size_t w = PyLong_AsSize_t(PyTuple_GET_ITEM(ret, 0));
    size_t h = PyLong_AsSize_t(PyTuple_GET_ITEM(ret, 1));
    int fd = PyLong_AsLong(PyTuple_GET_ITEM(ret, 2));
#undef fail_on_python_error
    size_t data_size = 8 + w * h * 4;
    *data = mmap(NULL, data_size, PROT_READ, MAP_PRIVATE, fd, 0);
    int saved_errno = errno;
    safe_close(fd, __FILE__, __LINE__);
    if (*data == MAP_FAILED) {
        log_error("Failed to mmap bitmap data for image at %s with error: %s", path, strerror(saved_errno));
        return false;
    }
    *sz = data_size;
    *width = w;
    *height = h;
    return true;
}

static Image *
// The image with the client id, or a new one, NULL when out of memory
find_or_create_image(GraphicsManager *self, uint32_t id, bool *existing) {
    if (id) {
        Image *img = img_by_client_id(self, id);
        if (img) {
            *existing = true;
            return img;
        }
    }
    *existing = false;
    Image *ans = allocation_fails(ALLOCATION_IMAGE) ? NULL : calloc(1, sizeof(Image));
    if (!ans) return NULL;
    if (!(ans->texture = new_texture_ref())) {
        free(ans);
        return NULL;
    }
    ans->internal_id = next_id(&self->image_id_counter);
    vt_init(&ans->refs_by_internal_id);
    vt_init(&ans->refs_by_client_id);
    if (vt_is_end(vt_insert(&self->images_by_internal_id, ans->internal_id, ans))) {
        clear_texture_ref(&ans->texture);
        vt_cleanup(&ans->refs_by_internal_id);
        vt_cleanup(&ans->refs_by_client_id);
        free(ans);
        return NULL;
    }
    return ans;
}

// Zero, which is no client id, when out of memory
static uint32_t
get_free_client_id(const GraphicsManager *self) {
    size_t num_images = vt_size(&((GraphicsManager *)self)->images_by_internal_id);
    if (!num_images) return 1;
    RAII_ALLOC(uint32_t, client_ids, allocation_fails(ALLOCATION_CLIENT_IDS) || num_images > SIZE_MAX / sizeof(uint32_t) ? NULL : malloc(num_images * sizeof(uint32_t)));
    if (!client_ids) return 0;
    size_t count = 0;
    iter_images((GraphicsManager *)self) {
        Image *img = i.data->val;
        if (img->client_id) client_ids[count++] = img->client_id;
    }
    if (!count) return 1;
#define int_lt(a, b) ((*a) < (*b))
    QSORT(uint32_t, client_ids, count, int_lt)
#undef int_lt
    uint32_t prev_id = 0, ans = 1;
    for (size_t i = 0; i < count; i++) {
        if (client_ids[i] == prev_id) continue;
        prev_id = client_ids[i];
        if (client_ids[i] != ans) break;
        ans = client_ids[i] + 1;
    }
    return ans;
}

#define ABRT(code, ...)                                                 \
    {                                                                   \
        set_command_failed_response(code, __VA_ARGS__);                 \
        self->currently_loading.loading_completed_successfully = false; \
        free_load_data(&self->currently_loading);                       \
        return NULL;                                                    \
    }

#define MAX_DATA_SZ (4u * 100000000u)
// Silkie's raw formats (docs/silkie/packed-image-protocol.rst,
// docs/silkie/palette-image-protocol.rst and
// docs/silkie/precision-image-protocol.rst) are the base of their family plus
// their bits per pixel: 300 for gray, 400 for palette images (the bits of each
// index), 500 for RGB and 600 for RGBA. That keeps them clear of the formats
// of the kitty graphics protocol, such as its indexed color formats f=8 and f=16.
#define PACKED_FORMAT_BASE 300u
#define PALETTE_FORMAT_BASE 400u
#define RGB_FORMAT_BASE 500u
#define RGBA_FORMAT_BASE 600u
enum FORMATS {
    INDEXED = 8,
    INDEXED_ALPHA = 16,
    RGB = 24,
    RGBA = 32,
    RGBA16F = 64,
    PNG = 100,
    VECTOR = 200,
    GRAY1 = PACKED_FORMAT_BASE + 1,
    GRAY2 = PACKED_FORMAT_BASE + 2,
    GRAY4 = PACKED_FORMAT_BASE + 4,
    GRAY8 = PACKED_FORMAT_BASE + 8,
    GRAY16 = PACKED_FORMAT_BASE + 16,
    PALETTE1 = PALETTE_FORMAT_BASE + 1,
    PALETTE2 = PALETTE_FORMAT_BASE + 2,
    PALETTE4 = PALETTE_FORMAT_BASE + 4,
    PALETTE8 = PALETTE_FORMAT_BASE + 8,
    RGB565 = RGB_FORMAT_BASE + 16,
    RGB10A2 = RGBA_FORMAT_BASE + 32,
    RGBA16 = RGBA_FORMAT_BASE + 64,
};

static bool
is_gray_format(uint32_t format) {
    return format == GRAY1 || format == GRAY2 || format == GRAY4 || format == GRAY8;
}

// The formats expanded to eight bit RGB when they are loaded
static bool
is_packed_format(uint32_t format) {
    return is_gray_format(format) || format == RGB565;
}

static bool
is_palette_format(uint32_t format) {
    return format == PALETTE1 || format == PALETTE2 || format == PALETTE4 || format == PALETTE8;
}

// The size of the palette at the start of the data of a palette image: every
// one of the 2 ** bits slots, four bytes each
static size_t
palette_size_of_format(uint32_t format) {
    return (size_t)4 << (format - PALETTE_FORMAT_BASE);
}

// The formats kept at their own precision, see docs/silkie/precision-image-protocol.rst
static uint8_t
deep_storage_of_format(uint32_t format) {
    switch (format) {
        case GRAY16: return DEEP_GRAY16;
        case RGB10A2: return DEEP_RGB10A2;
        case RGBA16: return DEEP_RGBA16;
        default: return DEEP_NONE;
    }
}

// The bits per pixel of a raw format, 0 for one that is not raw pixels. For a
// palette format, the bits of the indices after the palette.
static unsigned
bits_per_pixel(uint32_t format) {
    // the number of a Silkie raw format is the base of its family plus its bits per pixel
    if (is_packed_format(format) || is_palette_format(format) || deep_storage_of_format(format)) return format % 100;
    switch (format) {
        // the value of these formats is their bits per pixel
        case INDEXED: case INDEXED_ALPHA: case RGB: case RGBA: case RGBA16F: return format;
        default: return 0;
    }
}

static bool
raw_image_data_size(uint32_t format, uint32_t width, uint32_t height, size_t *size) {
    if (!width || !height) return false;
    size_t row;
    const unsigned bits = bits_per_pixel(format);
    if (!bits) return false;
    if (bits < 8) {
        const unsigned per_byte = 8 / bits;
        row = (size_t)width / per_byte + (width % per_byte != 0);
    } else {
        const unsigned per_pixel = bits / 8;
        if ((size_t)width > SIZE_MAX / per_pixel) return false;
        row = (size_t)width * per_pixel;
    }
    if ((size_t)height > SIZE_MAX / row) return false;
    *size = row * height;
    return true;
}

static inline unsigned
pixel_size(const bool is_opaque, const bool is_float) {
    return is_opaque ? 3 : is_float ? 8 : 4;
}

// The bytes per pixel of the stored data of a frame
static inline unsigned
stored_pixel_size(const bool is_opaque, const bool is_float, const uint8_t deep) {
    return deep ? deep_pixel_size(deep) : pixel_size(is_opaque, is_float);
}

static inline float
half_to_float(const uint16_t h) {
    const unsigned exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    float ans;
    if (exp == 0) ans = ldexpf((float)mant, -24);
    else if (exp == 31) ans = mant ? NAN : INFINITY;
    else ans = ldexpf((float)(0x400 | mant), (int)exp - 25);
    return h & 0x8000 ? -ans : ans;
}

static inline uint16_t
float_to_half(const float f) {
    // round to nearest even, as the hardware conversions do
    uint32_t bits, mant;
    memcpy(&bits, &f, sizeof(bits));
    const uint16_t sign = (bits >> 16) & 0x8000;
    const int exp = (int)((bits >> 23) & 0xff) - 112;
    mant = bits & 0x7fffff;
    if (exp == 143) return sign | 0x7c00 | (mant ? 0x200 : 0); // inf or nan
    if (exp >= 31) return sign | 0x7c00;
    unsigned shift = 13;
    if (exp <= 0) { // subnormal half
        shift = 14 - exp;
        if (shift > 24) return sign;
        mant |= 0x800000;
    }
    uint32_t h = (exp > 0 ? exp << 10 : 0) | (mant >> shift);
    const uint32_t rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rem > half || (rem == half && (h & 1))) h++;
    return sign | h;
}

static float
peak_of_half_data(const uint8_t *data, const size_t sz) {
    // the largest color component, at least SDR white
    float ans = 1.f;
    const uint16_t *p = (const uint16_t *)data;
    for (size_t i = 0; i + 3 < sz / sizeof(uint16_t); i += 4) {
        for (unsigned c = 0; c < 3; c++) {
            const float v = half_to_float(p[i + c]);
            if (v > ans && v < INFINITY) ans = v;
        }
    }
    return ans;
}

static bool
is_file_transmission(const unsigned char transmission_type) {
    return transmission_type == 'f' || transmission_type == 't' || transmission_type == 's';
}

// Ask the boss if path is allowed to be read. This is deliberately based on
// the path alone, so that it can be answered before the file is opened.
static bool
is_ok_to_read_image_path(const char *path) {
    if (!global_state.boss) return true;
    RAII_PyObject(ret, PyObject_CallMethod(global_state.boss, "is_ok_to_read_image_path", "s", path));
    if (!ret) {
        PyErr_Print();
        return false;
    }
    return ret == Py_True;
}

// Ask the boss if the already opened file at path is allowed to be read.
static bool
is_ok_to_read_image_fd(const char *path, int fd) {
    if (!global_state.boss) return true;
    RAII_PyObject(ret, PyObject_CallMethod(global_state.boss, "is_ok_to_read_image_file", "si", path, fd));
    if (!ret) {
        PyErr_Print();
        return false;
    }
    return ret == Py_True;
}

// Read image data from a file or a POSIX shared memory object. The fd is
// closed on all code paths, not just the successful one, otherwise a client
// can exhaust the process wide file descriptor limit, taking down the entire
// kitty process with it, simply by asking for files it is not allowed to read.
static bool
load_image_data_from_file(
    GraphicsManager *self, const GraphicsCommand *g, const unsigned char transmission_type, const char *fname, const size_t max_to_read, bool *was_opened) {
    const bool is_shm = transmission_type == 's';
    int fd;
    *was_opened = false;
    if (is_shm) {
        // Names without a leading slash are allowed, as, for example, macOS
        // sandboxed applications can only use names of the form group-id/name
        fd = safe_shm_open(fname, O_RDONLY, 0);
    } else {
        // The policy check has to happen before the file is opened. Merely
        // opening a file can have side-effects and whether the open succeeds
        // or not tells the client if the file exists, which it must not be
        // able to discover for files it is not allowed to read.
        if (!is_ok_to_read_image_path(fname)) {
            FAIL_IMAGE_FILE("Refusing to read image file: %s as permission was denied", fname);
            return false;
        }
        fd = safe_open(fname, O_CLOEXEC | O_RDONLY | O_NONBLOCK, 0); // O_NONBLOCK so that opening a FIFO pipe does not block
    }
    if (fd == -1) {
        FAIL_IMAGE_FILE("Failed to open file for graphics transmission: %s with error: [%d] %s", fname, errno, strerror(errno));
        return false;
    }
    *was_opened = true;
    bool ok;
    // Check again now that the file is open, since fname could have been
    // pointed at a different file in between the check above and the open().
    if (!is_shm && !is_ok_to_read_image_fd(fname, fd)) {
        FAIL_IMAGE_FILE("Refusing to read image file: %s as permission was denied", fname);
        ok = false;
    } else ok = read_img_file(self, fd, g->data_sz, g->data_offset, max_to_read, is_shm);
    safe_close(fd, __FILE__, __LINE__);
    return ok;
}

static Image *
load_image_data(
    GraphicsManager *self, Image *img, const GraphicsCommand *g, const unsigned char transmission_type, const uint32_t data_fmt, const uint8_t *payload) {
    LoadData *load_data = &self->currently_loading;
    const size_t limit = data_fmt == VECTOR ? (size_t)PTRDIFF_MAX : MAX_DATA_SZ;

    switch (transmission_type) {
        case 'd': // direct
            if (load_data->buf_capacity - load_data->buf_used < g->payload_sz) {
                if (load_data->buf_used > limit || g->payload_sz > limit-load_data->buf_used ||
                    (!g->compressed && data_fmt != PNG && data_fmt != VECTOR)) ABRT("EFBIG", "Too much data");
                const size_t capacity = MAX(load_data->buf_capacity > limit/2 ? limit : 2*load_data->buf_capacity, load_data->buf_used+g->payload_sz);
                void *next = realloc(load_data->buf, capacity);
                if (!next) {
                    // what was loaded so far is of no use without the rest, and is a lot of memory to hold on to
                    free_load_data(load_data);
                    ABRT("ENOMEM", "Out of memory");
                }
                load_data->buf = next;
                load_data->buf_capacity = capacity;
            }
            memcpy(load_data->buf + load_data->buf_used, payload, g->payload_sz);
            load_data->buf_used += g->payload_sz;
            if (!g->more) {
                load_data->loading_completed_successfully = true;
                load_data->loading_for = (const ImageAndFrame){0};
            }
            break;
        case 'f': // file
        case 't': // temporary file
        case 's': // POSIX shared memory
        {
            if (g->payload_sz >= PTRDIFF_MAX || memchr(payload, 0, g->payload_sz)) ABRT("EINVAL", "Invalid filename");
            RAII_ALLOC(char, fname, malloc(g->payload_sz+1));
            if (!fname) ABRT("ENOMEM", "Out of memory for filename");
            memcpy(fname, payload, g->payload_sz);
            fname[g->payload_sz] = 0;
            // When the data needs further processing the entire (possibly
            // compressed) payload is needed, otherwise reading more than the
            // expected number of bytes is pointless.
            const size_t max_to_read = (g->compressed || data_fmt == PNG || data_fmt == VECTOR) ? limit : load_data->data_sz;
            bool was_opened = false;
            const bool ok = load_image_data_from_file(self, g, transmission_type, fname, max_to_read, &was_opened);
            // Client created temporary files and shared memory objects that
            // were successfully opened are removed even when loading fails,
            // so that they are not left behind to accumulate.
            if (was_opened) {
                if (transmission_type == 't' && strstr(fname, "tty-graphics-protocol") != NULL) {
                    if (global_state.boss) {
                        call_boss(safe_delete_temp_file, "s", fname);
                    } else unlink(fname);
                } else if (transmission_type == 's') shm_unlink(fname);
            }
            load_data->loading_completed_successfully = ok;
            if (!ok) { // the error response has already been set
                free_load_data(load_data);
                return NULL;
            }
        } break;
        default: ABRT("EINVAL", "Unknown transmission type: %c", g->transmission_type);
    }
    return img;
}

// The number of bytes actually read is the size of the file the client asked
// kitty to read, which it must not learn for files it cannot read itself, so
// when transmitting via a file report the generic image file error instead.
#define ABRT_INSUFFICIENT_DATA(buf_used, data_sz)                                     \
    {                                                                                 \
        if (is_file_transmission(g->transmission_type)) {                             \
            log_error("Insufficient image data: %zu < %zu", (buf_used), (data_sz));   \
            ABRT(IMAGE_FILE_ERROR_CODE, "%s", IMAGE_FILE_ERROR_MSG);                  \
        }                                                                             \
        ABRT("ENODATA", "Insufficient image data: %zu < %zu", (buf_used), (data_sz)); \
    }

// The gamut a command says its pixels are in, see docs/wide-gamut-protocol.rst
static uint8_t
gamut_of_command(const GraphicsCommand *g) {
    return g->gamut == GAMUT_DISPLAY_P3 || g->gamut == GAMUT_REC2020 ? g->gamut : 0;
}

// The transfer function a command says its pixels are encoded with, the E key,
// see docs/silkie/precision-image-protocol.rst. False when the format does not
// take that value: only the formats of RGB, RGBA and gray values with a curve
// take the key. Half float pixels are linear by definition, which E=8 may
// repeat. PNG images say what they are themselves, indexed color images are in
// the colors of the terminal and vector images give each paint its own.
static bool
transfer_of_command(const GraphicsCommand *g, uint32_t format, uint8_t *transfer) {
    *transfer = TRANSFER_SRGB;
    switch (format) {
        case RGB:
        case RGBA:
        case GRAY1:
        case GRAY2:
        case GRAY4:
        case GRAY8:
        case GRAY16:
        case RGB565:
        case RGB10A2:
        case RGBA16: return transfer_from_code_point(g->transfer, transfer);
        case RGBA16F: return g->transfer == 0 || g->transfer == 8;
        default: return g->transfer == 0;
    }
}

// Packed wire formats do not depend on the display. Expand only after reading
// and decompressing their small payload, then use the existing RGB cache/GPU path.
static Image *
unpack_image_data(GraphicsManager *self, Image *img, uint32_t format) {
    LoadData *ld = &self->currently_loading;
    size_t size;
    if (!raw_image_data_size(RGB, ld->width, ld->height, &size)) ABRT("EOVERFLOW", "Expanded image dimensions overflow address space");
    uint8_t *rgb = malloc(size);
    if (!rgb) ABRT("ENOMEM", "Out of memory expanding packed image");
    const size_t row_size = ld->data_sz / ld->height;
    uint8_t *dst = rgb;
    for (uint32_t y = 0; y < ld->height; y++) {
        const uint8_t *src = ld->data + y * row_size;
        if (format == RGB565) {
            for (uint32_t x = 0; x < ld->width; x++, dst += 3) {
                // The wire word is little-endian; normalize each channel to
                // 8 bits with round-to-nearest, independently of host byte order.
                const unsigned value = src[2 * x] | ((unsigned)src[2 * x + 1] << 8);
                dst[0] = (((value >> 11) & 31) * 255 + 15) / 31;
                dst[1] = (((value >> 5) & 63) * 255 + 31) / 63;
                dst[2] = ((value & 31) * 255 + 15) / 31;
            }
        } else {
            const unsigned bits = bits_per_pixel(format), per_byte = 8 / bits, maximum = (1u << bits) - 1;
            for (uint32_t x = 0; x < ld->width; x++, dst += 3) {
                const unsigned shift = 8 - bits - (x % per_byte) * bits;
                const unsigned value = (src[x / per_byte] >> shift) & maximum;
                dst[0] = dst[1] = dst[2] = value * (255 / maximum);
            }
        }
    }
    free_load_data(ld);
    ld->buf = ld->data = rgb;
    ld->buf_capacity = ld->buf_used = ld->data_sz = size;
    ld->is_opaque = true;
    ld->is_float = false;
    ld->is_4byte_aligned = ld->width % 4 == 0;
    ld->loading_completed_successfully = true;
    return img;
}

// The data of a palette image is its palette followed by its packed indices.
// The indices are expanded to a byte each, for an integer texture the shaders
// look up in the palette, which is kept apart so that it can be replaced
// without the indices. The palette always has every slot, so no index can be
// out of range.
static Image *
unpack_palette_image(GraphicsManager *self, Image *img, uint32_t format) {
    LoadData *ld = &self->currently_loading;
    const unsigned bits = bits_per_pixel(format);
    const size_t palette_sz = palette_size_of_format(format);
    size_t size, row_size;
    if (!raw_image_data_size(PALETTE8, ld->width, ld->height, &size) || !raw_image_data_size(format, ld->width, 1, &row_size))
        ABRT("EOVERFLOW", "Expanded image dimensions overflow address space");
    ImagePalette *palette = alloc_palette(1u << bits);
    if (!palette) ABRT("ENOMEM", "Out of memory allocating palette");
    uint8_t *indices = malloc(size);
    if (!indices) {
        free(palette);
        ABRT("ENOMEM", "Out of memory expanding palette image");
    }
    memcpy(palette->rgba, ld->data, palette_sz);
    const uint8_t *packed = ld->data + palette_sz;
    uint8_t *dst = indices;
    const unsigned per_byte = 8 / bits, maximum = (1u << bits) - 1;
    for (uint32_t y = 0; y < ld->height; y++, dst += ld->width) {
        const uint8_t *src = packed + (size_t)y * row_size;
        if (bits == 8) memcpy(dst, src, ld->width);
        else for (uint32_t x = 0; x < ld->width; x++) dst[x] = (src[x / per_byte] >> (8 - bits - (x % per_byte) * bits)) & maximum;
    }
    free_load_data(ld);
    ld->buf = ld->data = indices;
    ld->buf_capacity = ld->buf_used = ld->data_sz = size;
    ld->palette = palette;
    ld->loading_completed_successfully = true;
    return img;
}

static Image *
process_image_data(GraphicsManager *self, Image *img, const GraphicsCommand *g, const uint32_t data_fmt, const bool is_frame) {
    if (data_fmt == VECTOR) {  // text records of a size known only once loaded
        LoadData *ld = &self->currently_loading;
        switch (g->compressed) {
            case 'z':
                if (!inflate_zlib_unbounded(ld, ld->buf, ld->buf_used)) {
                    ld->loading_completed_successfully = false;
                    return NULL;
                }
                break;
            case 0: break;
            default: ABRT("EINVAL", "Unknown image compression: %c", g->compressed);
        }
        ld->data = ld->buf;
        ld->data_sz = ld->buf_used;
        ld->gamut = 0;  // scene paints carry their own gamut
        ld->loading_completed_successfully = true;
        return img;
    }
    bool needs_processing = g->compressed || data_fmt == PNG;
    // An HDR image is linear extended sRGB whatever the command says, see docs/hdr-protocol.rst. The colors of an
    // indexed color image are those of the terminal, which are untagged, as are the colors of cells. Gray has no gamut.
    self->currently_loading.gamut =
        data_fmt == RGBA16F || is_gray_format(data_fmt) || data_fmt == GRAY16 || data_fmt == INDEXED || data_fmt == INDEXED_ALPHA ? 0 : gamut_of_command(g);
    if (needs_processing) {
        uint8_t *buf;
        size_t bufsz;
#define IB                                        \
    {                                             \
        buf = self->currently_loading.buf;        \
        bufsz = self->currently_loading.buf_used; \
    }
        switch (g->compressed) {
            case 'z':
                IB;
                if (!inflate_zlib(&self->currently_loading, buf, bufsz)) {
                    self->currently_loading.loading_completed_successfully = false;
                    return NULL;
                }
                break;
            case 0: break;
            default: ABRT("EINVAL", "Unknown image compression: %c", g->compressed);
        }
        switch (data_fmt) {
            case PNG:
                IB;
                if (!inflate_png(&self->currently_loading, buf, bufsz, is_frame ? img : NULL)) {
                    self->currently_loading.loading_completed_successfully = false;
                    return NULL;
                }
                break;
            default: break;
        }
#undef IB
        self->currently_loading.data = self->currently_loading.buf;
        if (self->currently_loading.buf_used < self->currently_loading.data_sz) {
            ABRT_INSUFFICIENT_DATA(self->currently_loading.buf_used, self->currently_loading.data_sz);
        }
    } else {
        if (self->currently_loading.buf_used < self->currently_loading.data_sz) {
            ABRT_INSUFFICIENT_DATA(self->currently_loading.buf_used, self->currently_loading.data_sz);
        } else self->currently_loading.data = self->currently_loading.buf;
        self->currently_loading.loading_completed_successfully = true;
    }
    if (is_palette_format(data_fmt)) return unpack_palette_image(self, img, data_fmt);
    return is_packed_format(data_fmt) ? unpack_image_data(self, img, data_fmt) : img;
}

static Image *
initialize_load_data(
    GraphicsManager *self, const GraphicsCommand *g, Image *img, const unsigned char transmission_type, const uint32_t data_fmt, const uint32_t frame_id) {
    free_load_data(&self->currently_loading);
    self->currently_loading = (const LoadData){0};
    self->currently_loading.start_command = *g;
    self->currently_loading.width = g->data_width;
    self->currently_loading.height = g->data_height;
    switch (data_fmt) {
        case PNG:
        case VECTOR:
            if (data_fmt != VECTOR && g->data_sz > MAX_DATA_SZ) ABRT("EINVAL", "PNG data size too large");
            self->currently_loading.is_4byte_aligned = true;
            self->currently_loading.is_opaque = false;
            self->currently_loading.data_sz = g->data_sz ? g->data_sz : 1024 * 100;
            break;
        case GRAY1:
        case GRAY2:
        case GRAY4:
        case GRAY8:
        case GRAY16:
        case RGB565:
        case RGB10A2:
        case RGBA16:
        case RGB:
        case RGBA:
        case RGBA16F:
            if (!g->data_width || !g->data_height) ABRT("EINVAL", "Zero width/height not allowed");
            if (!raw_image_data_size(data_fmt, g->data_width, g->data_height, &self->currently_loading.data_sz))
                ABRT("EOVERFLOW", "Image dimensions overflow address space");
            self->currently_loading.deep = deep_storage_of_format(data_fmt);
            self->currently_loading.is_opaque = data_fmt == RGB || data_fmt == GRAY16 || is_packed_format(data_fmt);
            // the rows of a texture upload start at a multiple of four bytes, unless told otherwise
            self->currently_loading.is_4byte_aligned = data_fmt == GRAY16 ? self->currently_loading.width % 2 == 0
                                                                          : !self->currently_loading.is_opaque || self->currently_loading.width % 4 == 0;
            self->currently_loading.is_float = data_fmt == RGBA16F;
            break;
        case PALETTE1:
        case PALETTE2:
        case PALETTE4:
        case PALETTE8: {
            if (!g->data_width || !g->data_height) ABRT("EINVAL", "Zero width/height not allowed");
            size_t indices_sz;
            const size_t palette_sz = palette_size_of_format(data_fmt);
            if (!raw_image_data_size(data_fmt, g->data_width, g->data_height, &indices_sz) || indices_sz > SIZE_MAX - palette_sz)
                ABRT("EOVERFLOW", "Image dimensions overflow address space");
            self->currently_loading.data_sz = palette_sz + indices_sz;
            // The indices are expanded to a byte each once loaded, see unpack_palette_image()
            self->currently_loading.indexed_px_sz = 1;
            self->currently_loading.is_4byte_aligned = self->currently_loading.width % 4 == 0;
        } break;
        case INDEXED:
        case INDEXED_ALPHA:
            if (!g->data_width || !g->data_height) ABRT("EINVAL", "Zero width/height not allowed");
            if (!raw_image_data_size(data_fmt, g->data_width, g->data_height, &self->currently_loading.data_sz))
                ABRT("EOVERFLOW", "Image dimensions overflow address space");
            self->currently_loading.indexed_px_sz = data_fmt / 8;
            self->currently_loading.is_4byte_aligned = ((size_t)self->currently_loading.width * self->currently_loading.indexed_px_sz) % 4 == 0;
            self->currently_loading.is_opaque = data_fmt == INDEXED;
            break;
        default: ABRT("EINVAL", "Unknown image format: %u", data_fmt);
    }
    if (!transfer_of_command(g, data_fmt, &self->currently_loading.transfer))
        ABRT("EINVAL", "The transfer function %u is not supported for images of format %u", g->transfer, data_fmt);
    self->currently_loading.loading_for.image_id = img->internal_id;
    self->currently_loading.loading_for.frame_id = frame_id;
    if (transmission_type == 'd') {
        const size_t extra = g->compressed ? 1024 : 10; // initial slack; compressed data can grow as it arrives
        if (self->currently_loading.data_sz > SIZE_MAX - extra) ABRT("EOVERFLOW", "Image buffer size overflows address space");
        self->currently_loading.buf_capacity = self->currently_loading.data_sz + extra;
        self->currently_loading.buf = malloc(self->currently_loading.buf_capacity);
        self->currently_loading.buf_used = 0;
        if (self->currently_loading.buf == NULL) {
            self->currently_loading.buf_capacity = 0;
            self->currently_loading.buf_used = 0;
            ABRT("ENOMEM", "Out of memory");
        }
    }
    return img;
}

#define INIT_CHUNKED_LOAD                                                 \
    {                                                                     \
        self->currently_loading.start_command.more = g->more;             \
        self->currently_loading.start_command.payload_sz = g->payload_sz; \
        g = &self->currently_loading.start_command;                       \
        tt = g->transmission_type ? g->transmission_type : 'd';           \
        fmt = g->format ? g->format : RGBA;                               \
    }

typedef struct {
    uint8_t *buf;
    bool is_4byte_aligned, is_opaque, transient, is_float;
    uint8_t deep;
} CoalescedFrameData;

static void
upload_to_gpu(GraphicsManager *self, Image *img, const CoalescedFrameData *data) {
    if (!self->context_made_current_for_this_command) {
        if (!self->window_id) return;
        if (!make_window_context_current(self->window_id)) return;
        self->context_made_current_for_this_command = true;
    }
    if (!img->texture) return;
    if (img->indexed_px_sz) send_indexed_image_to_gpu(&img->texture->id, data->buf, img->width, img->height, img->indexed_px_sz, data->is_4byte_aligned);
    // high precision pixels, and eight bit ones with another transfer function than sRGB, are decoded by the shader
    else if (data->deep || (img->transfer != TRANSFER_SRGB && !data->is_float))
        send_encoded_image_to_gpu(&img->texture->id, data->buf, img->width, img->height, data->deep, data->is_opaque, data->is_4byte_aligned);
    else send_image_to_gpu(&img->texture->id, data->buf, img->width, img->height, data->is_opaque, data->is_float, data->is_4byte_aligned, true, REPEAT_CLAMP);
}

// Sends count slots of the palette of a palette image from first on to the
// GPU, or all of them when it has no texture yet. The indices are not sent again.
static void
upload_palette_to_gpu(GraphicsManager *self, Image *img, uint32_t first, uint32_t count) {
    if (!self->context_made_current_for_this_command) {
        if (!self->window_id) return;
        if (!make_window_context_current(self->window_id)) return;
        self->context_made_current_for_this_command = true;
    }
    ImagePalette *p = img->palette;
    if (!p->texture) send_palette_to_gpu(&p->texture, p->rgba, p->slots);
    else update_palette_on_gpu(p->texture, p->rgba + (size_t)first * 4, first, count);
}

#if defined(__APPLE__) || defined(__linux__)
// Makes the context of the window current, false when there is no window to have one, as in the tests
static bool
surface_context(GraphicsManager *self, bool *failed) {
    *failed = false;
    if (self->context_made_current_for_this_command) return true;
    if (!self->window_id) return false;
    if (!make_window_context_current(self->window_id)) {
        *failed = true;
        return false;
    }
    self->context_made_current_for_this_command = true;
    return true;
}
#endif

// Copy the current contents of the surface into the image texture, on the GPU
static bool
refresh_surface_image(GraphicsManager *self, Image *img) {
#if defined(__APPLE__) || defined(__linux__)
    bool failed;
    if (!surface_context(self, &failed)) return !failed;
#ifdef __APPLE__
    if (!iosurface_bind_rectangle_texture(img->surface, &img->surface_texture, img->surface_is_bgra)) return false;
    if (img->texture) copy_surface_texture_to_texture(true, img->surface_texture, &img->texture->id, img->width, img->height);
#else
    if (!dmabuf_bind_texture(img->surface, &img->surface_texture)) return false;
    if (img->texture) copy_surface_texture_to_texture(false, img->surface_texture, &img->texture->id, img->width, img->height);
#endif
    return true;
#else
    (void)self;
    (void)img;
    return false;
#endif
}

#ifdef __linux__
// A dma-buf the program names by its process and descriptors, see dmabuf_take_fds() for which processes it may name. A
// query takes the descriptors to check that it could, gives them back and leaves surface at NULL
static bool
load_dmabuf(GraphicsManager *self, const GraphicsCommand *g, const char *spec, bool is_query, void **surface, uint32_t *width, uint32_t *height, char *err,
            size_t errsz) {
    *surface = NULL;
    if (!global_state.is_wayland) {
        snprintf(err, errsz, "dma-buf surfaces need EGL, which the terminal uses on Wayland and not on X11");
        return false;
    }
    DmabufSpec ds;
    if (!dmabuf_parse_spec(spec, strlen(spec), &ds, err, errsz)) return false;
    // unlike an IOSurface, a dma-buf does not know its size
    if (!g->data_width || !g->data_height) {
        snprintf(err, errsz, "A dma-buf surface needs its width and height in the s and v keys");
        return false;
    }
    *width = g->data_width;
    *height = g->data_height;
    int fds[arraysz(ds.planes)];
    if (!dmabuf_take_fds(&ds, child_pid_for_window(self->window_id), fds, err, errsz)) return false;
    bool ok = true;
    if (!is_query) {
        bool failed;
        if (!surface_context(self, &failed)) {
            snprintf(err, errsz, "The terminal has no GPU context to import the dma-buf into");
            ok = false;
        } else ok = (*surface = dmabuf_import(&ds, fds, *width, *height, err, errsz)) != NULL;
    }
    dmabuf_close_fds(fds, ds.num_planes);
    return ok;
}
#endif

static Image *
load_surface_image(GraphicsManager *self, const GraphicsCommand *g, Image *img, const uint8_t *payload, bool is_query) {
#if defined(__APPLE__) || defined(__linux__)
    if (g->more || g->compressed) ABRT("EINVAL", "Chunking and compression are not supported for GPU surfaces");
    if (g->transfer) ABRT("EINVAL", "Transfer functions are not supported for GPU surfaces");
    char spec[256], err[256];
    if (g->payload_sz >= sizeof(spec)) ABRT("EINVAL", "GPU surface specification too long");
    snprintf(spec, sizeof(spec), "%.*s", (int)g->payload_sz, payload);
    uint32_t width = 0, height = 0;
#ifdef __APPLE__
    if (strncmp(spec, "iosurface:", 10) != 0) ABRT("EINVAL", "Unknown GPU surface type: %s", spec);
    bool is_bgra;
    void *surface = iosurface_lookup((uint32_t)strtoul(spec + 10, NULL, 10), &width, &height, &is_bgra, err, sizeof(err));
    if (!surface) ABRT("EINVAL", "%s", err);
    if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
        iosurface_release(surface);
        ABRT("EINVAL", "Image too large, width or height greater than %u", MAX_IMAGE_DIMENSION);
    }
    img->surface_is_bgra = is_bgra;
#else
    if (strncmp(spec, "dmabuf:", 7) != 0) ABRT("EINVAL", "Unknown GPU surface type: %s", spec);
    void *surface;
    if (!load_dmabuf(self, g, spec, is_query, &surface, &width, &height, err, sizeof(err))) ABRT("EINVAL", "%s", err);
#endif
    img->surface = surface;
    img->gamut = gamut_of_command(g);  // the pixels of a surface are those of f=32
    img->width = width;
    img->height = height;
    img->root_frame = (const Frame){.id = ++img->frame_id_counter, .is_4byte_aligned = true, .width = width, .height = height};
    if (!is_query && !refresh_surface_image(self, img)) ABRT("EINVAL", "Failed to bind the GPU surface to a texture");
    img->root_frame_data_loaded = true;
    return img;
#else
    (void)g;
    (void)img;
    (void)payload;
    (void)is_query;
    ABRT("EINVAL", "GPU surfaces are not supported on this platform");
#endif
}

static Image *
handle_add_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, bool *is_dirty, uint32_t iid, bool is_query) {
    bool existing, init_img = true;
    Image *img = NULL;
    unsigned char tt = g->transmission_type ? g->transmission_type : 'd';
    uint32_t fmt = g->format ? g->format : RGBA;
    if (tt == 'd' && self->currently_loading.loading_for.image_id) init_img = false;
    if (init_img) {
        self->currently_loading.loading_for = (const ImageAndFrame){0};
        // the response to a failure before the load data is initialized goes to this command, not an earlier one
        self->currently_loading.start_command = *g;
        self->currently_loading.start_command.id = iid;
        if (g->data_width > MAX_IMAGE_DIMENSION || g->data_height > MAX_IMAGE_DIMENSION)
            ABRT("EINVAL", "Image too large, width or height greater than %u", MAX_IMAGE_DIMENSION);
        remove_images(self, add_trim_predicate, 0);
        img = find_or_create_image(self, iid, &existing);
        if (!img) ABRT("ENOMEM", "Out of memory creating an image");
        if (existing) {
            free_image_resources(self, img);
            if (!(img->texture = new_texture_ref())) {
                // its pixels and placements are gone already, as they would be on success
                remove_image(self, img);
                *is_dirty = true;
                set_layers_dirty(self);
                ABRT("ENOMEM", "Out of memory creating the texture of an image");
            }
            img->root_frame_data_loaded = false;
            img->indexed_px_sz = 0;
            img->transfer = TRANSFER_SRGB;
            img->is_drawn = false;
            img->current_frame_shown_at = 0;
            img->extra_framecnt = 0;
            img->current_frame_index = 0;
            img->animation_duration = 0;
            img->animation_state = ANIMATION_STOPPED;
            img->max_loops = 0;
            img->current_loop = 0;
            *is_dirty = true;
            set_layers_dirty(self);
        } else {
            img->client_id = iid;
            img->client_number = g->image_number;
            if (!img->client_id && img->client_number) {
                if (!(img->client_id = get_free_client_id(self))) {
                    remove_image(self, img);
                    ABRT("ENOMEM", "Out of memory finding a free image id");
                }
                iid = img->client_id;
            }
        }
        img->atime = monotonic();
        img->used_storage = 0;
        if (tt == 'g') {
            self->currently_loading.start_command = *g;
            if (g->action != 'v') self->currently_loading.start_command.id = iid;
            return load_surface_image(self, g, img, payload, is_query);
        }
        if (!initialize_load_data(self, g, img, tt, fmt, 0)) return NULL;
        if (g->action != 'v') self->currently_loading.start_command.id = iid;
    } else {
        INIT_CHUNKED_LOAD;
        img = img_by_internal_id(self, self->currently_loading.loading_for.image_id);
        if (img == NULL) {
            self->currently_loading.loading_for = (const ImageAndFrame){0};
            ABRT("EILSEQ", "More payload loading refers to non-existent image");
        }
    }
    img = load_image_data(self, img, g, tt, fmt, payload);
    if (!img || !self->currently_loading.loading_completed_successfully) return NULL;
    self->currently_loading.loading_for = (const ImageAndFrame){0};
    img = process_image_data(self, img, g, fmt, false);
    if (!img) return NULL;
    if (fmt == VECTOR) {
        LoadData *ld = &self->currently_loading;
        if (g->action == 'v') {
            if (ld->data_sz > PY_SSIZE_T_MAX) ABRT("EOVERFLOW", "Vector request too large");
            RAII_PyObject(module, PyImport_ImportModule("kitty.fonts.vector"));
            if (!module) { PyErr_Clear(); ABRT("ENOMEM", "Cannot load vector font service"); }
            Py_XSETREF(self->vector_response, PyObject_CallMethod(module, "service", "Oy#I", self->vector_font_session, ld->data, (Py_ssize_t)ld->data_sz, g->id));
            if (!self->vector_response) { PyErr_Clear(); ABRT("ENOMEM", "Vector font service failed"); }
            return img;
        }
        if (!ld->width || !ld->height) ABRT("EINVAL", "Zero width/height not allowed");
        VectorDoc *doc = vector_doc_new(ld->width, ld->height);
        if (!doc) ABRT("ENOMEM", "Out of memory");
        vector_doc_set_font_session(doc, self->vector_font_session);
        const char *err = vector_doc_apply(doc, ld->data, ld->data_sz);
        if (err) {
            vector_doc_free(doc);
            ABRT("EINVAL", "%s", err);
        }
        img->width = ld->width;
        img->height = ld->height;
        img->root_frame = (const Frame){
            .id = ++img->frame_id_counter, .is_float = true, .is_4byte_aligned = true, .width = img->width, .height = img->height, .transient = (g->usage_hints & GRAPHICS_USAGE_HINT_TRANSIENT) != 0};
        img->tone_map = g->tone_map == 1;
        img->gamut = 0;
        if (is_query) vector_doc_free(doc);
        else {
            img->vector = doc;
            img->used_storage = vector_doc_size(doc);
            self->used_storage += img->used_storage;
        }
        img->root_frame_data_loaded = true;
        return img;
    }
    const size_t px_sz = self->currently_loading.indexed_px_sz
                             ? self->currently_loading.indexed_px_sz
                             : stored_pixel_size(self->currently_loading.is_opaque, self->currently_loading.is_float, self->currently_loading.deep);
    size_t required_sz = px_sz * self->currently_loading.width * self->currently_loading.height;
    if (self->currently_loading.data_sz != required_sz)
        ABRT(
            "EINVAL",
            "Image dimensions: %ux%u do not match data size: %zu, expected size: %zu",
            self->currently_loading.width,
            self->currently_loading.height,
            self->currently_loading.data_sz,
            required_sz);
    if (self->currently_loading.loading_completed_successfully) {
        img->width = self->currently_loading.width;
        img->height = self->currently_loading.height;
        img->gamut = self->currently_loading.gamut;
        img->transfer = self->currently_loading.transfer;
        img->indexed_px_sz = self->currently_loading.indexed_px_sz;
        release_palette(&img->palette);
        img->palette = self->currently_loading.palette;
        self->currently_loading.palette = NULL;
        if (img->root_frame.id) remove_from_cache(self, (const ImageAndFrame){.image_id = img->internal_id, .frame_id = img->root_frame.id});
        img->root_frame = (const Frame){
            .id = ++img->frame_id_counter,
            .is_opaque = self->currently_loading.is_opaque,
            .is_float = self->currently_loading.is_float,
            .is_4byte_aligned = self->currently_loading.is_4byte_aligned,
            .width = img->width,
            .height = img->height,
            .transient = (g->usage_hints & GRAPHICS_USAGE_HINT_TRANSIENT) != 0,
            .deep = self->currently_loading.deep,
        };
        img->tone_map = g->tone_map == 1;
        if (img->root_frame.is_float) img->peak = peak_of_half_data(self->currently_loading.data, required_sz);
        else if (transfer_is_hdr(img->transfer)) {
            PixelDecoder d;
            if (!pixel_decoder_init(&d, img->root_frame.deep, img->root_frame.is_opaque, img->transfer, img->gamut)) ABRT("ENOMEM", "Out of memory");
            img->peak = pixel_decoder_peak(&d, self->currently_loading.data, (size_t)img->width * img->height);
            pixel_decoder_free(&d);
        } else img->peak = 0;
        if (!is_query) {
            // Upload before caching, the cache takes ownership of the data
            upload_to_gpu(self, img, &(const CoalescedFrameData){
                .buf = self->currently_loading.data, .is_opaque = img->root_frame.is_opaque, .is_float = img->root_frame.is_float,
                .is_4byte_aligned = img->root_frame.is_4byte_aligned, .deep = img->root_frame.deep});
            if (img->palette) upload_palette_to_gpu(self, img, 0, img->palette->slots);
            if (!add_load_data_to_cache(self, (const ImageAndFrame){.image_id = img->internal_id, .frame_id = img->root_frame.id}, img->root_frame.transient)) {
                if (PyErr_Occurred()) PyErr_Print();
                ABRT("ENOSPC", "Failed to store image data in cache");
            }
            const size_t stored = required_sz + (img->palette ? (size_t)img->palette->slots * 4 : 0);
            self->used_storage += stored;
            img->used_storage = stored;
        }
        img->root_frame_data_loaded = true;
    }
    return img;
#undef MAX_DATA_SZ
}

static const char *
supported_keys_for_query(const GraphicsCommand *g) {
    // A query with e=<action> is a feature query. Echo back every key from it
    // that has a non-default value which is supported when used with that
    // action, so that clients can detect support for protocol features. A
    // leading e=<action> tells the client that the action itself is supported.
    // The reply is bounded by the protocol, not by the input: each key is
    // echoed at most once and the longest action (T) echoes 27 keys and e, each at
    // most 14 bytes (",k=" and a 32 bit integer with its sign), so 512 bytes
    // always fits. Grow it if keys are added.
    static char buf[512];
    size_t pos = 0;
    buf[0] = 0;
    const unsigned char action = g->report_supported;
    // Besides kitty's actions, Silkie has e (edit a vector image), r (refresh
    // an image backed by a GPU surface) and v (the vector font service).
    if (!action || !strchr("tTqpdfacerv", action)) return buf;
#define print(fmt, ...) \
    if (sizeof(buf) - 1 > pos) pos += snprintf(buf + pos, sizeof(buf) - pos, fmt, __VA_ARGS__)
#define ECHO_FLAG(key, attr) \
    if (g->attr) print(",%c=%c", key, g->attr)
#define ECHO_UINT(key, attr) \
    if (g->attr) print(",%c=%u", key, g->attr)
#define ECHO_INT(key, attr) \
    if (g->attr) print(",%c=%d", key, g->attr)
#define ECHO_ONE(key, attr) \
    if (g->attr == 1) print(",%c=%u", key, 1u)
    print(",e=%c", action);
    switch (action) {
        case 't':
        case 'T':
        case 'q':
        case 'f': {
            // GPU surfaces (t=g) exist only where they are implemented, cannot
            // be chunked or compressed and cannot be animation frames
#if defined(__APPLE__) || defined(__linux__)
            const bool surfaces = true;
#else
            const bool surfaces = false;
#endif
            const bool surface = g->transmission_type == 'g';
            if (!surface || (surfaces && action != 'f' && !g->compressed && !g->more)) ECHO_FLAG('t', transmission_type);
            if (!surface) ECHO_FLAG('o', compressed);
            const uint32_t fmt = g->format;
            // Silkie adds the packed formats (packed-image-protocol), half
            // float RGBA (hdr-protocol) and vector images, which cannot have
            // animation frames (vector-protocol)
            // and kitty's indexed color formats, which cannot have animation frames either
            // and Silkie's palette images (palette-image-protocol) and high precision images (precision-image-protocol),
            // which cannot have animation frames either
            const bool is_indexed = fmt == INDEXED || fmt == INDEXED_ALPHA, is_palette = is_palette_format(fmt);
            const bool is_deep = deep_storage_of_format(fmt) != DEEP_NONE;
            const bool has_frames = fmt == RGB || fmt == RGBA || fmt == PNG || fmt == RGBA16F || is_packed_format(fmt);
            if (has_frames || ((fmt == VECTOR || is_indexed || is_palette || is_deep) && action != 'f')) print(",f=%u", fmt);
            // The colors of an indexed color image are those of the terminal, it has no gamut of its own and is never HDR.
            // The palette of a palette image can be in a gamut, but is never HDR.
            if (!is_indexed && (g->gamut == GAMUT_DISPLAY_P3 || g->gamut == GAMUT_REC2020)) print(",G=%u", g->gamut);
            // A transfer function other than sRGB, for the formats that take it, see transfer_of_command(). Frames are sRGB.
            uint8_t transfer;
            if (g->transfer && !surface && transfer_of_command(g, fmt ? fmt : RGBA, &transfer) && (action != 'f' || transfer == TRANSFER_SRGB)) print(",E=%u", g->transfer);
            if (action != 'f' && !is_indexed && !is_palette) ECHO_ONE('T', tone_map);
            if (g->data_width <= MAX_IMAGE_DIMENSION) ECHO_UINT('s', data_width);
            if (g->data_height <= MAX_IMAGE_DIMENSION) ECHO_UINT('v', data_height);
            ECHO_UINT('S', data_sz);
            ECHO_UINT('O', data_offset);
            if (g->usage_hints && !(g->usage_hints & ~GRAPHICS_USAGE_HINT_TRANSIENT)) print(",N=%u", g->usage_hints);
            if (action == 'f') {
                ECHO_UINT('x', x_offset);
                ECHO_UINT('y', y_offset);
                ECHO_UINT('c', other_frame_number);
                ECHO_UINT('r', frame_number);
                ECHO_INT('z', gap);
                ECHO_ONE('X', blend_mode);
                ECHO_UINT('Y', bgcolor);
            }
            if (action != 'T') break;
        } /* fallthrough */
        case 'p':
            ECHO_UINT('p', placement_id);
            ECHO_UINT('x', x_offset);
            ECHO_UINT('y', y_offset);
            ECHO_UINT('w', width);
            ECHO_UINT('h', height);
            ECHO_UINT('X', cell_x_offset);
            ECHO_UINT('Y', cell_y_offset);
            ECHO_UINT('c', num_cells);
            ECHO_UINT('r', num_lines);
            ECHO_ONE('C', cursor_movement);
            ECHO_ONE('U', unicode_placement);
            ECHO_INT('z', z_index);
            ECHO_UINT('P', parent_id);
            ECHO_UINT('Q', parent_placement_id);
            ECHO_INT('H', offset_from_parent_x);
            ECHO_INT('V', offset_from_parent_y);
            break;
        case 'd':
            ECHO_FLAG('d', delete_action);
            ECHO_UINT('p', placement_id);
            ECHO_UINT('x', x_offset);
            ECHO_UINT('y', y_offset);
            ECHO_INT('z', z_index);
            break;
        case 'a':
            if (g->animation_state <= 3) ECHO_UINT('s', animation_state);
            ECHO_UINT('v', loop_count);
            ECHO_UINT('r', frame_number);
            ECHO_UINT('c', other_frame_number);
            ECHO_INT('z', gap);
            break;
        case 'v':
            // the vector font service takes f=200 data sent directly or from a file
            if (g->format == VECTOR) print(",f=%u", g->format);
            if (g->transmission_type != 'g') ECHO_FLAG('t', transmission_type);
            ECHO_FLAG('o', compressed);
            break;
        case 'e':
            // a vector edit carries a patch and a palette update palette slots, sent directly or from a file
            if (g->transmission_type != 'g') ECHO_FLAG('t', transmission_type);
            ECHO_FLAG('o', compressed);
            // the first palette slot to replace and the number of slots
            ECHO_UINT('x', x_offset);
            ECHO_UINT('w', width);
            break;
        case 'r':
            // a refresh has no keys besides the image it refers to
            break;
        case 'c':
            ECHO_UINT('r', frame_number);
            ECHO_UINT('c', other_frame_number);
            ECHO_UINT('x', x_offset);
            ECHO_UINT('y', y_offset);
            ECHO_UINT('w', width);
            ECHO_UINT('h', height);
            ECHO_UINT('X', cell_x_offset);
            ECHO_UINT('Y', cell_y_offset);
            ECHO_ONE('C', compose_mode);
            break;
    }
#undef ECHO_ONE
#undef ECHO_INT
#undef ECHO_UINT
#undef ECHO_FLAG
#undef print
    return buf;
}

static const char *
finish_command_response_with_keys(const GraphicsCommand *g, bool data_loaded, const char *extra_keys) {
    static char rbuf[sizeof(command_response) / sizeof(command_response[0]) + 1024];
    bool is_ok_response = !command_response[0];
    if (g->quiet) {
        if (is_ok_response || g->quiet > 1) return NULL;
    }
    if (g->id || g->image_number) {
        if (is_ok_response) {
            if (!data_loaded) return NULL;
            snprintf(command_response, 10, "OK");
        }
        size_t pos = 0;
        rbuf[pos++] = 'G';
#define print(fmt, ...) \
    if (arraysz(rbuf) - 1 > pos) pos += snprintf(rbuf + pos, arraysz(rbuf) - 1 - pos, fmt, __VA_ARGS__)
        if (g->id) print("i=%u", g->id);
        if (g->image_number) print(",I=%u", g->image_number);
        if (g->placement_id) print(",p=%u", g->placement_id);
        if (g->num_lines && (g->action == 'f' || g->action == 'a')) print(",r=%u", g->num_lines);
        if (is_ok_response && extra_keys) print("%s", extra_keys);
        print(";%s", command_response);
        return rbuf;
#undef print
    }
    return NULL;
}

static const char *
finish_command_response(const GraphicsCommand *g, bool data_loaded) {
    return finish_command_response_with_keys(g, data_loaded, NULL);
}

// }}}

// Displaying images {{{

static void
update_src_rect(ImageRef *ref, Image *img) {
    // The src rect in OpenGL co-ords [0, 1] with origin at top-left corner of image
    ref->src_rect.left = (float)ref->src_x / (float)img->width;
    ref->src_rect.right = (float)(ref->src_x + ref->src_width) / (float)img->width;
    ref->src_rect.top = (float)ref->src_y / (float)img->height;
    ref->src_rect.bottom = (float)(ref->src_y + ref->src_height) / (float)img->height;
}

static void
update_dest_rect(ImageRef *ref, uint32_t num_cols, uint32_t num_rows, CellPixelSize cell) {
    uint32_t t;
    const bool auto_cols = num_cols == 0, auto_rows = num_rows == 0;
    if (auto_cols) {
        if (auto_rows) {
            t = (uint32_t)(ref->src_width + ref->cell_x_offset);
            num_cols = t / cell.width;
            if (t > num_cols * cell.width) num_cols += 1;
        } else {
            double height_px = cell.height * num_rows + ref->cell_y_offset;
            double width_px = height_px * ref->src_width / (double)ref->src_height;
            num_cols = (uint32_t)ceil(width_px / cell.width);
        }
    }
    if (auto_rows) {
        if (auto_cols) {
            t = (uint32_t)(ref->src_height + ref->cell_y_offset);
            num_rows = t / cell.height;
            if (t > num_rows * cell.height) num_rows += 1;
        } else {
            double width_px = cell.width * num_cols + ref->cell_x_offset;
            double height_px = width_px * ref->src_height / (double)ref->src_width;
            num_rows = (uint32_t)ceil(height_px / cell.height);
        }
    }
    ref->effective_num_rows = num_rows;
    ref->effective_num_cols = num_cols;
}

// A part of an image, as fractions of its width and height: in doubles, as a part of a large image is a tiny fraction of it
typedef struct {
    double left, top, right, bottom;
} ImageFraction;
static void update_vector_texture(GraphicsManager *self, Image *img, double width, double height, ImageFraction shown, double view_width, double view_height);

// NULL when out of memory
static ImageRef *
create_ref(Image *img, ImageRef *clone_from) {
    ImageRef *ans = allocation_fails(ALLOCATION_REF) ? NULL : calloc(1, sizeof(ImageRef));
    if (!ans) return NULL;
    if (clone_from) *ans = *clone_from;
    ans->internal_id = next_id(&img->ref_id_counter);
    if (vt_is_end(vt_insert(&img->refs_by_internal_id, ans->internal_id, ans))) {
        free(ans);
        return NULL;
    }
    return ans;
}

static inline bool
is_cell_image(const ImageRef *self) {
    return self->virtual_ref_id != 0;
}

// Create a real image ref for a virtual image ref (placement) positioned in the
// given cells. This is used for images positioned using Unicode placeholders.
//
// The image is resized to fit a box of cells with dimensions
// `image_ref->columns` by `image_ref->rows`. The parameters `img_col`,
// `img_row, `columns`, `rows` describe a part of this box that we want to
// display.
//
// Parameters:
// - `self` - the graphics manager
// - `screen_row` - the starting row of the screen
// - `screen_col` - the starting column of the screen
// - `image_id` - the id of the image
// - `placement_id` - the id of the placement (0 to find it automatically), it
//                    must be a virtual placement
// - `img_col` - the column of the image box we want to start with (base 0)
// - `img_row` - the row of the image box we want to start with (base 0)
// - `columns` - the number of columns we want to display
// - `rows` - the number of rows we want to display
// - `cell` - the size of a screen cell
void
grman_put_cell_image(
    GraphicsManager *self,
    uint32_t screen_row,
    uint32_t screen_col,
    uint32_t image_id,
    uint32_t placement_id,
    uint32_t img_col,
    uint32_t img_row,
    uint32_t columns,
    uint32_t rows,
    CellPixelSize cell) {
    Image *img = img_by_client_id(self, image_id);
    if (img == NULL) return;

    ImageRef *virt_img_ref = NULL;
    if (placement_id) {
        // Find the placement by the id. It must be a virtual placement.
        ImageRef *r = ref_by_client_id(img, placement_id);
        if (r && r->is_virtual_ref) virt_img_ref = r;
    } else {
        // Find the first virtual image placement.
        iter_refs(img) {
            ImageRef *r = i.data->val;
            if (r->is_virtual_ref) {
                virt_img_ref = r;
                break;
            }
        }
    }

    if (!virt_img_ref) return;

    // Create the ref structure on stack first. We will not create a real
    // reference if the image is completely out of bounds.
    ImageRef ref = {0};
    ref.virtual_ref_id = virt_img_ref->internal_id;
    ref.depth = virt_img_ref->depth;
    ref.depth_pixels = virt_img_ref->depth_pixels;
    ref.phase = virt_img_ref->phase;

    uint32_t img_rows = virt_img_ref->num_rows;
    uint32_t img_columns = virt_img_ref->num_cols;
    // If the number of columns or rows for the image is not set, compute them
    // in such a way that the image is as close as possible to its natural size.
    if (img_columns == 0) img_columns = (img->width + cell.width - 1) / cell.width;
    if (img_rows == 0) img_rows = (img->height + cell.height - 1) / cell.height;

    ref.start_row = screen_row;
    ref.start_column = screen_col;
    ref.num_cols = columns;
    ref.num_rows = rows;

    // The image is fit to the destination box of size
    //    (cell.width * img_columns) by (cell.height * img_rows)
    // The conversion from source (image) coordinates to destination (box)
    // coordinates is done by the following formula:
    //    x_dst = x_src * x_scale + x_offset
    //    y_dst = y_src * y_scale + y_offset
    float x_offset, y_offset, x_scale, y_scale;

    // Fit the image to the box while preserving aspect ratio
    if (img->width * img_rows * cell.height > img->height * img_columns * cell.width) {
        // Fit to width and center vertically.
        x_offset = 0;
        x_scale = (float)(img_columns * cell.width) / MAX(1u, img->width);
        y_scale = x_scale;
        y_offset = (img_rows * cell.height - img->height * y_scale) / 2;
    } else {
        // Fit to height and center horizontally.
        y_offset = 0;
        y_scale = (float)(img_rows * cell.height) / MAX(1u, img->height);
        x_scale = y_scale;
        x_offset = (img_columns * cell.width - img->width * x_scale) / 2;
    }

    // Now we can compute source (image) coordinates from destination (box)
    // coordinates by formula:
    //     x_src = (x_dst - x_offset) / x_scale
    //     y_src = (y_dst - y_offset) / y_scale

    // Destination (box) coordinates of the rectangle we want to display.
    uint32_t x_dst = img_col * cell.width;
    uint32_t y_dst = img_row * cell.height;
    uint32_t w_dst = columns * cell.width;
    uint32_t h_dst = rows * cell.height;

    // Compute the source coordinates of the rectangle.
    ref.src_x = (x_dst - x_offset) / x_scale;
    ref.src_y = (y_dst - y_offset) / y_scale;
    ref.src_width = w_dst / x_scale;
    ref.src_height = h_dst / y_scale;

    // If the top left corner is out of bounds of the source image, we can
    // adjust cell offsets and the starting row/column. And if the rectangle is
    // completely out of bounds, we can avoid creating a real reference. This
    // is just an optimization, the image will be displayed correctly even if we
    // do not do this.
    if (ref.src_x < 0) {
        ref.src_width += ref.src_x;
        ref.cell_x_offset = (uint32_t)(-ref.src_x * x_scale);
        ref.src_x = 0;
        uint32_t col_offset = ref.cell_x_offset / cell.width;
        ref.cell_x_offset %= cell.width;
        ref.start_column += col_offset;
        if (ref.num_cols <= col_offset) return;
        ref.num_cols -= col_offset;
    }
    if (ref.src_y < 0) {
        ref.src_height += ref.src_y;
        ref.cell_y_offset = (uint32_t)(-ref.src_y * y_scale);
        ref.src_y = 0;
        uint32_t row_offset = ref.cell_y_offset / cell.height;
        ref.cell_y_offset %= cell.height;
        ref.start_row += row_offset;
        if (ref.num_rows <= row_offset) return;
        ref.num_rows -= row_offset;
    }

    // For the bottom right corner we can remove only completely empty rows and
    // columns.
    if (ref.src_x + ref.src_width > img->width) {
        float redundant_w = ref.src_x + ref.src_width - img->width;
        uint32_t redundant_cols = (uint32_t)(redundant_w * x_scale) / cell.width;
        if (ref.num_cols <= redundant_cols) return;
        ref.src_width -= redundant_cols * cell.width / x_scale;
        ref.num_cols -= redundant_cols;
    }
    if (ref.src_y + ref.src_height > img->height) {
        float redundant_h = ref.src_y + ref.src_height - img->height;
        uint32_t redundant_rows = (uint32_t)(redundant_h * y_scale) / cell.height;
        if (ref.num_rows <= redundant_rows) return;
        ref.src_height -= redundant_rows * cell.height / y_scale;
        ref.num_rows -= redundant_rows;
    }

    // The cursor will be drawn on top of the image.
    ref.z_index = -1;

    // Create a real ref.
    ImageRef *real_ref = create_ref(img, &ref);
    if (!real_ref) {
        log_error("Out of memory placing an image for a Unicode placeholder, it is not shown");
        return;
    }

    img->atime = monotonic();
    set_layers_dirty(self);

    update_src_rect(real_ref, img);
    update_dest_rect(real_ref, ref.num_cols, ref.num_rows, cell);
}

static void remove_ref(Image *img, ImageRef *ref);
static ref_map_itr remove_ref_itr(Image *img, ref_map_itr x);

static bool
has_good_ancestry(GraphicsManager *self, ImageRef *ref) {
    ImageRef *r = ref;
    unsigned depth = 0;
    while (r->parent.img) {
        if (r == ref && depth) {
            set_command_failed_response("ECYCLE", "This parent reference creates a cycle");
            return false;
        }
        if (depth++ >= PARENT_DEPTH_LIMIT) {
            set_command_failed_response("ETOODEEP", "Too many levels of parent references");
            return false;
        }
        Image *parent = img_by_internal_id(self, r->parent.img);
        if (!parent) {
            set_command_failed_response("ENOENT", "One of the ancestors of this ref with image id: %llu not found", r->parent.img);
            return false;
        }
        ImageRef *parent_ref = ref_by_internal_id(parent, r->parent.ref);
        if (!parent_ref) {
            set_command_failed_response(
                "ENOENT", "One of the ancestors of this ref with image id: %llu and ref id: %llu not found", r->parent.img, r->parent.ref);
            return false;
        }
        r = parent_ref;
    }
    return true;
}

static uint32_t
handle_put_command(GraphicsManager *self, const GraphicsCommand *g, Cursor *c, bool *is_dirty, Image *img, CellPixelSize cell) {
    if (g->unicode_placement && g->parent_id) {
        set_command_failed_response("EINVAL", "Put command creating a virtual placement cannot refer to a parent");
        return g->id;
    }
    if (img == NULL) {
        if (g->id) img = img_by_client_id(self, g->id);
        else if (g->image_number) img = img_by_client_number(self, g->image_number);
        if (img == NULL) {
            set_command_failed_response("ENOENT", "Put command refers to non-existent image with id: %u and number: %u", g->id, g->image_number);
            return g->id;
        }
    }
    if (!img->root_frame_data_loaded) {
        set_command_failed_response("ENOENT", "Put command refers to image with id: %u that could not load its data", g->id);
        return img->client_id;
    }
    id_type parent_id = 0, parent_placement_id = 0;
    if (g->parent_id) {
        Image *parent = img_by_client_id(self, g->parent_id);
        if (!parent) {
            set_command_failed_response("ENOPARENT", "Put command refers to a parent image with id: %u that does not exist", g->parent_id);
            return g->id;
        }
        if (!vt_size(&parent->refs_by_internal_id)) {
            set_command_failed_response("ENOPARENT", "Put command refers to a parent image with id: %u that has no placements", g->parent_id);
            return g->id;
        }
        ImageRef *parent_ref = vt_first(&parent->refs_by_internal_id).data->val;
        if (g->parent_placement_id) {
            parent_ref = ref_by_client_id(parent, g->parent_placement_id);
            if (!parent_ref) {
                set_command_failed_response(
                    "ENOPARENT",
                    "Put command refers to a parent image placement with id: %u and placement id: %u that does not exist",
                    g->parent_id,
                    g->parent_placement_id);
                return g->id;
            }
        }
        parent_id = parent->internal_id;
        parent_placement_id = parent_ref->internal_id;
    }
    ImageRef *ref = NULL;
    if (g->placement_id && img->client_id) {
        ref = ref_by_client_id(img, g->placement_id);
        if (ref) {
            if (parent_id && parent_id == img->internal_id && parent_placement_id && parent_placement_id == ref->internal_id) {
                set_command_failed_response("EINVAL", "Put command refers to itself as its own parent");
                return g->id;
            }
            if (parent_id && parent_placement_id) {
                id_type rp = ref->parent.img, rpp = ref->parent.ref;
                ref->parent.img = parent_id;
                ref->parent.ref = parent_placement_id;
                bool ok = has_good_ancestry(self, ref);
                ref->parent.img = rp;
                ref->parent.ref = rpp;
                if (!ok) return g->id;
            }
        }
    }
    if (ref == NULL && !(ref = create_ref(img, NULL))) {
        set_command_failed_response("ENOMEM", "Out of memory creating a placement");
        return g->id;
    }

    *is_dirty = true;
    set_layers_dirty(self);
    img->atime = monotonic();
    ref->src_x = g->x_offset;
    ref->src_y = g->y_offset;
    ref->src_width = g->width ? g->width : img->width;
    ref->src_height = g->height ? g->height : img->height;
    ref->src_width = MIN(ref->src_width, img->width - ((float)img->width > ref->src_x ? ref->src_x : (float)img->width));
    ref->src_height = MIN(ref->src_height, img->height - ((float)img->height > ref->src_y ? ref->src_y : (float)img->height));
    ref->z_index = g->z_index;
    ref->depth = c->sgr.depth;
    ref->depth_pixels = c->sgr.depth_pixels;
    ref->phase = c->sgr.phase;
    ref->start_row = c->y;
    ref->start_column = c->x;
    ref->cell_x_offset = MIN(g->cell_x_offset, cell.width - 1);
    ref->cell_y_offset = MIN(g->cell_y_offset, cell.height - 1);
    ref->num_cols = g->num_cells;
    ref->num_rows = g->num_lines;
    if (img->client_id) {
        if (!ref->client_id && g->placement_id && vt_is_end(vt_insert(&img->refs_by_client_id, g->placement_id, ref))) fatal("Out of memory");
        ref->client_id = g->placement_id;
    }
    update_src_rect(ref, img);
    update_dest_rect(ref, g->num_cells, g->num_lines, cell);
    ref->parent.img = parent_id;
    ref->parent.ref = parent_placement_id;
    ref->parent.offset.x = g->offset_from_parent_x;
    ref->parent.offset.y = g->offset_from_parent_y;
    ref->is_virtual_ref = false;
    if (g->unicode_placement) {
        ref->is_virtual_ref = true;
        ref->start_row = ref->start_column = 0;
    }
    if (ref->parent.img) {
        if (!has_good_ancestry(self, ref)) {
            remove_ref(img, ref);
            return g->id;
        }
    } else {
        // Move the cursor, the screen will take care of ensuring it is in bounds
        if (g->cursor_movement != 1 && !g->unicode_placement) {
            c->x += ref->effective_num_cols;
            if (ref->effective_num_rows) c->y += ref->effective_num_rows - 1;
        }
    }
    return img->client_id;
}

void
gpu_data_for_image(ImageRenderData *ans, float left, float top, float right, float bottom) {
    // For dest rect: x-axis is from -1 to 1, y axis is from 1 to -1
    static const ImageRef source_rect = {.src_rect = {.left = 0, .top = 0, .bottom = 1, .right = 1}};
    ans->src_rect = source_rect.src_rect;
    ans->dest_rect = (ImageRect){.left = left, .right = right, .top = top, .bottom = bottom};
    ans->group_count = 1;
}

static bool
resolve_cell_ref(const Image *img, id_type virt_ref_id, int32_t *start_row, int32_t *start_column) {
    *start_row = 0;
    *start_column = 0;
    bool found = false;
    iter_refs((Image *)img) {
        ImageRef *ref = i.data->val;
        if (ref->virtual_ref_id == virt_ref_id) {
            if (!found || ref->start_row < *start_row) *start_row = ref->start_row;
            if (!found || ref->start_column < *start_column) *start_column = ref->start_column;
            found = true;
        }
    }
    return found;
}

static bool
resolve_parent_offset(const GraphicsManager *self, const ImageRef *ref, int32_t *start_row, int32_t *start_column, bool *has_virtual_ancestor) {
    *start_row = 0;
    *start_column = 0;
    *has_virtual_ancestor = false;
    int32_t x = 0, y = 0;
    unsigned depth = 0;
    ImageRef cell_ref = {0};
    while (ref->parent.img) {
        if (depth++ >= PARENT_DEPTH_LIMIT) return false; // either a cycle or too many ancestors
        Image *img = img_by_internal_id(self, ref->parent.img);
        if (!img) return false;
        ImageRef *parent = ref_by_internal_id(img, ref->parent.ref);
        if (!parent) return false;
        if (parent->is_virtual_ref) {
            *has_virtual_ancestor = true;
            if (!resolve_cell_ref(img, parent->internal_id, &cell_ref.start_row, &cell_ref.start_column)) return false;
            parent = &cell_ref;
        }
        x += ref->parent.offset.x;
        y += ref->parent.offset.y;
        ref = parent;
    }
    *start_row = ref->start_row + y;
    *start_column = ref->start_column + x;
    return true;
}

// Room for one more item in the render data, false when out of memory
static bool
ensure_render_data_space(GraphicsManager *self) {
    if (self->render_data.count < self->render_data.capacity) return true;
    const size_t old = self->render_data.capacity, item_sz = sizeof(self->render_data.item[0]);
    if (old > SIZE_MAX / 2 / item_sz) return false;
    const size_t cap = old ? 2 * old : 64;
    ImageRenderData *items = allocation_fails(ALLOCATION_RENDER_DATA) ? NULL : realloc(self->render_data.item, cap * item_sz);
    if (!items) return false;
    memset(items + old, 0, (cap - old) * item_sz);
    self->render_data.item = items;
    self->render_data.capacity = cap;
    return true;
}

bool
grman_update_layers(
    GraphicsManager *self,
    unsigned int scrolled_by,
    float scroll_offset_lines,
    float screen_left,
    float screen_top,
    float dx,
    float dy,
    unsigned int num_cols,
    unsigned int num_rows,
    CellPixelSize cell) {
    if (self->last_scrolled_by != scrolled_by || self->last_scroll_offset_lines != scroll_offset_lines) set_layers_dirty(self);
    self->last_scrolled_by = scrolled_by;
    self->last_scroll_offset_lines = scroll_offset_lines;
    if (!self->layers_dirty) return false;
    self->layers_dirty = false;
    size_t i;
    self->num_of_below_refs = 0;
    self->num_of_negative_refs = 0;
    self->num_of_positive_refs = 0;
    self->num_of_indexed_refs = 0;
    ImageRect r;
    float screen_width = dx * num_cols, screen_height = dy * num_rows;
    float screen_bottom = screen_top - screen_height;
    float screen_width_px = num_cols * cell.width;
    float screen_height_px = num_rows * cell.height;
    float y0 = screen_top - dy * ((float)scrolled_by + scroll_offset_lines);

    // Iterate over all visible refs and create render data
    self->render_data.count = 0;
    self->render_data.has_hdr = false;

    for (image_map_itr imgitr = vt_first(&self->images_by_internal_id); !vt_is_end(imgitr);) {
        Image *img = imgitr.data->val;
        bool was_drawn = img->is_drawn, ref_removed = false;
        img->is_drawn = false;
        double vector_width = 0, vector_height = 0;  // the largest size the image is shown at
        ImageFraction vector_shown = {.left = 1, .top = 1, .right = 0, .bottom = 0};  // the parts of it on screen
        const size_t first_render_item = self->render_data.count;

        for (ref_map_itr refitr = vt_first(&img->refs_by_internal_id); !vt_is_end(refitr);) {
            ImageRef *ref = refitr.data->val;
            if (ref->is_virtual_ref) {
                refitr = vt_next(refitr);
                continue;
            }
            int32_t start_row = ref->start_row, start_column = ref->start_column;
            if (ref->parent.img) {
                bool has_virtual_ancestor;
                if (!resolve_parent_offset(self, ref, &start_row, &start_column, &has_virtual_ancestor)) {
                    if (!has_virtual_ancestor) {
                        refitr = remove_ref_itr(img, refitr);
                        ref_removed = true;
                    } else refitr = vt_next(refitr);
                    continue;
                }
            }
            r.top = y0 - start_row * dy - dy * (float)ref->cell_y_offset / (float)cell.height;
            r.left = screen_left + start_column * dx + dx * (float)ref->cell_x_offset / (float)cell.width;

            int32_t nr = ref->num_rows, nc = ref->num_cols;
            if (nr) {
                r.bottom = y0 - (start_row + nr) * dy;
                if (nc) r.right = screen_left + (start_column + nc) * dx;
                else {
                    double height_px = (((double)r.top - r.bottom) / screen_height) * screen_height_px;
                    double width_px = height_px * ref->src_width / (double)ref->src_height;
                    r.right = r.left + (float)((width_px / screen_width_px) * screen_width);
                }
            } else {
                if (nc) r.right = screen_left + (start_column + nc) * dx;
                else r.right = r.left + screen_width * (float)ref->src_width / screen_width_px;
                double width_px = (((double)r.right - r.left) / screen_width) * screen_width_px;
                double height_px = width_px * ref->src_height / (double)ref->src_width;
                r.bottom = r.top - (float)((height_px / screen_height_px) * screen_height);
            }

            if (r.top <= screen_bottom || r.bottom >= screen_top) {
                refitr = vt_next(refitr);
                continue;
            } // not visible
            if (img->vector) {
                const double rw = (double)r.right - r.left, rh = (double)r.top - r.bottom;
                double w = rw / screen_width * screen_width_px * img->width / ref->src_width;
                double h = rh / screen_height * screen_height_px * img->height / ref->src_height;
                vector_width = MAX(vector_width, w);
                vector_height = MAX(vector_height, h);
                if (rw > 0 && rh > 0) {
                    // the part of the placement on screen, and so of the image, in doubles since a part of a
                    // large image is a tiny fraction of it
                    const double fx0 = (MAX((double)r.left, (double)screen_left) - r.left) / rw;
                    const double fx1 = (MIN((double)r.right, (double)screen_left + screen_width) - r.left) / rw;
                    const double fy0 = ((double)r.top - MIN((double)r.top, (double)screen_top)) / rh;
                    const double fy1 = ((double)r.top - MAX((double)r.bottom, (double)screen_bottom)) / rh;
                    if (fx0 < fx1 && fy0 < fy1) {
                        vector_shown.left = MIN(vector_shown.left, (ref->src_x + fx0 * ref->src_width) / img->width);
                        vector_shown.right = MAX(vector_shown.right, (ref->src_x + fx1 * ref->src_width) / img->width);
                        vector_shown.top = MIN(vector_shown.top, (ref->src_y + fy0 * ref->src_height) / img->height);
                        vector_shown.bottom = MAX(vector_shown.bottom, (ref->src_y + fy1 * ref->src_height) / img->height);
                    }
                }
            }

            if (!ensure_render_data_space(self)) {
                log_error("Out of memory for the render data of images, a placement is not drawn");
                self->layers_dirty = true;  // try again for the next frame
                refitr = vt_next(refitr);
                continue;
            }
            if (ref->z_index < ((int32_t)INT32_MIN / 2)) self->num_of_below_refs++;
            else if (ref->z_index < 0) self->num_of_negative_refs++;
            else self->num_of_positive_refs++;
            ImageRenderData *rd = self->render_data.item + self->render_data.count;
            zero_at_ptr(rd);
            rd->dest_rect = r;
            rd->src_rect = ref->src_rect;
            self->render_data.has_hdr |= img->root_frame.is_float || transfer_is_hdr(img->transfer);
            self->render_data.count++;
            rd->z_index = ref->z_index;
            rd->depth = ref->depth;
            rd->depth_pixels = ref->depth_pixels;
            rd->phase = ref->phase;
            rd->image_id = img->internal_id;
            rd->ref_id = ref->internal_id;
            rd->texture_id = texture_id_for_img(img);
            rd->peak = img->peak;
            rd->tone_map = img->tone_map;
            rd->gamut = img->gamut;
            rd->decode = img->vector || img->surface ? IMAGE_DECODE_NONE : image_decode_for(img->root_frame.deep, img->transfer);
            rd->is_gray = img->root_frame.deep == DEEP_GRAY16;
            rd->indexed_px_sz = img->indexed_px_sz;
            rd->palette_texture_id = img->palette ? img->palette->texture : 0;
            if (img->indexed_px_sz) self->num_of_indexed_refs++;
            if (img->indexed_px_sz || rd->decode != IMAGE_DECODE_NONE) {
                // The rect is in normalized device co-ordinates, so allow for rounding errors
                const float width_px = (r.right - r.left) / screen_width * screen_width_px;
                const float height_px = (r.top - r.bottom) / screen_height * screen_height_px;
                rd->is_unscaled = fabsf(width_px - ref->src_width) < 0.01f && fabsf(height_px - ref->src_height) < 0.01f;
            }
            rd->filter_decoded = rd->decode != IMAGE_DECODE_NONE && !rd->is_unscaled;
            img->is_drawn = true;
            refitr = vt_next(refitr);
        }
        if (ref_removed && !vt_size(&img->refs_by_internal_id)) {
            imgitr = remove_image_itr(self, imgitr);
            continue;
        }
        if (img->vector && img->is_drawn) {
            update_vector_texture(self, img, vector_width, vector_height, vector_shown, screen_width_px, screen_height_px);
            const VectorView v = img->vector_view;
            for (size_t k = first_render_item; k < self->render_data.count; k++) {
                ImageRenderData *rd = self->render_data.item + k;
                rd->texture_id = texture_id_for_img(img);
                if (!v.cols || !v.rows) continue;
                // The texture holds a part of the image, the coordinates of what to show are of all of it
                ref_map_itr ri = vt_get(&img->refs_by_internal_id, rd->ref_id);
                if (vt_is_end(ri)) continue;
                const ImageRef *ref = ri.data->val;
                const double sx = v.width / img->width, sy = v.height / img->height;
                rd->src_rect.left = (float)((ref->src_x * sx - v.x) / v.cols);
                rd->src_rect.right = (float)(((ref->src_x + ref->src_width) * sx - v.x) / v.cols);
                rd->src_rect.top = (float)((ref->src_y * sy - v.y) / v.rows);
                rd->src_rect.bottom = (float)(((ref->src_y + ref->src_height) * sy - v.y) / v.rows);
            }
        }
        if (img->is_drawn && !was_drawn && img->animation_state != ANIMATION_STOPPED && img->extra_framecnt && img->animation_duration) {
            self->has_images_needing_animation = true;
            global_state.check_for_active_animated_images = true;
        }
        imgitr = vt_next(imgitr);
    }
    if (!self->render_data.count) return false;
    // Sort visible refs in draw order (z-index, img, ref)
#define lt(a, b)                    \
    ((a)->z_index < (b)->z_index || \
     ((a)->z_index == (b)->z_index && ((a)->image_id < (b)->image_id || ((a)->image_id == (b)->image_id && a->ref_id < b->ref_id))))
    QSORT(ImageRenderData, self->render_data.item, self->render_data.count, lt);
#undef lt
    // Calculate the group counts. Refs drawn with different programs must not
    // be in the same group as programs are switched only between groups.
    i = 0;
    while (i < self->render_data.count) {
        id_type num_identical = 1, image_id = self->render_data.item[i].image_id, start = i;
        const bool is_unscaled = self->render_data.item[i].is_unscaled;
        while (++i < self->render_data.count) {
            if (self->render_data.item[i].image_id != image_id || self->render_data.item[i].is_unscaled != is_unscaled) break;
            num_identical++;
        }
        while (num_identical > 0) { self->render_data.item[start++].group_count = num_identical--; }
    }
    return true;
}

// }}}

// Animation {{{
#define DEFAULT_GAP 40

static Frame *
current_frame(Image *img) {
    if (img->current_frame_index > img->extra_framecnt) return NULL;
    return img->current_frame_index ? img->extra_frames + img->current_frame_index - 1 : &img->root_frame;
}

static Frame *
frame_for_id(Image *img, const uint32_t frame_id) {
    if (img->root_frame.id == frame_id) return &img->root_frame;
    for (unsigned i = 0; i < img->extra_framecnt; i++) {
        if (img->extra_frames[i].id == frame_id) return img->extra_frames + i;
    }
    return NULL;
}

static Frame *
frame_for_number(Image *img, const uint32_t frame_number) {
    switch (frame_number) {
        case 1: return &img->root_frame;
        case 0: return NULL;
        default:
            if (frame_number - 2 < img->extra_framecnt) return img->extra_frames + frame_number - 2;
            return NULL;
    }
}

static void
change_gap(Image *img, Frame *f, int32_t gap) {
    uint32_t prev_gap = f->gap;
    f->gap = MAX(0, gap);
    img->animation_duration = prev_gap < img->animation_duration ? img->animation_duration - prev_gap : 0;
    img->animation_duration += f->gap;
}

typedef struct {
    bool needs_blending;
    uint32_t over_px_sz, under_px_sz;
    uint32_t over_width, over_height, under_width, under_height, over_offset_x, over_offset_y, under_offset_x, under_offset_y;
    uint32_t stride;
} ComposeData;

static void
blend_over_half(uint8_t *under_row, const uint8_t *over_row, const unsigned num_px) {
    // the over operator on straight alpha, in linear light, see blend_pixel_over_straight()
    uint16_t *d = (uint16_t *)under_row;
    const uint16_t *s = (const uint16_t *)over_row;
    for (unsigned x = 0; x < num_px; x++, d += 4, s += 4) {
        const float alpha = half_to_float(s[3]), dst_alpha = half_to_float(d[3]) * (1.f - alpha), out_alpha = alpha + dst_alpha;
        if (out_alpha <= 0.f) continue;
        for (unsigned c = 0; c < 3; c++) d[c] = float_to_half((half_to_float(s[c]) * alpha + half_to_float(d[c]) * dst_alpha) / out_alpha);
        d[3] = float_to_half(out_alpha);
    }
}

static void
copy_or_blend_row(
    uint8_t *under_row, const uint8_t *over_row, const unsigned num_px, const uint32_t under_px_sz, const uint32_t over_px_sz, const bool needs_blending) {
    if (needs_blending && over_px_sz == 8) blend_over_half(under_row, over_row, num_px);
    else if (needs_blending && over_px_sz == 4) {
        // over is straight alpha RGBA
        if (under_px_sz == 4) blend_over_straight(under_row, over_row, num_px);
        else blend_over_opaque(under_row, 3, over_row, num_px);
    } else {
        for (unsigned x = 0; x < num_px; x++) {
            uint8_t *under_px = under_row + (size_t)under_px_sz * x;
            const uint8_t *over_px = over_row + (size_t)over_px_sz * x;
            under_px[0] = over_px[0];
            under_px[1] = over_px[1];
            under_px[2] = over_px[2];
            if (under_px_sz == 4) under_px[3] = over_px_sz == 4 ? over_px[3] : 255;
        }
    }
}

static void
compose_rectangles(const ComposeData d, uint8_t *under_data, const size_t under_data_sz, const uint8_t *over_data, const size_t over_data_sz) {
    // compose two equal sized, non-overlapping rectangles at different offsets.
    // The per-row guard ensures that inconsistent geometry (offsets or
    // dimensions larger than the actual buffers) can never cause an out of
    // bounds read or write. All offset arithmetic is done in size_t to avoid
    // 32-bit overflow.
    const bool can_copy_rows = !d.needs_blending && d.over_px_sz == d.under_px_sz;
    const unsigned min_width = MIN(d.under_width, d.over_width);
    for (unsigned y = 0; y < d.under_height && y < d.over_height; y++) {
        const size_t under_off = (size_t)(y + d.under_offset_y) * d.under_px_sz * d.stride + (size_t)d.under_offset_x * d.under_px_sz;
        const size_t over_off = (size_t)(y + d.over_offset_y) * d.over_px_sz * d.stride + (size_t)d.over_offset_x * d.over_px_sz;
        if (under_off + (size_t)d.under_px_sz * min_width > under_data_sz || over_off + (size_t)d.over_px_sz * min_width > over_data_sz) break;
        uint8_t *under_row = under_data + under_off;
        const uint8_t *over_row = over_data + over_off;
        if (can_copy_rows) memcpy(under_row, over_row, (size_t)d.over_px_sz * min_width);
        else copy_or_blend_row(under_row, over_row, min_width, d.under_px_sz, d.over_px_sz, d.needs_blending);
    }
}

static void
compose(const ComposeData d, uint8_t *under_data, const size_t under_data_sz, const uint8_t *over_data, const size_t over_data_sz) {
    // The per-row guard ensures that inconsistent geometry (an overlay
    // declaring more data than it actually has, or an oversized base) can
    // never cause an out of bounds read or write. All offset arithmetic is
    // done in size_t to avoid 32-bit overflow.
    const bool can_copy_rows = !d.needs_blending && d.over_px_sz == d.under_px_sz;
    unsigned min_row_sz = d.over_offset_x < d.under_width ? d.under_width - d.over_offset_x : 0;
    min_row_sz = MIN(min_row_sz, d.over_width);
    for (unsigned y = 0; y + d.over_offset_y < d.under_height && y < d.over_height; y++) {
        const size_t under_off = (size_t)(y + d.over_offset_y) * d.under_px_sz * d.under_width + (size_t)d.under_px_sz * d.over_offset_x;
        const size_t over_off = (size_t)y * d.over_px_sz * d.over_width;
        if (under_off + (size_t)d.under_px_sz * min_row_sz > under_data_sz || over_off + (size_t)d.over_px_sz * min_row_sz > over_data_sz) break;
        uint8_t *under_row = under_data + under_off;
        const uint8_t *over_row = over_data + over_off;
        if (can_copy_rows) memcpy(under_row, over_row, (size_t)d.over_px_sz * min_row_sz);
        else copy_or_blend_row(under_row, over_row, min_row_sz, d.under_px_sz, d.over_px_sz, d.needs_blending);
    }
}

static CoalescedFrameData
get_coalesced_frame_data_standalone(const Image *img, const Frame *f, uint8_t *frame_data, const size_t frame_data_sz) {
    CoalescedFrameData ans = {0};
    ans.transient = f->transient;
    bool is_full_frame = f->width == img->width && f->height == img->height && !f->x && !f->y;
    if (is_full_frame) {
        ans.buf = frame_data;
        ans.is_4byte_aligned = f->is_4byte_aligned;
        ans.is_opaque = f->is_opaque;
        ans.is_float = f->is_float;
        ans.deep = f->deep;  // only the root frame, which is always a full frame, can be high precision
        return ans;
    }
    const unsigned bytes_per_pixel = pixel_size(f->is_opaque, f->is_float);
    uint8_t *base;
    if (f->bgcolor) {
        base = malloc((size_t)img->width * img->height * bytes_per_pixel);
        if (base) {
            uint8_t *p = base;
            const uint8_t r = (f->bgcolor >> 24) & 0xff, g = (f->bgcolor >> 16) & 0xff, b = (f->bgcolor >> 8) & 0xff, a = f->bgcolor & 0xff;
            if (bytes_per_pixel == 8) {
                const uint16_t px[4] = {float_to_half(r / 255.f), float_to_half(g / 255.f), float_to_half(b / 255.f), float_to_half(a / 255.f)};
                for (uint32_t i = 0; i < img->width * img->height; i++, p += sizeof(px)) memcpy(p, px, sizeof(px));
            } else if (bytes_per_pixel == 4) {
                for (uint32_t i = 0; i < img->width * img->height; i++) {
                    *(p++) = r;
                    *(p++) = g;
                    *(p++) = b;
                    *(p++) = a;
                }
            } else {
                for (uint32_t i = 0; i < img->width * img->height; i++) {
                    *(p++) = r;
                    *(p++) = g;
                    *(p++) = b;
                }
            }
        }
    } else base = calloc((size_t)img->width * img->height, bytes_per_pixel);
    if (!base) {
        free(frame_data);
        return ans;
    }
    ComposeData d = {
        .over_px_sz = bytes_per_pixel,
        .under_px_sz = bytes_per_pixel,
        .over_width = f->width,
        .over_height = f->height,
        .over_offset_x = f->x,
        .over_offset_y = f->y,
        .under_width = img->width,
        .under_height = img->height,
        .needs_blending = f->alpha_blend && !f->is_opaque};
    compose(d, base, (size_t)img->width * img->height * bytes_per_pixel, frame_data, frame_data_sz);
    ans.buf = base;
    ans.is_4byte_aligned = bytes_per_pixel != 3 || (img->width % 4) == 0;
    ans.is_opaque = f->is_opaque;
    ans.is_float = f->is_float;
    free(frame_data);
    return ans;
}


static CoalescedFrameData
get_coalesced_frame_data_impl(GraphicsManager *self, Image *img, const Frame *f, unsigned count) {
    CoalescedFrameData ans = {0};
    if (count > 32) return ans; // prevent stack overflows, infinite recursion
    size_t frame_data_sz;
    void *frame_data;
    ImageAndFrame key = {.image_id = img->internal_id, .frame_id = f->id};
    if (!read_from_cache(self, key, &frame_data, &frame_data_sz)) return ans;
    if (!f->base_frame_id) return get_coalesced_frame_data_standalone(img, f, frame_data, frame_data_sz);
    Frame *base = frame_for_id(img, f->base_frame_id);
    if (!base) {
        free(frame_data);
        return ans;
    }
    CoalescedFrameData base_data = get_coalesced_frame_data_impl(self, img, base, count + 1);
    if (!base_data.buf) {
        free(frame_data);
        return ans;
    }
    ComposeData d = {
        .over_px_sz = pixel_size(f->is_opaque, f->is_float),
        .under_px_sz = pixel_size(base_data.is_opaque, base_data.is_float),
        .over_width = f->width,
        .over_height = f->height,
        .over_offset_x = f->x,
        .over_offset_y = f->y,
        .under_width = img->width,
        .under_height = img->height,
        .needs_blending = f->alpha_blend && !f->is_opaque};
    compose(d, base_data.buf, (size_t)img->width * img->height * d.under_px_sz, frame_data, frame_data_sz);
    free(frame_data);
    base_data.transient = base_data.transient || f->transient;
    return base_data;
}

static CoalescedFrameData
get_coalesced_frame_data(GraphicsManager *self, Image *img, const Frame *f) {
    return get_coalesced_frame_data_impl(self, img, f, 0);
}

// Whether the pixels of a frame are something other than eight bit sRGB or
// half float, which the GPU decodes by itself, see docs/silkie/precision-image-protocol.rst
static bool
needs_decoding(const Image *img, const CoalescedFrameData *cfd) {
    return cfd->deep || (img->transfer != TRANSFER_SRGB && !cfd->is_float);
}

// Decodes the pixels of a frame that needs_decoding() a row at a time into
// linear RGBA floats in the sRGB primaries, where 1.0 is SDR white, as the
// graphics shader does, handing each row to emit
static bool
decode_frame_rows(const Image *img, const CoalescedFrameData *cfd, void (*emit)(void *ctx, uint32_t y, const float *rgba), void *ctx) {
    PixelDecoder d;
    if (!pixel_decoder_init(&d, cfd->deep, cfd->is_opaque, img->transfer, img->gamut)) return false;
    float *row = malloc((size_t)img->width * 4 * sizeof(float));
    if (!row) {
        pixel_decoder_free(&d);
        return false;
    }
    const size_t stride = (size_t)img->width * stored_pixel_size(cfd->is_opaque, cfd->is_float, cfd->deep);
    for (uint32_t y = 0; y < img->height; y++) {
        pixel_decoder_decode(&d, cfd->buf + y * stride, img->width, true, row);
        emit(ctx, y, row);
    }
    free(row);
    pixel_decoder_free(&d);
    return true;
}

typedef struct {
    uint16_t *out;
    uint32_t width;
} HalfRows;

static void
emit_half_row(void *ctx, uint32_t y, const float *rgba) {
    HalfRows *h = ctx;
    uint16_t *dest = h->out + (size_t)y * h->width * 4;
    for (size_t i = 0; i < (size_t)h->width * 4; i++) dest[i] = float_to_half(rgba[i]);
}

typedef struct {
    uint8_t *out;
    uint32_t width;
} Srgb8Rows;

static void
emit_srgb8_row(void *ctx, uint32_t y, const float *rgba) {
    Srgb8Rows *h = ctx;
    uint8_t *dest = h->out + (size_t)y * h->width * 4;
    for (size_t i = 0; i < (size_t)h->width * 4; i++) {
        float v = rgba[i];
        v = v > 0.f ? (v < 1.f ? v : 1.f) : 0.f;  // also takes NaN to zero
        if (i % 4 != 3) v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
        dest[i] = (uint8_t)lrintf(v * 255.f);
    }
}

// Hands each row of a half float frame (f=64) to emit as floats, like decode_frame_rows(): its values are already
// linear, in the sRGB primaries, with straight alpha
static bool
half_frame_rows(const Image *img, const CoalescedFrameData *cfd, void (*emit)(void *ctx, uint32_t y, const float *rgba), void *ctx) {
    const size_t num = (size_t)img->width * 4;
    float *row = malloc(num * sizeof(float));
    if (!row) return false;
    for (uint32_t y = 0; y < img->height; y++) {
        const uint8_t *src = cfd->buf + (size_t)y * num * sizeof(uint16_t);
        for (size_t i = 0; i < num; i++) {
            uint16_t h;  // memcpy as the rows need not be aligned
            memcpy(&h, src + i * sizeof(h), sizeof(h));
            row[i] = half_to_float(h);
        }
        emit(ctx, y, row);
    }
    free(row);
    return true;
}

// The pixels of a frame that needs_decoding() as half float RGBA, the f=64 representation, or NULL when out of memory
static uint8_t *
decode_frame_to_half(const Image *img, const CoalescedFrameData *cfd) {
    if ((size_t)img->width * img->height > SIZE_MAX / 8) return NULL;
    HalfRows h = {.out = malloc((size_t)img->width * img->height * 8), .width = img->width};
    if (!h.out) return NULL;
    if (!decode_frame_rows(img, cfd, emit_half_row, &h)) {
        free(h.out);
        return NULL;
    }
    return (uint8_t *)h.out;
}

// Vector images {{{

// The pixels of a palette image as RGBA: its colors, straight alpha, in the gamut of the image
static uint8_t *
palette_image_rgba(GraphicsManager *self, Image *img) {
    CoalescedFrameData cfd = get_coalesced_frame_data(self, img, &img->root_frame);
    if (!cfd.buf) return NULL;
    // the indices are a byte each, so the number of pixels fits in memory, four bytes for each might not
    const size_t num = (size_t)img->width * img->height;
    uint8_t *ans = num <= SIZE_MAX / 4 ? malloc(num * 4) : NULL;
    if (ans) for (size_t i = 0; i < num; i++) memcpy(ans + 4 * i, img->palette->rgba + 4 * (size_t)cfd.buf[i], 4);
    free(cfd.buf);
    return ans;
}

static bool
vector_lookup(void *ctx, uint32_t image_id, bool with_pixels, VectorImageData *out) {
    GraphicsManager *self = ctx;
    Image *img = img_by_client_id(self, image_id);
    // The pixels of an indexed color image are indices into the palette of a
    // window, not colors, so a vector image cannot draw one. A palette image
    // has a palette of its own.
    if (!img || img->vector || (img->indexed_px_sz && !img->palette) || !img->root_frame_data_loaded) return false;
    out->width = img->width;
    out->height = img->height;
    // High precision pixels, and those with a transfer function other than sRGB, are drawn as linear half floats
    const bool decoded = img->root_frame.deep || (img->transfer != TRANSFER_SRGB && !img->root_frame.is_float);
    out->is_float = img->root_frame.is_float || decoded;
    out->gamut = decoded ? 0 : img->gamut;
    if (!with_pixels) return true;
    if (img->palette) {
        if (!(out->data = palette_image_rgba(self, img))) return false;
        out->is_opaque = false;
        out->is_float = false;
        return true;
    }
    CoalescedFrameData cfd = get_coalesced_frame_data(self, img, &img->root_frame);
    if (!cfd.buf) return false;
    if (needs_decoding(img, &cfd)) {
        uint8_t *half = decode_frame_to_half(img, &cfd);
        free(cfd.buf);
        if (!half) return false;
        cfd = (CoalescedFrameData){.buf = half, .is_float = true};
    }
    out->data = cfd.buf;
    out->is_opaque = cfd.is_opaque;
    out->is_float = cfd.is_float;
    return true;
}

static uint8_t *
render_vector_image(GraphicsManager *self, Image *img, unsigned width, unsigned height) {
    return vector_doc_render(img->vector, width, height, vector_lookup, self);
}

// Rasterizes what has to be drawn again of a vector image shown at width x
// height: all of it when there are no pixels of that size to patch, otherwise
// the part the records applied since the last time changed. Returns the pixels
// of rect, or NULL when nothing changed or on failure.
static uint8_t *
render_vector_damage(GraphicsManager *self, Image *img, unsigned width, unsigned height, bool all, VectorRect *rect) {
    *rect = (VectorRect){0, 0, width, height};
    if (!all && !vector_doc_damage(img->vector, width, height, rect)) {
        vector_doc_clear_damage(img->vector);  // it was all off the image
        return NULL;
    }
    uint8_t *ans = vector_doc_render_rect(img->vector, width, height, *rect, vector_lookup, self);
    if (ans) vector_doc_clear_damage(img->vector);
    return ans;
}

unsigned long kitty_vector_depth_limit(void) { return OPT(vector_depth_limit); }
unsigned long kitty_vector_dash_limit(void) { return OPT(vector_dash_limit); }
unsigned long kitty_vector_curve_limit(void) { return OPT(vector_curve_limit); }

// The part of a vector image to rasterize, for the image shown at width x height
// pixels with the fractions of it in shown on screen, in a window view_width x
// view_height pixels. current is the part held now, kept while it will do.
//
// A raster bigger than the window is never needed in full: only what is on
// screen can be seen, in physical pixels, at the size it is shown. Rasterizing
// the whole image instead would take memory in proportion to the size it is
// shown at, which a placement much larger than the window, or one showing a
// small part of the image magnified, makes as large as anyone likes. So the
// part on screen is rasterized, with a quarter of the window more all round,
// so that scrolling a little does not draw it again, or the whole image when
// that is no bigger. The size the image is drawn at stays that of the largest
// placement, unless several placements show different parts of it at
// different magnifications that would together take more than four windows'
// worth of pixels at the largest: then it is reduced until they fit. Doubles
// count pixels exactly up to 2^53, and an image shown larger than that is
// reduced to it.
static VectorView
choose_vector_view(double width, double height, ImageFraction shown, double view_width, double view_height, const VectorView *current) {
    const double exact = 9007199254740992.0;
    view_width = MAX(1., view_width), view_height = MAX(1., view_height);
    if (width > exact || height > exact) {
        const double s = exact / MAX(width, height);
        width *= s, height *= s;
    }
    double W = MAX(1., round(width)), H = MAX(1., round(height)), nx0, ny0, nx1, ny1;
    for (int pass = 0; pass < 2; pass++) {
        // what is on screen in pixels of the raster, with two more for the filtering at its edges
        nx0 = MAX(0., floor(shown.left * W) - 2), nx1 = MIN(W, ceil(shown.right * W) + 2);
        ny0 = MAX(0., floor(shown.top * H) - 2), ny1 = MIN(H, ceil(shown.bottom * H) + 2);
        if (!(nx0 < nx1)) nx0 = 0, nx1 = MIN(W, 1.);
        if (!(ny0 < ny1)) ny0 = 0, ny1 = MIN(H, 1.);
        const double budget = 4 * view_width * view_height, area = (nx1 - nx0) * (ny1 - ny0);
        if (area <= budget || pass) break;
        const double s = sqrt(budget / area);
        W = MAX(1., round(W * s)), H = MAX(1., round(H * s));
    }
    const double mx = ceil(view_width / 4), my = ceil(view_height / 4);
    if (W * H <= (nx1 - nx0 + 2 * mx) * (ny1 - ny0 + 2 * my) && W <= INT32_MAX && H <= INT32_MAX) return (VectorView){W, H, 0, 0, (unsigned)W, (unsigned)H};
    if (current && current->width == W && current->height == H && current->x <= nx0 && current->y <= ny0 && current->x + current->cols >= nx1 &&
        current->y + current->rows >= ny1)
        return *current;
    const double x0 = MAX(0., nx0 - mx), y0 = MAX(0., ny0 - my), x1 = MIN(W, nx1 + mx), y1 = MIN(H, ny1 + my);
    return (VectorView){W, H, x0, y0, (unsigned)(x1 - x0), (unsigned)(y1 - y0)};
}

static inline bool
same_vector_view(const VectorView *a, const VectorView *b) {
    return a->width == b->width && a->height == b->height && a->x == b->x && a->y == b->y && a->cols == b->cols && a->rows == b->rows;
}

// Draws view of a vector image, for one eye, into img->texture: all of it, or
// when the texture already holds that view and all is false only what changed.
static bool
update_vector_eye(GraphicsManager *self, Image *img, VectorView view, VectorStereo stereo, bool all) {
    const bool has_pixels = !all && img->texture && img->texture->id && same_vector_view(&img->vector_view, &view);
    VectorRect rect = {0, 0, view.cols, view.rows};
    if (has_pixels && !vector_doc_damage_view(img->vector, view, &rect)) return true;
    float *linear = vector_doc_render_view_stereo(img->vector, view, rect, vector_lookup, self, stereo);
    if (!linear) {
        // out of memory, most likely: the image stays as it was until something about it changes
        log_error("Failed to rasterize %ux%u pixels of a vector image", rect.width, rect.height);
        return false;
    }
    // the render above allocated four floats a pixel, so these sizes cannot overflow
    const size_t count = (size_t)rect.width * rect.height * 4;
    uint16_t *data = malloc(count * sizeof(uint16_t));
    if (!data) {
        free(linear);
        log_error("Out of memory converting %ux%u pixels of a vector image", rect.width, rect.height);
        return false;
    }
    float peak = 0;
    for (size_t i = 0; i < count; i += 4) {
        if (linear[i + 3] == 0.f) {
            // Transparent damage often contains mostly untouched pixels. Only
            // bitwise zero RGBA may skip conversion: preserve negative zero
            // and RGB values (including HDR) even when alpha is zero.
            uint64_t bits[2];
            memcpy(bits, linear + i, sizeof(bits));
            if (!(bits[0] | bits[1])) {
                memset(data + i, 0, 4 * sizeof(*data));
                continue;
            }
        }
        for (unsigned k = 0; k < 4; k++) {
            // RGBA16F is the existing GPU transport format, so saturate only to
            // its representable range, never to SDR or the display's headroom.
            const float value = fminf(65504, fmaxf(-65504, linear[i + k]));
            data[i + k] = float_to_half(value);
            if (k != 3) peak = fmaxf(peak, value);
        }
    }
    free(linear);
    img->peak = has_pixels ? MAX(img->peak, peak) : peak;
    if (img->texture) {
        if (!has_pixels) send_image_to_gpu(&img->texture->id, data, view.cols, view.rows, false, true, true, true, REPEAT_CLAMP);
        else update_image_on_gpu(img->texture->id, data, rect.x, rect.y, rect.width, rect.height, true);
    }
    for (size_t i = 0; i < self->render_data.count; i++) if (self->render_data.item[i].image_id == img->internal_id) {
        self->render_data.item[i].peak = img->peak;
        self->render_data.item[i].texture_id = texture_id_for_img(img);
    }
    free(data);
    return true;
}

// Brings the textures of a vector image up to date, see choose_vector_view()
// for what they hold. vector_raster_limit, when set, caps the size the image is
// drawn at before that.
static void
update_vector_texture(GraphicsManager *self, Image *img, double width, double height, ImageFraction shown, double view_width, double view_height) {
    if (!self->window_id || !(width >= 1) || !(height >= 1)) return;
    const unsigned long cap = OPT(vector_raster_limit);
    if (cap && width * height > (double)cap) {
        double scale = sqrt((double)cap / (width * height));
        if (scale < 1) width *= scale, height *= scale;
    }
    const VectorView view = choose_vector_view(width, height, shown, view_width, view_height, &img->vector_view);
    const bool same_view = same_vector_view(&img->vector_view, &view);
    OSWindow *w = os_window_for_kitty_window(self->window_id);
    VectorStereo stereo = {.multiplier = 1.};
    const bool enabled = w && w->spatial.mode && vector_doc_has_depth(img->vector);
    if (enabled) stereo = (VectorStereo){
        .eye_pixels = -w->viewport_width / (w->spatial.mode == 2 ? 2. * w->spatial.scale : 2.),
        .positive = w->spatial.positive, .negative = w->spatial.negative, .fov = w->spatial.fov,
        .phase_pixels = -0.5, .phase_limit = w->spatial.phase_limit, .multiplier = w->spatial.multiplier};
    const bool changed = memcmp(&stereo, &img->vector_stereo, sizeof(stereo)) != 0;
    VectorRect damage;
    const bool dirty = vector_doc_damage_view(img->vector, img->vector_view, &damage);
    if (enabled) {
        if (changed || dirty || !same_view || !img->vector_right_texture) {
            if (!img->vector_right_texture) img->vector_right_texture = new_texture_ref();
            if (!update_vector_eye(self, img, view, stereo, true)) return;
            const float left_peak = img->peak;
            TextureRef *left = img->texture;
            img->texture = img->vector_right_texture;
            stereo.eye_pixels = -stereo.eye_pixels;
            stereo.phase_pixels = -stereo.phase_pixels;
            const bool success = update_vector_eye(self, img, view, stereo, true);
            stereo.eye_pixels = -stereo.eye_pixels;
            stereo.phase_pixels = -stereo.phase_pixels;
            img->vector_right_texture = img->texture;
            img->texture = left;
            if (!success) return;
            img->peak = MAX(img->peak, left_peak);
        }
    } else if (!update_vector_eye(self, img, view, stereo, changed)) return;
    img->vector_view = view;
    img->vector_stereo = stereo;
    vector_doc_clear_damage(img->vector);
    for (size_t i = 0; i < self->render_data.count; i++) if (self->render_data.item[i].image_id == img->internal_id) {
        self->render_data.item[i].texture_id = texture_id_for_img(img);
        self->render_data.item[i].right_texture_id = enabled && img->vector_right_texture ? img->vector_right_texture->id : 0;
        self->render_data.item[i].peak = img->peak;
        if (enabled) {
            self->render_data.item[i].depth = 0;
            self->render_data.item[i].depth_pixels = self->render_data.item[i].phase = 0.f;
        }
    }
}

// Terminal-side ink, see docs/ink-protocol.rst. The terminal applies to a
// vector image the records a program would have sent for a stroke of the pen.

// The rectangle a placement is shown in, in pixels from the top left of the
// cell area, by the arithmetic of grman_update_layers()
static bool
ref_rect_in_pixels(const GraphicsManager *self, const ImageRef *ref, const InkView *view, double *left, double *top, double *width, double *height) {
    if (ref->is_virtual_ref || !(ref->src_width > 0) || !(ref->src_height > 0)) return false;
    int32_t start_row = ref->start_row, start_column = ref->start_column;
    if (ref->parent.img) {
        bool has_virtual_ancestor;
        if (!resolve_parent_offset(self, ref, &start_row, &start_column, &has_virtual_ancestor)) return false;
    }
    const double cw = view->cell.width, ch = view->cell.height, first_row_at = (double)view->scrolled_by * ch + view->scroll_offset;
    *left = start_column * cw + ref->cell_x_offset;
    *top = first_row_at + start_row * ch + ref->cell_y_offset;
    if (ref->num_rows) {
        *height = first_row_at + ((double)start_row + ref->num_rows) * ch - *top;
        *width = ref->num_cols ? ((double)start_column + ref->num_cols) * cw - *left : *height * ref->src_width / ref->src_height;
    } else {
        *width = ref->num_cols ? ((double)start_column + ref->num_cols) * cw - *left : ref->src_width;
        *height = *width * ref->src_height / ref->src_width;
    }
    return *width > 0 && *height > 0;
}

// Takes a position in the cell area to the coordinate system of a group of
// the image, through the placement it is shown in
static bool
ink_point(const GraphicsManager *self, const Image *img, const ImageRef *ref, uint32_t group, const InkView *view, double *x, double *y) {
    double left, top, width, height;
    if (!ref_rect_in_pixels(self, ref, view, &left, &top, &width, &height)) return false;
    *x = ref->src_x + (*x - left) * ref->src_width / width;
    *y = ref->src_y + (*y - top) * ref->src_height / height;
    return vector_doc_point_to_group(img->vector, group, x, y);
}

static bool
ink_apply(GraphicsManager *self, Image *img, const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    const int sz = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    char *records = sz > 0 ? malloc((size_t)sz + 1) : NULL;
    if (!records) {
        va_end(args);
        log_error("Cannot format ink record: allocation or formatting failed");
        return false;
    }
    const int written = vsnprintf(records, (size_t)sz + 1, format, args);
    va_end(args);
    if (written != sz) {
        free(records);
        log_error("Cannot format ink record");
        return false;
    }
    const size_t before = vector_doc_size(img->vector);
    const char *err = vector_doc_apply(img->vector, (const uint8_t *)records, (size_t)sz);
    free(records);
    img->used_storage = vector_doc_size(img->vector);
    self->used_storage = self->used_storage - MIN(self->used_storage, before) + img->used_storage;
    img->atime = monotonic();
    set_layers_dirty(self);
    if (err) log_error("Cannot apply ink record: %s", err);
    return err == NULL;
}

bool
grman_ink_start(GraphicsManager *self, const InkView *view, uint32_t image, uint32_t group, uint32_t element, const char *fill, InkPoint p, InkStrokeTarget *target) {
    Image *img = img_by_client_id(self, image);
    if (!img || !img->vector) return false;
    // the placement on top of the others under the pen
    const ImageRef *under = NULL;
    iter_refs(img) {
        const ImageRef *ref = i.data->val;
        double left, top, width, height;
        if (!ref_rect_in_pixels(self, ref, view, &left, &top, &width, &height)) continue;
        if (p.x < left || p.y < top || p.x >= left + width || p.y >= top + height) continue;
        if (!under || ref->z_index > under->z_index || (ref->z_index == under->z_index && ref->internal_id > under->internal_id)) under = ref;
    }
    if (!under || !ink_point(self, img, under, group, view, &p.x, &p.y)) return false;
    if (!ink_apply(self, img, "k id=%u g=%u pts=%.17g,%.17g,%.17g fill=%s\n", element, group, p.x, p.y, p.width, fill)) return false;
    *target = (InkStrokeTarget){.image = img->internal_id, .placement = under->internal_id};
    return true;
}

bool
grman_ink_append(GraphicsManager *self, const InkView *view, InkStrokeTarget target, uint32_t group, uint32_t element, InkPoint p) {
    Image *img = img_by_internal_id(self, target.image);
    const ImageRef *ref = img && img->vector ? ref_by_internal_id(img, target.placement) : NULL;
    if (!ref) return false;  // the image or the placement the stroke began in has gone
    // a position that cannot be taken into the group is left out, the stroke goes on with the next
    if (!ink_point(self, img, ref, group, view, &p.x, &p.y)) return true;
    return ink_apply(self, img, "a id=%u pts=%.17g,%.17g,%.17g\n", element, p.x, p.y, p.width);
}

void
grman_ink_remove(GraphicsManager *self, InkStrokeTarget target, uint32_t element) {
    Image *img = img_by_internal_id(self, target.image);
    if (!img || !img->vector) return;
    ink_apply(self, img, "x id=%u\n", element);
}

// }}}

static void
update_current_frame(GraphicsManager *self, Image *img, const CoalescedFrameData *data) {
    bool needs_load = data == NULL;
    CoalescedFrameData cfd;
    if (needs_load) {
        Frame *f = current_frame(img);
        if (f == NULL) return;
        cfd = get_coalesced_frame_data(self, img, f);
        if (!cfd.buf) {
            if (PyErr_Occurred()) PyErr_Print();
            return;
        }
        data = &cfd;
    }
    upload_to_gpu(self, img, data);
    img->displayed_frame_id = current_frame(img)->id;
    if (needs_load) free(data->buf);
    img->current_frame_shown_at = monotonic();
}

static bool
reference_chain_too_large(Image *img, const Frame *frame) {
    uint32_t limit = img->width * img->height * 2;
    uint32_t drawn_area = frame->width * frame->height;
    unsigned num = 1;
    while (drawn_area < limit && num < 5) {
        if (!frame->base_frame_id || !(frame = frame_for_id(img, frame->base_frame_id))) break;
        drawn_area += frame->width * frame->height;
        num++;
    }
    return num >= 5 || drawn_area >= limit;
}

static bool
frame_chain_is_transient(Image *img, const Frame *frame) {
    // matches the recursion depth limit in get_coalesced_frame_data_impl
    unsigned num = 0;
    while (frame) {
        if (frame->transient) return true;
        if (!frame->base_frame_id || ++num > 32 || !(frame = frame_for_id(img, frame->base_frame_id))) break;
    }
    return false;
}

static Image *
handle_animation_frame_load_command(GraphicsManager *self, GraphicsCommand *g, Image *img, const uint8_t *payload, bool *is_dirty) {
    if (img->vector) ABRT("EINVAL", "Animation frames are not supported for vector images");
    uint32_t fmt = g->format ? g->format : RGBA;
    unsigned char tt = g->transmission_type ? g->transmission_type : 'd';
    const bool is_chunked_continuation = tt == 'd' && self->currently_loading.loading_for.image_id == img->internal_id;
    if (is_chunked_continuation) {
        INIT_CHUNKED_LOAD; // g now points to the command that started the chunked load
    }
    if (img->palette || is_palette_format(fmt)) ABRT("EINVAL", "Animation is not supported for palette images");
    if (img->indexed_px_sz || fmt == INDEXED || fmt == INDEXED_ALPHA) ABRT("EINVAL", "Animation is not supported for indexed color images");
    // Frames are blended on the CPU in eight bit sRGB or in half float, see docs/silkie/precision-image-protocol.rst
    if (img->root_frame.deep || img->transfer != TRANSFER_SRGB || deep_storage_of_format(fmt) || (fmt != RGBA16F && g->transfer != 0 && g->transfer != 13))
        ABRT("EINVAL", "Animation is not supported for high precision images or images with a transfer function other than sRGB");
    uint32_t frame_number = g->frame_number;
    if (!frame_number || frame_number > img->extra_framecnt + 2) frame_number = img->extra_framecnt + 2;
    bool is_new_frame = frame_number == img->extra_framecnt + 2;
    g->frame_number = frame_number;
    if (!is_chunked_continuation) {
        self->currently_loading.loading_for = (const ImageAndFrame){0};
        if (g->data_width > MAX_IMAGE_DIMENSION || g->data_height > MAX_IMAGE_DIMENSION)
            ABRT("EINVAL", "Image too large, width or height greater than %u", MAX_IMAGE_DIMENSION);
        if (img->root_frame.is_float != (fmt == RGBA16F)) ABRT("EINVAL", "Animation frame format must match the image format");
        if (img->surface) ABRT("EINVAL", "Animation frames are not supported for GPU surface images");
        if (!initialize_load_data(self, g, img, tt, fmt, frame_number - 1)) return NULL;
    }
    LoadData *load_data = &self->currently_loading;
    img = load_image_data(self, img, g, tt, fmt, payload);
    if (!img || !load_data->loading_completed_successfully) return NULL;
    self->currently_loading.loading_for = (const ImageAndFrame){0};
    img = process_image_data(self, img, g, fmt, true);
    if (!img || !load_data->loading_completed_successfully) return img;

    const unsigned long bytes_per_pixel = pixel_size(load_data->is_opaque, load_data->is_float);
    if (load_data->data_sz < bytes_per_pixel * load_data->width * load_data->height)
        ABRT("ENODATA", "Insufficient image data %zu < %zu", load_data->data_sz, bytes_per_pixel * g->data_width * g->data_height);
    // The frames of an image are all in its gamut, as they are blended with each other
    if (load_data->gamut != img->gamut) {
        if (!convert_pixels_between_gamuts(load_data->data, (size_t)load_data->width * load_data->height, !load_data->is_opaque, load_data->gamut, img->gamut))
            ABRT("EINVAL", "Failed to convert the frame from gamut %u to the gamut of the image: %u", load_data->gamut, img->gamut);
        load_data->gamut = img->gamut;
    }
    if (load_data->width > img->width) ABRT("EINVAL", "Frame width %u larger than image width: %u", load_data->width, img->width);
    if (load_data->is_float) img->peak = MAX(img->peak, peak_of_half_data(load_data->data, load_data->data_sz));
    if (load_data->height > img->height) ABRT("EINVAL", "Frame height %u larger than image height: %u", load_data->height, img->height);
    if (is_new_frame && cache_size(self) + load_data->data_sz > self->storage_limit * 5) {
        remove_images(self, trim_predicate, img->internal_id);
        if (cache_size(self) + load_data->data_sz > self->storage_limit * 5) ABRT("ENOSPC", "Cache size exceeded cannot add new frames");
    }

    Frame transmitted_frame = {
        .width = load_data->width,
        .height = load_data->height,
        .x = g->x_offset,
        .y = g->y_offset,
        .is_4byte_aligned = load_data->is_4byte_aligned,
        .is_opaque = load_data->is_opaque,
        .is_float = load_data->is_float,
        .alpha_blend = g->blend_mode != 1 && !load_data->is_opaque,
        .gap = g->gap > 0     ? g->gap
               : (g->gap < 0) ? 0
                              : DEFAULT_GAP,
        .bgcolor = g->bgcolor,
        .transient = (g->usage_hints & GRAPHICS_USAGE_HINT_TRANSIENT) != 0,
    };
    Frame *frame;
    if (is_new_frame) {
        transmitted_frame.id = ++img->frame_id_counter;
        Frame *frames = realloc(img->extra_frames, sizeof(img->extra_frames[0]) * (img->extra_framecnt + 1));
        if (!frames) ABRT("ENOMEM", "Out of memory");
        img->extra_frames = frames;
        img->extra_framecnt++;
        frame = img->extra_frames + frame_number - 2;
        const ImageAndFrame key = {.image_id = img->internal_id, .frame_id = transmitted_frame.id};
        if (g->other_frame_number) {
            Frame *other_frame = frame_for_number(img, g->other_frame_number);
            if (!other_frame) {
                img->extra_framecnt--;
                ABRT("EINVAL", "No frame with number: %u found", g->other_frame_number);
            }
            if (other_frame->base_frame_id && reference_chain_too_large(img, other_frame)) {
                // since there is a long reference chain to render this frame, make
                // it a fully coalesced key frame, for performance
                CoalescedFrameData cfd = get_coalesced_frame_data(self, img, other_frame);
                if (!cfd.buf) ABRT("EINVAL", "Failed to get data from frame referenced by frame: %u", frame_number);
                ComposeData d = {
                    .over_px_sz = pixel_size(transmitted_frame.is_opaque, transmitted_frame.is_float),
                    .under_px_sz = pixel_size(cfd.is_opaque, cfd.is_float),
                    .over_width = transmitted_frame.width,
                    .over_height = transmitted_frame.height,
                    .over_offset_x = transmitted_frame.x,
                    .over_offset_y = transmitted_frame.y,
                    .under_width = img->width,
                    .under_height = img->height,
                    .needs_blending = transmitted_frame.alpha_blend && !transmitted_frame.is_opaque};
                compose(d, cfd.buf, (size_t)img->width * img->height * d.under_px_sz, load_data->data, load_data->data_sz);
                free_load_data(load_data);
                // Transfer ownership of the coalesced frame to the load data,
                // keeping data aliased to buf so it is freed/moved as usual
                load_data->data_sz = (size_t)img->width * img->height * d.under_px_sz;
                load_data->buf = cfd.buf;
                cfd.buf = NULL;
                load_data->buf_capacity = load_data->data_sz;
                load_data->buf_used = load_data->data_sz;
                load_data->data = load_data->buf;
                transmitted_frame.width = img->width;
                transmitted_frame.height = img->height;
                transmitted_frame.x = 0;
                transmitted_frame.y = 0;
                transmitted_frame.is_4byte_aligned = cfd.is_4byte_aligned;
                transmitted_frame.is_opaque = cfd.is_opaque;
                transmitted_frame.is_float = cfd.is_float;
                transmitted_frame.transient = transmitted_frame.transient || cfd.transient;
            } else {
                transmitted_frame.base_frame_id = other_frame->id;
                transmitted_frame.transient = transmitted_frame.transient || frame_chain_is_transient(img, other_frame);
            }
        }
        *frame = transmitted_frame;
        if (!add_load_data_to_cache(self, key, frame->transient)) {
            img->extra_framecnt--;
            if (PyErr_Occurred()) PyErr_Print();
            ABRT("ENOSPC", "Failed to cache data for image frame");
        }
        img->animation_duration += frame->gap;
        if (img->animation_state == ANIMATION_LOADING) {
            self->has_images_needing_animation = true;
            global_state.check_for_active_animated_images = true;
        }
    } else {
        frame = frame_for_number(img, frame_number);
        if (!frame) ABRT("EINVAL", "No frame with number: %u found", frame_number);
        if (g->gap != 0) change_gap(img, frame, transmitted_frame.gap);
        CoalescedFrameData cfd = get_coalesced_frame_data(self, img, frame);
        if (!cfd.buf) ABRT("EINVAL", "No data associated with frame number: %u", frame_number);
        frame->transient = cfd.transient || transmitted_frame.transient;
        frame->alpha_blend = false;
        frame->base_frame_id = 0;
        frame->bgcolor = 0;
        frame->is_opaque = cfd.is_opaque;
        frame->is_float = cfd.is_float;
        frame->is_4byte_aligned = cfd.is_4byte_aligned;
        frame->x = 0;
        frame->y = 0;
        frame->width = img->width;
        frame->height = img->height;
        const unsigned bytes_per_pixel = pixel_size(frame->is_opaque, frame->is_float);
        ComposeData d = {
            .over_px_sz = pixel_size(transmitted_frame.is_opaque, transmitted_frame.is_float),
            .under_px_sz = bytes_per_pixel,
            .over_width = transmitted_frame.width,
            .over_height = transmitted_frame.height,
            .over_offset_x = transmitted_frame.x,
            .over_offset_y = transmitted_frame.y,
            .under_width = frame->width,
            .under_height = frame->height,
            .needs_blending = transmitted_frame.alpha_blend && !transmitted_frame.is_opaque};
        compose(d, cfd.buf, (size_t)frame->width * frame->height * d.under_px_sz, load_data->data, load_data->data_sz);
        const ImageAndFrame key = {.image_id = img->internal_id, .frame_id = frame->id};
        // Upload before caching, the cache takes ownership of the data
        if (frame == current_frame(img)) {
            update_current_frame(self, img, &cfd);
            *is_dirty = true;
        }
        void *frame_data = cfd.buf;
        cfd.buf = NULL;
        bool added = add_to_cache(self, key, &frame_data, (size_t)bytes_per_pixel * frame->width * frame->height, frame->transient);
        free(frame_data); // NULL if ownership was transferred to the cache
        if (!added) {
            if (PyErr_Occurred()) PyErr_Print();
            ABRT("ENOSPC", "Failed to cache data for image frame");
        }
    }
    return img;
}

// Applies the records of an a=e command to an existing vector image
static Image *
handle_vector_edit_command(GraphicsManager *self, GraphicsCommand *g, Image *img, const uint8_t *payload, bool *is_dirty) {
    unsigned char tt = g->transmission_type ? g->transmission_type : 'd';
    const bool is_chunked_continuation = tt == 'd' && self->currently_loading.loading_for.image_id == img->internal_id;
    if (is_chunked_continuation) {
        self->currently_loading.start_command.more = g->more;
        self->currently_loading.start_command.payload_sz = g->payload_sz;
        g = &self->currently_loading.start_command;
        tt = g->transmission_type ? g->transmission_type : 'd';
    }
    if (!img->vector) ABRT("EINVAL", "Not a vector image");
    if (!is_chunked_continuation) {
        self->currently_loading.loading_for = (const ImageAndFrame){0};
        if (!initialize_load_data(self, g, img, tt, VECTOR, 0)) return NULL;
    }
    LoadData *load_data = &self->currently_loading;
    img = load_image_data(self, img, g, tt, VECTOR, payload);
    if (!img || !load_data->loading_completed_successfully) return NULL;
    self->currently_loading.loading_for = (const ImageAndFrame){0};
    img = process_image_data(self, img, g, VECTOR, false);
    if (!img) return NULL;
    const size_t before = vector_doc_size(img->vector);
    const char *err = vector_doc_apply(img->vector, load_data->data, load_data->data_sz);
    // the records before one that could not be applied stay applied, so the image changed either way
    img->used_storage = vector_doc_size(img->vector);
    self->used_storage = self->used_storage - MIN(self->used_storage, before) + img->used_storage;
    img->atime = monotonic();
    *is_dirty = true;
    set_layers_dirty(self);
    if (err) ABRT("EINVAL", "%s", err);
    return img;
}

// Replaces slots of the palette of a palette image, without sending its
// indices again, see docs/silkie/palette-image-protocol.rst. The slots are
// loaded as an RGBA image with one pixel for each, so that every transmission
// medium, chunking and compression work as they do for f=32.
static Image *
handle_palette_edit_command(GraphicsManager *self, GraphicsCommand *g, Image *img, const uint8_t *payload, bool *is_dirty) {
    unsigned char tt = g->transmission_type ? g->transmission_type : 'd';
    const bool is_chunked_continuation = tt == 'd' && self->currently_loading.loading_for.image_id == img->internal_id;
    GraphicsCommand slots_command;
    if (is_chunked_continuation) {
        self->currently_loading.start_command.more = g->more;
        self->currently_loading.start_command.payload_sz = g->payload_sz;
        g = &self->currently_loading.start_command;
        tt = g->transmission_type ? g->transmission_type : 'd';
    } else {
        const uint32_t slots = img->palette->slots, first = g->x_offset;
        if (first >= slots) ABRT("EINVAL", "The palette has %u slots, there is no slot: %u", slots, first);
        const uint32_t count = g->width ? g->width : slots - first;
        if (count > slots - first) ABRT("EINVAL", "The palette has %u slots, %u slots from slot %u do not fit", slots, count, first);
        // the colors of the palette are all in the gamut of the image
        if (g->gamut && gamut_of_command(g) != img->gamut) ABRT("EINVAL", "The palette is in gamut %u, an update cannot change it", img->gamut);
        slots_command = *g;
        slots_command.format = RGBA;
        slots_command.data_width = count;
        slots_command.data_height = 1;
        g = &slots_command;
        self->currently_loading.loading_for = (const ImageAndFrame){0};
        if (!initialize_load_data(self, g, img, tt, RGBA, 0)) return NULL;
    }
    LoadData *ld = &self->currently_loading;
    img = load_image_data(self, img, g, tt, RGBA, payload);
    if (!img || !ld->loading_completed_successfully) return NULL;
    ld->loading_for = (const ImageAndFrame){0};
    // Direct, uncompressed data has a known size, so more than that does not fit
    if (tt == 'd' && !g->compressed && ld->buf_used > ld->data_sz) ABRT("EFBIG", "Too much data: %zu > %zu", ld->buf_used, ld->data_sz);
    img = process_image_data(self, img, g, RGBA, false);
    if (!img) return NULL;
    ImagePalette *p = palette_for_change(img);
    if (!p) ABRT("ENOMEM", "Out of memory copying the palette");
    const uint32_t first = g->x_offset, count = ld->width;
    memcpy(p->rgba + (size_t)first * 4, ld->data, (size_t)count * 4);
    p->version++;
    upload_palette_to_gpu(self, img, first, count);
    img->atime = monotonic();
    // every placement is drawn with the new colors, as are vector images that draw this one
    *is_dirty = true;
    set_layers_dirty(self);
    invalidate_vector_users(self, img->client_id);
    return img;
}

#undef ABRT

static bool
promote_dependents_to_keyframes(GraphicsManager *self, Image *img, const uint32_t base_id) {
    for (unsigned i = 0; i < img->extra_framecnt; i++) {
        Frame *f = img->extra_frames + i;
        if (f->base_frame_id != base_id) continue;
        CoalescedFrameData cfd = get_coalesced_frame_data(self, img, f);
        if (!cfd.buf) return false;
        const ImageAndFrame key = {.image_id = img->internal_id, .frame_id = f->id};
        void *frame_data = cfd.buf;
        const bool transient = f->transient || cfd.transient;
        bool added = add_to_cache(self, key, &frame_data, (size_t)(cfd.is_opaque ? 3 : 4) * img->width * img->height, transient);
        free(frame_data); // NULL if ownership was transferred to the cache
        if (!added) return false;
        f->transient = transient;
        f->alpha_blend = false;
        f->base_frame_id = 0;
        f->bgcolor = 0;
        f->is_opaque = cfd.is_opaque;
        f->is_4byte_aligned = cfd.is_4byte_aligned;
        f->x = 0;
        f->y = 0;
        f->width = img->width;
        f->height = img->height;
    }
    return true;
}

static bool
dependents_fit_in_cache(GraphicsManager *self, Image *img, const uint32_t base_id, const size_t victim_sz) {
    const size_t full = (size_t)4 * img->width * img->height;
    size_t growth = 0;
    for (unsigned i = 0; i < img->extra_framecnt; i++) {
        const Frame *f = img->extra_frames + i;
        if (f->base_frame_id != base_id) continue;
        const size_t cur = (size_t)(f->is_opaque ? 3 : 4) * f->width * f->height;
        if (full > cur) growth += full - cur;
    }
    if (growth <= victim_sz) return true;
    growth -= victim_sz;
    const size_t limit = self->storage_limit * 5;
    if (cache_size(self) + growth <= limit) return true;
    remove_images(self, trim_predicate, img->internal_id);
    return cache_size(self) + growth <= limit;
}

static Image *
handle_delete_frame_command(GraphicsManager *self, const GraphicsCommand *g, bool *is_dirty) {
    if (!g->id && !g->image_number) {
        REPORT_ERROR("Delete frame data command without image id or number");
        return NULL;
    }
    Image *img = g->id ? img_by_client_id(self, g->id) : img_by_client_number(self, g->image_number);
    if (!img) {
        REPORT_ERROR("Animation command refers to non-existent image with id: %u and number: %u", g->id, g->image_number);
        return NULL;
    }
    uint32_t frame_number = MIN(img->extra_framecnt + 1, g->frame_number);
    if (!frame_number) frame_number = 1;
    if (!img->extra_framecnt) return g->delete_action == 'F' ? img : NULL;
    *is_dirty = true;
    ImageAndFrame key = {.image_id = img->internal_id};
    bool remove_root = frame_number == 1;
    uint32_t removed_gap = 0;
    const uint32_t victim_id = remove_root ? img->root_frame.id : img->extra_frames[frame_number - 2].id;
    const Frame *victim = remove_root ? &img->root_frame : img->extra_frames + frame_number - 2;
    const size_t victim_sz = (size_t)(victim->is_opaque ? 3 : 4) * victim->width * victim->height;
    if (!dependents_fit_in_cache(self, img, victim_id, victim_sz) || !promote_dependents_to_keyframes(self, img, victim_id)) {
        if (PyErr_Occurred()) PyErr_Print();
        REPORT_ERROR("Failed to preserve frames that depend on deleted frame: %u", frame_number);
        return NULL;
    }
    if (remove_root) {
        key.frame_id = img->root_frame.id;
        remove_from_cache(self, key);
        if (PyErr_Occurred()) PyErr_Print();
        removed_gap = img->root_frame.gap;
        img->root_frame = img->extra_frames[0];
    }
    unsigned removed_idx = remove_root ? 0 : frame_number - 2;
    if (!remove_root) {
        key.frame_id = img->extra_frames[removed_idx].id;
        removed_gap = img->extra_frames[removed_idx].gap;
        remove_from_cache(self, key);
    }
    img->animation_duration = removed_gap < img->animation_duration ? img->animation_duration - removed_gap : 0;
    if (PyErr_Occurred()) PyErr_Print();
    if (removed_idx < img->extra_framecnt - 1)
        memmove(img->extra_frames + removed_idx, img->extra_frames + removed_idx + 1, sizeof(img->extra_frames[0]) * (img->extra_framecnt - 1 - removed_idx));
    img->extra_framecnt--;
    if (img->current_frame_index > img->extra_framecnt) {
        img->current_frame_index = img->extra_framecnt;
        update_current_frame(self, img, NULL);
        return NULL;
    }
    const unsigned removed_index = remove_root ? 0 : removed_idx + 1;
    if (img->current_frame_index > removed_index) img->current_frame_index--;
    else if (img->current_frame_index == removed_index) update_current_frame(self, img, NULL);
    return NULL;
}

static void
handle_animation_control_command(GraphicsManager *self, bool *is_dirty, const GraphicsCommand *g, Image *img) {
    if (g->frame_number) {
        uint32_t frame_idx = g->frame_number - 1;
        if (frame_idx <= img->extra_framecnt) {
            Frame *f = frame_idx ? img->extra_frames + frame_idx - 1 : &img->root_frame;
            if (g->gap) change_gap(img, f, g->gap);
        }
    }
    if (g->other_frame_number) {
        uint32_t frame_idx = g->other_frame_number - 1;
        if (frame_idx != img->current_frame_index && frame_idx <= img->extra_framecnt) {
            img->current_frame_index = frame_idx;
            *is_dirty = true;
            update_current_frame(self, img, NULL);
        }
    }
    if (g->animation_state) {
        AnimationState old_state = img->animation_state;
        switch (g->animation_state) {
            case 1: img->animation_state = ANIMATION_STOPPED; break;
            case 2: img->animation_state = ANIMATION_LOADING; break;
            case 3: img->animation_state = ANIMATION_RUNNING; break;
            default: break;
        }
        if (img->animation_state == ANIMATION_STOPPED) {
            img->current_loop = 0;
        } else {
            if (old_state == ANIMATION_STOPPED) {
                img->current_frame_shown_at = monotonic();
                img->is_drawn = true;
            }
            self->has_images_needing_animation = true;
            global_state.check_for_active_animated_images = true;
        }
        img->current_loop = 0;
    }
    if (g->loop_count) {
        img->max_loops = g->loop_count - 1;
        global_state.check_for_active_animated_images = true;
    }
}

static bool
image_is_animatable(const Image *img) {
    return img->animation_state != ANIMATION_STOPPED && img->extra_framecnt && img->is_drawn && img->animation_duration &&
           (!img->max_loops || img->current_loop < img->max_loops);
}

bool
scan_active_animations(GraphicsManager *self, const monotonic_t now, monotonic_t *minimum_gap, bool os_window_context_set) {
    bool dirtied = false;
    *minimum_gap = MONOTONIC_T_MAX;
    if (!self->has_images_needing_animation) return dirtied;
    self->has_images_needing_animation = false;
    self->context_made_current_for_this_command = os_window_context_set;
    iter_images(self) {
        Image *img = i.data->val;
        if (image_is_animatable(img)) {
            Frame *f = current_frame(img);
            if (f) {
                self->has_images_needing_animation = true;
                monotonic_t next_frame_at = img->current_frame_shown_at + ms_to_monotonic_t(f->gap);
                if (now >= next_frame_at) {
                    do {
                        uint32_t next = (img->current_frame_index + 1) % (img->extra_framecnt + 1);
                        if (!next) {
                            if (img->animation_state == ANIMATION_LOADING) goto skip_image;
                            if (++img->current_loop >= img->max_loops && img->max_loops) goto skip_image;
                        }
                        img->current_frame_index = next;
                    } while (!current_frame(img)->gap);
                    dirtied = true;
                    update_current_frame(self, img, NULL);
                    f = current_frame(img);
                    next_frame_at = img->current_frame_shown_at + ms_to_monotonic_t(f->gap);
                }
                if (next_frame_at > now && next_frame_at - now < *minimum_gap) *minimum_gap = next_frame_at - now;
            }
        }
    skip_image:;
    }
    return dirtied;
}
// }}}

// {{{ composition a=c
static void
cfd_free(CoalescedFrameData *p) {
    free((p)->buf);
    p->buf = NULL;
}

// Whether an image with the client id exists; *image and *version change whenever its pixels do: the current frame
// in the low 32 bits of version and the number of changes to the palette of a palette image in the high 32 bits.
bool
grman_image_version(GraphicsManager *self, uint32_t client_id, id_type *image, uint64_t *version) {
    Image *img = client_id ? img_by_client_id(self, client_id) : NULL;
    if (!img) return false;
    Frame *f = current_frame(img);
    *image = img->internal_id;
    *version = ((uint64_t)(img->palette ? img->palette->version : 0) << 32) | (f ? f->id : 0);
    return true;
}

// The current frame of the image with the client id as RGBA, or NULL. Used for the mouse pointer.
uint8_t *
grman_image_rgba(GraphicsManager *self, uint32_t client_id, uint32_t *width, uint32_t *height) {
    Image *img = client_id ? img_by_client_id(self, client_id) : NULL;
    // The pixels of an indexed color image are indices into the palette of a window, not colors
    Frame *f = img && img->root_frame_data_loaded && (!img->indexed_px_sz || img->palette) ? current_frame(img) : NULL;
    if (!f) return NULL;
    if (img->palette) {
        uint8_t *ans = palette_image_rgba(self, img);
        if (ans) {
            *width = img->width;
            *height = img->height;
        }
        return ans;
    }
    RAII_CoalescedFrameData(cfd, get_coalesced_frame_data(self, img, f));
    if (!cfd.buf) {
        if (PyErr_Occurred()) PyErr_Print();
        return NULL;
    }
    *width = img->width;
    *height = img->height;
    uint8_t *ans = cfd.buf;
    const bool decode = needs_decoding(img, &cfd);
    if (decode || cfd.is_float) {
        // A pointer image is eight bit sRGB: what is brighter than SDR white is shown at SDR white. Half float pixels
        // are eight bytes each, not RGBA bytes, and linear, so they are encoded as the decoded pixels are.
        if ((size_t)img->width * img->height > SIZE_MAX / 4) return NULL;
        Srgb8Rows rows = {.out = malloc((size_t)img->width * img->height * 4), .width = img->width};
        if (!rows.out) return NULL;
        if (!(decode ? decode_frame_rows : half_frame_rows)(img, &cfd, emit_srgb8_row, &rows)) {
            free(rows.out);
            return NULL;
        }
        return rows.out;
    }
    if (cfd.is_opaque) {
        const size_t num = (size_t)img->width * img->height;
        if (!(ans = malloc(num * 4))) return NULL;
        for (size_t i = 0; i < num; i++) {
            memcpy(ans + 4 * i, cfd.buf + 3 * i, 3);
            ans[4 * i + 3] = 255;
        }
    } else cfd.buf = NULL;
    return ans;
}

static void
handle_compose_command(GraphicsManager *self, bool *is_dirty, const GraphicsCommand *g, Image *img) {
    if (img->vector) {
        set_command_failed_response("EINVAL", "Composition is not supported for vector images");
        return;
    }
    if (img->indexed_px_sz) {
        if (img->palette) set_command_failed_response("EINVAL", "Composition is not supported for palette images");
        else set_command_failed_response("EINVAL", "Composition is not supported for indexed color images");
        return;
    }
    if (img->root_frame.deep || img->transfer != TRANSFER_SRGB) {
        set_command_failed_response("EINVAL", "Composition is not supported for high precision images or images with a transfer function other than sRGB");
        return;
    }
    Frame *src_frame = frame_for_number(img, g->frame_number);
    if (!src_frame) {
        set_command_failed_response("ENOENT", "No source frame number %u exists in image id: %u\n", g->frame_number, img->client_id);
        return;
    }
    Frame *dest_frame = frame_for_number(img, g->other_frame_number);
    if (!dest_frame) {
        set_command_failed_response("ENOENT", "No destination frame number %u exists in image id: %u\n", g->other_frame_number, img->client_id);
        return;
    }
    if (img->surface) {
        set_command_failed_response("EINVAL", "Composition is not supported for GPU surface images");
        return;
    }
    // Use uint64_t to avoid overflow when testing for validity. All dimensions are 32bit numbers.
    const uint64_t width = g->width ? g->width : img->width;
    const uint64_t height = g->height ? g->height : img->height;
    const uint64_t dest_x = g->x_offset, dest_y = g->y_offset, src_x = g->cell_x_offset, src_y = g->cell_y_offset;
    if (dest_x + width > img->width || dest_y + height > img->height) {
        set_command_failed_response("EINVAL", "The destination rectangle is out of bounds");
        return;
    }
    if (src_x + width > img->width || src_y + height > img->height) {
        set_command_failed_response("EINVAL", "The source rectangle is out of bounds");
        return;
    }
    if (src_frame == dest_frame) {
        bool x_overlaps = MAX(src_x, dest_x) < (MIN(src_x, dest_x) + width);
        bool y_overlaps = MAX(src_y, dest_y) < (MIN(src_y, dest_y) + height);
        if (x_overlaps && y_overlaps) {
            set_command_failed_response("EINVAL", "The source and destination rectangles overlap and the src and destination frames are the same");
            return;
        }
    }

    RAII_CoalescedFrameData(src_data, get_coalesced_frame_data(self, img, src_frame));
    if (!src_data.buf) {
        set_command_failed_response("EINVAL", "Failed to get data for src frame: %u", g->frame_number - 1);
        return;
    }
    RAII_CoalescedFrameData(dest_data, get_coalesced_frame_data(self, img, dest_frame));
    if (!dest_data.buf) {
        set_command_failed_response("EINVAL", "Failed to get data for destination frame: %u", g->other_frame_number - 1);
        return;
    }
    ComposeData d = {
        .over_px_sz = pixel_size(src_data.is_opaque, src_data.is_float),
        .under_px_sz = pixel_size(dest_data.is_opaque, dest_data.is_float),
        .needs_blending = !g->compose_mode && !src_data.is_opaque,
        .over_offset_x = src_x,
        .over_offset_y = src_y,
        .under_offset_x = dest_x,
        .under_offset_y = dest_y,
        .over_width = width,
        .over_height = height,
        .under_width = width,
        .under_height = height,
        .stride = img->width};
    compose_rectangles(d, dest_data.buf, (size_t)img->width * img->height * d.under_px_sz, src_data.buf, (size_t)img->width * img->height * d.over_px_sz);
    bool transient = src_data.transient || dest_data.transient;
    const ImageAndFrame key = {.image_id = img->internal_id, .frame_id = dest_frame->id};
    // Upload before caching, the cache takes ownership of the data
    const bool is_current_frame = (g->other_frame_number - 1) == img->current_frame_index;
    *is_dirty = is_current_frame;
    if (is_current_frame) update_current_frame(self, img, &dest_data);
    const size_t dest_sz = ((size_t)pixel_size(dest_data.is_opaque, dest_data.is_float)) * img->width * img->height;
    void *dest_buf = dest_data.buf;
    dest_data.buf = NULL;
    if (!add_to_cache(self, key, &dest_buf, dest_sz, transient)) {
        free(dest_buf);
        if (PyErr_Occurred()) PyErr_Print();
        set_command_failed_response("ENOSPC", "Failed to store image data in cache");
    } else {
        // The frame is now a fully coalesced frame. Commit its descriptor only
        // after the matching data has been accepted by the cache.
        dest_frame->x = 0;
        dest_frame->y = 0;
        dest_frame->width = img->width;
        dest_frame->height = img->height;
        dest_frame->base_frame_id = 0;
        dest_frame->bgcolor = 0;
        dest_frame->is_opaque = dest_data.is_opaque;
        dest_frame->is_4byte_aligned = dest_data.is_4byte_aligned;
        dest_frame->alpha_blend = false;
        dest_frame->transient = transient;
    }
}
// }}}

// Image lifetime/scrolling {{{

static ref_map_itr
remove_ref_itr(Image *img, ref_map_itr x) {
    if (x.data->val->client_id) vt_erase(&img->refs_by_client_id, x.data->val->client_id);
    free(x.data->val);
    return vt_erase_itr(&img->refs_by_internal_id, x);
}


static void
remove_ref(Image *img, ImageRef *ref) {
    ref_map_itr i = vt_get(&img->refs_by_internal_id, ref->internal_id);
    if (vt_is_end(i)) return;
    remove_ref_itr(img, i);
}

static void
filter_refs(
    GraphicsManager *self,
    const void *data,
    bool free_images,
    bool (*filter_func)(const ImageRef *, Image *, const void *, CellPixelSize),
    CellPixelSize cell,
    bool only_first_image,
    bool free_only_matched) {
    for (image_map_itr ii = vt_first(&self->images_by_internal_id); !vt_is_end(ii);) {
        Image *img = ii.data->val;
        bool matched = false;
        for (ref_map_itr ri = vt_first(&img->refs_by_internal_id); !vt_is_end(ri);) {
            ImageRef *ref = ri.data->val;
            if (filter_func(ref, img, data, cell)) {
                ri = remove_ref_itr(img, ri);
                set_layers_dirty(self);
                matched = true;
            } else ri = vt_next(ri);
        }
        if ((!free_only_matched || matched) && !vt_size(&img->refs_by_internal_id) && (free_images || img->client_id == 0)) ii = remove_image_itr(self, ii);
        else ii = vt_next(ii);
        if (only_first_image && matched) break;
    }
}


static void
modify_refs(GraphicsManager *self, const void *data, bool (*filter_func)(ImageRef *, Image *, const void *, CellPixelSize), CellPixelSize cell) {
    for (image_map_itr ii = vt_first(&self->images_by_internal_id); !vt_is_end(ii);) {
        Image *img = ii.data->val;
        for (ref_map_itr ri = vt_first(&img->refs_by_internal_id); !vt_is_end(ri);) {
            ImageRef *ref = ri.data->val;
            if (filter_func(ref, img, data, cell)) ri = remove_ref_itr(img, ri);
            else ri = vt_next(ri);
        }
        if (!vt_size(&img->refs_by_internal_id) && img->client_id == 0 && img->client_number == 0) {
            // references have all scrolled off the history buffer and the image has no way to reference it
            // to create new references so remove it.
            ii = remove_image_itr(self, ii);
        } else ii = vt_next(ii);
    }
}


static bool
scroll_filter_func(ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref) return false;
    ScrollData *d = (ScrollData *)data;
    // A reverse scroll that does not pull lines back from the history leaves the history in place
    if (d->amt > 0 && !d->scroll_history && ref->start_row + (int32_t)ref->effective_num_rows <= 0) return false;
    ref->start_row += d->amt;
    // A cell image is tied to the placeholders on its line, which are lost when
    // the line is scrolled off the bottom, so the ref must not come back on a
    // later forward scroll.
    if (is_cell_image(ref) && ref->start_row > (int32_t)d->margin_bottom) return true;
    return ref->start_row + (int32_t)ref->effective_num_rows <= d->limit;
}

static bool
ref_within_region(const ImageRef *ref, index_type margin_top, index_type margin_bottom) {
    return ref->start_row >= (int32_t)margin_top && ref->start_row + (int32_t)ref->effective_num_rows - 1 <= (int32_t)margin_bottom;
}

static bool
ref_outside_region(const ImageRef *ref, index_type margin_top, index_type margin_bottom) {
    return ref->start_row + (int32_t)ref->effective_num_rows <= (int32_t)margin_top || ref->start_row > (int32_t)margin_bottom;
}

static float
src_pixels_per_screen_pixel(const ImageRef *ref, CellPixelSize cell) {
    // The vertical scale factor mapping pixels in the destination rectangle on
    // the screen to pixels in the source rectangle. Must match the dest rect
    // calculation used for rendering in grman_update_layers().
    float dest_height_px;
    if (ref->num_rows) dest_height_px = (float)(ref->num_rows * cell.height) - (float)ref->cell_y_offset;
    else if (ref->num_cols && ref->src_width > 0) {
        float dest_width_px = (float)(ref->num_cols * cell.width) - (float)ref->cell_x_offset;
        dest_height_px = dest_width_px * ref->src_height / ref->src_width;
    } else return 1.f; // rendered at native size
    return dest_height_px > 0 ? ref->src_height / dest_height_px : 1.f;
}

static bool
scroll_filter_margins_func(ImageRef *ref, Image *img, const void *data, CellPixelSize cell) {
    if (ref->is_virtual_ref) return false;
    ScrollData *d = (ScrollData *)data;
    // Lines scrolled out of a region at the top of the screen still enter the history and
    // lines popped from the history move it down, so a cell image whose line is in the
    // history moves with it.
    if (d->scroll_history && is_cell_image(ref) && ref->start_row + (int32_t)ref->effective_num_rows <= 0) {
        ref->start_row += d->amt;
        return ref->start_row + (int32_t)ref->effective_num_rows <= d->limit;
    }
    if (ref_within_region(ref, d->margin_top, d->margin_bottom)) {
        ref->start_row += d->amt;
        if (ref_outside_region(ref, d->margin_top, d->margin_bottom)) return true;
        // Clip the image if scrolling has resulted in part of it being outside the page area
        uint32_t clipped_rows;
        float clip_amt;
        const float scale = src_pixels_per_screen_pixel(ref, cell);
        if (ref->start_row < (int32_t)d->margin_top) {
            // image moved up
            clipped_rows = d->margin_top - ref->start_row;
            clip_amt = scale * (float)(cell.height * clipped_rows - ref->cell_y_offset);
            if (ref->src_height <= clip_amt) return true;
            ref->src_y += clip_amt;
            ref->src_height -= clip_amt;
            ref->effective_num_rows -= clipped_rows;
            if (ref->num_rows) ref->num_rows -= clipped_rows;
            ref->cell_y_offset = 0;
            update_src_rect(ref, img);
            ref->start_row += clipped_rows;
        } else if (ref->start_row + (int32_t)ref->effective_num_rows - 1 > (int32_t)d->margin_bottom) {
            // image moved down
            clipped_rows = ref->start_row + ref->effective_num_rows - 1 - d->margin_bottom;
            // the last row may be only partially covered by the image,
            // so calculate the clip amount from the height that remains visible
            float visible_height_px = (float)(cell.height * (ref->effective_num_rows - clipped_rows)) - (float)ref->cell_y_offset;
            float new_src_height = scale * visible_height_px;
            if (new_src_height <= 0) return true;
            clip_amt = ref->src_height - new_src_height;
            if (clip_amt > 0) ref->src_height -= clip_amt;
            ref->effective_num_rows -= clipped_rows;
            if (ref->num_rows) ref->num_rows -= clipped_rows;
            update_src_rect(ref, img);
        }
        return ref_outside_region(ref, d->margin_top, d->margin_bottom);
    }
    return false;
}

void
grman_scroll_images(GraphicsManager *self, const ScrollData *data, CellPixelSize cell) {
    if (vt_size(&self->images_by_internal_id)) {
        set_layers_dirty(self);
        modify_refs(self, data, data->has_margins ? scroll_filter_margins_func : scroll_filter_func, cell);
    }
}

bool
grman_has_any_images(GraphicsManager *self) {
    return vt_size(&self->images_by_internal_id) > 0;
}

bool
grman_has_hdr_images(GraphicsManager *self) {
    return self->render_data.has_hdr;
}

static bool
cell_image_row_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref || !is_cell_image(ref)) return false;
    int32_t top = *(int32_t *)data;
    int32_t bottom = *((int32_t *)data + 1);
    return ref_within_region(ref, top, bottom);
}

static bool
cell_image_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data UNUSED, CellPixelSize cell UNUSED) {
    return !ref->is_virtual_ref && is_cell_image(ref);
}

// Remove cell images within the given region.
void
grman_remove_cell_images(GraphicsManager *self, int32_t top, int32_t bottom) {
    CellPixelSize dummy = {0};
    int32_t data[] = {top, bottom};
    filter_refs(self, data, false, cell_image_row_filter_func, dummy, false, true);
}

void
grman_remove_all_cell_images(GraphicsManager *self) {
    CellPixelSize dummy = {0};
    filter_refs(self, NULL, false, cell_image_filter_func, dummy, false, true);
}


static bool
clear_filter_func(const ImageRef *ref, Image UNUSED *img, const void UNUSED *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref) return false;
    return ref->start_row + (int32_t)ref->effective_num_rows > 0;
}

static bool
clear_filter_func_noncell(const ImageRef *ref, Image UNUSED *img, const void UNUSED *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref || is_cell_image(ref)) return false;
    return ref->start_row + (int32_t)ref->effective_num_rows > 0;
}

static bool
clear_all_filter_func(const ImageRef *ref UNUSED, Image UNUSED *img, const void UNUSED *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref) return false;
    return true;
}

void
grman_clear(GraphicsManager *self, bool all, CellPixelSize cell) {
    filter_refs(self, NULL, true, all ? clear_all_filter_func : clear_filter_func, cell, false, false);
}

static bool
id_filter_func(const ImageRef *ref, Image *img, const void *data, CellPixelSize cell UNUSED) {
    const GraphicsCommand *g = data;
    if (g->id && img->client_id == g->id) return !g->placement_id || ref->client_id == g->placement_id;
    return false;
}

static bool
id_range_filter_func(const ImageRef *ref UNUSED, Image *img, const void *data, CellPixelSize cell UNUSED) {
    const GraphicsCommand *g = data;
    return img->client_id && g->x_offset <= img->client_id && img->client_id <= g->y_offset;
}


static bool
x_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref || is_cell_image(ref)) return false;
    const GraphicsCommand *g = data;
    return ref->start_column <= (int32_t)g->x_offset - 1 && ((int32_t)g->x_offset - 1) < ((int32_t)(ref->start_column + ref->effective_num_cols));
}

static bool
y_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref || is_cell_image(ref)) return false;
    const GraphicsCommand *g = data;
    return ref->start_row <= (int32_t)g->y_offset - 1 && ((int32_t)g->y_offset - 1) < ((int32_t)(ref->start_row + ref->effective_num_rows));
}

static bool
z_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    if (ref->is_virtual_ref || is_cell_image(ref)) return false;
    const GraphicsCommand *g = data;
    return ref->z_index == g->z_index;
}


static bool
point_filter_func(const ImageRef *ref, Image *img, const void *data, CellPixelSize cell) {
    if (ref->is_virtual_ref || is_cell_image(ref)) return false;
    return x_filter_func(ref, img, data, cell) && y_filter_func(ref, img, data, cell);
}

static bool
point3d_filter_func(const ImageRef *ref, Image *img, const void *data, CellPixelSize cell) {
    if (ref->is_virtual_ref || is_cell_image(ref)) return false;
    return z_filter_func(ref, img, data, cell) && point_filter_func(ref, img, data, cell);
}


static void
handle_delete_command(GraphicsManager *self, const GraphicsCommand *g, Cursor *c, bool *is_dirty, CellPixelSize cell) {
    if (self->currently_loading.loading_for.image_id) free_load_data(&self->currently_loading);
    GraphicsCommand d;
    if (!g->placement_id) {
        // special case freeing of images with no refs by id or number as
        // filter_refs doesnt handle this
        Image *img = NULL;
        switch (g->delete_action) {
            case 'I': img = img_by_client_id(self, g->id); break;
            case 'N': img = img_by_client_number(self, g->image_number); break;
            case 'R': {
                for (image_map_itr ii = vt_first(&self->images_by_internal_id); !vt_is_end(ii);) {
                    img = ii.data->val;
                    if (id_range_filter_func(NULL, img, g, cell) && !vt_size(&img->refs_by_internal_id)) ii = remove_image_itr(self, ii);
                    else ii = vt_next(ii);
                }
            }
                img = NULL;
                break;
        }
        if (img && !vt_size(&img->refs_by_internal_id)) {
            remove_image(self, img);
            goto end;
        }
    }
    switch (g->delete_action) {
#define I(u, data, func)                                                     \
    filter_refs(self, data, g->delete_action == u, func, cell, false, true); \
    *is_dirty = true;                                                        \
    break
#define D(l, u, data, func) \
    case l:                 \
    case u: I(u, data, func)
#define G(l, u, func) D(l, u, g, func)
        case 0:
            D('a', 'A', NULL, clear_filter_func_noncell);
            G('i', 'I', id_filter_func);
            G('r', 'R', id_range_filter_func);
            G('p', 'P', point_filter_func);
            G('q', 'Q', point3d_filter_func);
            G('x', 'X', x_filter_func);
            G('y', 'Y', y_filter_func);
            G('z', 'Z', z_filter_func);
        case 'c':
        case 'C':
            d.x_offset = c->x + 1;
            d.y_offset = c->y + 1;
            I('C', &d, point_filter_func);
        case 'n':
        case 'N': {
            Image *img = img_by_client_number(self, g->image_number);
            if (img) {
                for (ref_map_itr ri = vt_first(&img->refs_by_internal_id); !vt_is_end(ri);) {
                    ImageRef *ref = ri.data->val;
                    if (!g->placement_id || g->placement_id == ref->client_id) {
                        ri = remove_ref_itr(img, ri);
                        set_layers_dirty(self);
                    } else ri = vt_next(ri);
                }
                if (!vt_size(&img->refs_by_internal_id) && (g->delete_action == 'N' || img->client_id == 0)) remove_image(self, img);
            }
        } break;
        case 'f':
        case 'F': {
            Image *img = handle_delete_frame_command(self, g, is_dirty);
            if (img != NULL) {
                remove_image(self, img);
                *is_dirty = true;
            }
            break;
        }
        default: REPORT_ERROR("Unknown graphics command delete action: %c", g->delete_action); break;
#undef G
#undef D
#undef I
    }
end:
    if (!vt_size(&self->images_by_internal_id) && self->render_data.count) self->render_data.count = 0;
}

// }}}

void
grman_resize(
    GraphicsManager *self,
    index_type old_lines UNUSED,
    index_type lines UNUSED,
    index_type old_columns,
    index_type columns,
    index_type num_content_lines_before,
    index_type num_content_lines_after) {
    ImageRef *ref;
    Image *img;
    set_layers_dirty(self);
    if (columns == old_columns && num_content_lines_before > num_content_lines_after) {
        const unsigned int vertical_shrink_size = num_content_lines_before - num_content_lines_after;
        iter_images(self) {
            img = i.data->val;
            iter_refs(img) {
                ref = i.data->val;
                if (ref->is_virtual_ref || is_cell_image(ref)) continue;
                ref->start_row -= vertical_shrink_size;
            }
        }
    }
}

void
grman_rescale(GraphicsManager *self, CellPixelSize cell) {
    ImageRef *ref;
    Image *img;
    set_layers_dirty(self);
    iter_images(self) {
        img = i.data->val;
        iter_refs(img) {
            ref = i.data->val;
            if (ref->is_virtual_ref || is_cell_image(ref)) continue;
            ref->cell_x_offset = MIN(ref->cell_x_offset, cell.width - 1);
            ref->cell_y_offset = MIN(ref->cell_y_offset, cell.height - 1);
            update_dest_rect(ref, ref->num_cols, ref->num_rows, cell);
        }
    }
}

const char *
grman_handle_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, Cursor *c, bool *is_dirty, CellPixelSize cell) {
    const char *ret = NULL;
    command_response[0] = 0;
    Py_CLEAR(self->vector_response);
    self->context_made_current_for_this_command = false;

    if (g->id && g->image_number) {
        set_command_failed_response("EINVAL", "Must not specify both image id and image number");
        return finish_command_response(g, false);
    }

    unsigned char action = g->action;
    if (!action && self->currently_loading.loading_for.image_id && (self->currently_loading.start_command.action == 'f' || self->currently_loading.start_command.action == 'e' || self->currently_loading.start_command.action == 'v')) {
        // A continuation chunk carries no action key, so when the chunked
        // load was started by an a=f or a=e command, route it to that
        // handler rather than the add handler
        action = self->currently_loading.start_command.action;
    }
    switch (action) {
        case 'v': {
            const GraphicsCommand *request = self->currently_loading.loading_for.image_id ? &self->currently_loading.start_command : g;
            if (!request->id || request->format != VECTOR || request->image_number || request->transmission_type == 'g') {
                set_command_failed_response("EINVAL", "Vector service requires a=v,f=200 and a request id");
                ret = finish_command_response(request, false);
                break;
            }
            GraphicsCommand vg = *request;
            Image *image = handle_add_command(self, g, payload, is_dirty, 0, true);
            if (!self->currently_loading.loading_for.image_id) {
                free_load_data(&self->currently_loading);
                if (self->vector_response) {
                    const char *response = PyBytes_AS_STRING(self->vector_response);
                    const char *body = strchr(response, ';');
                    if (!vg.quiet || (vg.quiet == 1 && body && strncmp(body+1, "OK;", 3))) ret = response;
                } else ret = finish_command_response(&vg, image != NULL);
                remove_images(self, add_trim_predicate, 0);
            }
        } break;
        case 0:
        case 't':
        case 'T':
        case 'q': {
            uint32_t iid = g->id, q_iid = iid;
            bool is_query = g->action == 'q';
            if (is_query) {
                iid = 0;
                if (!q_iid) {
                    REPORT_ERROR("Query graphics command without image id");
                    break;
                }
            }
            if (is_query && g->report_supported) {
                // A feature query transmits no image data, so it must not
                // interfere with any chunked load that is in progress
                ret = finish_command_response_with_keys(&(const GraphicsCommand){.id = q_iid, .quiet = g->quiet}, true, supported_keys_for_query(g));
                break;
            }
            Image *image = handle_add_command(self, g, payload, is_dirty, iid, is_query);
            if (!is_query && image && !image->vector && invalidate_vector_users(self, image->client_id)) *is_dirty = true;
            if (!self->currently_loading.loading_for.image_id) free_load_data(&self->currently_loading);
            GraphicsCommand *lg = &self->currently_loading.start_command;
            if (g->quiet) lg->quiet = g->quiet;
            if (is_query) ret = finish_command_response(&(const GraphicsCommand){.id = q_iid, .quiet = g->quiet}, image != NULL);
            else ret = finish_command_response(lg, image != NULL);
            if (lg->action == 'T' && image && image->root_frame_data_loaded) handle_put_command(self, lg, c, is_dirty, image, cell);
            id_type added_image_id = image ? image->internal_id : 0;
            if (g->action == 'q') remove_images(self, add_trim_predicate, 0);
            if (self->used_storage > self->storage_limit) apply_storage_quota(self, self->storage_limit, added_image_id);
            break;
        }
        case 'a':
        case 'f': {
            if (!g->id && !g->image_number && !self->currently_loading.loading_for.image_id) {
                REPORT_ERROR("Add frame data command without image id or number");
                break;
            }
            Image *img;
            if (self->currently_loading.loading_for.image_id) img = img_by_internal_id(self, self->currently_loading.loading_for.image_id);
            else img = g->id ? img_by_client_id(self, g->id) : img_by_client_number(self, g->image_number);
            if (!img) {
                set_command_failed_response("ENOENT", "Animation command refers to non-existent image with id: %u and number: %u", g->id, g->image_number);
                ret = finish_command_response(g, false);
            } else {
                GraphicsCommand ag = *g;
                if (action == 'f') {
                    ag.action = action;
                    if (!ag.id && !ag.image_number) {
                        // continuation chunk, which carries either no action
                        // key or a=f, the response must identify the image
                        // from the start command
                        ag.id = self->currently_loading.start_command.id;
                        ag.image_number = self->currently_loading.start_command.image_number;
                    }
                    img = handle_animation_frame_load_command(self, &ag, img, payload, is_dirty);
                    if (img && invalidate_vector_users(self, img->client_id)) *is_dirty = true;
                    if (!self->currently_loading.loading_for.image_id) free_load_data(&self->currently_loading);
                    if (!ag.frame_number) ag.frame_number = self->currently_loading.start_command.frame_number;
                    if (g->quiet) ag.quiet = g->quiet;
                    else ag.quiet = self->currently_loading.start_command.quiet;
                    ret = finish_command_response(&ag, img != NULL);
                } else if (ag.action == 'a') {
                    handle_animation_control_command(self, is_dirty, &ag, img);
                }
            }
            break;
        }
        case 'e': {
            if (!g->id && !g->image_number && !self->currently_loading.loading_for.image_id) {
                REPORT_ERROR("Edit command without image id or number");
                break;
            }
            Image *img;
            if (self->currently_loading.loading_for.image_id) img = img_by_internal_id(self, self->currently_loading.loading_for.image_id);
            else img = g->id ? img_by_client_id(self, g->id) : img_by_client_number(self, g->image_number);
            if (!img) {
                set_command_failed_response("ENOENT", "Edit command refers to non-existent image with id: %u and number: %u", g->id, g->image_number);
                ret = finish_command_response(g, false);
            } else {
                GraphicsCommand eg = *g;
                eg.action = 'e';
                if (!eg.id && !eg.image_number) {  // continuation chunk, the response must identify the image from the start command
                    eg.id = self->currently_loading.start_command.id;
                    eg.image_number = self->currently_loading.start_command.image_number;
                }
                // a palette image has its palette updated, any other image is edited as a vector image
                if (img->palette) img = handle_palette_edit_command(self, &eg, img, payload, is_dirty);
                else img = handle_vector_edit_command(self, &eg, img, payload, is_dirty);
                if (!self->currently_loading.loading_for.image_id) free_load_data(&self->currently_loading);
                if (g->quiet) eg.quiet = g->quiet;
                else eg.quiet = self->currently_loading.start_command.quiet;
                ret = finish_command_response(&eg, img != NULL);
            }
            break;
        }
        case 'p': {
            if (!g->id && !g->image_number) {
                REPORT_ERROR("Put graphics command without image id or number");
                break;
            }
            uint32_t image_id = handle_put_command(self, g, c, is_dirty, NULL, cell);
            GraphicsCommand rg = *g;
            rg.id = image_id;
            ret = finish_command_response(&rg, true);
            break;
        }
        case 'd': handle_delete_command(self, g, c, is_dirty, cell); break;
        case 'r': {
            if (!g->id && !g->image_number) {
                REPORT_ERROR("Refresh command without image id or number");
                break;
            }
            Image *img = g->id ? img_by_client_id(self, g->id) : img_by_client_number(self, g->image_number);
            if (!img) set_command_failed_response("ENOENT", "Refresh command refers to non-existent image with id: %u and number: %u", g->id, g->image_number);
            else if (!img->surface) set_command_failed_response("EINVAL", "Refresh command refers to image with id: %u that is not backed by a GPU surface", img->client_id);
            else if (refresh_surface_image(self, img)) *is_dirty = true;
            else set_command_failed_response("EINVAL", "Failed to refresh the GPU surface of image with id: %u", img->client_id);
            ret = finish_command_response(g, true);
        } break;
        case 'c':
            if (!g->id && !g->image_number) {
                REPORT_ERROR("Compose frame data command without image id or number");
                break;
            }
            Image *img = g->id ? img_by_client_id(self, g->id) : img_by_client_number(self, g->image_number);
            if (!img) {
                set_command_failed_response("ENOENT", "Animation command refers to non-existent image with id: %u and number: %u", g->id, g->image_number);
                ret = finish_command_response(g, false);
            } else {
                handle_compose_command(self, is_dirty, g, img);
                if (invalidate_vector_users(self, img->client_id)) *is_dirty = true;
                ret = finish_command_response(g, true);
            }
            break;
        default: REPORT_ERROR("Unknown graphics command action: %c", g->action); break;
    }
    return ret;
}


// Boilerplate {{{
static PyObject *
new_graphicsmanager_object(PyTypeObject UNUSED *type, PyObject UNUSED *args, PyObject UNUSED *kwds) {
    PyObject *ans = (PyObject *)grman_alloc(false);
    if (ans == NULL) PyErr_NoMemory();
    return ans;
}

static PyObject *
image_as_dict(GraphicsManager *self, Image *img) {
#define U(x) #x, (unsigned int)(img->x)
#define B(x) #x, img->x ? Py_True : Py_False
    PyObject *frames = PyTuple_New(img->extra_framecnt);
    for (unsigned i = 0; i < img->extra_framecnt; i++) {
        Frame *f = img->extra_frames + i;
        CoalescedFrameData cfd = get_coalesced_frame_data(self, img, f);
        if (!cfd.buf) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to get data for frame");
            return NULL;
        }
        PyTuple_SET_ITEM(
            frames,
            i,
            Py_BuildValue(
                "{sI sI sy#}", "gap", f->gap, "id", f->id, "data", cfd.buf,
                (Py_ssize_t)(stored_pixel_size(cfd.is_opaque, cfd.is_float, cfd.deep) * img->width * img->height)));
        free(cfd.buf);
        if (PyErr_Occurred()) {
            Py_CLEAR(frames);
            return NULL;
        }
    }
    CoalescedFrameData cfd = {.is_4byte_aligned = true};
    if (img->vector) cfd.buf = render_vector_image(self, img, img->width, img->height);
    else if (!img->surface) cfd = get_coalesced_frame_data(self, img, &img->root_frame);
    if (!cfd.buf && !img->surface) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to get data for root frame");
        return NULL;
    }
    PyObject *ans = Py_BuildValue(
        "{sI sI sI sI sI sI sI "
        "sO sI sO "
        "sI sI sI "
        "sI sI sI sy# sN sO sf sI sI sN sI sI}",
        "texture_id",
        texture_id_for_img(img),
        U(client_id),
        U(width),
        U(height),
        U(internal_id),
        "refs.count",
        (unsigned int)vt_size(&img->refs_by_internal_id),
        U(client_number),

        B(root_frame_data_loaded),
        U(animation_state),
        "is_4byte_aligned",
        img->root_frame.is_4byte_aligned ? Py_True : Py_False,

        U(current_frame_index),
        "root_frame_gap",
        img->root_frame.gap,
        U(current_frame_index),

        U(animation_duration),
        U(gamut),
        U(displayed_frame_id),
        "data",
        cfd.buf,
        (Py_ssize_t)(cfd.buf ? (img->indexed_px_sz ? img->indexed_px_sz : stored_pixel_size(cfd.is_opaque, cfd.is_float, cfd.deep)) * img->width * img->height : 0),
        "extra_frames",
        frames,
        B(tone_map),
        "peak",
        (double)img->peak,
        "vector_elements",
        (unsigned int)(img->vector ? vector_doc_element_count(img->vector) : 0),
        U(indexed_px_sz),
        "palette",
        img->palette ? PyBytes_FromStringAndSize((const char *)img->palette->rgba, (Py_ssize_t)img->palette->slots * 4) : Py_NewRef(Py_None),
        U(transfer),
        "deep",
        (unsigned int)img->root_frame.deep);
    free(cfd.buf);
    return ans;
#undef B
#undef U
}

#define W(x) static PyObject *py##x(GraphicsManager UNUSED *self, PyObject *args)
#define PA(fmt, ...) \
    if (!PyArg_ParseTuple(args, fmt, __VA_ARGS__)) return NULL;

W(image_for_client_id) {
    unsigned long id = PyLong_AsUnsignedLong(args);
    bool existing = false;
    Image *img = find_or_create_image(self, id, &existing);
    if (!existing) { Py_RETURN_NONE; }
    return image_as_dict(self, img);
}

// For tests: the current frame of an image as RGBA, as pointer and vector images get it, or None
W(rgba_for_client_id) {
    const unsigned long id = PyLong_AsUnsignedLong(args);
    if (PyErr_Occurred()) return NULL;
    uint32_t width, height;
    uint8_t *data = grman_image_rgba(self, (uint32_t)id, &width, &height);
    if (!data) { Py_RETURN_NONE; }
    PyObject *ans = PyBytes_FromStringAndSize((const char *)data, (Py_ssize_t)width * height * 4);
    free(data);
    return ans;
}

// For tests: the version of the pixels of an image, see grman_image_version(), or None
W(version_for_client_id) {
    const unsigned long id = PyLong_AsUnsignedLong(args);
    if (PyErr_Occurred()) return NULL;
    id_type image;
    uint64_t version;
    if (!grman_image_version(self, (uint32_t)id, &image, &version)) { Py_RETURN_NONE; }
    return PyLong_FromUnsignedLongLong(version);
}

W(image_for_client_number) {
    unsigned long num = PyLong_AsUnsignedLong(args);
    Image *img = img_by_client_number(self, num);
    if (!img) Py_RETURN_NONE;
    return image_as_dict(self, img);
}

W(shm_write) {
    const char *name, *data;
    Py_ssize_t sz;
    PA("ss#", &name, &data, &sz);
    int fd = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, name);
        return NULL;
    }
    int ret = ftruncate(fd, sz);
    if (ret != 0) {
        safe_close(fd, __FILE__, __LINE__);
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, name);
        return NULL;
    }
    void *addr = mmap(0, sz, PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        safe_close(fd, __FILE__, __LINE__);
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, name);
        return NULL;
    }
    memcpy(addr, data, sz);
    if (munmap(addr, sz) != 0) {
        safe_close(fd, __FILE__, __LINE__);
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, name);
        return NULL;
    }
    safe_close(fd, __FILE__, __LINE__);
    Py_RETURN_NONE;
}

W(shm_unlink) {
    char *name;
    PA("s", &name);
    int ret = shm_unlink(name);
    if (ret == -1) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, name);
        return NULL;
    }
    Py_RETURN_NONE;
}

PyObject *
grman_render_data_as_python(GraphicsManager *self) {
    PyObject *ans = PyTuple_New(self->render_data.count);
    for (size_t i = 0; i < self->render_data.count; i++) {
        ImageRenderData *r = self->render_data.item + i;
#define R(which) Py_BuildValue("{sf sf sf sf}", "left", r->which.left, "top", r->which.top, "right", r->which.right, "bottom", r->which.bottom)
        PyTuple_SET_ITEM(
            ans,
            i,
            Py_BuildValue(
                "{sN sN sI si sK sK sI sO sO}",
                "src_rect",
                R(src_rect),
                "dest_rect",
                R(dest_rect),
                "group_count",
                r->group_count,
                "z_index",
                r->z_index,
                "image_id",
                r->image_id,
                "ref_id",
                r->ref_id,
                "indexed_px_sz",
                (unsigned int)r->indexed_px_sz,
                "is_unscaled",
                r->is_unscaled ? Py_True : Py_False,
                "filter_decoded",
                r->filter_decoded ? Py_True : Py_False));
#undef R
    }
    return ans;
}

W(update_layers) {
    unsigned int scrolled_by, sx, sy;
    float xstart, ystart, dx, dy, scroll_offset_lines = 0.0f;
    CellPixelSize cell;
    PA("IffffIIII|f", &scrolled_by, &xstart, &ystart, &dx, &dy, &sx, &sy, &cell.width, &cell.height, &scroll_offset_lines);
    grman_update_layers(self, scrolled_by, scroll_offset_lines, xstart, ystart, dx, dy, sx, sy, cell);
    return grman_render_data_as_python(self);
}

static Image *
vector_image_for_tests(GraphicsManager *self, unsigned long client_id) {
    Image *img = img_by_client_id(self, client_id);
    if (!img || !img->vector) {
        PyErr_SetString(PyExc_KeyError, "No vector image with that id");
        return NULL;
    }
    return img;
}

// For tests: a vector image rasterized from nothing at the given size
W(render_vector_image) {
    unsigned long id;
    unsigned int width, height;
    PA("kII", &id, &width, &height);
    Image *img = vector_image_for_tests(self, id);
    if (!img) return NULL;
    uint8_t *data = render_vector_image(self, img, width, height);
    if (!data) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to rasterize the vector image");
        return NULL;
    }
    PyObject *ans = PyBytes_FromStringAndSize((const char *)data, (Py_ssize_t)width * height * 4);
    free(data);
    return ans;
}

typedef struct {
    PyObject *ans;
    uint32_t width;
} FloatRows;

static void
emit_float_row(void *ctx, uint32_t y, const float *rgba) {
    FloatRows *r = ctx;
    memcpy(PyBytes_AS_STRING(r->ans) + (size_t)y * r->width * 4 * sizeof(float), rgba, (size_t)r->width * 4 * sizeof(float));
}

// For tests: the root frame of an image that has to be decoded on the CPU,
// see needs_decoding(), as linear RGBA floats in the sRGB primaries
W(decoded_image_pixels) {
    unsigned long id = PyLong_AsUnsignedLong(args);
    if (PyErr_Occurred()) return NULL;
    Image *img = img_by_client_id(self, id);
    if (!img || !img->root_frame_data_loaded || img->vector || img->surface || img->indexed_px_sz) {
        PyErr_SetString(PyExc_KeyError, "No raster image with that id");
        return NULL;
    }
    RAII_CoalescedFrameData(cfd, get_coalesced_frame_data(self, img, &img->root_frame));
    if (!cfd.buf) return PyErr_NoMemory();
    if (!needs_decoding(img, &cfd)) {
        PyErr_SetString(PyExc_ValueError, "The image is decoded by the GPU alone");
        return NULL;
    }
    if ((size_t)img->width * img->height > (size_t)PY_SSIZE_T_MAX / (4 * sizeof(float))) return PyErr_NoMemory();
    FloatRows rows = {.width = img->width};
    rows.ans = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)img->width * img->height * 4 * sizeof(float));
    if (!rows.ans) return NULL;
    if (!decode_frame_rows(img, &cfd, emit_float_row, &rows)) {
        Py_DECREF(rows.ans);
        return PyErr_NoMemory();
    }
    return rows.ans;
}

// Native floats for checking the same unclipped output sent to the GPU.
W(render_vector_linear) {
    unsigned long id;
    unsigned width, height;
    VectorStereo stereo = {.multiplier = 1.};
    PA("kII|ddddddd", &id, &width, &height, &stereo.eye_pixels, &stereo.positive, &stereo.negative, &stereo.fov, &stereo.phase_pixels, &stereo.phase_limit, &stereo.multiplier);
    Image *img = vector_image_for_tests(self, id);
    if (!img) return NULL;
    float *pixels = vector_doc_render_stereo(img->vector, width, height, (VectorRect){0, 0, width, height}, vector_lookup, self, stereo);
    if (!pixels) return PyErr_Format(PyExc_RuntimeError, "Vector render failed");
    PyObject *ans = PyBytes_FromStringAndSize((char *)pixels, (Py_ssize_t)width * height * 4 * sizeof(float));
    free(pixels);
    return ans;
}

// For tests: what update_vector_texture() does, with a bytearray standing in
// for the texture. Returns the rectangle that was drawn again, None when
// there was nothing to draw.
W(update_vector_pixels) {
    unsigned long id;
    unsigned int width, height;
    PyObject *pixels;
    PA("kIIO!", &id, &width, &height, &PyByteArray_Type, &pixels);
    Image *img = vector_image_for_tests(self, id);
    if (!img) return NULL;
    const Py_ssize_t sz = (Py_ssize_t)width * height * 4;
    const bool has_pixels = PyByteArray_GET_SIZE(pixels) == sz;
    VectorRect rect;
    uint8_t *data = render_vector_damage(self, img, width, height, !has_pixels, &rect);
    if (!data) Py_RETURN_NONE;
    if (!has_pixels && PyByteArray_Resize(pixels, sz) != 0) {
        free(data);
        return NULL;
    }
    uint8_t *dest = (uint8_t *)PyByteArray_AS_STRING(pixels) + ((size_t)rect.y * width + rect.x) * 4;
    for (unsigned y = 0; y < rect.height; y++) memcpy(dest + (size_t)y * width * 4, data + (size_t)y * rect.width * 4, (size_t)rect.width * 4);
    free(data);
    return Py_BuildValue("IIII", rect.x, rect.y, rect.width, rect.height);
}

// For tests: a part of a vector image drawn at a size, see VectorView
W(render_vector_view) {
    unsigned long id;
    VectorView v;
    PA("kddddII", &id, &v.width, &v.height, &v.x, &v.y, &v.cols, &v.rows);
    Image *img = vector_image_for_tests(self, id);
    if (!img) return NULL;
    uint8_t *data = vector_doc_render_view(img->vector, v, (VectorRect){0, 0, v.cols, v.rows}, vector_lookup, self);
    if (!data) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to rasterize the vector image");
        return NULL;
    }
    PyObject *ans = PyBytes_FromStringAndSize((const char *)data, (Py_ssize_t)v.cols * v.rows * 4);
    free(data);
    return ans;
}

// For tests: the part of a vector image update_vector_texture() rasterizes
W(choose_vector_view) {
    double width, height, view_width, view_height;
    ImageFraction shown;
    VectorView current = {0};
    PA("dd(dddd)dd|(ddddII)", &width, &height, &shown.left, &shown.top, &shown.right, &shown.bottom, &view_width, &view_height, &current.width,
        &current.height, &current.x, &current.y, &current.cols, &current.rows);
    const VectorView v = choose_vector_view(width, height, shown, view_width, view_height, &current);
    return Py_BuildValue("ddddII", v.width, v.height, v.x, v.y, v.cols, v.rows);
}

#define M(x, va) {#x, (PyCFunction)py##x, va, ""}

static PyMethodDef methods[] = {
    M(image_for_client_id, METH_O),
    M(rgba_for_client_id, METH_O),
    M(version_for_client_id, METH_O),
    M(image_for_client_number, METH_O),
    M(update_layers, METH_VARARGS),
    M(render_vector_image, METH_VARARGS),
    M(render_vector_linear, METH_VARARGS),
    M(decoded_image_pixels, METH_O),
    M(update_vector_pixels, METH_VARARGS),
    M(render_vector_view, METH_VARARGS),
    M(choose_vector_view, METH_VARARGS),
    {NULL} /* Sentinel */
};

static PyObject *
get_image_count(GraphicsManager *self, void *closure UNUSED) {
    return PyLong_FromSize_t(vt_size(&self->images_by_internal_id));
}

static PyGetSetDef getsets[] = {
    {"image_count", (getter)get_image_count, NULL, NULL, NULL},
    {NULL},
};

static PyMemberDef members[] = {
    {"storage_limit", T_PYSSIZET, offsetof(GraphicsManager, storage_limit), 0, "storage_limit"},
    {"disk_cache", T_OBJECT_EX, offsetof(GraphicsManager, disk_cache), READONLY, "disk_cache"},
    {NULL},
};

PyTypeObject GraphicsManager_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "fast_data_types.GraphicsManager",
    .tp_basicsize = sizeof(GraphicsManager),
    .tp_dealloc = (destructor)dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "GraphicsManager",
    .tp_new = new_graphicsmanager_object,
    .tp_methods = methods,
    .tp_members = members,
    .tp_getset = getsets,
};

static PyObject *
pycreate_canvas(PyObject *self UNUSED, PyObject *args) {
    unsigned int bytes_per_pixel;
    unsigned int over_width, width, height, x, y;
    Py_ssize_t over_sz;
    const uint8_t *over_data;
    if (!PyArg_ParseTuple(args, "y#IIIIII", &over_data, &over_sz, &over_width, &x, &y, &width, &height, &bytes_per_pixel)) return NULL;
    if (bytes_per_pixel != 3 && bytes_per_pixel != 4) {
        PyErr_SetString(PyExc_ValueError, "bytes_per_pixel must be 3 or 4");
        return NULL;
    }
    if (!over_width) {
        PyErr_SetString(PyExc_ValueError, "over_width must be non-zero");
        return NULL;
    }
    size_t canvas_sz = (size_t)width * height * bytes_per_pixel;
    PyObject *ans = PyBytes_FromStringAndSize(NULL, canvas_sz);
    if (!ans) return NULL;

    uint8_t *canvas = (uint8_t *)PyBytes_AS_STRING(ans);
    memset(canvas, 0, canvas_sz);
    ComposeData cd = {
        .needs_blending = bytes_per_pixel == 4,
        .over_width = over_width,
        .over_height = (size_t)over_sz / ((size_t)bytes_per_pixel * over_width),
        .under_width = width,
        .under_height = height,
        .over_px_sz = bytes_per_pixel,
        .under_px_sz = bytes_per_pixel,
        .over_offset_x = x,
        .over_offset_y = y};
    compose(cd, canvas, canvas_sz, over_data, (size_t)over_sz);

    return ans;
}

#ifdef __linux__
// For the tests: what a dma-buf surface names, as (pid, fourcc, modifier, ((fd, offset, stride), ...))
static PyObject *
py_dmabuf_parse_spec(PyObject *self UNUSED, PyObject *args) {
    const char *spec;
    Py_ssize_t sz;
    if (!PyArg_ParseTuple(args, "y#", &spec, &sz)) return NULL;
    DmabufSpec ds;
    char err[256];
    if (!dmabuf_parse_spec(spec, (size_t)sz, &ds, err, sizeof(err))) {
        PyErr_SetString(PyExc_ValueError, err);
        return NULL;
    }
    RAII_PyObject(planes, PyTuple_New(ds.num_planes));
    if (!planes) return NULL;
    for (unsigned i = 0; i < ds.num_planes; i++) {
        PyObject *t = Py_BuildValue("iII", ds.planes[i].fd, ds.planes[i].offset, ds.planes[i].stride);
        if (!t) return NULL;
        PyTuple_SET_ITEM(planes, i, t);
    }
    const char fourcc[4] = {(char)(ds.fourcc & 0xff), (char)((ds.fourcc >> 8) & 0xff), (char)((ds.fourcc >> 16) & 0xff), (char)(ds.fourcc >> 24)};
    return Py_BuildValue("is#KO", (int)ds.pid, fourcc, (Py_ssize_t)4, (unsigned long long)ds.modifier, planes);
}

// For the tests: the descriptors the terminal would take for a dma-buf surface from a window whose program is session_of
static PyObject *
py_dmabuf_take_fds(PyObject *self UNUSED, PyObject *args) {
    const char *spec;
    Py_ssize_t sz;
    int session_of;
    if (!PyArg_ParseTuple(args, "y#i", &spec, &sz, &session_of)) return NULL;
    DmabufSpec ds;
    char err[256];
    int fds[arraysz(ds.planes)];
    if (!dmabuf_parse_spec(spec, (size_t)sz, &ds, err, sizeof(err)) || !dmabuf_take_fds(&ds, session_of, fds, err, sizeof(err))) {
        PyErr_SetString(PyExc_ValueError, err);
        return NULL;
    }
    PyObject *ans = PyTuple_New(ds.num_planes);
    if (!ans) {
        dmabuf_close_fds(fds, ds.num_planes);
        return NULL;
    }
    for (unsigned i = 0; i < ds.num_planes; i++) PyTuple_SET_ITEM(ans, i, PyLong_FromLong(fds[i]));
    return ans;
}
#endif

#ifdef __APPLE__
static void
release_test_iosurface(PyObject *capsule) {
    iosurface_release_for_tests(PyCapsule_GetPointer(capsule, "iosurface"));
}

// For the tests: an IOSurface of this process, as a client would share one, as (id, handle). The surface lives as long
// as the handle.
static PyObject *
py_iosurface_create_for_tests(PyObject *self UNUSED, PyObject *args) {
    unsigned int width, height;
    if (!PyArg_ParseTuple(args, "II", &width, &height)) return NULL;
    uint32_t id;
    void *surface = iosurface_create_for_tests(width, height, &id);
    if (!surface) {
        PyErr_SetString(PyExc_OSError, "Failed to create an IOSurface");
        return NULL;
    }
    PyObject *handle = PyCapsule_New(surface, "iosurface", release_test_iosurface);
    if (!handle) {
        iosurface_release_for_tests(surface);
        return NULL;
    }
    return Py_BuildValue("IN", (unsigned int)id, handle);
}

// For the tests: how many surfaces the terminal holds
static PyObject *
py_iosurface_held_count(PyObject *self UNUSED, PyObject *args UNUSED) {
    return PyLong_FromLong(iosurface_held_count());
}
#endif

// For the tests: make the next allocation of the named kind fail, as when out of memory, see Allocation
static PyObject *
pyfail_next_graphics_allocation(PyObject *self UNUSED, PyObject *args) {
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return NULL;
    for (size_t i = 1; i < arraysz(allocation_names); i++) {
        if (strcmp(name, allocation_names[i]) == 0) {
            failing_allocation = (Allocation)i;
            Py_RETURN_NONE;
        }
    }
    PyErr_Format(PyExc_KeyError, "Unknown allocation: %s", name);
    return NULL;
}

static PyMethodDef module_methods[] = {
    M(shm_write, METH_VARARGS), M(shm_unlink, METH_VARARGS), M(create_canvas, METH_VARARGS), M(fail_next_graphics_allocation, METH_VARARGS),
#ifdef __linux__
    {"dmabuf_parse_spec", py_dmabuf_parse_spec, METH_VARARGS, ""}, {"dmabuf_take_fds", py_dmabuf_take_fds, METH_VARARGS, ""},
#endif
#ifdef __APPLE__
    {"iosurface_create_for_tests", py_iosurface_create_for_tests, METH_VARARGS, ""}, {"iosurface_held_count", py_iosurface_held_count, METH_NOARGS, ""},
#endif
    {NULL, NULL, 0, NULL} /* Sentinel */
};


bool
init_graphics(PyObject *module) {
    if (PyType_Ready(&GraphicsManager_Type) < 0) return false;
    if (PyModule_AddObject(module, "GraphicsManager", (PyObject *)&GraphicsManager_Type) != 0) return false;
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    if (PyModule_AddIntMacro(module, IMAGE_PLACEHOLDER_CHAR) != 0) return false;
    Py_INCREF(&GraphicsManager_Type);
    return true;
}

void
grman_mark_layers_dirty(GraphicsManager *self) {
    set_layers_dirty(self);
}
void
grman_set_window_id(GraphicsManager *self, id_type id) {
    self->window_id = id;
}
bool
grman_has_images(GraphicsManager *self) {
    return self->num_of_below_refs + self->num_of_negative_refs + self->num_of_positive_refs > 0;
}
GraphicsRenderData
grman_render_data(GraphicsManager *self) {
    GraphicsRenderData ans = {
        .count = self->render_data.count,
        .capacity = self->render_data.capacity,
        .images = self->render_data.item,
        .num_of_below_refs = self->num_of_below_refs,
        .num_of_negative_refs = self->num_of_negative_refs,
        .num_of_positive_refs = self->num_of_positive_refs,
        .num_of_indexed_refs = self->num_of_indexed_refs,
    };
    return ans;
}
// }}}
