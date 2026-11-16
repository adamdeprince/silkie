/* Copyright (C) 2026 Adam DePrince. GPLv3. */
#pragma once
#include <math.h>
#include <stdint.h>

// Positive separates the right view from the left (behind the plane);
// negative crosses them (in front). Limits are degrees, independent of resolution.
static inline double
spatial_disparity(double millidegrees, double positive, double negative, double fov) {
    const double radians = acos(-1.) / 180.;
    const double degrees = fmin(positive, fmax(-negative, millidegrees / 1000.));
    return tan(degrees * radians / 2.) / tan(fov * radians / 2.);
}

// Pixel requests use a 1920-wide reference eye, independent of backing scale.
// Convert before clamping, so the user's angular limits also cover multipliers.
static inline double
spatial_requested_disparity(double millidegrees, double pixels, double multiplier, double positive, double negative, double fov) {
    if (pixels != 0.) {
        const double request = pixels / 1920. * multiplier;
        return fmin(spatial_disparity(positive * 1000., positive, negative, fov),
            fmax(spatial_disparity(-negative * 1000., positive, negative, fov), request));
    }
    return spatial_disparity(millidegrees * multiplier, positive, negative, fov);
}
