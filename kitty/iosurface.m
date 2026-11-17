/*
 * iosurface.m
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

#define GL_SILENCE_DEPRECATION 1
#include "iosurface.h"
#include <stdio.h>
#include <IOSurface/IOSurface.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <OpenGL/gl3.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/CGLIOSurface.h>

// The number of surfaces looked up and not yet released, for the tests. Only the main thread looks surfaces up and
// releases them.
static long held_surfaces = 0;

void *
iosurface_lookup(uint32_t id, uint32_t *width, uint32_t *height, bool *is_bgra, char *err, size_t errsz) {
    IOSurfaceRef surface = IOSurfaceLookup(id);
    if (!surface) {
        snprintf(err, errsz, "No IOSurface with id: %u", id);
        return NULL;
    }
    OSType fmt = IOSurfaceGetPixelFormat(surface);
    if (fmt != kCVPixelFormatType_32BGRA && fmt != kCVPixelFormatType_32RGBA) {
        CFRelease(surface);
        snprintf(err, errsz, "IOSurface with id: %u has unsupported pixel format: 0x%x", id, (unsigned)fmt);
        return NULL;
    }
    *is_bgra = fmt == kCVPixelFormatType_32BGRA;
    *width = (uint32_t)IOSurfaceGetWidth(surface);
    *height = (uint32_t)IOSurfaceGetHeight(surface);
    held_surfaces++;
    return surface;
}

bool
iosurface_bind_rectangle_texture(void *surface, uint32_t *texture_id, bool is_bgra) {
    CGLContextObj ctx = CGLGetCurrentContext();
    if (!ctx) return false;
    IOSurfaceRef s = surface;
    if (!*texture_id) glGenTextures(1, texture_id);
    glBindTexture(GL_TEXTURE_RECTANGLE, *texture_id);
    CGLError err = CGLTexImageIOSurface2D(
        ctx, GL_TEXTURE_RECTANGLE, GL_RGBA, (GLsizei)IOSurfaceGetWidth(s), (GLsizei)IOSurfaceGetHeight(s), is_bgra ? GL_BGRA : GL_RGBA,
        is_bgra ? GL_UNSIGNED_INT_8_8_8_8_REV : GL_UNSIGNED_BYTE, s, 0);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    return err == kCGLNoError;
}

void
iosurface_release(void *surface) {
    CFRelease((IOSurfaceRef)surface);
    held_surfaces--;
}

void *
iosurface_create_for_tests(uint32_t width, uint32_t height, uint32_t *id) {
    const int w = (int)width, h = (int)height, bpe = 4, fmt = kCVPixelFormatType_32BGRA;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe), nf = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    IOSurfaceRef surface = NULL;
    if (nw && nh && nb && nf) {
        const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
        const void *values[] = {nw, nh, nb, nf};
        CFDictionaryRef props = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (props) {
            surface = IOSurfaceCreate(props);
            CFRelease(props);
        }
    }
    if (nw) CFRelease(nw);
    if (nh) CFRelease(nh);
    if (nb) CFRelease(nb);
    if (nf) CFRelease(nf);
    if (surface) *id = IOSurfaceGetID(surface);
    return surface;
}

void
iosurface_release_for_tests(void *surface) {
    CFRelease((IOSurfaceRef)surface);
}

long
iosurface_held_count(void) {
    return held_surfaces;
}
