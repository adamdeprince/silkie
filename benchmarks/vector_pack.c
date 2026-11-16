/* License: GPLv3 Copyright: 2026, Adam DePrince
 *
 * Compare the old vector texture packing loop with its sparse-pixel fast path:
 *     cc -O3 benchmarks/vector_pack.c -lm -o /tmp/vector-pack && /tmp/vector-pack
 *
 * These are synthetic RGBA damage buffers at 0%, 5%, and 100% occupancy, not
 * complete game frame timings. Ground Stop's radar paints only a small part
 * of a large damage rectangle, so checking sparse and dense controls matters.
 * Both packed pixels and peak metadata must match exactly before timing.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Same IEEE-754 binary32 -> binary16 conversion as kitty/graphics.c.
static inline uint16_t
float_to_half(const float f) {
    uint32_t bits, mant;
    memcpy(&bits, &f, sizeof(bits));
    const uint16_t sign = (bits >> 16) & 0x8000;
    const int exp = (int)((bits >> 23) & 0xff) - 112;
    mant = bits & 0x7fffff;
    if (exp == 143) return sign | 0x7c00 | (mant ? 0x200 : 0);
    if (exp >= 31) return sign | 0x7c00;
    unsigned shift = 13;
    if (exp <= 0) {
        shift = 14 - exp;
        if (shift > 24) return sign;
        mant |= 0x800000;
    }
    uint32_t h = (exp > 0 ? exp << 10 : 0) | (mant >> shift);
    const uint32_t rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rem > half || (rem == half && (h & 1))) h++;
    return sign | h;
}

__attribute__((noinline)) static float
pack_baseline(const float *linear, uint16_t *data, size_t count) {
    float peak = 0;
    for (size_t i = 0; i < count; i++) {
        const float value = fminf(65504, fmaxf(-65504, linear[i]));
        data[i] = float_to_half(value);
        if ((i & 3) != 3) peak = fmaxf(peak, value);
    }
    return peak;
}

__attribute__((noinline)) static float
pack_sparse(const float *linear, uint16_t *data, size_t count) {
    float peak = 0;
    for (size_t i = 0; i < count; i += 4) {
        if (linear[i + 3] == 0.f) {
            uint64_t bits[2];
            memcpy(bits, linear + i, sizeof(bits));
            if (!(bits[0] | bits[1])) {
                memset(data + i, 0, 4 * sizeof(*data));
                continue;
            }
        }
        for (unsigned k = 0; k < 4; k++) {
            const float value = fminf(65504, fmaxf(-65504, linear[i + k]));
            data[i + k] = float_to_half(value);
            if (k != 3) peak = fmaxf(peak, value);
        }
    }
    return peak;
}

static double
now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        perror("clock_gettime");
        exit(1);
    }
    return t.tv_sec + t.tv_nsec / 1e9;
}

int
main(void) {
    const size_t pixels = 1536 * 1024, count = pixels * 4;
    float *linear = calloc(count, sizeof(*linear));
    uint16_t *baseline = malloc(count * sizeof(*baseline)), *sparse = malloc(count * sizeof(*sparse));
    if (!linear || !baseline || !sparse) {
        perror("allocating benchmark buffers");
        free(linear);
        free(baseline);
        free(sparse);
        return 1;
    }
    const unsigned occupancy[] = {0, 5, 100};
    for (unsigned test = 0; test < sizeof(occupancy) / sizeof(*occupancy); test++) {
        memset(linear, 0, count * sizeof(*linear));
        for (size_t i = 0; i < pixels; i++) if (i % 100 < occupancy[test]) {
            linear[4 * i] = (float)(i % 117) / 31;
            linear[4 * i + 1] = -.3f;
            linear[4 * i + 2] = 1e-7f;
            linear[4 * i + 3] = .7f;
        }
        // Transparent HDR RGB and signed zeros must not take the zero fast path.
        const float special[] = {
            0, -0.f, INFINITY, -INFINITY,
            NAN, 65504, 1e-40f, -1e-40f,
            4, -.25f, 1.5f, 0,
            -0.f, 0, 0, 0,
            0, 0, 0, -0.f,
        };
        memcpy(linear, special, sizeof(special));
        float old_peak = pack_baseline(linear, baseline, count), new_peak = pack_sparse(linear, sparse, count);
        if (memcmp(baseline, sparse, count * sizeof(*baseline)) || memcmp(&old_peak, &new_peak, sizeof(old_peak))) {
            fprintf(stderr, "Packing changed pixels or peak at %u%% occupancy\n", occupancy[test]);
            free(linear);
            free(baseline);
            free(sparse);
            return 1;
        }
        double old_time = 0, new_time = 0;
        const unsigned repeats = 20;
        for (unsigned repeat = 0; repeat < repeats; repeat++) for (unsigned order = 0; order < 2; order++) {
            const bool old_first = ((repeat + order) & 1) == 0;
            const double start = now();
            if (old_first) {
                old_peak = pack_baseline(linear, baseline, count);
                old_time += now() - start;
            } else {
                new_peak = pack_sparse(linear, sparse, count);
                new_time += now() - start;
            }
        }
        printf("%3u%% painted: baseline %.3f ms, sparse %.3f ms, %.2fx, peaks %g/%g\n",
               occupancy[test], old_time * 1000 / repeats, new_time * 1000 / repeats, old_time / new_time, old_peak, new_peak);
    }
    free(linear);
    free(baseline);
    free(sparse);
    return 0;
}
