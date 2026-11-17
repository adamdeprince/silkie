/*
 * vector.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

// Vector images for the graphics protocol: a small retained scene graph of
// paths, symbols, pen strokes, raster images and gradients, parsed from text
// records and rasterized on the CPU with analytic antialiasing. See
// docs/vector-protocol.rst for the format.

#include "vector.h"
#include "spatial.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include "vector-color.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TOLERANCE 0.25 // flattening error in device pixels
#define SUBSAMPLES 16  // scanlines per pixel row

// Geometry {{{

typedef struct {
    double a, b, c, d, e, f;  // x' = a x + c y + e, y' = b x + d y + f
} Mat;
static const Mat identity = {1, 0, 0, 1, 0, 0};

static Mat
mat_mul(Mat m, Mat n) {  // apply n then m
    return (Mat){
        m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}

static bool
mat_invert(Mat m, Mat *out) {
    double det = m.a * m.d - m.b * m.c;
    if (fabs(det) < 1e-12) return false;
    *out = (Mat){m.d / det, -m.b / det, -m.c / det, m.a / det, (m.c * m.f - m.d * m.e) / det, (m.b * m.e - m.a * m.f) / det};
    return true;
}

// The inverse of any transform whose determinant is not zero, as long as doubles can hold it
static bool
mat_invert_any(Mat m, Mat *out) {
    const double det = m.a * m.d - m.b * m.c;
    if (!(det != 0) || !isfinite(det)) return false;
    const Mat inv = {m.d / det, -m.b / det, -m.c / det, m.a / det, (m.c * m.f - m.d * m.e) / det, (m.b * m.e - m.a * m.f) / det};
    if (!isfinite(inv.a) || !isfinite(inv.b) || !isfinite(inv.c) || !isfinite(inv.d) || !isfinite(inv.e) || !isfinite(inv.f)) return false;
    *out = inv;
    return true;
}

static bool
mat_is_finite(Mat m) {
    return isfinite(m.a) && isfinite(m.b) && isfinite(m.c) && isfinite(m.d) && isfinite(m.e) && isfinite(m.f);
}

static double
mat_scale(Mat m) {  // largest stretch, used to scale tolerances into user space
    double s = fmax(m.a * m.a + m.b * m.b, m.c * m.c + m.d * m.d);
    return s > 1e-18 ? sqrt(s) : 1e-9;
}

static inline void
apply(Mat m, double x, double y, double *ox, double *oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

// A box with its sides along the axes, empty when x1 < x0
typedef struct {
    double x0, y0, x1, y1;
} Box;
static const Box empty_box = {INFINITY, INFINITY, -INFINITY, -INFINITY};
static const Box everything = {-INFINITY, -INFINITY, INFINITY, INFINITY};

static inline bool
box_is_empty(Box b) {
    return !(b.x0 <= b.x1 && b.y0 <= b.y1);
}

static inline void
box_add(Box *b, double x, double y) {
    b->x0 = fmin(b->x0, x), b->y0 = fmin(b->y0, y), b->x1 = fmax(b->x1, x), b->y1 = fmax(b->y1, y);
}

static inline void
box_union(Box *b, Box o) {
    if (box_is_empty(o)) return;
    box_add(b, o.x0, o.y0);
    box_add(b, o.x1, o.y1);
}

static inline Box
box_grow(Box b, double by) {
    return box_is_empty(b) ? b : (Box){b.x0 - by, b.y0 - by, b.x1 + by, b.y1 + by};
}

// The box around b after the transform, everything when that cannot be worked out
static Box
box_transform(Box b, Mat m) {
    if (box_is_empty(b)) return b;
    Box ans = empty_box;
    const double xs[2] = {b.x0, b.x1}, ys[2] = {b.y0, b.y1};
    for (int i = 0; i < 4; i++) {
        double x, y;
        apply(m, xs[i & 1], ys[i >> 1], &x, &y);
        if (!isfinite(x) || !isfinite(y)) return everything;
        box_add(&ans, x, y);
    }
    return ans;
}

static inline bool
box_misses(Box b, Box region) {
    return b.x1 < region.x0 || b.x0 > region.x1 || b.y1 < region.y0 || b.y0 > region.y1;
}

enum { PC_MOVE, PC_LINE, PC_QUAD, PC_CUBIC, PC_CLOSE };
typedef struct {
    uint8_t op;
    double p[6];
} PathCmd;
typedef struct {
    PathCmd *item;
    size_t count, capacity;
} Path;

typedef struct {
    double *pts;  // x, y pairs
    size_t count, capacity;
    bool closed;
    // A piece of a dash cut where it can no longer be seen ends there without a cap, see dash_polyline()
    bool cut_start, cut_end;
} Poly;
typedef struct {
    Poly *item;
    size_t count, capacity;
} PolyList;

// Reserve storage without imposing a content limit or overflowing allocation sizes.
static void *
reserve_array(void *items, size_t *capacity, size_t needed, size_t item_size, size_t initial) {
    const size_t max = SIZE_MAX / item_size;
    if (needed > max) return NULL;
    size_t nc = *capacity ? *capacity : initial;
    while (nc < needed) {
        if (nc > max / 2) {
            nc = needed;
            break;
        }
        nc *= 2;
    }
    if (nc > max) nc = needed;
    void *np = realloc(items, nc * item_size);
    if (np) *capacity = nc;
    return np;
}

#define GROW(arr, field, needed)                                                                                      \
    if ((needed) > SIZE_MAX - (arr)->count) return false;                                                             \
    if ((arr)->count + (needed) > (arr)->capacity) {                                                                  \
        void *np = reserve_array((arr)->field, &(arr)->capacity, (arr)->count + (needed), sizeof(*(arr)->field), 16); \
        if (!np) return false;                                                                                        \
        (arr)->field = np;                                                                                            \
    }

static bool
path_add(Path *p, uint8_t op, const double *pts, unsigned n) {
    GROW(p, item, 1);
    PathCmd *c = p->item + p->count++;
    c->op = op;
    memset(c->p, 0, sizeof(c->p));
    if (n) memcpy(c->p, pts, n * sizeof(double));
    return true;
}

static bool
poly_add(Poly *p, double x, double y) {
    GROW(p, pts, 2);
    p->pts[p->count++] = x;
    p->pts[p->count++] = y;
    return true;
}

static Poly *
polylist_new(PolyList *pl) {
    if (pl->count == SIZE_MAX) return NULL;
    if (pl->count + 1 > pl->capacity) {
        void *np = reserve_array(pl->item, &pl->capacity, pl->count + 1, sizeof(Poly), 8);
        if (!np) return NULL;
        pl->item = np;
    }
    Poly *ans = pl->item + pl->count++;
    memset(ans, 0, sizeof(*ans));
    return ans;
}

static void
polylist_free(PolyList *pl) {
    for (size_t i = 0; i < pl->count; i++) free(pl->item[i].pts);
    free(pl->item);
    memset(pl, 0, sizeof(*pl));
}

static Poly *
start_subpath(PolyList *out, double x, double y) {
    Poly *p = polylist_new(out);
    if (p && !poly_add(p, x, y)) return NULL;
    return p;
}

// }}}

// Flattening only what can be seen {{{
//
// The work of flattening a curve grows with its size. Wang's formula asks for
// a number of segments that grows with the square root of how far the curve
// bends, in device pixels, and a circle needs segments in proportion to the
// square root of its radius. Coordinates, radii, widths and transforms all come
// from the program drawing, so left alone a curve a hundred million million
// pixels across would take billions of segments, although only a sliver of it
// can be seen. What can be seen is bounded: the pixels of the image being
// drawn. So only the part of a curve that can reach those pixels is flattened
// to the tolerance, and the rest is replaced by coarse pieces that keep out of
// them.
//
// Replacing a piece of a curve by the straight line between its ends leaves the
// winding number of every point outside the convex hull of the piece as it was:
// the piece and the line together make a closed loop inside the hull, which
// winds around nothing outside it. The hull of a Bézier curve is that of its
// control points, which lies inside their bounding box, so a piece whose control
// points have a bounding box clear of the visible region can become a line
// without changing the coverage of a single visible pixel: of a fill under
// either rule, of a clip, and of a stroke when the region is grown by how far
// the stroke reaches from its path. The off-screen parts of a fill still count
// for the winding of the pixels that can be seen, which is why they become
// lines and are not dropped. A piece that does reach the region is split in
// two, until each piece either misses it or could fit inside it. A curve whose
// control points fit in a box the size of the region needs at most
// Cull.direct segments, see make_cull(), and a curve that does not fit has only
// a few pieces at each halving of its size that reach the region, so the
// segments of any curve come to a small multiple of Cull.direct, whatever its
// coordinates. A curve that fits in the region is flattened directly, exactly
// as it always was.
//
// Circles, for round joins, caps and pen points, are treated the same way along
// arcs of the points they are flattened to, so the points kept are exactly
// those of flattening the whole circle, see circle_points(). Dashes are skipped
// through rather than cut where they cannot be seen, see dash_polyline().
//
// The region is that of the whole raster being drawn into, not that of the part
// of it being drawn, so that drawing a part flattens every curve exactly as
// drawing all of it does, and the part comes out the same.

typedef struct {
    Mat to_device;  // from the coordinates points are made in to device pixels
    Box region;     // device pixels from which something can reach a visible pixel
    double tol;     // flattening error in device pixels
    double direct;  // the most segments a curve that fits in the region needs
} Cull;

// view is the raster being drawn into, reach how far what is made of the
// flattened path can reach from it, in device pixels
static Cull
make_cull(Mat to_device, Box view, double reach, double tol) {
    // 2 more for the pixel of slack polygon_misses() and the rasterizer allow
    const Box region = box_grow(view, reach + 2);
    const double size = fmax(region.x1 - region.x0, region.y1 - region.y0);
    // Control points in a box of side s have second differences of at most 2s
    // along each axis, so Wang's formula in wang_segments() gives at most
    // sqrt(0.75 * 2 sqrt(2) s / tol) segments for such a curve
    const double direct = ceil(sqrt(0.75 * 2.8284271247461903 * size / tol));
    return (Cull){to_device, region, tol, fmax(1, direct)};
}

// Wang's formula for the number of segments a cubic needs for the error to stay below tol
static inline double
wang_segments(const double *d, double tol) {
    double ddx = fmax(fabs(d[0] - 2 * d[2] + d[4]), fabs(d[2] - 2 * d[4] + d[6])), ddy = fmax(fabs(d[1] - 2 * d[3] + d[5]), fabs(d[3] - 2 * d[5] + d[7]));
    double dd = sqrt(ddx * ddx + ddy * ddy);
    return sqrt(0.75 * dd / tol);
}

// A curve is halved at most this many times. A piece that is 2^-64 of a curve
// and still needs more than Cull.direct segments can only come from coordinates
// so large that doubles cannot place points of it to within a pixel.
#define MAX_SPLITS 64
// In exact arithmetic only a few pieces at each halving reach the region, so a
// curve is looked at far fewer times than this. Rounding in curves with absurd
// coordinates is all this stops from multiplying pieces; past it a piece becomes
// its chord.
#define PIECE_BUDGET (1u << 16)

// Adds the points after the first of the cubic p (four x, y pairs) to cur
static bool
flatten_curve(Poly *cur, const double *p, const Cull *c) {
    typedef struct {
        double p[8];
        unsigned depth;
    } Piece;
    // each halving leaves one half waiting, so this holds every piece not yet looked at
    Piece stack[MAX_SPLITS + 1];
    size_t top = 1, budget = PIECE_BUDGET;
    memcpy(stack[0].p, p, sizeof(stack[0].p));
    stack[0].depth = 0;
    while (top) {
        const Piece pc = stack[--top];
        const double *q = pc.p;
        double d[8];
        bool finite = true;
        Box b = empty_box;
        for (int k = 0; k < 4; k++) {
            apply(c->to_device, q[2 * k], q[2 * k + 1], d + 2 * k, d + 2 * k + 1);
            finite = finite && isfinite(q[2 * k]) && isfinite(q[2 * k + 1]) && isfinite(d[2 * k]) && isfinite(d[2 * k + 1]);
            box_add(&b, d[2 * k], d[2 * k + 1]);
        }
        // A piece that overflowed cannot be flattened, and one that misses the region need not be: see above
        if (!finite || box_misses(b, c->region) || !budget) {
            if (!poly_add(cur, q[6], q[7])) return false;
            continue;
        }
        budget--;
        const double nd = wang_segments(d, c->tol);
        if (nd > c->direct && pc.depth < MAX_SPLITS) {
            // de Casteljau at t = 1/2, the second half first so that the first is taken first
            Piece l = {.depth = pc.depth + 1}, r = {.depth = pc.depth + 1};
            for (int k = 0; k < 2; k++) {
                const double p01 = (q[k] + q[2 + k]) / 2, p12 = (q[2 + k] + q[4 + k]) / 2, p23 = (q[4 + k] + q[6 + k]) / 2;
                const double p012 = (p01 + p12) / 2, p123 = (p12 + p23) / 2, mid = (p012 + p123) / 2;
                l.p[k] = q[k], l.p[2 + k] = p01, l.p[4 + k] = p012, l.p[6 + k] = mid;
                r.p[k] = mid, r.p[2 + k] = p123, r.p[4 + k] = p23, r.p[6 + k] = q[6 + k];
            }
            stack[top++] = r;
            stack[top++] = l;
            continue;
        }
        size_t n = nd > 1 ? (size_t)ceil(fmin(nd, c->direct)) : 1;
        // vector_curve_limit 0 keeps every segment the error bound asks for.
        if (kitty_vector_curve_limit() && n > kitty_vector_curve_limit()) n = (size_t)kitty_vector_curve_limit();
        for (size_t k = 1; k <= n; k++) {
            double t = (double)k / n, mt = 1 - t;
            double w0 = mt * mt * mt, w1 = 3 * mt * mt * t, w2 = 3 * mt * t * t, w3 = t * t * t;
            if (!poly_add(cur, w0 * q[0] + w1 * q[2] + w2 * q[4] + w3 * q[6], w0 * q[1] + w1 * q[3] + w2 * q[5] + w3 * q[7])) return false;
        }
    }
    return true;
}

// Flattens a path into polylines, with the transform m applied to the control
// points first. cull says what can be seen, from where the points end up.
static bool
flatten_path(const Path *path, Mat m, const Cull *cull, PolyList *out) {
    Poly *cur = NULL;
    double cx = 0, cy = 0, sx = 0, sy = 0;  // current and subpath start points, in output space
    for (size_t i = 0; i < path->count; i++) {
        const PathCmd *c = path->item + i;
        double x, y;
        switch (c->op) {
            case PC_MOVE:
                apply(m, c->p[0], c->p[1], &x, &y);
                if (!(cur = start_subpath(out, x, y))) return false;
                cx = sx = x;
                cy = sy = y;
                break;
            case PC_LINE:
                if (!cur && !(cur = start_subpath(out, cx, cy))) return false;
                apply(m, c->p[0], c->p[1], &x, &y);
                if (!poly_add(cur, x, y)) return false;
                cx = x;
                cy = y;
                break;
            case PC_QUAD:
            case PC_CUBIC: {
                if (!cur && !(cur = start_subpath(out, cx, cy))) return false;
                double x1, y1, x2, y2, x3, y3;
                apply(m, c->p[0], c->p[1], &x1, &y1);
                apply(m, c->p[2], c->p[3], &x2, &y2);
                if (c->op == PC_QUAD) {  // elevate to a cubic
                    x3 = x2, y3 = y2;
                    x2 = x1 + (x3 - x1) / 3, y2 = y1 + (y3 - y1) / 3;
                    x1 = cx + 2 * (x1 - cx) / 3, y1 = cy + 2 * (y1 - cy) / 3;
                } else apply(m, c->p[4], c->p[5], &x3, &y3);
                const double q[8] = {cx, cy, x1, y1, x2, y2, x3, y3};
                if (!flatten_curve(cur, q, cull)) return false;
                cx = x3;
                cy = y3;
            } break;
            case PC_CLOSE:
                if (!cur) break;
                cur->closed = true;
                cx = sx;
                cy = sy;
                cur = NULL;
                break;
        }
    }
    return true;
}

// }}}

// Rasterizer {{{

typedef struct {
    double x0, y0, x1, y1, slope;  // y0 < y1
    int dir;
} Edge;
typedef struct {
    Edge *item;
    size_t count, capacity;
} EdgeList;

static bool
add_edge(EdgeList *el, double x0, double y0, double x1, double y1, int dir) {
    if (y0 == y1 || !isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return true;
    GROW(el, item, 1);
    Edge *e = el->item + el->count++;
    if (y0 > y1) {
        double t = x0;
        x0 = x1, x1 = t, t = y0, y0 = y1, y1 = t;
        dir = -dir;
    }
    *e = (Edge){x0, y0, x1, y1, (x1 - x0) / (y1 - y0), dir};
    return true;
}

// Adds the closed polygon pts (n points) transformed by m. With orient, the
// polygon is wound consistently so that overlapping pieces union under the
// nonzero rule.
static bool
add_polygon(EdgeList *el, const double *pts, size_t n, Mat m, bool orient) {
    if (n < 2) return true;
    int dir = 1;
    if (orient) {
        double area = 0;
        for (size_t i = 0; i < n; i++) {
            size_t j = (i + 1) % n;
            area += pts[2 * i] * pts[2 * j + 1] - pts[2 * j] * pts[2 * i + 1];
        }
        if (area < 0) dir = -1;
        if ((m.a * m.d - m.b * m.c) < 0) dir = -dir;
    }
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        double x0, y0, x1, y1;
        apply(m, pts[2 * i], pts[2 * i + 1], &x0, &y0);
        apply(m, pts[2 * j], pts[2 * j + 1], &x1, &y1);
        if (!add_edge(el, x0, y0, x1, y1, dir)) return false;
    }
    return true;
}

typedef void (*span_fn)(void *ctx, int y, int x0, int x1, const float *cov);

// Pixels of the image, x1 and y1 exclusive
typedef struct {
    int x0, y0, x1, y1;
} Rect;

static inline size_t
rect_size(Rect r) {
    return (size_t)(r.x1 - r.x0) * (size_t)(r.y1 - r.y0);
}

static inline size_t
rect_index(Rect r, int x, int y) {
    return (size_t)(y - r.y0) * (size_t)(r.x1 - r.x0) + (size_t)(x - r.x0);
}

// Whether a closed polygon stays clear of the pixels of clip. Such a polygon
// winds around none of them, so leaving it out changes nothing there.
static bool
polygon_misses(const double *pts, size_t n, Mat m, Rect clip) {
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        double x, y;
        apply(m, pts[2 * i], pts[2 * i + 1], &x, &y);
        if (!isfinite(x) || !isfinite(y)) return false;
        x0 = fmin(x0, x), y0 = fmin(y0, y), x1 = fmax(x1, x), y1 = fmax(y1, y);
    }
    return x1 < clip.x0 - 1 || x0 > clip.x1 + 1 || y1 < clip.y0 - 1 || y0 > clip.y1 + 1;
}

typedef struct {
    double x;
    Edge *edge;
} Crossing;

static int
cmp_edge_y(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    return x->y0 < y->y0 ? -1 : x->y0 > y->y0;
}

static int
cmp_crossing_x(const void *a, const void *b) {
    const Crossing *x = a, *y = b;
    return x->x < y->x ? -1 : x->x > y->x;
}

#define COVERAGE_ONE 4096  // a whole pixel on one subsample line

// Coverage is summed in whole numbers, so a span adds exactly nothing to the
// pixels past its end and the coverage of a pixel depends on the geometry over
// that pixel alone. That is what lets a part of an image be drawn again and
// come out the same as when all of it is drawn.
static inline void
add_span(int32_t *acc, double xa, double xb, double limit) {
    if (!(xa >= 0)) xa = 0;
    if (!(xb <= limit)) xb = limit;
    if (!(xb > xa)) return;
    const int ia = (int)xa, ib = (int)xb;
    const int32_t fa = (int32_t)((xa - ia) * COVERAGE_ONE + 0.5), fb = (int32_t)((xb - ib) * COVERAGE_ONE + 0.5);
    acc[ia] += COVERAGE_ONE - fa;
    acc[ia + 1] += fa;
    acc[ib] += fb - COVERAGE_ONE;
    acc[ib + 1] -= fb;
}

// Scanline rasterization of the pixels in clip, with horizontal coverage to
// one part in COVERAGE_ONE and SUBSAMPLES-fold vertical coverage, calling fn
// once per row with the coverage of the covered pixels.
static bool
rasterize(EdgeList *el, bool evenodd, Rect clip, span_fn fn, void *ctx) {
    if (!el->count || clip.x1 <= clip.x0 || clip.y1 <= clip.y0) return true;
    double minx = INFINITY, maxx = -INFINITY, miny = INFINITY, maxy = -INFINITY;
    bool reusable_order = true;
    for (size_t i = 0; i < el->count; i++) {
        const Edge *e = el->item + i;
        minx = fmin(minx, fmin(e->x0, e->x1));
        maxx = fmax(maxx, fmax(e->x0, e->x1));
        miny = fmin(miny, e->y0);
        maxy = fmax(maxy, e->y1);
        // Finite input coordinates can still overflow slope/intersection
        // arithmetic. Preserve the original active order for that entire
        // shape: an earlier finite sample cannot predict later NaN barriers.
        const double sy0 = fmax(e->y0, clip.y0), sy1 = fmin(e->y1, clip.y1);
        if (!isfinite(e->slope) || (sy0 < sy1 &&
            (!isfinite(e->x0 + (sy0 - e->y0) * e->slope) || !isfinite(e->x0 + (sy1 - e->y0) * e->slope)))) reusable_order = false;
    }
    // compared before they are converted, the edges can be anywhere
    const double fx0 = fmax(clip.x0, floor(minx)), fx1 = fmin(clip.x1 - 1, ceil(maxx)), fy0 = fmax(clip.y0, floor(miny)), fy1 = fmin(clip.y1 - 1, ceil(maxy));
    if (!(fx0 <= fx1 && fy0 <= fy1)) return true;
    const int bx0 = (int)fx0, bx1 = (int)fx1, by0 = (int)fy0, by1 = (int)fy1;
    const int bw = bx1 - bx0 + 1;
    int32_t *acc = calloc(bw + 2, sizeof(int32_t));
    float *cov = malloc(bw * sizeof(float));
    Edge **active = malloc(el->count * sizeof(Edge *));
    Crossing *xs = malloc(el->count * sizeof(Crossing));
    bool ok = acc && cov && active && xs;
    if (ok) {
        qsort(el->item, el->count, sizeof(Edge), cmp_edge_y);
        size_t next = 0, nactive = 0;
        for (int y = by0; y <= by1; y++) {
            bool any = false;
            for (int k = 0; k < SUBSAMPLES; k++) {
                double sy = y + (k + 0.5) / SUBSAMPLES;
                while (next < el->count && el->item[next].y0 <= sy) active[nactive++] = el->item + next++;
                size_t nx = 0;
                bool finite_crossings = reusable_order;
                for (size_t i = 0; i < nactive;) {
                    Edge *e = active[i];
                    if (e->y1 <= sy) {
                        active[i] = active[--nactive];
                        continue;
                    }
                    // taking a whole number of pixels off a crossing at or right of bx0 loses nothing of where in its pixel it is
                    xs[nx++] = (Crossing){e->x0 + (sy - e->y0) * e->slope - bx0, e};
                    finite_crossings &= isfinite(xs[nx - 1].x);
                    i++;
                }
                if (nx < 2) continue;
                // Keep active edges in crossing order between subsamples.
                // Insertion handles their usual small changes in linear work;
                // fall back to O(n log n) if new edges or intersections make
                // this sample disorderly. This budget changes the algorithm,
                // never the number of crossings processed.
                size_t moves = 0;
                for (size_t i = 1; i < nx; i++) {
                    Crossing c = xs[i];
                    size_t j = i;
                    while (j > 0 && xs[j - 1].x > c.x) {
                        xs[j] = xs[j - 1];
                        j--;
                        if (finite_crossings && ++moves == nx) break;
                    }
                    xs[j] = c;
                    if (finite_crossings && moves == nx) {
                        qsort(xs, nx, sizeof(Crossing), cmp_crossing_x);
                        break;
                    }
                }
                if (finite_crossings) for (size_t i = 0; i < nx; i++) active[i] = xs[i].edge;
                int wind = 0;
                double start = 0;
                for (size_t i = 0; i < nx; i++) {
                    bool was = evenodd ? (wind & 1) : wind != 0;
                    wind += xs[i].edge->dir;
                    bool now = evenodd ? (wind & 1) : wind != 0;
                    if (!was && now) start = xs[i].x;
                    else if (was && !now) {
                        add_span(acc, start, xs[i].x, bw);
                        any = true;
                    }
                }
            }
            if (!any) continue;
            int32_t run = 0;
            int first = -1, last = -1;
            for (int i = 0; i < bw; i++) {
                run += acc[i];
                float c = (float)run * (1.f / (SUBSAMPLES * COVERAGE_ONE));
                c = c < 0 ? 0 : (c > 1 ? 1 : c);
                // Every positive quantized coverage must be painted, including
                // when a damage crop excludes stronger spans elsewhere in the row.
                if (c > 0) {
                    if (first < 0) first = i;
                    last = i;
                }
                cov[i] = c;
            }
            memset(acc, 0, (bw + 2) * sizeof(int32_t));
            if (first >= 0) fn(ctx, y, bx0 + first, bx0 + last + 1, cov + first);
        }
    }
    free(acc);
    free(cov);
    free(active);
    free(xs);
    return ok;
}

// }}}

// Stroking {{{

typedef struct {
    EdgeList *edges;
    Mat m;
    Rect clip;   // the pixels being drawn, pieces that stay clear of them are left out
    Box region;  // the device pixels a filled shape can be seen from, see make_cull()
} StrokeOut;

static bool
emit(StrokeOut *o, const double *pts, size_t n) {
    if (polygon_misses(pts, n, o->m, o->clip)) return true;
    return add_polygon(o->edges, pts, n, o->m, true);
}

// How many segments a circle of radius r is flattened to for an error of tol
static double
circle_segments(double r, double tol) {
    if (!(r > tol)) return 6;
    const double x = tol / r;
    // acos(1 - x) is all rounding for a tiny x, 2 asin(sqrt(x / 2)) is the same angle without the cancellation
    double n = ceil(M_PI / (x > 1e-8 ? acos(1 - x) : 2 * asin(sqrt(x / 2))));
    // Doubles hold whole numbers exactly up to 2^53, and so the angles of the points.
    // A circle that needs more is so large that its points cannot be placed to a pixel anyway.
    if (!(n <= 9007199254740992.0)) n = 9007199254740992.0;
    return fmax(n, 6);
}

static inline bool
circle_point(Poly *p, double cx, double cy, double r, double i, double n) {
    double a = 2 * M_PI * i / n;
    return poly_add(p, cx + r * cos(a), cy + r * sin(a));
}

#define CIRCLE_RUN 16  // arcs of no more segments than this are kept whole

// The points a circle is flattened to, which are n points spaced evenly
// around it, leaving out runs of them along arcs that cannot be seen. An arc
// of a quarter of the circle or less lies inside the triangle of its ends and
// the point where the tangents at its ends meet, so when that triangle misses
// the region the chord can stand for the arc, see "Flattening only what can be
// seen". Only arcs that reach the region are split, so this takes a few points
// for each halving of the arcs and the points that can be seen, however many
// n is.
static bool
circle_points(const StrokeOut *o, Poly *p, double cx, double cy, double r, double n) {
    if (!circle_point(p, cx, cy, r, 0, n)) return false;
    if (n <= 4 * CIRCLE_RUN) {
        for (double i = 1; i < n; i++)
            if (!circle_point(p, cx, cy, r, i, n)) return false;
        return true;
    }
    typedef struct {
        double i, j;  // the arc from point i to point j
    } Arc;
    // four quarters, then one more waiting for each halving of an arc of at most 2^53 points
    Arc stack[64];
    size_t top = 0;
    const double q[5] = {0, floor(n / 4), floor(n / 2), floor(3 * n / 4), n};
    for (int k = 3; k >= 0; k--) stack[top++] = (Arc){q[k], q[k + 1]};
    while (top) {
        const Arc arc = stack[--top];
        if (arc.j - arc.i > CIRCLE_RUN) {
            const double half = M_PI * (arc.j - arc.i) / n, mid = M_PI * (arc.i + arc.j) / n, reach = r / cos(half);
            const double a0 = 2 * M_PI * arc.i / n, a1 = 2 * M_PI * arc.j / n;
            Box b = empty_box;
            box_add(&b, cx + r * cos(a0), cy + r * sin(a0));
            box_add(&b, cx + r * cos(a1), cy + r * sin(a1));
            box_add(&b, cx + reach * cos(mid), cy + reach * sin(mid));
            // box_transform() gives everything for corners that overflow, which cannot be drawn either
            const Box db = box_transform(b, o->m);
            if (box_misses(db, o->region) || !isfinite(db.x0) || !isfinite(db.x1) || !isfinite(db.y0) || !isfinite(db.y1)) {
                if (arc.j < n && !circle_point(p, cx, cy, r, arc.j, n)) return false;
                continue;
            }
            const double m = floor((arc.i + arc.j) / 2);
            stack[top++] = (Arc){m, arc.j};
            stack[top++] = (Arc){arc.i, m};
            continue;
        }
        for (double k = arc.i + 1; k <= arc.j && k < n; k++)
            if (!circle_point(p, cx, cy, r, k, n)) return false;
    }
    return true;
}

static bool
emit_circle(StrokeOut *o, double cx, double cy, double r, double tol) {
    double dx, dy;
    apply(o->m, cx, cy, &dx, &dy);
    // nothing a double can draw
    if (!(r > 0) || !isfinite(r) || !isfinite(cx) || !isfinite(cy) || !isfinite(fabs(dx) + fabs(dy) + 2 * r * mat_scale(o->m))) return true;
    const double box[8] = {cx - r, cy - r, cx + r, cy - r, cx + r, cy + r, cx - r, cy + r};
    if (polygon_misses(box, 4, o->m, o->clip)) return true;
    double segments = circle_segments(r, tol);
    // vector_curve_limit 0 keeps every segment the error bound asks for.
    if (kitty_vector_curve_limit() && segments > kitty_vector_curve_limit()) segments = (double)kitty_vector_curve_limit();
    Poly poly = {0};
    bool ok = circle_points(o, &poly, cx, cy, r, segments) && emit(o, poly.pts, poly.count / 2);
    free(poly.pts);
    return ok;
}

enum { CAP_BUTT, CAP_ROUND, CAP_SQUARE };
enum { JOIN_MITER, JOIN_ROUND, JOIN_BEVEL };

static bool
emit_join(StrokeOut *o, double vx, double vy, double d0x, double d0y, double d1x, double d1y, double hw, int join, double miter_limit, double tol) {
    double cross = d0x * d1y - d0y * d1x;
    if (join == JOIN_ROUND) return emit_circle(o, vx, vy, hw, tol);
    if (fabs(cross) < 1e-9) return true;
    // outer side of the turn, where the segment quads leave a gap
    double s = cross > 0 ? -1 : 1;
    double u0x = -d0y * s, u0y = d0x * s, u1x = -d1y * s, u1y = d1x * s;
    double o0x = vx + u0x * hw, o0y = vy + u0y * hw, o1x = vx + u1x * hw, o1y = vy + u1y * hw;
    if (join == JOIN_MITER) {
        double bx = u0x + u1x, by = u0y + u1y, bl = bx * bx + by * by;
        if (bl > 1e-12 && 2 / sqrt(bl) <= miter_limit) {
            double tx = vx + bx * hw * 2 / bl, ty = vy + by * hw * 2 / bl;
            double q[8] = {vx, vy, o0x, o0y, tx, ty, o1x, o1y};
            return emit(o, q, 4);
        }
    }
    double t[6] = {vx, vy, o0x, o0y, o1x, o1y};
    return emit(o, t, 3);
}

static bool
emit_cap(StrokeOut *o, double px, double py, double dx, double dy, double hw, int cap, double tol) {
    // dx, dy points outward from the stroke
    switch (cap) {
        case CAP_ROUND: return emit_circle(o, px, py, hw, tol);
        case CAP_SQUARE: {
            double nx = -dy * hw, ny = dx * hw, ex = dx * hw, ey = dy * hw;
            double q[8] = {px + nx, py + ny, px + nx + ex, py + ny + ey, px - nx + ex, py - ny + ey, px - nx, py - ny};
            return emit(o, q, 4);
        }
    }
    return true;
}

// Removes repeated points. Returns the number of points left.
static size_t
dedupe(double *pts, size_t n) {
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        if (out && fabs(pts[2 * i] - pts[2 * out - 2]) < 1e-9 && fabs(pts[2 * i + 1] - pts[2 * out - 1]) < 1e-9) continue;
        pts[2 * out] = pts[2 * i];
        pts[2 * out + 1] = pts[2 * i + 1];
        out++;
    }
    return out;
}

// Strokes a polyline with cap0 at its start and cap1 at its end
static bool
stroke_polyline(StrokeOut *o, double *pts, size_t n, bool closed, double hw, int cap0, int cap1, int join, double miter_limit, double tol) {
    n = dedupe(pts, n);
    if (closed && n > 2 && fabs(pts[0] - pts[2 * n - 2]) < 1e-9 && fabs(pts[1] - pts[2 * n - 1]) < 1e-9) n--;
    if (n == 1) {
        // a dot, which a piece cut out of a longer dash never is
        if (cap0 != cap1 || cap0 == CAP_BUTT) return true;
        return cap0 == CAP_ROUND ? emit_circle(o, pts[0], pts[1], hw, tol) : emit_cap(o, pts[0], pts[1], 1, 0, hw, CAP_SQUARE, tol);
    }
    if (n < 2) return true;
    if (n == 2) closed = false;
    size_t nseg = closed ? n : n - 1;
    double pdx = 0, pdy = 0;
    for (size_t i = 0; i < nseg; i++) {
        size_t j = (i + 1) % n;
        double x0 = pts[2 * i], y0 = pts[2 * i + 1], x1 = pts[2 * j], y1 = pts[2 * j + 1];
        double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
        dx /= len, dy /= len;
        double nx = -dy * hw, ny = dx * hw;
        double q[8] = {x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny};
        if (!emit(o, q, 4)) return false;
        if (i > 0 && !emit_join(o, x0, y0, pdx, pdy, dx, dy, hw, join, miter_limit, tol)) return false;
        pdx = dx, pdy = dy;
    }
    if (closed) {
        double dx = pts[2] - pts[0], dy = pts[3] - pts[1], len = sqrt(dx * dx + dy * dy);
        if (!emit_join(o, pts[0], pts[1], pdx, pdy, dx / len, dy / len, hw, join, miter_limit, tol)) return false;
    } else {
        double dx = pts[0] - pts[2], dy = pts[1] - pts[3], len = sqrt(dx * dx + dy * dy);
        if (!emit_cap(o, pts[0], pts[1], dx / len, dy / len, hw, cap0, tol)) return false;
        if (!emit_cap(o, pts[2 * n - 2], pts[2 * n - 1], pdx, pdy, hw, cap1, tol)) return false;
    }
    return true;
}

// Dashes {{{

// A dash pattern, repeated: the entries of the cycle are the dash lengths, or
// those twice over for an odd number of them, and the even ones are dashes
typedef struct {
    const double *dash;
    size_t ndash, entries;
    double *prefix;   // where each entry of the cycle starts, and where the last ends: entries + 1 of them
    double cycle;     // the length of the cycle
    double shortest;  // the shortest entry that is not 0
    double coverage;  // the part of a path the dashes and their caps cover, for patterns too fine to draw
} DashPattern;

static void
dash_pattern_free(DashPattern *p) {
    free(p->prefix);
    p->prefix = NULL;
}

// False only when out of memory. A pattern with nothing to it leaves cycle at 0.
static bool
dash_pattern_init(DashPattern *p, const double *dash, size_t ndash, uint8_t cap, double hw) {
    memset(p, 0, sizeof(*p));
    if (!ndash) return true;
    if (ndash > (SIZE_MAX / sizeof(double) - 1) / 2) return false;
    p->dash = dash, p->ndash = ndash, p->entries = ndash & 1 ? 2 * ndash : ndash;
    if (!(p->prefix = malloc((p->entries + 1) * sizeof(double)))) return false;
    // How far beyond its end a cap covers, on average across the width: a
    // square cap all of hw, a round one a quarter circle's area over hw
    const double ext = cap == CAP_SQUARE ? hw : (cap == CAP_ROUND ? M_PI / 4 * hw : 0);
    double uncovered = 0;
    p->prefix[0] = 0;
    p->shortest = INFINITY;
    for (size_t k = 0; k < p->entries; k++) {
        const double len = dash[k % ndash];
        p->prefix[k + 1] = p->prefix[k] + len;
        if (k & 1) uncovered += fmax(0, len - 2 * ext);
        if (len > 0) p->shortest = fmin(p->shortest, len);
    }
    p->cycle = p->prefix[p->entries];
    // A pattern too long for a double to add up cannot repeat along any path a double can measure, and is drawn solid
    if (!(p->cycle > 0) || !isfinite(p->cycle)) p->cycle = 0;
    else p->coverage = fmin(1, fmax(0, 1 - uncovered / p->cycle));
    return true;
}

// Where along a pattern a point of a path is
typedef struct {
    size_t k;     // the entry of the cycle
    double left;  // how much of it is still to come
} DashState;

static inline double
dash_len(const DashPattern *p, size_t k) {
    return p->dash[k % p->ndash];
}

// The state at pos along the cycle, 0 <= pos < cycle. A point where an entry
// starts is in that entry, as the start of the pattern is in its first.
static void
dash_locate(const DashPattern *p, DashState *st, double pos) {
    // the first prefix at or past pos, the last being the cycle, which is past it
    size_t lo = 0, hi = p->entries;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (p->prefix[mid] < pos) lo = mid + 1;
        else hi = mid;
    }
    if (p->prefix[lo] == pos) *st = lo < p->entries ? (DashState){lo, dash_len(p, lo)} : (DashState){0, dash_len(p, 0)};
    else *st = (DashState){lo - 1, p->prefix[lo] - pos};
}

// Moves st on by dist along the path, in time that does not depend on how many dashes that passes
static void
dash_advance(const DashPattern *p, DashState *st, double dist) {
    if (!(dist > 0)) return;
    if (dist < st->left) {
        st->left -= dist;
        return;
    }
    double pos = p->prefix[st->k + 1] - st->left + dist;
    if (pos >= p->cycle) pos = fmod(pos, p->cycle);
    dash_locate(p, st, pos);
}

typedef struct {
    const DashPattern *pat;
    double hw, ext;   // the half width, and how far a cap reaches past the end of a dash
    Mat m;            // to device pixels
    bool cull;        // when the transform can be undone, the corners of the region pieces can be seen from, in path coordinates
    double wx[4], wy[4];
    Box join_region;  // device pixels a join can be seen from
    PolyList *out;    // the dashes
    PolyList *fine;   // pieces of the path along which the pattern is finer than the rasterizer samples
} Dasher;

// A point of the segment from x0, y0 to x1, y1 of length len, s along it
static inline void
seg_point(double x0, double y0, double x1, double y1, double len, double s, double *x, double *y) {
    const double t = s / len;
    *x = x0 + (x1 - x0) * t;
    *y = y0 + (y1 - y0) * t;
}

static Poly *
piece_new(PolyList *list, double x, double y, bool cut) {
    Poly *p = polylist_new(list);
    if (!p || !poly_add(p, x, y)) return NULL;
    p->cut_start = cut;
    return p;
}

static bool
piece_end(Poly *p, double x, double y, bool cut) {
    if (!poly_add(p, x, y)) return false;
    p->cut_end = cut;
    return true;
}

// Splits a polyline into the "on" pieces of the dash pattern.
//
// A dash is cut only where it can be seen. For each segment the corners of the
// visible region are projected onto it: along the segment they give the stretch
// [a, b] the region lies beside, and across it whether the region comes within
// the half width at all. Only the dashes over [a, b] are cut one by one, and the
// pattern is moved through the rest of the segment by dash_advance(), so a
// segment costs the dashes beside the region whatever its length. Outside
// [a, b] what can be seen is a cap reaching back into it, which can only be
// that of the last dash to end before a or of the first to start after b, as
// the caps of the others reach less far into it; and a join at a corner in the
// middle of a dash, which reaches no further than stroke_reach() from the
// corner. Pieces cut at a or b, or out of a dash around a corner, end there
// without a cap, so they draw no more than the dash they are a part of.
//
// A pattern that repeats more than SUBSAMPLES times a pixel along the segment
// cannot be sampled by the rasterizer: the dashes of a vertical line would be
// point sampled, aliasing to whatever happens to fall on the scanlines. That
// stretch is drawn whole into fine instead, which the caller fills at
// pat->coverage, the part of it the dashes cover. That is what the dashes
// average to over any pixel, to within the one period of the pattern that a
// pixel can see partly, less than 1/SUBSAMPLES of it. It also keeps the number of
// dashes cut at SUBSAMPLES a pixel, with the region bounded. The same goes for a
// segment so long that positions along it are further apart than the shortest
// dash, where stepping from dash to dash would not get anywhere.
static bool
dash_polyline(const Dasher *ds, const Poly *p, double phase) {
    const DashPattern *pat = ds->pat;
    const size_t n = p->count / 2;
    if (n < 2) {
        Poly *q = polylist_new(ds->out);
        if (!q) return false;
        q->closed = p->closed;
        for (size_t i = 0; i < n; i++)
            if (!poly_add(q, p->pts[2 * i], p->pts[2 * i + 1])) return false;
        return true;
    }
    DashState st = {0, dash_len(pat, 0)};
    double start = fmod(phase, pat->cycle);
    if (start < 0) start += pat->cycle;
    if (start > 0 && start < pat->cycle) dash_locate(pat, &st, start);
    Poly *cur = NULL;   // the piece that runs on into the next segment
    bool cur_fine = false;
    const size_t nseg = p->closed ? n : n - 1;
    double pdx = 0, pdy = 0, plen = 0;  // the direction and length of the segment before
    for (size_t i = 0; i < nseg; i++) {
        const size_t j = (i + 1) % n;
        const double x0 = p->pts[2 * i], y0 = p->pts[2 * i + 1], x1 = p->pts[2 * j], y1 = p->pts[2 * j + 1];
        const double len = hypot(x1 - x0, y1 - y0);
        if (!isfinite(len)) {  // a segment whose length overflows cannot be measured along
            if (cur && !piece_end(cur, x0, y0, true)) return false;
            cur = NULL;
            plen = 0;
            continue;
        }
        if (len == 0) {
            if (cur && !poly_add(cur, x1, y1)) return false;
            continue;
        }
        const double ux = (x1 - x0) / len, uy = (y1 - y0) / len;
        double a = 0, b = len;
        bool near = true;  // whether the region comes within the half width of the segment's line
        if (ds->cull) {
            double cmin = INFINITY, cmax = -INFINITY;
            a = INFINITY, b = -INFINITY;
            for (int k = 0; k < 4; k++) {
                const double wx = ds->wx[k] - x0, wy = ds->wy[k] - y0, along = wx * ux + wy * uy, across = wy * ux - wx * uy;
                a = fmin(a, along), b = fmax(b, along), cmin = fmin(cmin, across), cmax = fmax(cmax, across);
            }
            near = !(cmax < -ds->hw || cmin > ds->hw);
        }
        const bool last = i + 1 == nseg, first = i == 0;
        // the device length of a unit of the path along this segment
        const double dev = hypot(ds->m.a * ux + ds->m.c * uy, ds->m.b * ux + ds->m.d * uy);
        // Along a segment so long that adding the shortest entry to a position along it changes nothing, the
        // pattern cannot be followed in doubles at all, and is drawn as one finer than the samples is
        const bool fine = near && (pat->cycle * dev < 1.0 / SUBSAMPLES || !(len + pat->shortest > len));
        const double r0 = fmin(len, fmax(0, a)), r1 = fmin(len, fmax(0, b));
        const bool over = near && a < len && b > 0;  // whether some of the segment is beside the region
        const bool on_at_start = !(st.k & 1), spans_start = on_at_start && st.left > 0 && dash_len(pat, st.k) - st.left > 0;
        // Carry the piece of the last segment on if this one goes on with it
        const bool carry = cur && over && r0 == 0 && fine == cur_fine;
        if (cur && !carry) {
            if (!piece_end(cur, x0, y0, cur_fine || st.left > 0)) return false;
            cur = NULL;
        }
        // A join around this corner in the middle of a dash that no piece goes on through
        if (!carry && !first && plen > 0 && spans_start && !fine) {
            double vx, vy;
            apply(ds->m, x0, y0, &vx, &vy);
            if (!(vx < ds->join_region.x0 || vx > ds->join_region.x1 || vy < ds->join_region.y0 || vy > ds->join_region.y1)) {
                const double back = fmin(dash_len(pat, st.k) - st.left, plen) / 2, ahead = fmin(st.left, len) / 2;
                Poly *q = piece_new(ds->out, x0 - pdx * back, y0 - pdy * back, true);
                if (!q || !poly_add(q, x0, y0) || !piece_end(q, x0 + ux * ahead, y0 + uy * ahead, true)) return false;
            }
        }
        pdx = ux, pdy = uy, plen = len;
        if (fine) {
            if (over) {
                double x, y;
                if (!carry) {
                    seg_point(x0, y0, x1, y1, len, r0, &x, &y);
                    if (!(cur = piece_new(ds->fine, x, y, true))) return false;
                    cur_fine = true;
                }
                seg_point(x0, y0, x1, y1, len, r1, &x, &y);
                if (!poly_add(cur, x, y)) return false;
                if (r1 < len) {
                    cur->cut_end = true;
                    cur = NULL;
                }
            }
            dash_advance(pat, &st, len);
            continue;
        }
        if (!near) {  // nothing but a join at the end of the segment can be seen
            dash_advance(pat, &st, len);
            continue;
        }
        double pos = 0, x, y;
        // The cap of the last dash to end before the region
        if (ds->ext > 0 && a > 0 && r0 > 0) {
            dash_advance(pat, &st, r0 - pos);
            pos = r0;
            const bool off = st.k & 1;
            if (off || st.left == 0) {
                const double s_end = off ? r0 - (dash_len(pat, st.k) - st.left) : r0;
                const double dlen = dash_len(pat, off ? (st.k + pat->entries - 1) % pat->entries : st.k);
                if (s_end >= 0 && s_end >= a - ds->ext) {
                    const double s_start = s_end - dlen;
                    seg_point(x0, y0, x1, y1, len, fmax(0, s_start), &x, &y);
                    Poly *q = piece_new(ds->out, x, y, s_start < 0 && !first);
                    seg_point(x0, y0, x1, y1, len, s_end, &x, &y);
                    if (!q || !piece_end(q, x, y, false)) return false;
                }
            }
        }
        // The dashes beside the region
        if (over) {
            dash_advance(pat, &st, r0 - pos);
            pos = r0;
            bool on = !(st.k & 1);
            if (on && !carry) {
                seg_point(x0, y0, x1, y1, len, r0, &x, &y);
                // the start of the path is the start of a dash, any other point but the start of a dash is a cut
                if (!(cur = piece_new(ds->out, x, y, !(first && r0 == 0) && dash_len(pat, st.k) - st.left > 0))) return false;
                cur_fine = false;
            }
            while (r1 - pos > st.left) {
                pos += st.left;
                seg_point(x0, y0, x1, y1, len, pos, &x, &y);
                if (on) {
                    if (!piece_end(cur, x, y, false)) return false;
                    cur = NULL;
                } else {
                    if (!(cur = piece_new(ds->out, x, y, false))) return false;
                    cur_fine = false;
                }
                on = !on;
                st.k = (st.k + 1) % pat->entries;
                st.left = dash_len(pat, st.k);
            }
            st.left -= r1 - pos;
            pos = r1;
            if (on) {
                seg_point(x0, y0, x1, y1, len, r1, &x, &y);
                if (r1 == len && !last) {
                    if (!poly_add(cur, x, y)) return false;  // runs on into the next segment
                } else {
                    // where the path ends the dash ends, anywhere else it is cut if it goes on
                    if (!piece_end(cur, x, y, st.left > 0 && !(last && r1 == len))) return false;
                    cur = NULL;
                }
            }
        }
        // The cap of the first dash to start after the region
        if (ds->ext > 0 && b < len && r1 < len) {
            dash_advance(pat, &st, r1 - pos);
            pos = r1;
            const bool on = !(st.k & 1);
            if (!on || st.left == dash_len(pat, st.k)) {
                const double s_start = on ? r1 : r1 + st.left;
                const double dlen = dash_len(pat, on ? st.k : (st.k + 1) % pat->entries);
                if (s_start <= len && s_start <= b + ds->ext) {
                    const double s_end = s_start + dlen;
                    seg_point(x0, y0, x1, y1, len, s_start, &x, &y);
                    Poly *q = piece_new(ds->out, x, y, false);
                    seg_point(x0, y0, x1, y1, len, fmin(len, s_end), &x, &y);
                    if (!q || !piece_end(q, x, y, s_end > len && !last)) return false;
                }
            }
        }
        dash_advance(pat, &st, len - pos);
    }
    if (cur) cur->cut_end = cur_fine;  // the end of the path ends the last dash
    return true;
}

// }}}

// }}}

// Elements {{{

enum { PAINT_NONE, PAINT_SOLID, PAINT_GRADIENT };
typedef struct {
    uint8_t type;
    float r, g, b, a; // straight alpha, linear extended sRGB
    uint32_t gradient;
} Paint;
typedef struct {
    double offset;
    float r, g, b, a;
} Stop;
enum { BLEND_NORMAL, BLEND_MULTIPLY, BLEND_SCREEN };

// Where flatten_path() is after some of the commands of a path
typedef struct {
    bool open;
    double cx, cy, sx, sy;
} PathCursor;

typedef struct Elem {
    uint32_t id, parent, ref;  // ref: clip path of a group, symbol of a use, image of an image
    char type;
    bool has_t, visible, has_focus, in_progress;
    bool has_depth;
    double depth;
    bool depth_in_pixels;
    bool has_phase;
    double phase;
    Mat t;
    Path path;   // p and s
    double *pts; // k: x, y, width triples
    size_t npts, pts_capacity;
    Paint fill, stroke;
    double width, miter_limit, dash_phase;
    double *dash;
    size_t ndash;
    uint8_t rule, cap, join, opacity, blend;
    double g[5];  // l: x1 y1 x2 y2, r: cx cy r fx fy
    Stop *stops;
    size_t nstops;
    size_t slot, sorted_slot, bytes;
    size_t child_slot;  // where it is in the list of the children of its parent
    // The box of the geometry in the element's own coordinate system, worked
    // out when first asked for and kept up as geometry is appended
    Box box;
    bool has_box;
    PathCursor cursor;  // where the path has got to
    struct Elem **runs;
    size_t nruns;
    PyObject *text;
    bool has_clip_box;
    Box clip_box;
} Elem;

#define NAME elem_map
#define KEY_TY uint32_t
#define VAL_TY Elem *
#include "kitty-verstable.h"

typedef struct {
    Elem **item;
    size_t count, capacity;
} ChildList;

#define NAME child_map
#define KEY_TY uint32_t
#define VAL_TY ChildList
#include "kitty-verstable.h"

#define NAME count_map
#define KEY_TY uint32_t
#define VAL_TY size_t
#include "kitty-verstable.h"

struct VectorDoc {
    unsigned width, height;
    elem_map by_id;
    Elem **order;  // creation order, NULL where deleted
    size_t count, capacity, live;
    size_t used_bytes;
    Elem root;
    Elem **sorted;  // the elements that are drawn, by parent then slot, rebuilt when an element comes or goes
    size_t nsorted;
    bool is_sorted;
    unsigned clip_groups;  // the groups other than the root that have a clip path
    count_map clip_refs;   // how many of them clip to each id
    count_map image_refs;  // how many i records draw each image id, for vector_doc_image_changed()
    // The elements with each parent id, whether or not that is an element, so
    // that deleting a group takes time in proportion to what it holds
    child_map children;
    // How many more parent links working out damage may follow in this
    // vector_doc_apply(), see damage_under()
    size_t damage_steps;
    // What changed since the damage was last cleared: everything, or the
    // part of the image inside the box, which is in the image's user units
    bool damage_all;
    Box damage;
    PyObject *font_session;
};

bool
vector_doc_has_depth(const VectorDoc *d) {
    if (d->root.has_depth || d->root.has_phase) return true;
    for (size_t i = 0; i < d->count; i++) if (d->order[i] && (d->order[i]->has_depth || d->order[i]->has_phase)) return true;
    return false;
}

static void
elem_free(Elem *e) {
    if (!e) return;
    free(e->path.item);
    free(e->pts);
    free(e->dash);
    free(e->stops);
    for (size_t i = 0; i < e->nruns; i++) elem_free(e->runs[i]);
    free(e->runs);
    Py_XDECREF(e->text);
    free(e);
}

static Elem *
elem_new(void) {
    Elem *e = calloc(1, sizeof(Elem));
    if (!e) return NULL;
    e->t = identity;
    e->visible = true;
    e->width = 1;
    e->miter_limit = 4;
    e->opacity = 100;
    e->fill.type = PAINT_NONE;
    e->stroke.type = PAINT_NONE;
    e->sorted_slot = SIZE_MAX;
    return e;
}

VectorDoc *
vector_doc_new(unsigned width, unsigned height) {
    VectorDoc *d = calloc(1, sizeof(VectorDoc));
    if (!d) return NULL;
    d->width = width;
    d->height = height;
    vt_init(&d->by_id);
    vt_init(&d->clip_refs);
    vt_init(&d->image_refs);
    vt_init(&d->children);
    d->root.t = identity;
    d->root.visible = true;
    d->root.opacity = 100;
    d->root.type = 'g';
    d->damage_all = true;  // nothing of it has been drawn yet
    d->damage = empty_box;
    return d;
}

void
vector_doc_free(VectorDoc *d) {
    if (!d) return;
    for (size_t i = 0; i < d->count; i++) elem_free(d->order[i]);
    free(d->order);
    free(d->sorted);
    vt_cleanup(&d->by_id);
    Py_XDECREF(d->font_session);
    vt_create_for_loop(child_map_itr, it, &d->children) free(it.data->val.item);
    vt_cleanup(&d->children);
    vt_cleanup(&d->clip_refs);
    vt_cleanup(&d->image_refs);
    free(d);
}

void
vector_doc_set_font_session(VectorDoc *d, PyObject *session) {
    Py_XSETREF(d->font_session, Py_XNewRef(session));
}

size_t
vector_doc_size(const VectorDoc *d) {
    return d->used_bytes;
}

unsigned
vector_doc_element_count(const VectorDoc *d) {
    return (unsigned)d->live;
}

static Elem *
lookup(const VectorDoc *d, uint32_t id) {
    elem_map_itr i = vt_get((elem_map *)&d->by_id, id);
    return vt_is_end(i) ? NULL : i.data->val;
}

static inline bool
is_clip_group(const Elem *e) {
    return e->type == 'g' && e->ref;
}

static bool
count_inc(count_map *m, uint32_t key) {
    count_map_itr it = vt_get(m, key);
    if (vt_is_end(it)) return !vt_is_end(vt_insert(m, key, 1));
    it.data->val++;
    return true;
}

static void
count_dec(count_map *m, uint32_t key) {
    count_map_itr it = vt_get(m, key);
    if (!vt_is_end(it) && !--it.data->val) vt_erase_itr(m, it);
}

static bool
children_add(VectorDoc *d, Elem *e) {
    child_map_itr it = vt_get(&d->children, e->parent);
    if (vt_is_end(it) && vt_is_end(it = vt_insert(&d->children, e->parent, (ChildList){0}))) return false;
    ChildList *cl = &it.data->val;
    if (cl->count == cl->capacity) {
        void *np = cl->count == SIZE_MAX ? NULL : reserve_array(cl->item, &cl->capacity, cl->count + 1, sizeof(Elem *), 4);
        if (!np) {
            if (!cl->count) vt_erase_itr(&d->children, it);
            return false;
        }
        cl->item = np;
    }
    e->child_slot = cl->count;
    cl->item[cl->count++] = e;
    return true;
}

static void
children_remove(VectorDoc *d, Elem *e) {
    child_map_itr it = vt_get(&d->children, e->parent);
    if (vt_is_end(it)) return;
    ChildList *cl = &it.data->val;
    Elem *last = cl->item[--cl->count];
    cl->item[e->child_slot] = last;
    last->child_slot = e->child_slot;
    if (!cl->count) {
        free(cl->item);
        vt_erase_itr(&d->children, it);
    }
}

static void
remove_elem(VectorDoc *d, Elem *e) {
    vt_erase(&d->by_id, e->id);
    children_remove(d, e);
    d->order[e->slot] = NULL;
    d->used_bytes -= e->bytes;
    d->live--;
    d->is_sorted = false;
    if (is_clip_group(e)) {
        d->clip_groups--;
        count_dec(&d->clip_refs, e->ref);
    }
    if (e->type == 'i') count_dec(&d->image_refs, e->ref);
    elem_free(e);
}

static void
compact(VectorDoc *d) {
    if (d->count < 64 || d->live * 2 > d->count) return;
    size_t out = 0;
    for (size_t i = 0; i < d->count; i++) {
        if (!d->order[i]) continue;
        d->order[i]->slot = out;
        d->order[out++] = d->order[i];
    }
    d->count = out;
}

static bool
has_render_order(const Elem *e) {
    return e->type != 's' && e->type != 'l' && e->type != 'r';
}

// Puts e into the document, replacing any element with the same id in place.
// What can fail is done first, so that a failure leaves the document as it was.
static bool
insert_elem(VectorDoc *d, Elem *e) {
    elem_map_itr existing = vt_get(&d->by_id, e->id);
    Elem *old = vt_is_end(existing) ? NULL : existing.data->val;
    const bool same_parent = old && old->parent == e->parent;
    const bool keep_sorted = d->is_sorted && same_parent && has_render_order(old) == has_render_order(e);
    if (is_clip_group(e) && !count_inc(&d->clip_refs, e->ref)) return false;
    if (e->type == 'i' && !count_inc(&d->image_refs, e->ref)) goto undo_clip;
    if (!same_parent && !children_add(d, e)) goto undo_image;
    if (old) {
        if (same_parent) {
            e->child_slot = old->child_slot;
            vt_get(&d->children, e->parent).data->val.item[e->child_slot] = e;
        } else children_remove(d, old);
        e->slot = old->slot;
        if (keep_sorted && has_render_order(e)) {
            e->sorted_slot = old->sorted_slot;
            d->sorted[e->sorted_slot] = e;
        }
        d->order[e->slot] = e;
        existing.data->val = e;
        d->used_bytes -= old->bytes;
        if (is_clip_group(old)) {
            d->clip_groups--;
            count_dec(&d->clip_refs, old->ref);
        }
        if (old->type == 'i') count_dec(&d->image_refs, old->ref);
        elem_free(old);
        d->live--;
    } else {
        if (d->count == SIZE_MAX) goto undo_children;
        if (d->count + 1 > d->capacity) {
            void *np = reserve_array(d->order, &d->capacity, d->count + 1, sizeof(Elem *), 64);
            if (!np) goto undo_children;
            d->order = np;
        }
        if (vt_is_end(vt_insert(&d->by_id, e->id, e))) goto undo_children;
        e->slot = d->count;
        d->order[d->count++] = e;
    }
    if (!keep_sorted) d->is_sorted = false;
    d->used_bytes += e->bytes;
    d->live++;
    if (is_clip_group(e)) d->clip_groups++;
    return true;
undo_children:
    children_remove(d, e);
undo_image:
    if (e->type == 'i') count_dec(&d->image_refs, e->ref);
undo_clip:
    if (is_clip_group(e)) count_dec(&d->clip_refs, e->ref);
    return false;
}

// Deletes gid and everything under it: what has it as parent, and what is
// under any group among those. Walking down from gid through the lists of
// children takes time in proportion to what is deleted, where looking up from
// every element of the document would take that for each delete.
static bool
delete_group(VectorDoc *d, uint32_t gid) {
    Elem **found = NULL;
    uint32_t *todo = NULL;
    size_t nfound = 0, found_cap = 0, ntodo = 0, todo_cap = 0;
    bool ok = true;
#define PUSH(arr, n, cap, val)                                                     \
    if (n == cap) {                                                                \
        void *np = reserve_array(arr, &cap, n + 1, sizeof(*arr), 16);             \
        if (!np) {                                                                 \
            ok = false;                                                            \
            break;                                                                 \
        }                                                                          \
        arr = np;                                                                  \
    }                                                                              \
    arr[n++] = val;
    do {
        Elem *g = lookup(d, gid);
        if (g) {
            PUSH(found, nfound, found_cap, g);
            g->in_progress = true;
        }
        PUSH(todo, ntodo, todo_cap, gid);
    } while (0);
    while (ok && ntodo) {
        child_map_itr it = vt_get(&d->children, todo[--ntodo]);
        if (vt_is_end(it)) continue;
        const ChildList *cl = &it.data->val;
        for (size_t i = 0; i < cl->count; i++) {
            Elem *c = cl->item[i];
            if (c->in_progress) continue;  // a cycle of groups back to gid
            PUSH(found, nfound, found_cap, c);
            c->in_progress = true;
            if (c->type == 'g') {
                PUSH(todo, ntodo, todo_cap, c->id);
            }
        }
    }
#undef PUSH
    for (size_t i = 0; i < nfound; i++) {
        if (ok) remove_elem(d, found[i]);
        else found[i]->in_progress = false;
    }
    free(found);
    free(todo);
    return ok;
}

// }}}

// Extent and damage {{{

// Adds to box every point flatten_path() makes of the commands from the given
// one on. A curve stays inside its control points, so they stand for it.
static void
path_box(const Path *path, size_t from, PathCursor *pc, Box *box) {
    for (size_t i = from; i < path->count; i++) {
        const PathCmd *c = path->item + i;
        unsigned n = 0;
        switch (c->op) {
            case PC_MOVE:
                box_add(box, c->p[0], c->p[1]);
                pc->cx = pc->sx = c->p[0];
                pc->cy = pc->sy = c->p[1];
                pc->open = true;
                continue;
            case PC_CLOSE:
                if (!pc->open) continue;
                box_add(box, pc->sx, pc->sy);  // the closing segment runs to the start of the subpath
                pc->cx = pc->sx;
                pc->cy = pc->sy;
                pc->open = false;
                continue;
            case PC_LINE: n = 1; break;
            case PC_QUAD: n = 2; break;
            case PC_CUBIC: n = 3; break;
            default: continue;
        }
        if (!pc->open) {  // a subpath that starts where the last one left off
            box_add(box, pc->cx, pc->cy);
            pc->open = true;
        }
        for (unsigned k = 0; k < n; k++) box_add(box, c->p[2 * k], c->p[2 * k + 1]);
        pc->cx = c->p[2 * n - 2];
        pc->cy = c->p[2 * n - 1];
    }
}

static inline void
pen_box(const Elem *e, size_t from, Box *box) {
    for (size_t i = from; i < e->npts; i++) {
        const double x = e->pts[3 * i], y = e->pts[3 * i + 1], r = fabs(e->pts[3 * i + 2]) / 2;
        box_add(box, x - r, y - r);
        box_add(box, x + r, y + r);
    }
}

// The box of the geometry of an element, in its own coordinate system and
// without the width of a stroke along a path
static Box
elem_box(Elem *e) {
    if (!e->has_box) {
        e->box = empty_box;
        e->cursor = (PathCursor){0};
        path_box(&e->path, 0, &e->cursor, &e->box);
        pen_box(e, 0, &e->box);
        for (size_t i = 0; i < e->nruns; i++) box_union(&e->box, box_grow(elem_box(e->runs[i]), e->runs[i]->stroke.type == PAINT_NONE ? 0 : e->runs[i]->width / 2));
        e->has_box = true;
    }
    return e->box;
}

// How far the stroke of a path drawn with this style reaches from the path
static double
stroke_reach(const Elem *style) {
    if (style->stroke.type == PAINT_NONE || !(style->width > 0)) return 0;
    double k = 1;
    if (style->join == JOIN_MITER) k = fmax(k, style->miter_limit);
    if (style->cap == CAP_SQUARE) k = fmax(k, 1.4143);  // the corner of a square cap
    return style->width / 2 * k;
}

// The transform from the coordinate system of e to that of the image. Returns
// 0 when e hangs off something that is not a group, so that it is not drawn,
// and -1 when the parent chain contains a cycle or is longer than *steps, when
// steps is given, which is then reduced by the length of the chain.
static int
elem_matrix_within(const VectorDoc *d, const Elem *e, Mat *out, size_t *steps) {
    Mat m = e->has_t ? e->t : identity;
    uint32_t p = e->parent;
    size_t remaining = d->live, depth = 0;
    const unsigned long depth_limit = kitty_vector_depth_limit();
    while (p) {
        if (!remaining--) return -1; // a cycle in the parent chain
        if (depth_limit && ++depth > depth_limit) return -1;
        if (steps) {
            if (!*steps) return -1;
            --*steps;
        }
        const Elem *g = lookup(d, p);
        if (!g || g->type != 'g') return 0;
        if (g->has_t) m = mat_mul(g->t, m);
        p = g->parent;
    }
    *out = d->root.has_t ? mat_mul(d->root.t, m) : m;
    return 1;
}

// unused here, but branches built on this one call it
__attribute__((unused)) static int
elem_matrix(const VectorDoc *d, const Elem *e, Mat *out) {
    return elem_matrix_within(d, e, out, NULL);
}

static bool
is_clip_path(const VectorDoc *d, uint32_t id) {
    if (d->root.ref == id) return true;
    return d->clip_groups && !vt_is_end(vt_get((count_map *)&d->clip_refs, id));
}

// Damages the part of the image under box, which is in the coordinate system of
// e. Finding where that is follows the parents of e, which in a deep tree of
// groups takes as long for each record as drawing all of the image does. So
// the links followed for a vector_doc_apply() are limited to a few for each
// element and record, past which the whole image is damaged instead: drawing
// all of it again costs no more than following them would have.
static void
damage_under(VectorDoc *d, const Elem *e, Box box) {
    if (d->damage_all || box_is_empty(box)) return;
    Mat m;
    switch (elem_matrix_within(d, e, &m, &d->damage_steps)) {
        case 0: return;
        case 1: break;
        default:
            d->damage_all = true;
            return;
    }
    box = box_transform(box, m);
    if (!isfinite(box.x0) || !isfinite(box.y0) || !isfinite(box.x1) || !isfinite(box.y1)) d->damage_all = true;
    else box_union(&d->damage, box);
}

// Damages everything e draws, when that can be bounded, and the whole image
// when it cannot: an element that others refer to, a group, a raster image.
static void
damage_elem(VectorDoc *d, Elem *e) {
    if (d->damage_all) return;
    Box box;
    switch (e->type) {
        case 'p':
            if (is_clip_path(d, e->id)) goto all;
            box = box_grow(elem_box(e), stroke_reach(e));
            break;
        case 'k': case 't': box = elem_box(e); break;
        case 'u': {
            Elem *sym = lookup(d, e->ref);
            if (!sym || sym->type != 's') return;  // draws nothing
            box = box_grow(elem_box(sym), stroke_reach(e));
        } break;
        default: goto all;
    }
    damage_under(d, e, box);
    return;
all:
    d->damage_all = true;
}

// Damages what appending geometry to e changed, given how much geometry it
// had and where its path was before
static void
damage_appended(VectorDoc *d, Elem *e, size_t ncmds, size_t npts, PathCursor before) {
    Box added = empty_box;
    if (e->type == 'k') {
        // the last of the old points too: the piece to the first new point starts
        // there, and it may no longer be drawn as the end of the stroke
        pen_box(e, npts ? npts - 1 : 0, &added);
        box_union(&e->box, added);
        damage_under(d, e, added);
        return;
    }
    // the new pieces run on from where the path was, which is where the old
    // end cap sat and where the join to the first of them now is
    if (ncmds) box_add(&added, before.cx, before.cy);
    path_box(&e->path, ncmds, &e->cursor, &added);
    box_union(&e->box, added);
    // a fill is closed across the ends of its subpaths, so more of it changes than was added
    if (e->type == 'p' && e->fill.type == PAINT_NONE && !is_clip_path(d, e->id)) damage_under(d, e, box_grow(added, stroke_reach(e)));
    else damage_elem(d, e);
}

// Takes a point of the image, in its user units, into the coordinate system of
// a group, the one the geometry of the elements in that group is given in.
// False when there is no such group or its transforms cannot be undone.
bool
vector_doc_point_to_group(const VectorDoc *d, uint32_t group, double *x, double *y) {
    Mat m = d->root.has_t ? d->root.t : identity, inv;
    if (group) {
        const Elem *g = lookup(d, group);
        if (!g || g->type != 'g' || elem_matrix(d, g, &m) != 1) return false;
    }
    if (!mat_invert(m, &inv)) return false;
    apply(inv, *x, *y, x, y);
    return isfinite(*x) && isfinite(*y);
}

// }}}

// Record parsing {{{

static const char *parse_error;

#define FAIL(msg)              \
    {                          \
        parse_error = (msg);   \
        return false;          \
    }

static bool
parse_num(const char **s, const char *end, double *out) {
    const char *p = *s;
    while (p < end && *p == ',') p++;
    if (p >= end) return false;
    const char *start = p;
    while (p < end && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) {
        // a sign starts a new number unless it follows an exponent
        if (p > start && (*p == '-' || *p == '+') && p[-1] != 'e' && p[-1] != 'E') break;
        p++;
    }
    const size_t n = p - start;
    if (!n || n == SIZE_MAX) return false;
    char *buf = malloc(n + 1);
    if (!buf) {
        parse_error = "Out of memory";
        return false;
    }
    memcpy(buf, start, n);
    buf[n] = 0;
    char *e;
    *out = strtod(buf, &e);
    const bool ok = !*e && isfinite(*out);
    free(buf);
    if (!ok) return false;
    *s = p;
    return true;
}

static bool
parse_nums(const char **s, const char *end, double *out, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (!parse_num(s, end, out + i)) return false;
    return true;
}

static bool
parse_path(const char *s, const char *end, Path *path) {
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;  // current point, subpath start, last control point
    char cmd = 0, last = 0;
    while (s < end) {
        if (*s == ',') {
            s++;
            continue;
        }
        if (isalpha((unsigned char)*s)) cmd = *s++;
        else if (!cmd) FAIL("Number without a path command");
        bool rel = islower((unsigned char)cmd);
        double v[6];
        switch (toupper((unsigned char)cmd)) {
            case 'M':
                if (!parse_nums(&s, end, v, 2)) FAIL("Bad moveto in path");
                if (rel) v[0] += cx, v[1] += cy;
                cx = sx = v[0], cy = sy = v[1];
                if (!path_add(path, PC_MOVE, v, 2)) FAIL("Out of memory");
                cmd = rel ? 'l' : 'L';  // further pairs are implicit linetos
                break;
            case 'L':
                if (!parse_nums(&s, end, v, 2)) FAIL("Bad lineto in path");
                if (rel) v[0] += cx, v[1] += cy;
                cx = v[0], cy = v[1];
                if (!path_add(path, PC_LINE, v, 2)) FAIL("Out of memory");
                break;
            case 'H':
                if (!parse_nums(&s, end, v, 1)) FAIL("Bad lineto in path");
                v[0] += rel ? cx : 0, v[1] = cy;
                cx = v[0];
                if (!path_add(path, PC_LINE, v, 2)) FAIL("Out of memory");
                break;
            case 'V':
                if (!parse_nums(&s, end, v + 1, 1)) FAIL("Bad lineto in path");
                v[0] = cx, v[1] += rel ? cy : 0;
                cy = v[1];
                if (!path_add(path, PC_LINE, v, 2)) FAIL("Out of memory");
                break;
            case 'C':
                if (!parse_nums(&s, end, v, 6)) FAIL("Bad curveto in path");
                if (rel)
                    for (unsigned i = 0; i < 6; i += 2) v[i] += cx, v[i + 1] += cy;
                lcx = v[2], lcy = v[3], cx = v[4], cy = v[5];
                if (!path_add(path, PC_CUBIC, v, 6)) FAIL("Out of memory");
                break;
            case 'S':
                if (!parse_nums(&s, end, v + 2, 4)) FAIL("Bad curveto in path");
                if (rel)
                    for (unsigned i = 2; i < 6; i += 2) v[i] += cx, v[i + 1] += cy;
                if (last == 'C' || last == 'S') v[0] = 2 * cx - lcx, v[1] = 2 * cy - lcy;
                else v[0] = cx, v[1] = cy;
                lcx = v[2], lcy = v[3], cx = v[4], cy = v[5];
                if (!path_add(path, PC_CUBIC, v, 6)) FAIL("Out of memory");
                break;
            case 'Q':
                if (!parse_nums(&s, end, v, 4)) FAIL("Bad curveto in path");
                if (rel)
                    for (unsigned i = 0; i < 4; i += 2) v[i] += cx, v[i + 1] += cy;
                lcx = v[0], lcy = v[1], cx = v[2], cy = v[3];
                if (!path_add(path, PC_QUAD, v, 4)) FAIL("Out of memory");
                break;
            case 'T':
                if (!parse_nums(&s, end, v + 2, 2)) FAIL("Bad curveto in path");
                if (rel) v[2] += cx, v[3] += cy;
                if (last == 'Q' || last == 'T') v[0] = 2 * cx - lcx, v[1] = 2 * cy - lcy;
                else v[0] = cx, v[1] = cy;
                lcx = v[0], lcy = v[1], cx = v[2], cy = v[3];
                if (!path_add(path, PC_QUAD, v, 4)) FAIL("Out of memory");
                break;
            case 'Z':
                cx = sx, cy = sy;
                if (!path_add(path, PC_CLOSE, NULL, 0)) FAIL("Out of memory");
                cmd = 0;  // a number after a closepath would repeat it forever
                break;
            default: FAIL("Unknown path command");
        }
        last = toupper((unsigned char)cmd);
    }
    return true;
}

static bool
parse_color(const char *s, const char *end, Paint *p) {
    size_t n = end - s;
    if (n == 4 && !memcmp(s, "none", 4)) {
        p->type = PAINT_NONE;
        return true;
    }
    if (n > 1 && *s == '@') {
        double id;
        s++;
        if (!parse_num(&s, end, &id) || s != end || id < 0 || id > UINT32_MAX || id != floor(id)) FAIL("Bad gradient reference");
        p->type = PAINT_GRADIENT;
        p->gradient = (uint32_t)id;
        return true;
    }
    const char *star = memchr(s, '*', n);
    const char *color_end = star ? star : end;
    float channels[4] = {0, 0, 0, 1};
    const char *colon = memchr(s, ':', color_end - s);
    if (colon) {
        unsigned gamut = 0;
        bool linear = false;
        size_t prefix = colon-s;
        if (prefix == 4 && !memcmp(s, "srgb", 4)) {}
        else if (prefix == 2 && !memcmp(s, "p3", 2)) gamut = 1;
        else if (prefix == 7 && !memcmp(s, "rec2020", 7)) gamut = 2;
        else if (prefix == 6 && !memcmp(s, "linear", 6)) linear = true;
        else FAIL("Unknown color space");
        const char *q = colon+1;
        unsigned count = 0;
        while (q < color_end) {
            double v;
            if (count == 4 || !parse_num(&q, color_end, &v)) FAIL("Bad color components");
            if ((!linear || count == 3) && (v < 0 || v > 1)) FAIL("Color component outside 0..1");
            channels[count++] = (float)v;
            if (q < color_end) {
                if (*q++ != '/' || q == color_end) FAIL("Bad color components");
            }
        }
        if (count < 3) FAIL("Color needs three or four components");
        if (!linear) for (unsigned i = 0; i < 3; i++) channels[i] = vector_decode_srgb(channels[i]);
        vector_to_srgb(channels, gamut);
    } else {
        if (n < 7 || *s != '#') FAIL("Bad color");
        size_t hexlen = (star ? (size_t)(star - s) : n) - 1;
        if (hexlen != 6 && hexlen != 8) FAIL("Bad color");
        unsigned v[4] = {0, 0, 0, 255};
        for (size_t i = 0; i < hexlen / 2; i++) {
            unsigned x = 0;
            for (int k = 0; k < 2; k++) {
                char c = s[1 + 2 * i + k];
                x <<= 4;
                if (c >= '0' && c <= '9') x |= c - '0';
                else if (c >= 'a' && c <= 'f') x |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') x |= c - 'A' + 10;
                else FAIL("Bad color");
            }
            v[i] = x;
        }
        for (unsigned i = 0; i < 3; i++) channels[i] = vector_decode_srgb(v[i] / 255.f);
        channels[3] = v[3] / 255.f;
    }
    if (star) {
        double b;
        const char *q = star + 1;
        if (!parse_num(&q, end, &b) || q != end || b < 0) FAIL("Bad brightness");
        for (unsigned i = 0; i < 3; i++) channels[i] *= (float)(b / 100);
    }
    for (unsigned i = 0; i < 4; i++) if (!isfinite(channels[i])) FAIL("Color overflow");
    p->type = PAINT_SOLID;
    p->r = channels[0], p->g = channels[1], p->b = channels[2], p->a = channels[3];
    return true;
}

// Whether s is a color that paints something, written as records write one
bool
vector_color_paints(const char *s, size_t len) {
    Paint p;
    return parse_color(s, s + len, &p) && p.type != PAINT_NONE;
}

static bool
parse_list(const char *s, const char *end, double **out, size_t *count) {
    size_t n = 0, cap = 0;
    double *arr = NULL;
    while (s < end) {
        double v;
        if (!parse_num(&s, end, &v)) {
            free(arr);
            FAIL("Bad number list");
        }
        if (n == cap) {
            void *np = n == SIZE_MAX ? NULL : reserve_array(arr, &cap, n + 1, sizeof(double), 16);
            if (!np) {
                free(arr);
                FAIL("Out of memory");
            }
            arr = np;
        }
        arr[n++] = v;
    }
    *out = arr;
    *count = n;
    return true;
}

static int
cmp_stop(const void *a, const void *b) {
    const Stop *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

static bool
parse_stops(const char *s, const char *end, Elem *e) {
    size_t cap = 0;
    while (s < end) {
        double off;
        if (!parse_num(&s, end, &off)) FAIL("Bad gradient stop");
        if (s >= end || *s != ',') FAIL("Bad gradient stop");
        s++;
        const char *c = memchr(s, ',', end - s);
        if (!c) c = end;
        Paint p;
        if (!parse_color(s, c, &p) || p.type != PAINT_SOLID) FAIL("Bad gradient stop color");
        s = c;
        if (e->nstops == cap) {
            void *np = e->nstops == SIZE_MAX ? NULL : reserve_array(e->stops, &cap, e->nstops + 1, sizeof(Stop), 4);
            if (!np) FAIL("Out of memory");
            e->stops = np;
        }
        e->stops[e->nstops++] = (Stop){fmin(1, fmax(0, off)), p.r, p.g, p.b, p.a};
    }
    if (!e->nstops) FAIL("Gradient without stops");
    qsort(e->stops, e->nstops, sizeof(Stop), cmp_stop);
    return true;
}

static bool
parse_choice(const char *s, const char *end, const char *const *names, unsigned n, uint8_t *out) {
    for (unsigned i = 0; i < n; i++)
        if (strlen(names[i]) == (size_t)(end - s) && !memcmp(s, names[i], end - s)) {
            *out = i;
            return true;
        }
    FAIL("Unknown keyword");
}

static bool
parse_uint(const char *s, const char *end, uint32_t *out) {
    double v;
    if (!parse_num(&s, end, &v) || s != end || v < 0 || v > UINT32_MAX || v != floor(v)) FAIL("Bad integer");
    *out = (uint32_t)v;
    return true;
}

typedef struct {
    char op;  // record type, or 'x' with id/all for deletes
    Elem *elem;
    uint32_t id;
    bool have_id, have_g, all, delete_group, have_d, have_pts, have_fill, have_stroke, have_cap;
} Record;

static bool
parse_field(Record *r, const char *key, size_t klen, const char *v, const char *end) {
    Elem *e = r->elem;
    double d;
#define IS(k) (klen == strlen(k) && !memcmp(key, k, klen))
#define NUM(field)                                   \
    {                                                \
        const char *q = v;                           \
        if (!parse_num(&q, end, &d) || q != end) FAIL("Bad number"); \
        field = d;                                   \
    }
    if (IS("id")) {
        if (!parse_uint(v, end, &e->id)) return false;
        r->have_id = true;
    } else if (IS("g")) {
        uint32_t g;
        if (!parse_uint(v, end, &g)) return false;
        if (r->op == 'x') r->id = g, r->delete_group = true;
        else e->parent = g;
        r->have_g = true;
    } else if (IS("p")) {
        if (!parse_uint(v, end, &e->parent)) return false;
    } else if (IS("t")) {
        double m[6];
        const char *q = v;
        if (!parse_nums(&q, end, m, 6) || q != end) FAIL("Bad transform");
        e->t = (Mat){m[0], m[1], m[2], m[3], m[4], m[5]};
        e->has_t = true;
    } else if (IS("depth")) {
        NUM(e->depth);
        e->has_depth = true;
        e->depth_in_pixels = false;
    } else if (IS("depth_px")) {
        NUM(e->depth);
        e->has_depth = true;
        e->depth_in_pixels = true;
    } else if (IS("phase")) {
        NUM(e->phase);
        e->has_phase = true;
    } else if (IS("box")) {
        double b[4];
        const char *q = v;
        if (!parse_nums(&q, end, b, 4) || q != end || b[2] < 0 || b[3] < 0 || !isfinite(b[0]+b[2]) || !isfinite(b[1]+b[3])) FAIL("Bad clipping box");
        e->clip_box = (Box){b[0], b[1], b[0]+b[2], b[1]+b[3]};
        e->has_clip_box = true;
    } else if (IS("text") && r->op == 't') {
        if ((size_t)(end-v) > PY_SSIZE_T_MAX) FAIL("Text too large");
        Py_XSETREF(e->text, PyBytes_FromStringAndSize(v, end-v));
        if (!e->text) { PyErr_Clear(); FAIL("Out of memory"); }
    } else if (IS("clip") || IS("s") || IS("img")) {
        if (!parse_uint(v, end, &e->ref)) return false;
    } else if (IS("o")) {
        NUM(d);
        e->opacity = (uint8_t)fmin(100, fmax(0, d));
    } else if (IS("blend")) {
        static const char *const names[] = {"normal", "multiply", "screen"};
        if (!parse_choice(v, end, names, 3, &e->blend)) return false;
    } else if (IS("v")) {
        NUM(d);
        e->visible = d != 0;
    } else if (IS("d")) {
        if (!parse_path(v, end, &e->path)) return false;
        r->have_d = true;
    } else if (IS("fill")) {
        if (!parse_color(v, end, &e->fill)) return false;
        r->have_fill = true;
    } else if (IS("stroke")) {
        if (!parse_color(v, end, &e->stroke)) return false;
        r->have_stroke = true;
    } else if (IS("rule")) {
        static const char *const names[] = {"nz", "eo"};
        if (!parse_choice(v, end, names, 2, &e->rule)) return false;
    } else if (IS("w")) {
        NUM(e->width);
    } else if (IS("cap")) {
        static const char *const names[] = {"butt", "round", "square"};
        if (!parse_choice(v, end, names, 3, &e->cap)) return false;
        r->have_cap = true;
    } else if (IS("join")) {
        static const char *const names[] = {"miter", "round", "bevel"};
        if (!parse_choice(v, end, names, 3, &e->join)) return false;
    } else if (IS("ml")) {
        NUM(e->miter_limit);
    } else if (IS("dash")) {
        size_t n;
        free(e->dash);
        e->dash = NULL;
        if (!parse_list(v, end, &e->dash, &n)) return false;
        e->ndash = n;
        for (size_t i = 0; i < e->ndash; i++)
            if (e->dash[i] < 0) FAIL("Negative dash length");
    } else if (IS("dp")) {
        NUM(e->dash_phase);
    } else if (IS("pts")) {
        size_t n;
        free(e->pts);
        e->pts = NULL;
        if (!parse_list(v, end, &e->pts, &n)) return false;
        if (n % 3) FAIL("Pen points must be x,y,width triples");
        e->npts = e->pts_capacity = n / 3;
        r->have_pts = true;
    } else if (IS("x1")) {
        NUM(e->g[0]);
    } else if (IS("y1")) {
        NUM(e->g[1]);
    } else if (IS("x2")) {
        NUM(e->g[2]);
    } else if (IS("y2")) {
        NUM(e->g[3]);
    } else if (IS("cx")) {
        NUM(e->g[0]);
    } else if (IS("cy")) {
        NUM(e->g[1]);
    } else if (IS("r")) {
        NUM(e->g[2]);
    } else if (IS("fx")) {
        NUM(e->g[3]);
        e->has_focus = true;
    } else if (IS("fy")) {
        NUM(e->g[4]);
        e->has_focus = true;
    } else if (IS("stops")) {
        if (!parse_stops(v, end, e)) return false;
    }  // unknown keys are ignored for forward compatibility
#undef IS
#undef NUM
    return true;
}

static bool
compile_text(VectorDoc *doc, Elem *e) {
    if (!e->text) FAIL("Text record without text");
    PyObject *module = PyImport_ImportModule("kitty.fonts.vector");
    PyObject *session = doc->font_session ? Py_NewRef(doc->font_session) : PyDict_New();
    PyObject *runs = module && session ? PyObject_CallMethod(module, "compile_text", "OO", session, e->text) : NULL;
    Py_XDECREF(module);
    Py_XDECREF(session);
    bool ok = false;
    if (!runs) goto done;
    Py_ssize_t count = PyList_Size(runs);
    if (count < 0) goto done;
    if ((size_t)count > SIZE_MAX / sizeof(Elem *)) goto allocation_failure;
    e->runs = calloc((size_t)count, sizeof(Elem *));
    if (count && !e->runs) goto allocation_failure;
    e->nruns = (size_t)count;
    e->bytes = e->nruns * sizeof(Elem *);
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *commands;
        const char *fill, *stroke;
        double width;
        if (!PyArg_ParseTuple(PyList_GET_ITEM(runs, i), "Ossd", &commands, &fill, &stroke, &width)) goto done;
        Elem *run = e->runs[i] = elem_new();
        if (!run) goto allocation_failure;
        run->width = width;
        run->join = JOIN_ROUND;
        if (!parse_color(fill, fill+strlen(fill), &run->fill) || !parse_color(stroke, stroke+strlen(stroke), &run->stroke)) goto done;
        for (Py_ssize_t k = 0; k < PyList_Size(commands); k++) {
            PyObject *cmd = PyList_GET_ITEM(commands, k);
            const long op = PyLong_AsLong(PyTuple_GET_ITEM(cmd, 0));
            if (op < PC_MOVE || op > PC_CLOSE) goto done;
            PathCmd pc = {.op = (uint8_t)op};
            Py_ssize_t n = PyTuple_GET_SIZE(cmd)-1;
            if (n != (op == PC_CLOSE ? 0 : op == PC_CUBIC ? 6 : op == PC_QUAD ? 4 : 2)) goto done;
            for (Py_ssize_t j = 0; j < n; j++) {
                pc.p[j] = PyFloat_AsDouble(PyTuple_GET_ITEM(cmd, j+1));
                if (!isfinite(pc.p[j]) || PyErr_Occurred()) goto done;
            }
            if (run->path.count == SIZE_MAX) goto allocation_failure;
            if (run->path.count == run->path.capacity) {
                void *np = reserve_array(run->path.item, &run->path.capacity, run->path.count+1, sizeof(PathCmd), 16);
                if (!np) goto allocation_failure;
                run->path.item = np;
            }
            run->path.item[run->path.count++] = pc;
        }
        if (run->path.capacity > (SIZE_MAX-sizeof(Elem)) / sizeof(PathCmd)) goto allocation_failure;
        size_t bytes = sizeof(Elem) + run->path.capacity * sizeof(PathCmd);
        if (e->bytes > SIZE_MAX-bytes) goto allocation_failure;
        e->bytes += bytes;
    }
    ok = true;
    goto done;
allocation_failure:
    parse_error = "Out of memory or vector text allocation overflow";
done:
    Py_XDECREF(runs);
    if (PyErr_ExceptionMatches(PyExc_MemoryError)) parse_error = "Out of memory laying out vector text";
    if (PyErr_Occurred()) PyErr_Clear();
    if (!ok && !parse_error) parse_error = "Invalid vector text, font or layout";
    return ok;
}

// With compile false the text of a t record is not laid out, see vector_doc_apply()
static bool
parse_record(VectorDoc *doc, const char *line, const char *end, Record *r, bool compile) {
    memset(r, 0, sizeof(*r));
    while (line < end && *line == ' ') line++;
    if (line >= end) return true;  // blank
    r->op = *line++;
    if (line < end && *line != ' ') FAIL("Record type must be a single character");
    if (!strchr("gpsuikalrxt", r->op)) {
        r->op = 0;  // unknown record types are ignored for forward compatibility
        return true;
    }
    if (!(r->elem = elem_new())) FAIL("Out of memory");
    Elem *e = r->elem;
    e->type = r->op;
    while (line < end) {
        while (line < end && *line == ' ') line++;
        if (line >= end) break;
        const char *tok_end = memchr(line, ' ', end - line);
        if (!tok_end) tok_end = end;
        const char *eq = memchr(line, '=', tok_end - line);
        if (!eq) {
            if (r->op == 'x' && tok_end - line == 3 && !memcmp(line, "all", 3)) r->all = true;
            else FAIL("Field without =");
        } else if (!parse_field(r, line, eq - line, eq + 1, tok_end)) return false;
        line = tok_end;
    }
    if (r->op == 'x') {
        if (!r->all && !r->have_id && !r->have_g) FAIL("Delete record without id, g or all");
        if (!r->delete_group) r->id = e->id;
        return true;
    }
    if (!r->have_id) FAIL("Record without id");
    if (!e->id && r->op != 'g') FAIL("Only the root group has id 0");
    r->id = e->id;
    switch (r->op) {
        case 't':
            if (compile && !compile_text(doc, e)) return false;
            break;
        case 'p':
        case 's':
            if (!r->have_d) FAIL("Path without d");
            if (!r->have_fill && !r->have_stroke) e->fill = (Paint){PAINT_SOLID, 0, 0, 0, 1, 0};
            break;
        case 'u':
            if (!e->ref) FAIL("Use without s");
            if (!r->have_fill && !r->have_stroke) e->fill = (Paint){PAINT_SOLID, 0, 0, 0, 1, 0};
            break;
        case 'i':
            if (!e->ref) FAIL("Image without img");
            break;
        case 'k':
            if (!r->have_pts) FAIL("Pen without pts");
            if (!r->have_fill) e->fill = (Paint){PAINT_SOLID, 0, 0, 0, 1, 0};
            if (!r->have_cap) e->cap = CAP_ROUND;
            break;
        case 'a':
            if (!r->have_d && !r->have_pts) FAIL("Append without d or pts");
            break;
        case 'l':
        case 'r':
            if (!e->nstops) FAIL("Gradient without stops");
            break;
    }
    e->bytes += sizeof(Elem) + e->path.count * sizeof(PathCmd) + e->npts * 3 * sizeof(double) + e->ndash * sizeof(double) + e->nstops * sizeof(Stop);
    if (e->text) e->bytes += (size_t)PyBytes_GET_SIZE(e->text);
    return true;
}

static bool
append_geometry(VectorDoc *d, Elem *target, Elem *src) {
    if (src->path.count) {
        if (target->type != 'p' && target->type != 's') return false;
        GROW(&target->path, item, src->path.count);
        memcpy(target->path.item + target->path.count, src->path.item, src->path.count * sizeof(PathCmd));
        target->path.count += src->path.count;
        d->used_bytes += src->path.count * sizeof(PathCmd);
        target->bytes += src->path.count * sizeof(PathCmd);
    }
    if (src->npts) {
        if (target->type != 'k') return false;
        if (src->npts > SIZE_MAX - target->npts) return false;
        const size_t needed = target->npts + src->npts;
        if (needed > target->pts_capacity) {
            const size_t before = target->pts_capacity;
            void *np = reserve_array(target->pts, &target->pts_capacity, needed, 3 * sizeof(double), 16);
            if (!np) return false;
            target->pts = np;
            const size_t added = (target->pts_capacity - before) * 3 * sizeof(double);
            d->used_bytes += added;
            target->bytes += added;
        }
        memcpy(target->pts + target->npts * 3, src->pts, src->npts * 3 * sizeof(double));
        target->npts += src->npts;
    }
    return true;
}

// Applies one record that has been parsed, returning an error message or NULL
static const char *
apply_record(VectorDoc *d, Record *r) {
    // a few more parent links for damage_under() to follow for each record
    d->damage_steps = d->damage_steps > SIZE_MAX - 4 ? SIZE_MAX : d->damage_steps + 4;
    switch (r->op) {
        case 0: break;
        case 'x':
            if (r->all) {
                for (size_t k = 0; k < d->count; k++)
                    if (d->order[k]) remove_elem(d, d->order[k]);
                d->count = 0;
                d->root.t = identity;
                d->root.has_t = false;
                d->root.ref = 0;
                d->root.opacity = 100;
                d->root.blend = BLEND_NORMAL;
                d->root.visible = true;
                d->root.has_clip_box = false;
                d->damage_all = true;
            } else if (r->delete_group) {
                d->damage_all = true;
                if (!delete_group(d, r->id)) return "Out of memory";
            } else {
                Elem *e = lookup(d, r->id);
                if (e) {
                    damage_elem(d, e);
                    remove_elem(d, e);
                }
            }
            break;
        case 'a': {
            Elem *e = lookup(d, r->id);
            if (!e) break;
            elem_box(e);  // leaves the cursor at the end of the path there is
            const PathCursor before = e->cursor;
            const size_t ncmds = e->path.count, npts = e->npts;
            const bool wrong_kind = (r->elem->path.count && e->type != 'p' && e->type != 's') || (r->elem->npts && e->type != 'k');
            if (wrong_kind) return "Append to an element of another kind";
            if (append_geometry(d, e, r->elem)) damage_appended(d, e, ncmds, npts, before);
            else {
                e->has_box = false;  // some of it may have been appended
                d->damage_all = true;
                return "Out of memory or geometry size overflow";
            }
        } break;
        case 'g':
            d->damage_all = true;  // a group changes everything under it
            if (!r->elem->id) {    // the root group
                d->root.t = r->elem->t;
                d->root.has_t = r->elem->has_t;
                d->root.ref = r->elem->ref;
                d->root.opacity = r->elem->opacity;
                d->root.blend = r->elem->blend;
                d->root.visible = r->elem->visible;
                d->root.has_clip_box = r->elem->has_clip_box;
                d->root.clip_box = r->elem->clip_box;
                break;
            }
            /* fallthrough */
        default: {
            Elem *old = lookup(d, r->id), *e = r->elem;
            if (old) damage_elem(d, old);
            if (!insert_elem(d, e)) return "Out of memory or too many elements";
            r->elem = NULL;  // now owned by the document
            damage_elem(d, e);
        } break;
    }
    return NULL;
}

// The end of the line at s, without its line ending, setting *next to the start of the next one
static const char *
line_end(const char *s, const char *end, const char **next) {
    const char *nl = memchr(s, '\n', end - s), *le = nl ? nl : end;
    *next = nl ? nl + 1 : end;
    if (le > s && le[-1] == '\r') le--;
    return le;
}

// Every record is parsed before any is applied, so that a bad one changes
// nothing. They are not kept from one pass to the next but parsed again as they
// are applied: kept, they would take many times the memory of the text they
// came from, a delete of a few bytes holding a whole element. Text records are
// the exception: laying out text costs too much to do twice, and what it makes
// is kept by the document anyway.
const char *
vector_doc_apply(VectorDoc *d, const uint8_t *text, size_t sz) {
    static char errbuf[128];
    const char *const begin = (const char *)text, *end = begin + sz;
    const char *err = NULL;
    Elem **texts = NULL;
    size_t ntexts = 0, texts_cap = 0, next_text = 0, lineno = 0;
    for (const char *s = begin, *next; s < end; s = next) {
        const char *le = line_end(s, end, &next);
        lineno++;
        Record r;
        parse_error = NULL;
        const bool ok = parse_record(d, s, le, &r, true);
        if (ok && r.op == 't') {
            if (ntexts == texts_cap) {
                void *np = ntexts == SIZE_MAX ? NULL : reserve_array(texts, &texts_cap, ntexts + 1, sizeof(Elem *), 16);
                if (!np) {
                    elem_free(r.elem);
                    err = "Out of memory";
                    goto done;
                }
                texts = np;
            }
            texts[ntexts++] = r.elem;
            r.elem = NULL;
        }
        elem_free(r.elem);
        if (!ok) {
            snprintf(errbuf, sizeof(errbuf), "Line %zu: %s", lineno, parse_error ? parse_error : "Bad record");
            err = errbuf;
            goto done;
        }
    }
    lineno = 0;
    d->damage_steps = d->live > (SIZE_MAX - 64) / 2 ? SIZE_MAX : 2 * d->live + 64;
    for (const char *s = begin, *next; s < end && !err; s = next) {
        const char *le = line_end(s, end, &next);
        lineno++;
        Record r;
        parse_error = NULL;
        if (parse_record(d, s, le, &r, false)) {
            if (r.op == 't') {  // the one laid out in the first pass
                elem_free(r.elem);
                r.elem = texts[next_text];
                texts[next_text++] = NULL;
            }
            err = apply_record(d, &r);
        } else {  // only memory can run out the second time
            snprintf(errbuf, sizeof(errbuf), "Line %zu: %s", lineno, parse_error ? parse_error : "Bad record");
            err = errbuf;
        }
        elem_free(r.elem);
    }
    if (err) d->damage_all = true;  // the records before the bad one were applied
    compact(d);
done:
    for (size_t i = 0; i < ntexts; i++) elem_free(texts[i]);
    free(texts);
    return err;
}

// }}}

// Rendering {{{

typedef struct {
    float *px;  // premultiplied linear extended-sRGB RGBA
    Rect r;       // the pixels of the image it holds
} Canvas;

enum { FILL_SOLID, FILL_LINEAR, FILL_RADIAL, FILL_IMAGE };

typedef struct {
    Canvas *canvas;
    const uint8_t *clip;  // coverage mask the size of the canvas or NULL
    int type;
    uint8_t blend;
    float r, g, b, a;  // premultiplied solid color
    float opacity;
    const Elem *grad;
    Mat inv;  // device to paint space
    const VectorImageData *img;
    const float *srgb_bytes;
} Fill;

typedef struct {
    VectorDoc *doc;
    VectorStereo stereo;
    Elem **sorted;  // live elements sorted by parent then slot
    size_t nsorted;
    Rect area;  // the pixels being drawn
    Box view;   // the pixels of the raster they are a part of, see make_cull()
    vector_image_lookup lookup;
    void *lookup_ctx;
    bool failed;
    bool has_srgb_bytes;
    float srgb_bytes[256];  // all possible values of one channel in an 8-bit raster
} Render;

static inline float
clampf(float x) {
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

static void
eval_stops(const Elem *g, double t, float *r, float *gg, float *b, float *a) {
    const Stop *st = g->stops;
    const size_t n = g->nstops;
    double c[4];
    if (t <= st[0].offset || n == 1) {
        c[0] = st[0].r, c[1] = st[0].g, c[2] = st[0].b, c[3] = st[0].a;
    } else if (t >= st[n - 1].offset) {
        c[0] = st[n - 1].r, c[1] = st[n - 1].g, c[2] = st[n - 1].b, c[3] = st[n - 1].a;
    } else {
        // the first stop at or after t, by bisection: this is done for every pixel, and a gradient can have any number of stops
        size_t i = 1, hi = n - 1;
        while (i < hi) {
            const size_t mid = i + (hi - i) / 2;
            if (st[mid].offset < t) i = mid + 1;
            else hi = mid;
        }
        double span = st[i].offset - st[i - 1].offset, f = span > 0 ? (t - st[i - 1].offset) / span : 0;
        c[0] = st[i - 1].r + (st[i].r - st[i - 1].r) * f;
        c[1] = st[i - 1].g + (st[i].g - st[i - 1].g) * f;
        c[2] = st[i - 1].b + (st[i].b - st[i - 1].b) * f;
        c[3] = st[i - 1].a + (st[i].a - st[i - 1].a) * f;
    }
    *r = (float)c[0], *gg = (float)c[1], *b = (float)c[2], *a = (float)c[3];
}

static inline void
sample_image(const VectorImageData *img, const float *srgb_bytes, double u, double v, float *r, float *g, float *b, float *a) {
    const unsigned bpp = img->is_float ? 8 : img->is_opaque ? 3 : 4;
    double fx = u - 0.5, fy = v - 0.5;
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    float tx = (float)(fx - x0), ty = (float)(fy - y0), acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++) {
        int y = y0 + j;
        if (y < 0) y = 0;
        if (y >= (int)img->height) y = img->height - 1;
        for (int i = 0; i < 2; i++) {
            int x = x0 + i;
            if (x < 0) x = 0;
            if (x >= (int)img->width) x = img->width - 1;
            const uint8_t *p = img->data + ((size_t)y * img->width + x) * bpp;
            float c[4], w = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            if (img->is_float) {
                uint16_t h[4]; // one RGBA16F pixel; memcpy also permits unaligned sources
                memcpy(h, p, sizeof(h));
                for (unsigned k = 0; k < 4; k++) c[k] = vector_half_to_float(h[k]);
            } else {
                for (unsigned k = 0; k < 3; k++) c[k] = srgb_bytes[p[k]];
                c[3] = img->is_opaque ? 1 : p[3] / 255.f;
                vector_to_srgb(c, img->gamut);
            }
            for (unsigned k = 0; k < 3; k++) acc[k] += c[k] * c[3] * w;
            acc[3] += c[3] * w;
        }
    }
    *r = acc[0], *g = acc[1], *b = acc[2], *a = acc[3];
}

static inline void
paint_at(const Fill *f, int x, int y, float *r, float *g, float *b, float *a) {
    if (f->type == FILL_SOLID) {
        *r = f->r, *g = f->g, *b = f->b, *a = f->a;
        return;
    }
    double u, v;
    apply(f->inv, x + 0.5, y + 0.5, &u, &v);
    if (f->type == FILL_IMAGE) {
        sample_image(f->img, f->srgb_bytes, u, v, r, g, b, a);
    } else {
        const Elem *gr = f->grad;
        double t;
        if (f->type == FILL_LINEAR) {
            double dx = gr->g[2] - gr->g[0], dy = gr->g[3] - gr->g[1], l2 = dx * dx + dy * dy;
            t = l2 > 0 ? ((u - gr->g[0]) * dx + (v - gr->g[1]) * dy) / l2 : 0;
        } else {
            // radial: find t such that p lies on the circle of radius t*r around focus + t*(center - focus)
            double cx = gr->g[0], cy = gr->g[1], rad = gr->g[2], fx = gr->has_focus ? gr->g[3] : cx, fy = gr->has_focus ? gr->g[4] : cy;
            double px = u - fx, py = v - fy, cdx = cx - fx, cdy = cy - fy;
            double A = cdx * cdx + cdy * cdy - rad * rad, B = px * cdx + py * cdy, C = px * px + py * py;
            if (fabs(A) < 1e-12) t = B != 0 ? C / (2 * B) : 0;
            else {
                double disc = B * B - A * C;
                t = disc < 0 ? 1 : (B - sqrt(disc)) / A;
                if (t < 0) t = (B + sqrt(fmax(0, disc))) / A;
            }
        }
        eval_stops(gr, t, r, g, b, a);
        *r *= *a, *g *= *a, *b *= *a;
    }
    *r *= f->opacity, *g *= f->opacity, *b *= f->opacity, *a *= f->opacity;
}

// Composites the premultiplied source S onto D, both in [0, 1], writing D's new value to out
static inline void
blend_pixel(float *out, const float *S, const float *D, uint8_t blend) {
    float O[4];
    switch (blend) {
        case BLEND_MULTIPLY:
            for (int k = 0; k < 3; k++) O[k] = S[k] * D[k] + S[k] * (1 - D[3]) + D[k] * (1 - S[3]);
            break;
        case BLEND_SCREEN:
            for (int k = 0; k < 3; k++) O[k] = S[k] + D[k] - S[k] * D[k];
            break;
        default:
            for (int k = 0; k < 3; k++) O[k] = S[k] + D[k] * (1 - S[3]);
    }
    O[3] = S[3] + D[3] * (1 - S[3]);
    for (int k = 0; k < 3; k++) out[k] = O[k];
    out[3] = clampf(O[3]);
}

static void
composite_span(void *ctx, int y, int x0, int x1, const float *cov) {
    const Fill *f = ctx;
    Canvas *c = f->canvas;
    const size_t at = rect_index(c->r, x0, y);
    float *row = c->px + at * 4;
    const uint8_t *clip = f->clip ? f->clip + at : NULL;
    for (int x = x0; x < x1; x++, row += 4) {
        float cv = cov[x - x0];
        if (clip) cv *= clip[x - x0] / 255.f;
        if (cv <= 0) continue;
        float S[4], D[4];
        paint_at(f, x, y, S, S + 1, S + 2, S + 3);
        for (int k = 0; k < 4; k++) S[k] *= cv, D[k] = row[k];
        blend_pixel(row, S, D, f->blend);
    }
}

typedef struct {
    uint8_t *mask;
    const uint8_t *outer;
    Rect r;
} MaskCtx;

static void
mask_span(void *ctx, int y, int x0, int x1, const float *cov) {
    const MaskCtx *m = ctx;
    const size_t at = rect_index(m->r, x0, y);
    uint8_t *row = m->mask + at;
    const uint8_t *outer = m->outer ? m->outer + at : NULL;
    for (int i = 0; i < x1 - x0; i++) row[i] = (uint8_t)(cov[i] * (outer ? outer[i] : 255) + 0.5f);
}

typedef struct {
    Canvas *canvas;
    const uint8_t *clip;
    uint8_t blend;
} Target;

static bool
setup_fill(Render *r, Fill *f, const Target *t, const Paint *p, Mat m, float opacity) {
    memset(f, 0, sizeof(*f));
    f->canvas = t->canvas;
    f->clip = t->clip;
    f->blend = t->blend;
    f->opacity = opacity;
    if (p->type == PAINT_SOLID) {
        f->type = FILL_SOLID;
        float a = p->a * opacity;
        f->r = p->r * a, f->g = p->g * a, f->b = p->b * a, f->a = a;
        return true;
    }
    Elem *g = lookup(r->doc, p->gradient);
    if (!g || (g->type != 'l' && g->type != 'r') || !mat_invert(m, &f->inv)) return false;
    f->type = g->type == 'l' ? FILL_LINEAR : FILL_RADIAL;
    f->grad = g;
    return true;
}

static void
fill_edges(Render *r, EdgeList *el, bool evenodd, Fill *f) {
    if (!rasterize(el, evenodd, r->area, composite_span, f)) r->failed = true;
}

// Whether what lies inside box, in the coordinate system m maps to pixels,
// stays clear of the pixels being drawn
static bool
outside(const Render *r, Box box, Mat m) {
    if (box_is_empty(box)) return true;
    box = box_transform(box, m);
    return box.x1 < r->area.x0 - 1 || box.x0 > r->area.x1 + 1 || box.y1 < r->area.y0 - 1 || box.y0 > r->area.y1 + 1;
}

// The edges of the subpaths that reach the pixels being drawn
static bool
add_polys(const Render *r, const PolyList *polys, EdgeList *el) {
    for (size_t i = 0; i < polys->count; i++) {
        const Poly *p = polys->item + i;
        if (polygon_misses(p->pts, p->count / 2, identity, r->area)) continue;
        if (!add_polygon(el, p->pts, p->count / 2, identity, false)) return false;
    }
    return true;
}

// Strokes the flattened subpaths of a path drawn with the style of style, into
// el, and into fine the stretches whose dashes are too fine to draw one by one,
// which are to be filled at *fine_coverage, see dash_polyline()
static bool
stroke_polys(const Render *r, PolyList *polys, const Elem *style, Mat m, double tol, EdgeList *el, EdgeList *fine, float *fine_coverage) {
    const double hw = style->width / 2;
    StrokeOut out = {el, m, r->area, box_grow(r->view, 2)};
    StrokeOut fine_out = {fine, m, r->area, out.region};
    DashPattern pat;
    if (!dash_pattern_init(&pat, style->dash, style->ndash, style->cap, hw)) return false;
    bool ok = true;
    if (!(pat.cycle > 0)) {
        for (size_t i = 0; ok && i < polys->count; i++) {
            Poly *p = polys->item + i;
            // A drawing can have thousands of subpaths while a small part of it
            // is drawn. Leave out those that cannot reach it before building
            // their caps and joins.
            Box box = empty_box;
            for (size_t k = 0; k < p->count; k += 2) box_add(&box, p->pts[k], p->pts[k + 1]);
            box = box_transform(box_grow(box, stroke_reach(style)), m);
            if (box.x1 < r->area.x0 - 1 || box.x0 > r->area.x1 + 1 || box.y1 < r->area.y0 - 1 || box.y0 > r->area.y1 + 1) continue;
            ok = stroke_polyline(&out, p->pts, p->count / 2, p->closed, hw, style->cap, style->cap, style->join, style->miter_limit, tol);
        }
        return ok;
    }
    Dasher ds = {.pat = &pat, .hw = hw, .ext = style->cap == CAP_BUTT ? 0 : hw, .m = m};
    Mat inv;
    // A transform that cannot be undone flattens the stroke to nothing
    if (!mat_invert_any(m, &inv)) goto done;
    {
        const Box region = out.region;
        const double xs[4] = {region.x0, region.x1, region.x1, region.x0}, ys[4] = {region.y0, region.y0, region.y1, region.y1};
        for (int k = 0; k < 4; k++) apply(inv, xs[k], ys[k], ds.wx + k, ds.wy + k);
        ds.cull = true;
        for (int k = 0; k < 4; k++) ds.cull = ds.cull && isfinite(ds.wx[k]) && isfinite(ds.wy[k]);
        if (!ds.cull) goto done;  // nothing of a stroke that large can be placed to a pixel
        ds.join_region = box_grow(r->view, stroke_reach(style) * mat_scale(m) + 2);
    }
    *fine_coverage = (float)pat.coverage;
    // vector_dash_limit, when set, is how many dash ends the path may have, past which a subpath is stroked solid
    size_t budget = kitty_vector_dash_limit();
    const bool limited = budget > 0;
    for (size_t i = 0; ok && i < polys->count; i++) {
        PolyList pieces = {0}, fine_pieces = {0};
        ds.out = &pieces, ds.fine = &fine_pieces;
        ok = dash_polyline(&ds, polys->item + i, style->dash_phase);
        if (ok && limited) {
            const size_t ends = pieces.count > SIZE_MAX / 2 ? SIZE_MAX : 2 * pieces.count;
            if (ends > budget) {
                Poly *p = polys->item + i;
                ok = stroke_polyline(&out, p->pts, p->count / 2, p->closed, hw, style->cap, style->cap, style->join, style->miter_limit, tol);
                polylist_free(&pieces);
                polylist_free(&fine_pieces);
                continue;
            }
            budget -= ends;
        }
        for (size_t k = 0; ok && k < pieces.count; k++) {
            Poly *q = pieces.item + k;
            ok = stroke_polyline(&out, q->pts, q->count / 2, q->closed, hw, q->cut_start ? CAP_BUTT : style->cap, q->cut_end ? CAP_BUTT : style->cap,
                style->join, style->miter_limit, tol);
        }
        for (size_t k = 0; ok && k < fine_pieces.count; k++) {
            Poly *q = fine_pieces.item + k;
            ok = stroke_polyline(&fine_out, q->pts, q->count / 2, false, hw, CAP_BUTT, CAP_BUTT, style->join, style->miter_limit, tol);
        }
        polylist_free(&pieces);
        polylist_free(&fine_pieces);
    }
done:
    dash_pattern_free(&pat);
    return ok;
}

// Draws path with the style of e (which is e's own path or the symbol it uses)
static void
draw_path(Render *r, const Elem *e, const Path *path, Mat m, const Target *t) {
    if (!mat_is_finite(m)) return;  // a transform that overflowed puts nothing anywhere
    if (e->fill.type != PAINT_NONE) {
        PolyList polys = {0};
        EdgeList el = {0};
        Fill f;
        const Cull cull = make_cull(identity, r->view, 0, TOLERANCE);
        bool ok = flatten_path(path, m, &cull, &polys) && add_polys(r, &polys, &el);
        if (ok && setup_fill(r, &f, t, &e->fill, m, 1)) fill_edges(r, &el, e->rule == 1, &f);
        if (!ok) r->failed = true;
        polylist_free(&polys);
        free(el.item);
    }
    if (e->stroke.type != PAINT_NONE && e->width > 0) {
        const double scale = mat_scale(m), reach = stroke_reach(e) * scale;
        if (!isfinite(reach)) return;  // nor does a stroke wider than a double can hold
        PolyList polys = {0};
        EdgeList el = {0}, fine = {0};
        float coverage = 0;
        Fill f;
        // The path is flattened in its own coordinates, for the stroke to be
        // built there, with the error and what can be seen measured in device
        // pixels. Everything within the reach of the stroke can be seen, so for
        // a stroke millions of pixels wide that is a great deal, and the
        // tolerance becomes a fixed fraction of the reach rather than of a pixel.
        // It still keeps the outline to within a millionth of the reach, and
        // keeps the segments of each curve bounded, see make_cull().
        const Cull cull = make_cull(m, r->view, reach, fmax(TOLERANCE, reach * 0x1p-24));
        bool ok = flatten_path(path, identity, &cull, &polys) && stroke_polys(r, &polys, e, m, TOLERANCE / scale, &el, &fine, &coverage);
        if (ok && setup_fill(r, &f, t, &e->stroke, m, 1)) fill_edges(r, &el, false, &f);
        if (ok && fine.count && coverage > 0 && setup_fill(r, &f, t, &e->stroke, m, coverage)) fill_edges(r, &fine, false, &f);
        if (!ok) r->failed = true;
        polylist_free(&polys);
        free(el.item);
        free(fine.item);
    }
}

static void
draw_pen(Render *r, const Elem *e, Mat m, const Target *t) {
    if (e->fill.type == PAINT_NONE || !e->npts || !mat_is_finite(m)) return;
    EdgeList el = {0};
    StrokeOut out = {&el, m, r->area, box_grow(r->view, 2)};
    double tol = TOLERANCE / mat_scale(m);
    bool ok = true;
    const double *p = e->pts;
    for (size_t i = 0; ok && i + 1 < e->npts; i++) {
        double x0 = p[3 * i], y0 = p[3 * i + 1], w0 = p[3 * i + 2] / 2, x1 = p[3 * i + 3], y1 = p[3 * i + 4], w1 = p[3 * i + 5] / 2;
        double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
        if (len < 1e-9) continue;
        dx /= len, dy /= len;
        double q[8] = {x0 - dy * w0, y0 + dx * w0, x1 - dy * w1, y1 + dx * w1, x1 + dy * w1, y1 - dx * w1, x0 + dy * w0, y0 - dx * w0};
        ok = emit(&out, q, 4);
    }
    for (size_t i = 0; ok && i < e->npts; i++) {
        bool end = i == 0 || i + 1 == e->npts;
        if (end && e->cap == CAP_BUTT && e->npts > 1) continue;
        ok = emit_circle(&out, p[3 * i], p[3 * i + 1], p[3 * i + 2] / 2, tol);
    }
    Fill f;
    if (ok && setup_fill(r, &f, t, &e->fill, m, 1)) fill_edges(r, &el, false, &f);
    if (!ok) r->failed = true;
    free(el.item);
}

static void
draw_image(Render *r, const Elem *e, Mat m, const Target *t) {
    VectorImageData img = {0};
    // its size first, fetching the pixels costs a lot more than finding it is not in the part being drawn
    if (!r->lookup || !r->lookup(r->lookup_ctx, e->ref, false, &img) || !img.width || !img.height) return;
    if (outside(r, (Box){0, 0, img.width, img.height}, m)) return;
    if (!r->lookup(r->lookup_ctx, e->ref, true, &img) || !img.data || !img.width || !img.height) {
        free(img.data);
        return;
    }
    Fill f = {.canvas = t->canvas, .clip = t->clip, .blend = t->blend, .type = FILL_IMAGE, .opacity = e->opacity / 100.f, .img = &img};
    if (!img.is_float) {
        if (!r->has_srgb_bytes) {
            for (unsigned i = 0; i < 256; i++) r->srgb_bytes[i] = vector_decode_srgb(i / 255.f);
            r->has_srgb_bytes = true;
        }
        f.srgb_bytes = r->srgb_bytes;
    }
    EdgeList el = {0};
    double q[8] = {0, 0, img.width, 0, img.width, img.height, 0, img.height};
    if (mat_invert(m, &f.inv) && add_polygon(&el, q, 4, m, false)) fill_edges(r, &el, false, &f);
    free(el.item);
    free(img.data);
}

static uint8_t *
make_clip(Render *r, const Elem *path, Mat m, const uint8_t *outer) {
    uint8_t *mask = calloc(rect_size(r->area), 1);
    if (!mask) {
        r->failed = true;
        return NULL;
    }
    PolyList polys = {0};
    EdgeList el = {0};
    const Cull cull = make_cull(identity, r->view, 0, TOLERANCE);
    bool ok = flatten_path(&path->path, mat_mul(m, path->t), &cull, &polys) && add_polys(r, &polys, &el);
    MaskCtx mc = {mask, outer, r->area};
    if (ok) ok = rasterize(&el, path->rule == 1, r->area, mask_span, &mc);
    if (!ok) r->failed = true;
    polylist_free(&polys);
    free(el.item);
    return mask;
}

static void
composite_group(Canvas *dst, const Canvas *src, float opacity) {
    size_t n = rect_size(dst->r);
    for (size_t i = 0; i < n; i++) {
        const float *s = src->px + 4 * i;
        float *d = dst->px + 4 * i;
        if (s[3] <= 0) continue;
        float S[4], D[4];
        for (int k = 0; k < 4; k++) S[k] = s[k] * opacity, D[k] = d[k];
        blend_pixel(d, S, D, BLEND_NORMAL);
    }
}

static size_t
first_child(const Render *r, uint32_t gid) {
    size_t lo = 0, hi = r->nsorted;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (r->sorted[mid]->parent < gid) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Draws the children of g. The blend mode of a group applies to every drawing
// operation below it, as the PDF blend mode does; opacity composites the
// group as a whole through a layer.
typedef struct {
    Elem *group;
    double depth;
    double phase;
    bool depth_in_pixels;
    Mat m;
    Target parent, target;
    Canvas *layer;
    uint8_t *own_clip;
    size_t child;
    bool entered;
} GroupFrame;

// An explicit growing stack allows deeply nested groups without consuming the C stack.
static void
render_group(Render *r, Elem *group, Mat parent_m, Target parent) {
    size_t count = 1, capacity = 0;
    GroupFrame *stack = reserve_array(NULL, &capacity, count, sizeof(GroupFrame), 16);
    if (!stack) {
        r->failed = true;
        return;
    }
    stack[0] = (GroupFrame){.group = group, .m = parent_m, .parent = parent};
    while (count && !r->failed) {
        GroupFrame *f = stack + count - 1;
        Elem *g = f->group;
        if (!f->entered) {
            if (!g->visible || g->in_progress) {
                count--;
                continue;
            }
            f->entered = true;
            if (g->has_depth) { f->depth = g->depth; f->depth_in_pixels = g->depth_in_pixels; }
            if (g->has_phase) f->phase = g->phase;
            g->in_progress = true;
            f->m = g->has_t ? mat_mul(f->m, g->t) : f->m;
            f->target = f->parent;
            f->child = first_child(r, g->id);
            if (g->ref) {
                Elem *cp = lookup(r->doc, g->ref);
                if (cp && (cp->type == 'p' || cp->type == 's')) {
                    if (outside(r, elem_box(cp), mat_mul(f->m, cp->t))) {
                        g->in_progress = false;
                        count--;
                        continue;
                    }
                    f->target.clip = f->own_clip = make_clip(r, cp, f->m, f->target.clip);
                }
            }
            if (g->has_clip_box) {
                Box b = g->clip_box;
                PathCmd commands[] = {{PC_MOVE,{b.x0,b.y0}}, {PC_LINE,{b.x1,b.y0}}, {PC_LINE,{b.x1,b.y1}}, {PC_LINE,{b.x0,b.y1}}, {PC_CLOSE,{0}}};
                Elem box = {.t = identity, .path = {.item = commands, .count = 5}};
                uint8_t *mask = make_clip(r, &box, f->m, f->target.clip);
                free(f->own_clip);
                f->target.clip = f->own_clip = mask;
            }
            if (g->blend != BLEND_NORMAL) f->target.blend = g->blend;
            if (g->opacity < 100) {
                f->layer = calloc(1, sizeof(Canvas));
                if (!f->layer) {
                    r->failed = true;
                    break;
                }
                f->layer->r = r->area;
                f->layer->px = calloc(rect_size(r->area), 4 * sizeof(float));
                if (!f->layer->px) {
                    r->failed = true;
                    break;
                }
                f->target.canvas = f->layer;
            }
        }
        if (f->child >= r->nsorted || r->sorted[f->child]->parent != g->id) {
            if (f->layer) {
                composite_group(f->parent.canvas, f->layer, g->opacity / 100.f);
                free(f->layer->px);
                free(f->layer);
            }
            free(f->own_clip);
            g->in_progress = false;
            count--;
            continue;
        }
        Elem *e = r->sorted[f->child++];
        if (e == g) continue;
        const Mat m = f->m;
        Mat em = e->has_t ? mat_mul(m, e->t) : m;
        const double depth = e->has_depth ? e->depth : f->depth;
        const bool depth_in_pixels = e->has_depth ? e->depth_in_pixels : f->depth_in_pixels;
        const double phase = e->has_phase ? e->phase : f->phase;
        if (e->type != 'g' && r->stereo.eye_pixels != 0.)
            em.e += r->stereo.eye_pixels * spatial_requested_disparity(depth_in_pixels ? 0. : depth, depth_in_pixels ? depth : 0.,
                r->stereo.multiplier, r->stereo.positive, r->stereo.negative, r->stereo.fov);
        if (e->type != 'g') em.f += r->stereo.phase_pixels * fmin(r->stereo.phase_limit, fmax(-r->stereo.phase_limit, phase));
        const Target t = f->target;
        switch (e->type) {
            case 'g': {
                const double inherited_depth = f->depth;
                const double inherited_phase = f->phase;
                const bool inherited_depth_in_pixels = f->depth_in_pixels;
                if (count == capacity) {
                    if (count == SIZE_MAX) {
                        r->failed = true;
                        break;
                    }
                    void *np = reserve_array(stack, &capacity, count + 1, sizeof(GroupFrame), 16);
                    if (!np) {
                        r->failed = true;
                        break;
                    }
                    stack = np;
                }
                stack[count++] = (GroupFrame){.group = e, .m = m, .parent = t, .depth = inherited_depth,
                    .depth_in_pixels = inherited_depth_in_pixels, .phase = inherited_phase};
            } break;
            case 'p':
                if (!outside(r, box_grow(elem_box(e), stroke_reach(e)), em)) draw_path(r, e, &e->path, em, &t);
                break;
            case 't':
                if (!outside(r, elem_box(e), em)) for (size_t i = 0; i < e->nruns && !r->failed; i++) draw_path(r, e->runs[i], &e->runs[i]->path, em, &t);
                break;
            case 'u': {
                Elem *sym = lookup(r->doc, e->ref);
                if (sym && sym->type == 's' && !outside(r, box_grow(elem_box(sym), stroke_reach(e)), em)) draw_path(r, e, &sym->path, em, &t);
            } break;
            case 'k':
                if (!outside(r, elem_box(e), em)) draw_pen(r, e, em, &t);
                break;
            case 'i': draw_image(r, e, em, &t); break;
        }
    }
    // Release every entered frame after a failed allocation or rasterization too.
    while (count) {
        GroupFrame *f = stack + --count;
        if (f->layer) {
            free(f->layer->px);
            free(f->layer);
        }
        free(f->own_clip);
        if (f->entered) f->group->in_progress = false;
    }
    free(stack);
}

static int
cmp_parent_slot(const void *a, const void *b) {
    const Elem *x = *(Elem *const *)a, *y = *(Elem *const *)b;
    if (x->parent != y->parent) return x->parent < y->parent ? -1 : 1;
    return x->slot < y->slot ? -1 : x->slot > y->slot;
}

// The elements that are drawn, ordered so that the children of a group are
// together and in the order they are drawn in
static bool
sort_elements(VectorDoc *d) {
    if (d->is_sorted) return true;
    void *np = realloc(d->sorted, (d->live + 1) * sizeof(Elem *));
    if (!np) return false;
    d->sorted = np;
    d->nsorted = 0;
    for (size_t i = 0; i < d->count; i++) {
        Elem *e = d->order[i];
        if (e && has_render_order(e)) d->sorted[d->nsorted++] = e;
    }
    qsort(d->sorted, d->nsorted, sizeof(Elem *), cmp_parent_slot);
    for (size_t i = 0; i < d->nsorted; i++) d->sorted[i]->sorted_slot = i;
    d->is_sorted = true;
    return true;
}

static bool
view_is_valid(const VectorDoc *d, VectorView v) {
    return d->width && d->height && v.width > 0 && v.height > 0 && isfinite(v.width) && isfinite(v.height) && v.x >= 0 && v.y >= 0 &&
           isfinite(v.x) && isfinite(v.y) && v.x == floor(v.x) && v.y == floor(v.y) && v.cols && v.rows && v.cols <= INT32_MAX && v.rows <= INT32_MAX;
}

float *
vector_doc_render_view_stereo(VectorDoc *d, VectorView view, VectorRect rect, vector_image_lookup lookup, void *ctx, VectorStereo stereo) {
    if (!view_is_valid(d, view) || !rect.width || !rect.height) return NULL;
    if (rect.x >= view.cols || rect.y >= view.rows || rect.width > view.cols - rect.x || rect.height > view.rows - rect.y) return NULL;
    if (!sort_elements(d)) return NULL;
    const Rect area = {(int)rect.x, (int)rect.y, (int)(rect.x + rect.width), (int)(rect.y + rect.height)};
    Render r = {.doc = d, .sorted = d->sorted, .nsorted = d->nsorted, .area = area, .view = {0, 0, view.cols, view.rows}, .lookup = lookup,
        .lookup_ctx = ctx, .stereo = stereo};
    if (rect_size(area) > SIZE_MAX / (4 * sizeof(float))) return NULL;
    Canvas canvas = {.px = calloc(rect_size(area), 4 * sizeof(float)), .r = area};
    if (!canvas.px) return NULL;
    // The scale is that of the whole image whatever part of it is drawn, and
    // every step from here on works out a pixel from the geometry over that
    // pixel alone, so the part comes out as it does when all of it is drawn.
    // The whole image need not fit in memory, only the part of it held.
    Mat m = {view.width / d->width, 0, 0, view.height / d->height, -view.x, -view.y};
    render_group(&r, &d->root, m, (Target){&canvas, NULL, BLEND_NORMAL});
    if (r.failed) {
        free(canvas.px);
        return NULL;
    }
    // Straight alpha, without gamut or brightness clipping.
    for (size_t i = 0, n = rect_size(area); i < n; i++) {
        float *p = canvas.px + 4 * i;
        if (p[3] > 0) for (unsigned k = 0; k < 3; k++) p[k] /= p[3];
    }
    return canvas.px;
}

static VectorView
whole_view(unsigned width, unsigned height) {
    return (VectorView){width, height, 0, 0, width, height};
}

float *
vector_doc_render_stereo(VectorDoc *d, unsigned width, unsigned height, VectorRect rect, vector_image_lookup lookup, void *ctx, VectorStereo stereo) {
    return vector_doc_render_view_stereo(d, whole_view(width, height), rect, lookup, ctx, stereo);
}

float *
vector_doc_render_linear(VectorDoc *d, unsigned width, unsigned height, VectorRect rect, vector_image_lookup lookup, void *ctx) {
    return vector_doc_render_stereo(d, width, height, rect, lookup, ctx, (VectorStereo){0});
}

uint8_t *
vector_doc_render_view(VectorDoc *d, VectorView view, VectorRect rect, vector_image_lookup lookup, void *ctx) {
    float *linear = vector_doc_render_view_stereo(d, view, rect, lookup, ctx, (VectorStereo){0});
    if (!linear) return NULL;
    const size_t n = (size_t)rect.width * rect.height;
    uint8_t *out = malloc(n * 4);
    if (out) for (size_t i = 0; i < n; i++) {
        out[4*i+3] = (uint8_t)(clampf(linear[4*i+3]) * 255 + .5f);
        for (unsigned k = 0; k < 3; k++) out[4*i+k] = out[4*i+3] ? (uint8_t)(clampf(vector_encode_srgb(linear[4*i+k])) * 255 + .5f) : 0;
    }
    free(linear);
    return out;
}

uint8_t *
vector_doc_render_rect(VectorDoc *d, unsigned width, unsigned height, VectorRect rect, vector_image_lookup lookup, void *ctx) {
    return vector_doc_render_view(d, whole_view(width, height), rect, lookup, ctx);
}

uint8_t *
vector_doc_render(VectorDoc *d, unsigned width, unsigned height, vector_image_lookup lookup, void *ctx) {
    return vector_doc_render_rect(d, width, height, (VectorRect){0, 0, width, height}, lookup, ctx);
}

bool
vector_doc_damage_view(const VectorDoc *d, VectorView view, VectorRect *rect) {
    *rect = (VectorRect){0, 0, view.cols, view.rows};
    if (d->damage_all) return true;
    if (box_is_empty(d->damage) || !view_is_valid(d, view)) return false;
    const double sx = view.width / d->width, sy = view.height / d->height;
    // The pixels the box touches and one more all round, since the box and
    // the edges inside it reach pixels by different arithmetic
    const double x0 = fmax(0, floor(d->damage.x0 * sx - view.x) - 1), y0 = fmax(0, floor(d->damage.y0 * sy - view.y) - 1);
    const double x1 = fmin(view.cols, ceil(d->damage.x1 * sx - view.x) + 1), y1 = fmin(view.rows, ceil(d->damage.y1 * sy - view.y) + 1);
    if (!(x0 < x1 && y0 < y1)) return false;  // all of it is off the part held
    *rect = (VectorRect){(unsigned)x0, (unsigned)y0, (unsigned)(x1 - x0), (unsigned)(y1 - y0)};
    return true;
}

bool
vector_doc_damage(const VectorDoc *d, unsigned width, unsigned height, VectorRect *rect) {
    if (!width || !height) {
        *rect = (VectorRect){0, 0, width, height};
        return d->damage_all;
    }
    return vector_doc_damage_view(d, whole_view(width, height), rect);
}

void
vector_doc_clear_damage(VectorDoc *d) {
    d->damage_all = false;
    d->damage = empty_box;
}

// This is asked of every vector image whenever any image goes, so it looks
// the id up rather than going through the elements
bool
vector_doc_image_changed(VectorDoc *d, uint32_t id) {
    if (vt_is_end(vt_get(&d->image_refs, id))) return false;
    d->damage_all = true;
    return true;
}

// }}}
