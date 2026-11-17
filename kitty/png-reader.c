/*
 * png-reader.c
 * Copyright (C) 2018 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "data-types.h"
#include "png-reader.h"
#include "cleanup.h"
#include "state.h"
#include <lcms2.h>


static cmsHPROFILE srgb_profile = NULL;
// The gamuts of docs/wide-gamut-protocol.rst as profiles: the primaries of the
// gamut, the D65 white point and the sRGB transfer curve, indexed by gamut id
static cmsHPROFILE gamut_profiles[GAMUT_REC2020 + 1] = {0};

static cmsHPROFILE
profile_for_gamut(unsigned gamut) {
    if (gamut >= arraysz(gamut_profiles)) return NULL;
    if (!gamut_profiles[gamut]) {
        if (gamut == 0) gamut_profiles[0] = cmsCreate_sRGBProfile();
        else {
            static const cmsCIExyYTRIPLE primaries[arraysz(gamut_profiles)] = {
                [GAMUT_DISPLAY_P3] = {{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}},
                [GAMUT_REC2020] = {{0.708, 0.292, 1}, {0.170, 0.797, 1}, {0.131, 0.046, 1}},
            };
            static const cmsCIExyY d65 = {0.3127, 0.3290, 1};
            // The curve of IEC 61966-2-1, as cmsCreate_sRGBProfile() makes it
            static const cmsFloat64Number srgb_curve[] = {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
            cmsToneCurve *curve = cmsBuildParametricToneCurve(NULL, 4, srgb_curve);
            if (curve) {
                cmsToneCurve *curves[3] = {curve, curve, curve};
                gamut_profiles[gamut] = cmsCreateRGBProfile(&d65, &primaries[gamut], curves);
                cmsFreeToneCurve(curve);
            }
        }
    }
    return gamut_profiles[gamut];
}

// The chromaticities of the red, green and blue of an RGB profile as lcms
// sees them, which is adapted to the D50 white of its connection space
static bool
primaries_of_profile(cmsHPROFILE profile, double xy[6]) {
    if (cmsGetColorSpace(profile) != cmsSigRgbData) return false;
    cmsHPROFILE xyz_profile = cmsCreateXYZProfile();
    if (!xyz_profile) return false;
    const cmsUInt32Number flags = cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE;
    cmsHTRANSFORM t = cmsCreateTransform(profile, TYPE_RGB_DBL, xyz_profile, TYPE_XYZ_DBL, INTENT_RELATIVE_COLORIMETRIC, flags);
    cmsCloseProfile(xyz_profile);
    if (!t) return false;
    static const double rgb[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    cmsCIEXYZ xyz[3];
    cmsDoTransform(t, rgb, xyz, 3);
    cmsDeleteTransform(t);
    for (int i = 0; i < 3; i++) {
        const double sum = xyz[i].X + xyz[i].Y + xyz[i].Z;
        if (!(sum > 0)) return false;
        xy[2 * i] = xyz[i].X / sum;
        xy[2 * i + 1] = xyz[i].Y / sum;
    }
    return true;
}

// The wide gamut a profile is in, zero when it is in neither. A profile is in
// a gamut when each of its primaries is within 0.005 in x and in y of that of
// the gamut, both adapted to D50. That takes in the Display P3 and Rec. 2020
// profiles in circulation whatever their curve, which differ from ours in the
// fourth decimal, and leaves out sRGB, Adobe RGB and the P3 of cinemas, whose
// white is not D65 and whose adapted primaries are therefore elsewhere
static uint8_t
wide_gamut_of_profile(cmsHPROFILE profile) {
    double xy[6], wanted[6];
    if (!primaries_of_profile(profile, xy)) return 0;
    for (uint8_t gamut = GAMUT_DISPLAY_P3; gamut <= GAMUT_REC2020; gamut++) {
        cmsHPROFILE p = profile_for_gamut(gamut);
        if (!p || !primaries_of_profile(p, wanted)) continue;
        bool same = true;
        for (int i = 0; i < 6; i++) same = same && fabs(xy[i] - wanted[i]) <= 0.005;
        if (same) return gamut;
    }
    return 0;
}

bool
convert_pixels_between_gamuts(uint8_t *pixels, size_t num_pixels, bool has_alpha, unsigned from, unsigned to) {
    if (from == to) return true;
    cmsHPROFILE a = profile_for_gamut(from), b = profile_for_gamut(to);
    if (!a || !b) return false;
    const cmsUInt32Number fmt = has_alpha ? TYPE_RGBA_8 : TYPE_RGB_8;
    cmsHTRANSFORM t = cmsCreateTransform(a, fmt, b, fmt, INTENT_PERCEPTUAL, 0);
    if (!t) return false;
    // Done in place, which leaves the alpha bytes as they are
    for (size_t done = 0; done < num_pixels;) {
        const size_t n = MIN(num_pixels - done, (size_t)1 << 20);
        cmsDoTransform(t, pixels + done * (has_alpha ? 4 : 3), pixels + done * (has_alpha ? 4 : 3), (cmsUInt32Number)n);
        done += n;
    }
    cmsDeleteTransform(t);
    return true;
}

struct fake_file {
    const uint8_t *buf;
    size_t sz, cur;
};

static void
read_png_from_buffer(png_structp png, png_bytep out, png_size_t length) {
    struct fake_file *f = png_get_io_ptr(png);
    if (!f) png_error(png, "PNG read callback invoked with no input buffer");
    // libpng's png_rw_ptr contract requires supplying the full requested
    // length or aborting via png_error(). Returning fewer bytes silently leaves
    // libpng's read buffer partially uninitialized, which it then parses as
    // legitimate stream bytes. This happens for a truncated stream, e.g. IDAT
    // chunks that do not carry enough compressed data for the rows declared in
    // IHDR, with no further chunks following.
    if (length > f->sz - f->cur) png_error(png, "PNG data is truncated: not enough bytes to satisfy read request");
    memcpy(out, f->buf + f->cur, length);
    f->cur += length;
}

struct custom_error_handler {
    jmp_buf jb;
    png_read_data *d;
};

static void
read_png_error_handler(png_structp png_ptr, png_const_charp msg) {
    struct custom_error_handler *eh;
    eh = png_get_error_ptr(png_ptr);
    if (eh == NULL) fatal("read_png_error_handler: could not retrieve error handler");
    if (eh->d->err_handler) eh->d->err_handler(eh->d, "EBADPNG", msg);
    longjmp(eh->jb, 1);
}

static void
read_png_warn_handler(png_structp UNUSED png_ptr, png_const_charp msg) {
    if (global_state.debug_rendering) log_error("libpng WARNING: %s", msg);
}

#define ABRT(code, msg)                                    \
    {                                                      \
        if (d->err_handler) d->err_handler(d, #code, msg); \
        goto err;                                          \
    }

void
inflate_png_inner_limited(png_read_data *d, const uint8_t *buf, size_t bufsz, int max_image_dimension) {
    struct fake_file f = {.buf = buf, .sz = bufsz};
    png_structp png = NULL;
    png_infop info = NULL;
    struct custom_error_handler eh = {.d = d};
    // volatile because these are modified after setjmp() and read after a
    // longjmp() in the error path, so they must not be cached in registers
    cmsHPROFILE volatile input_profile = NULL;
    cmsHTRANSFORM volatile colorspace_transform = NULL;
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &eh, read_png_error_handler, read_png_warn_handler);
    if (!png) ABRT(ENOMEM, "Failed to create PNG read structure");
    info = png_create_info_struct(png);
    if (!info) ABRT(ENOMEM, "Failed to create PNG info structure");

    if (setjmp(eh.jb)) goto err;

    png_set_read_fn(png, &f, read_png_from_buffer);
    // libpng refuses images wider or taller than a million pixels unless told
    // otherwise. The PNG format allows 2^31 - 1, the memory needed is checked below.
    png_set_user_limits(png, 0x7fffffff, 0x7fffffff);
    png_read_info(png, info);
    png_byte color_type, bit_depth;
    d->width = png_get_image_width(png, info);
    d->height = png_get_image_height(png, info);
    if (max_image_dimension > 0 && (d->width > max_image_dimension || d->height > max_image_dimension)) { ABRT(ENOMEM, "PNG image is too large"); }
    color_type = png_get_color_type(png, info);
    bit_depth = png_get_bit_depth(png, info);
    double image_gamma;
    int intent;
    d->gamut = 0;
    // A file in a wide gamut that is to stay in one is dealt with here, every
    // other file by the code below, as it always was
    bool wide = false;
    if (d->keep_wide_gamut || d->convert_to_gamut) {
        uint8_t source = 0;
#ifdef PNG_READ_cICP_SUPPORTED
        // The code points of ITU-T H.273: primaries 12 are Display P3 and 9
        // Rec. 2020, transfer 13 is the sRGB curve, so the pixels are already
        // what such a gamut holds. A chunk that says anything else, the HDR
        // curves in particular, is ignored as before
        png_byte primaries, transfer, matrix, full_range;
        if ((png_get_cICP(png, info, &primaries, &transfer, &matrix, &full_range) & PNG_INFO_cICP) && transfer == 13 && matrix == 0 && full_range == 1)
            source = primaries == 12 ? GAMUT_DISPLAY_P3 : (primaries == 9 ? GAMUT_REC2020 : 0);
#endif
        cmsHPROFILE from = source ? profile_for_gamut(source) : NULL;
        if (!source && !png_get_sRGB(png, info, &intent)) {
            // An embedded profile takes precedence over gAMA and cHRM chunks
            png_charp name;
            int compression_type;
            png_bytep profdata;
            png_uint_32 proflen;
            if (png_get_iCCP(png, info, &name, &compression_type, &profdata, &proflen) & PNG_INFO_iCCP) {
                input_profile = cmsOpenProfileFromMem(profdata, proflen);
                if (input_profile && (source = wide_gamut_of_profile(input_profile))) from = input_profile;
            }
        }
        if (source) {
            d->gamut = d->convert_to_gamut ? d->convert_to_gamut : source;
            cmsHPROFILE to = profile_for_gamut(d->gamut);
            if (from == to) wide = from != NULL;
            else if (from && to) {
                colorspace_transform = cmsCreateTransform(from, TYPE_RGBA_8, to, TYPE_RGBA_8, INTENT_PERCEPTUAL, 0);
                wide = colorspace_transform != NULL;
            }
            if (!wide) d->gamut = 0;
        }
        if (!wide && input_profile) {
            cmsCloseProfile(input_profile);
            input_profile = NULL;
        }
    }
    if (wide) {
        // the colors are taken care of
    } else if (png_get_sRGB(png, info, &intent)) {
        // do nothing since we output sRGB
    } else if (png_get_gAMA(png, info, &image_gamma)) {
        if (image_gamma != 0 && fabs(image_gamma - 1.0 / 2.2) > 0.0001) png_set_gamma(png, 2.2, image_gamma);
    } else {
        // Look for an embedded color profile
        png_charp name;
        int compression_type;
        png_bytep profdata;
        png_uint_32 proflen;
        if (png_get_iCCP(png, info, &name, &compression_type, &profdata, &proflen) & PNG_INFO_iCCP) {
            input_profile = cmsOpenProfileFromMem(profdata, proflen);
            if (input_profile) {
                if (!srgb_profile) {
                    srgb_profile = cmsCreate_sRGBProfile();
                    if (!srgb_profile) ABRT(ENOMEM, "Out of memory allocating sRGB colorspace profile");
                }
                colorspace_transform = cmsCreateTransform(input_profile, TYPE_RGBA_8, srgb_profile, TYPE_RGBA_8, INTENT_PERCEPTUAL, 0);
            }
        }
    }
    if (!wide && d->convert_to_gamut) {
        // What the code above made sRGB is wanted in another gamut: straight
        // from the embedded profile when there is one, from sRGB otherwise
        cmsHPROFILE to = profile_for_gamut(d->convert_to_gamut), from = colorspace_transform ? input_profile : profile_for_gamut(0);
        if (colorspace_transform) cmsDeleteTransform(colorspace_transform);
        colorspace_transform = from && to ? cmsCreateTransform(from, TYPE_RGBA_8, to, TYPE_RGBA_8, INTENT_PERCEPTUAL, 0) : NULL;
        if (!colorspace_transform) ABRT(EINVAL, "Failed to convert the PNG image to the gamut wanted");
        d->gamut = d->convert_to_gamut;
    }

    // Ensure we get RGBA data out of libpng
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);

    // These color_type don't have an alpha channel then fill it with 0xff.
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    // a row of an image as wide as PNG allows is more than 32 bits can count
    const size_t rowbytes = png_get_rowbytes(png, info);
    size_t alloc_sz, pointers_sz;
    if (__builtin_mul_overflow(rowbytes, (size_t)d->height, &d->sz) || __builtin_add_overflow(d->sz, (size_t)16, &alloc_sz) ||
        __builtin_mul_overflow((size_t)d->height, sizeof(png_bytep), &pointers_sz))
        ABRT(EOVERFLOW, "PNG image dimensions overflow address space");
    d->decompressed = malloc(alloc_sz);
    if (d->decompressed == NULL) {
        d->out_of_memory = true;
        ABRT(ENOMEM, "Out of memory allocating decompression buffer for PNG");
    }
    d->row_pointers = malloc(pointers_sz);
    if (d->row_pointers == NULL) {
        d->out_of_memory = true;
        ABRT(ENOMEM, "Out of memory allocating row_pointers buffer for PNG");
    }
    for (size_t i = 0; i < (size_t)d->height; i++) d->row_pointers[i] = d->decompressed + i * rowbytes;
    png_read_image(png, d->row_pointers);

    if (colorspace_transform) {
        for (int i = 0; i < d->height; i++) { cmsDoTransform(colorspace_transform, d->row_pointers[i], d->row_pointers[i], d->width); }
    }

    d->ok = true;
err:
    if (colorspace_transform) cmsDeleteTransform(colorspace_transform);
    if (input_profile) cmsCloseProfile(input_profile);
    if (png) png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    return;
}

// Structure to hold memory write state
typedef struct {
    unsigned char *buffer;
    size_t size, capacity;
} png_memory_write_state;

// Custom write function for writing PNG data to memory
static void
png_write_to_memory(png_structp png_ptr, png_bytep data, png_size_t length) {
    png_memory_write_state *state = (png_memory_write_state *)png_get_io_ptr(png_ptr);
    if (state->size + length > state->capacity) {
        // Double the capacity or add enough space for the new data, whichever is larger
        size_t new_capacity = state->capacity * 2;
        if (new_capacity < state->size + length) new_capacity = state->size + length;
        unsigned char *new_buffer = realloc(state->buffer, new_capacity);
        if (!new_buffer) {
            png_error(png_ptr, "Failed to allocate memory for PNG buffer");
            return;
        }
        state->buffer = new_buffer;
        state->capacity = new_capacity;
    }
    // Copy the data to the buffer
    memcpy(state->buffer + state->size, data, length);
    state->size += length;
}
static void
png_flush_memory(png_structp png_ptr) {
    (void)png_ptr;
}

static const char *
create_png_from_data(const char *data, size_t width, size_t height, size_t stride, size_t *out_size, bool flip_vertically, int color_type) {
    *out_size = 0;
    // on the heap as the write callback changes it after setjmp
    png_memory_write_state *state = calloc(1, sizeof(*state));
    if (!state) return "Out of memory";
    state->capacity = width * height * sizeof(uint32_t);
    state->buffer = malloc(state->capacity);
    if (!state->buffer) {
        free(state);
        return "Out of memory";
    }
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        free(state->buffer);
        free(state);
        return "Failed to create PNG write struct";
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        free(state->buffer);
        free(state);
        png_destroy_write_struct(&png_ptr, NULL);
        return "Failed to create PNG info struct";
    }
    png_bytep *volatile row_pointers = NULL;
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(state->buffer);
        free(state);
        free(row_pointers);
        return ("Error during PNG creation\n");
    }
    png_set_write_fn(png_ptr, state, png_write_to_memory, png_flush_memory);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8, color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    // Allocate memory for row pointers
    row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(state->buffer);
        free(state);
        return ("Failed to allocate memory for row pointers");
    }
    if (flip_vertically)
        for (size_t y = 0; y < height; y++) row_pointers[height - 1 - y] = (png_byte *)&data[y * stride];
    else
        for (size_t y = 0; y < height; y++) row_pointers[y] = (png_byte *)&data[y * stride];
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    free(row_pointers);
    char *ans = (char *)state->buffer;
    *out_size = state->size;
    free(state);
    return ans;
}

const char *
png_from_32bit_rgba(const char *data, size_t width, size_t height, size_t *out_size, bool flip_vertically) {
    return create_png_from_data(data, width, height, 4 * width, out_size, flip_vertically, PNG_COLOR_TYPE_RGBA);
}

PyObject *
png_from_32bit_rgba_data(PyObject *self UNUSED, PyObject *args) {
    int flip_vertically = 0;
    const char *data;
    Py_ssize_t len;
    Py_ssize_t width, height;
    if (!PyArg_ParseTuple(args, "y#nn|p", &data, &len, &width, &height, &flip_vertically)) return NULL;
    if (width <= 0 || height <= 0) {
        PyErr_SetString(PyExc_ValueError, "width and height must be positive");
        return NULL;
    }
    if (width > PY_SSIZE_T_MAX / 4 / height || (size_t)width * height * 4 != (size_t)len) {
        PyErr_SetString(PyExc_ValueError, "data length does not match 4 * width * height");
        return NULL;
    }
    size_t out_size;
    const char *out = create_png_from_data(data, width, height, (size_t)4 * width, &out_size, flip_vertically, PNG_COLOR_TYPE_RGBA);
    if (!out_size) {
        PyErr_SetString(PyExc_RuntimeError, out);
        return NULL;
    }
    PyObject *ans = PyBytes_FromStringAndSize(out, out_size);
    free((void *)out);
    return ans;
}


const char *
png_from_24bit_rgb(const char *data, size_t width, size_t height, size_t *out_size, bool flip_vertically) {
    return create_png_from_data(data, width, height, 3 * width, out_size, flip_vertically, PNG_COLOR_TYPE_RGB);
}


static void
png_error_handler(png_read_data *d UNUSED, const char *code, const char *msg) {
    if (!PyErr_Occurred()) PyErr_Format(PyExc_ValueError, "[%s] %s", code, msg);
}

static PyObject *
load_png_data(PyObject *self UNUSED, PyObject *args) {
    Py_ssize_t sz;
    const char *data;
    if (!PyArg_ParseTuple(args, "s#", &data, &sz)) return NULL;
    png_read_data d = {.err_handler = png_error_handler};
    inflate_png_inner(&d, (const uint8_t *)data, sz);

    PyObject *ans = NULL;
    if (d.ok && !PyErr_Occurred()) {
        ans = Py_BuildValue("y#ii", d.decompressed, (Py_ssize_t)d.sz, d.width, d.height);
    } else {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "Unknown error while reading PNG data");
    }
    free(d.decompressed);
    free(d.row_pointers);
    return ans;
}

static PyMethodDef module_methods[] = {
    METHODB(load_png_data, METH_VARARGS), METHODB(png_from_32bit_rgba_data, METH_VARARGS), {NULL, NULL, 0, NULL} /* Sentinel */
};


static void
unload(void) {
    if (srgb_profile) cmsCloseProfile(srgb_profile);
    srgb_profile = NULL;
    for (size_t i = 0; i < arraysz(gamut_profiles); i++) {
        if (gamut_profiles[i]) cmsCloseProfile(gamut_profiles[i]);
        gamut_profiles[i] = NULL;
    }
}

bool
init_png_reader(PyObject *module) {
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    register_at_exit_cleanup_func(PNG_READER_CLEANUP_FUNC, unload);
    return true;
}
