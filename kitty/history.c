/*
 * history.c
 * Copyright (C) 2016 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

/*
 * Compressed scrollback
 * =====================
 *
 * The history is three runs of lines, oldest first, contiguous in sequence
 * numbers (line number lnum, 0 being the newest line, has sequence number
 * next_seq - 1 - lnum):
 *
 *   frames   zstd compressed lines, owned by the main thread
 *   pending  blocks of uncompressed lines handed to the compressor thread
 *   hot      blocks of uncompressed lines, the newest ones, owned by the main thread
 *
 * Lines are written into blocks of block_lines lines (128). Without
 * compression every block stays hot, which is kitty's old behaviour. With
 * compression, when there are more than hot_target (2048) hot lines the
 * oldest hot block is handed, without copying, to the compressor thread: a
 * single thread shared by all windows, so output never waits for zstd. It
 * first encodes the block (see encode_line(): about 100 bytes for a line of
 * log output instead of 48 bytes a cell), which is fast and done before any
 * compressing, and the main thread then releases the block's cells; then,
 * in order, it compresses the encoded blocks.
 *
 * The handoff: the compressor only reads the cells of a pending block until
 * it has encoded it, and otherwise only writes into the job's own buffers.
 * The main thread never writes the cells of a pending block: anything that
 * needs a pending line (rendering stores sprite positions in the cells,
 * garbage collection rewrites them) gets a copy of the block made on first
 * use, the shadow, decoded from the encoded lines once the cells are
 * released; garbage collection keeps what it changed encoded instead.
 * Changes are applied when the job is retired. Line attributes are in a
 * separate array that the compressor never reads. Jobs are queued and their
 * results published under one mutex (encoded and done are also
 * acquire/release atomics so the main thread can poll them without the
 * lock). The main thread retires finished jobs, in order, when it next
 * pushes a line, renders, or from the main loop, which the compressor wakes
 * when it runs out of work.
 *
 * There is no limit on how far behind the compressor may fall: if output
 * outpaces it, encoded blocks accumulate (compression_stats() reports them)
 * until it catches up. Pending lines are readable at all times. With a line
 * limit, pending blocks that fall past it before the compressor has started
 * on them are dropped without being compressed.
 *
 * The compressor keeps one zstd stream per window for the life of the
 * window, flushed after every block, so the lines of a retired block are
 * complete in the stream. After the line that brings a frame to frame_bytes
 * (1 MiB) of encoded lines the stream ends the frame and starts the next; a
 * longer line makes a bigger frame. Frames record how many lines they hold,
 * and as they are contiguous a line is found by a binary search over their
 * first lines. Frames exist for random access: reading an old line
 * decompresses only its frame, from the frame start. They
 * are also the unit of eviction when the scrollback has a line limit: lines
 * past the limit are dropped a whole frame at a time and are invisible until
 * then. Each frame starts without zstd's context, which is the measured cost
 * of frames (see the docs of scrollback_compression).
 *
 * Before zstd sees a line it is encoded (see encode_line()) as runs of cells
 * with the same attributes plus one varint per char. The sprite position and
 * the marker bits of a cell are not stored: rendering recomputes them, so
 * compressed lines are marked dirty. The line attributes and a byte of facts
 * about each line (wrapped, length, multicell) stay uncompressed next to each
 * frame, so the dirty and prompt flags can change, is_continued() can be
 * answered and a resize can decide which lines it may reuse without
 * decompressing.
 *
 * The decompression cache has two levels: the encoded lines of a frame,
 * decompressed, and their cells, decoded CHUNK_LINES (128) at a time, as a
 * frame of short lines holds tens of thousands. A pointer into a cached
 * chunk stays valid until either MIN_CACHED_CHUNKS - 1 other chunks have
 * been decoded or the next historybuf_cache_trim(), which the renderer calls
 * after each render to free what was not used since the previous render. No
 * caller holds pointers into more than two chunks at once. A chunk whose
 * cells were changed (garbage collection, deleting lines, the Python line()
 * method) is encoded again into its frame when it leaves the cache, and the
 * frame compressed again by the compressor thread; until then the frame is
 * read from the encoded copy.
 *
 * Nothing here limits how much is kept: the scrollback limit is the
 * scrollback_lines option (unlimited by default) and memory is allocated as
 * lines arrive. When an allocation fails the operation is refused (a line is
 * dropped, a frame that cannot be decompressed reads as blank, lines that
 * cannot be compressed stay encoded) and the terminal carries on.
 */

#include "wcswidth.h"
#include "lineops.h"
#include "charsets.h"
#include "resize.h"
#include "threading.h"
#include <structmember.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <zstd.h>
#include "../3rdparty/ringbuf/ringbuf.h"

extern PyTypeObject Line_Type;

// Defaults, chosen by measurement, see the docs of scrollback_compression in
// kitty/options/definition.py. These are tuning choices, not limits.
#define DEFAULT_HOT_LINES 2048u
#define DEFAULT_BLOCK_LINES 128u
// The tallest multicell char has (1 << SCALE_BITS) - 1 rows. Code that edits
// the rows of a multicell char that crosses the top of the screen writes to
// the newest history lines directly, so at least this many are kept hot.
#define HOT_MIN_LINES (1u << SCALE_BITS)
// A pointer into a compressed line stays valid until this many - 1 other
// chunks have been decoded, see above. Callers hold at most 2.
#define MIN_CACHED_CHUNKS 4u
// Recompressing a frame on the main thread (only when the compressor thread
// cannot be started) uses a fast level
#define MAIN_THREAD_LEVEL 1

static HistoryCompressionParams compression_params = {0};

void
historybuf_set_compression_params(const HistoryCompressionParams *p) {
    compression_params = *p;
}

static int
clamp_level(int level) {
    return MAX(ZSTD_minCLevel(), MIN(level, ZSTD_maxCLevel()));
}

// Facts about a line kept uncompressed for every compressed line
#define INFO_WRAPPED 1u
#define INFO_FULL 2u          // no trailing blank cells
#define INFO_EMPTY 4u         // only blank cells
#define INFO_BLOCKS_REUSE 8u  // has a multiline char or ends in a partial multicell char

static LineAttrs dummy_attrs;

// Cell encoding {{{

// The cells are encoded as 32 bit words, in the native byte order: the
// encoded form never leaves the process.
#define CPU_WORDS (sizeof(CPUCell) / sizeof(uint32_t))
#define GPU_WORDS (sizeof(GPUCell) / sizeof(uint32_t))
#define CELL_WORDS (CPU_WORDS + GPU_WORDS)
static_assert(sizeof(CPUCell) % sizeof(uint32_t) == 0 && sizeof(GPUCell) % sizeof(uint32_t) == 0, "cells must be whole words");

static struct {
    bool ready;
    // the bits that are stored; the others are recomputed by rendering
    uint32_t keep[CELL_WORDS];
    uint32_t wrap[CELL_WORDS], idx[CELL_WORDS], hyperlink[CELL_WORDS], multicell[CELL_WORDS];
} masks;

static void
init_masks(void) {
    if (masks.ready) return;
    CPUCell c;
    GPUCell g;
    memset(&c, 0xff, sizeof(c));
    memset(&g, 0xff, sizeof(g));
    g.sprite_idx = 0;
    g.attrs.mark = 0;
    memcpy(masks.keep, &c, sizeof(c));
    memcpy(masks.keep + CPU_WORDS, &g, sizeof(g));
#define M(field, which)                \
    memset(&c, 0, sizeof(c));          \
    c.field = 1;                       \
    memcpy(masks.which, &c, sizeof(c));
    M(next_char_was_wrapped, wrap);
    M(ch_is_idx, idx);
    M(is_multicell, multicell);
#undef M
    memset(&c, 0, sizeof(c));
    c.hyperlink_id = (hyperlink_id_type)~(hyperlink_id_type)0;
    memcpy(masks.hyperlink, &c, sizeof(c));
    masks.ready = true;
}

// Cells are read and written as 32 bit words through this type, as GPUCell has float fields
typedef uint32_t __attribute__((may_alias)) cell_word;
// word 0 of a CPUCell is ch_and_idx; the attribute words are all the others
#define ATTR_WORDS (CELL_WORDS - 1)
static_assert(ATTR_WORDS <= 16, "the run masks are 16 bit");

// The stored attribute words of a cell. The wrap flag of the last cell is
// stored in the line header, so that it does not start a run of its own.
static inline void
load_attrs(const CPUCell *c, const GPUCell *g, uint32_t *w, bool is_last) {
    const cell_word *cw = (const cell_word *)c, *gw = (const cell_word *)g;
    for (unsigned k = 1; k < CPU_WORDS; k++) w[k - 1] = cw[k] & (is_last ? ~masks.wrap[k] : ~0u);
    for (unsigned k = 0; k < GPU_WORDS; k++) w[CPU_WORDS - 1 + k] = gw[k] & masks.keep[CPU_WORDS + k];
}

static inline bool
cell_is_empty(const CPUCell *c, const GPUCell *g, bool is_last) {
    if (c->ch_and_idx) return false;
    uint32_t w[ATTR_WORDS], nz = 0;
    load_attrs(c, g, w, is_last);
    for (unsigned k = 0; k < ATTR_WORDS; k++) nz |= w[k];
    return !nz;
}

static inline uint8_t *
write_varint(uint8_t *p, uint64_t v) {
    while (v >= 0x80) {
        *p++ = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

static inline const uint8_t *
read_varint(const uint8_t *p, const uint8_t *end, uint64_t *v) {
    uint64_t ans = 0;
    for (unsigned shift = 0; p < end && shift < 64; shift += 7) {
        uint8_t b = *p++;
        ans |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *v = ans;
            return p;
        }
    }
    return NULL;
}

static index_type
text_limit(const CPUCell *c, index_type xnum) {
    index_type limit = xnum;
    while (limit && c[limit - 1].ch_and_idx == BLANK_CHAR) limit--;
    return limit;
}

static uint8_t
line_info(const CPUCell *c, index_type xnum, bool may_have_multicell) {
    uint8_t ans = c[xnum - 1].next_char_was_wrapped ? INFO_WRAPPED : 0;
    const index_type limit = text_limit(c, xnum);
    if (limit == xnum) ans |= INFO_FULL;
    if (!limit) ans |= INFO_EMPTY;
    if (may_have_multicell && limit) {
        if (c[limit - 1].is_multicell && c[limit - 1].x != mcd_x_limit(c + limit - 1) - 1) ans |= INFO_BLOCKS_REUSE;
        else
            for (index_type x = 0; x < limit; x++)
                if (c[x].is_multicell && c[x].scale > 1) {
                    ans |= INFO_BLOCKS_REUSE;
                    break;
                }
    }
    return ans;
}

// Where lines are encoded to: the thread that encodes owns it
typedef struct {
    uint8_t *enc, *runs;
    size_t enc_len, enc_capacity, runs_capacity;
} EncodeBuf;

static void
free_encode_buf(EncodeBuf *b) {
    free(b->enc);
    free(b->runs);
    zero_at_ptr(b);
}

static bool
grow_buffer(uint8_t **buf, size_t *capacity, size_t need) {
    if (need <= *capacity) return true;
    size_t cap = MAX(need, *capacity + *capacity / 2);
    uint8_t *n = realloc(*buf, cap);
    if (!n) return false;
    *buf = n;
    *capacity = cap;
    return true;
}

/*
 * A line is encoded as:
 *   u8 flags: bit 0 is the wrap flag of the last cell
 *   varint n: the cells from n on are zero in every stored bit and are not stored
 *   if n:
 *     varint runs_size: the size of the runs that follow
 *     runs: the n cells split into runs of cells with the same attribute words
 *       varint length, u16 mask of the words that differ from the previous
 *       run (the first run is compared with zero), the words that differ
 *     n varints: the ch_and_idx of each cell
 * Text with a few colour changes per line thus costs a byte or so per cell.
 */
static bool
encode_line(EncodeBuf *z, const CPUCell *cpu, const GPUCell *gpu, index_type xnum, uint8_t *info, uint8_t *has) {
    index_type n = xnum;
    while (n && cell_is_empty(cpu + n - 1, gpu + n - 1, n == xnum)) n--;
    // header + chars + runs, each run at most a varint, the mask and all the words
    const size_t max_run = 10 + sizeof(uint16_t) + ATTR_WORDS * sizeof(uint32_t);
    size_t need, runs_need;
    if (__builtin_mul_overflow((size_t)n, max_run, &runs_need) || __builtin_add_overflow(runs_need, (size_t)n * 5 + 32, &need) ||
        __builtin_add_overflow(need, z->enc_len, &need))
        return false;
    if (!grow_buffer(&z->enc, &z->enc_capacity, need) || !grow_buffer(&z->runs, &z->runs_capacity, runs_need)) return false;
    uint8_t *p = z->enc + z->enc_len;
    *p++ = cpu[xnum - 1].next_char_was_wrapped ? 1 : 0;
    p = write_varint(p, n);
    uint32_t any_attrs[ATTR_WORDS] = {0}, any_ch = 0;
    index_type limit = 0;
    if (n) {
        uint32_t tmpl[ATTR_WORDS] = {0}, w[ATTR_WORDS];
        uint8_t *r = z->runs, *run_len_at = NULL;
        index_type run_start = 0;
        // the chars go after the runs, whose size is not yet known: write them to the end of enc and move them later
        uint8_t *chars = p + runs_need + 10, *c = chars;
        for (index_type x = 0; x < n; x++) {
            load_attrs(cpu + x, gpu + x, w, x == xnum - 1);
            if (!run_len_at || memcmp(w, tmpl, sizeof(w)) != 0) {
                if (run_len_at) r = write_varint(r, x - run_start);
                uint16_t mask = 0;
                for (unsigned k = 0; k < ATTR_WORDS; k++)
                    if (w[k] != tmpl[k]) mask |= 1u << k;
                // a run is its mask and words, then, once it ends, its length
                run_len_at = r;
                memcpy(r, &mask, sizeof(mask));
                r += sizeof(mask);
                for (unsigned k = 0; k < ATTR_WORDS; k++) {
                    if (mask & (1u << k)) {
                        memcpy(r, w + k, sizeof(uint32_t));
                        r += sizeof(uint32_t);
                        any_attrs[k] |= w[k];
                    }
                }
                memcpy(tmpl, w, sizeof(w));
                run_start = x;
            }
            const uint32_t ch = cpu[x].ch_and_idx;
            any_ch |= ch;
            if (ch) limit = x + 1;
            c = write_varint(c, ch);
        }
        r = write_varint(r, n - run_start);
        const size_t runs_size = r - z->runs;
        p = write_varint(p, runs_size);
        memcpy(p, z->runs, runs_size);
        p += runs_size;
        memmove(p, chars, c - chars);
        p += c - chars;
    }
    z->enc_len = p - z->enc;
    bool may_have_multicell = false;
    uint8_t h = (any_ch & masks.idx[0]) ? HISTORY_HAS_TEXT_IDX : 0;
    for (unsigned k = 0; k < ATTR_WORDS; k++) {
        if (any_attrs[k] & masks.multicell[k + 1]) may_have_multicell = true;
        if (any_attrs[k] & masks.hyperlink[k + 1]) h |= HISTORY_HAS_HYPERLINKS;
    }
    *has |= h;
    uint8_t ans = cpu[xnum - 1].next_char_was_wrapped ? INFO_WRAPPED : 0;
    if (limit == xnum) ans |= INFO_FULL;
    if (!limit) ans |= INFO_EMPTY;
    if (may_have_multicell) ans |= line_info(cpu, xnum, true) & INFO_BLOCKS_REUSE;
    *info = ans;
    return true;
}

static const uint8_t *
decode_line(const uint8_t *p, const uint8_t *end, CPUCell *cpu, GPUCell *gpu, index_type xnum) {
    if (p >= end) return NULL;
    const uint8_t flags = *p++;
    uint64_t n, runs_size;
    if (!(p = read_varint(p, end, &n)) || n > xnum) return NULL;
    if (n < xnum) {
        zero_at_ptr_count(cpu + n, xnum - n);
        zero_at_ptr_count(gpu + n, xnum - n);
    }
    if (n) {
        if (!(p = read_varint(p, end, &runs_size)) || runs_size > (uint64_t)(end - p)) return NULL;
        const uint8_t *r = p, *rend = p + runs_size, *c = rend;
        uint32_t tmpl[ATTR_WORDS] = {0};
        index_type x = 0;
        while (x < n) {
            uint16_t mask;
            uint64_t len;
            if ((size_t)(rend - r) < sizeof(mask)) return NULL;
            memcpy(&mask, r, sizeof(mask));
            r += sizeof(mask);
            for (unsigned k = 0; k < ATTR_WORDS; k++) {
                if (mask & (1u << k)) {
                    if ((size_t)(rend - r) < sizeof(uint32_t)) return NULL;
                    memcpy(tmpl + k, r, sizeof(uint32_t));
                    r += sizeof(uint32_t);
                }
            }
            if (!(r = read_varint(r, rend, &len)) || !len || len > n - x) return NULL;
            for (const index_type limit = x + (index_type)len; x < limit; x++) {
                uint64_t ch;
                if (!(c = read_varint(c, end, &ch)) || ch > UINT32_MAX) return NULL;
                cell_word *cw = (cell_word *)(cpu + x), *gw = (cell_word *)(gpu + x);
                cw[0] = (uint32_t)ch;
                for (unsigned k = 1; k < CPU_WORDS; k++) cw[k] = tmpl[k - 1];
                for (unsigned k = 0; k < GPU_WORDS; k++) gw[k] = tmpl[CPU_WORDS - 1 + k];
            }
        }
        p = c;
    }
    if (flags & 1) cpu[xnum - 1].next_char_was_wrapped = true;
    return p;
}

// The end of the encoded line at p, NULL if it is corrupt
static const uint8_t *
skip_line(const uint8_t *p, const uint8_t *end) {
    if (p >= end) return NULL;
    p++;
    uint64_t n, runs_size;
    if (!(p = read_varint(p, end, &n))) return NULL;
    if (!n) return p;
    if (!(p = read_varint(p, end, &runs_size)) || runs_size > (uint64_t)(end - p)) return NULL;
    p += runs_size;
    for (uint64_t i = 0; i < n; i++) {
        while (p < end && (*p & 0x80)) p++;
        if (p >= end) return NULL;
        p++;
    }
    return p;
}

static void
clear_trailing_blanks(CPUCell *cpu, GPUCell *gpu, index_type xnum) {
    const index_type limit = text_limit(cpu, xnum);
    zero_at_ptr_count(cpu + limit, xnum - limit);
    zero_at_ptr_count(gpu + limit, xnum - limit);
}
// }}}
static size_t
initial_pagerhist_ringbuf_sz(size_t pagerhist_sz) {
    return MIN(1024u * 1024u, pagerhist_sz);
}

static PagerHistoryBuf *
alloc_pagerhist(size_t pagerhist_sz) {
    PagerHistoryBuf *ph;
    if (!pagerhist_sz) return NULL;
    ph = calloc(1, sizeof(PagerHistoryBuf));
    if (!ph) return NULL;
    size_t sz = initial_pagerhist_ringbuf_sz(pagerhist_sz);
    ph->ringbuf = ringbuf_new(sz);
    if (!ph->ringbuf) {
        free(ph);
        return NULL;
    }
    ph->maximum_size = pagerhist_sz;
    return ph;
}

static void
free_pagerhist(HistoryBuf *self) {
    if (self->pagerhist && self->pagerhist->ringbuf) ringbuf_free((ringbuf_t *)&self->pagerhist->ringbuf);
    free(self->pagerhist);
    self->pagerhist = NULL;
}

static bool
pagerhist_extend(PagerHistoryBuf *ph, size_t minsz) {
    size_t buffer_size = ringbuf_capacity(ph->ringbuf);
    if (buffer_size >= ph->maximum_size) return false;
    size_t newsz = MIN(ph->maximum_size, buffer_size + MAX(1024u * 1024u, minsz));
    ringbuf_t newbuf = ringbuf_new(newsz);
    if (!newbuf) return false;
    size_t count = ringbuf_bytes_used(ph->ringbuf);
    if (count) ringbuf_copy(newbuf, ph->ringbuf, count);
    ringbuf_free((ringbuf_t *)&ph->ringbuf);
    ph->ringbuf = newbuf;
    return true;
}

static void
pagerhist_clear(HistoryBuf *self) {
    if (self->pagerhist && self->pagerhist->ringbuf) {
        ringbuf_reset(self->pagerhist->ringbuf);
        size_t rsz = initial_pagerhist_ringbuf_sz(self->pagerhist->maximum_size);
        void *rbuf = ringbuf_new(rsz);
        if (rbuf) {
            ringbuf_free((ringbuf_t *)&self->pagerhist->ringbuf);
            self->pagerhist->ringbuf = rbuf;
        }
    }
}

static bool
pagerhist_write_bytes(PagerHistoryBuf *ph, const uint8_t *buf, size_t sz) {
    if (sz > ph->maximum_size) return false;
    if (!sz) return true;
    size_t space_in_ringbuf = ringbuf_bytes_free(ph->ringbuf);
    if (sz > space_in_ringbuf) pagerhist_extend(ph, sz);
    ringbuf_memcpy_into(ph->ringbuf, buf, sz);
    return true;
}

static bool
pagerhist_ensure_start_is_valid_utf8(PagerHistoryBuf *ph) {
    uint8_t scratch[8];
    size_t num = ringbuf_memcpy_from(scratch, ph->ringbuf, arraysz(scratch));
    uint32_t codep;
    UTF8State state = UTF8_ACCEPT;
    size_t count = 0;
    size_t last_reject_at = 0;
    while (count < num) {
        decode_utf8(&state, &codep, scratch[count++]);
        if (state == UTF8_ACCEPT) break;
        if (state == UTF8_REJECT) {
            state = UTF8_ACCEPT;
            last_reject_at = count;
        }
    }
    if (last_reject_at) {
        ringbuf_memmove_from(scratch, ph->ringbuf, last_reject_at);
        return true;
    }
    return false;
}

static bool
pagerhist_write_ucs4(PagerHistoryBuf *ph, const Py_UCS4 *buf, size_t sz) {
    uint8_t scratch[4];
    for (size_t i = 0; i < sz; i++) {
        unsigned int num = encode_utf8(buf[i], (char *)scratch);
        if (!pagerhist_write_bytes(ph, scratch, num)) return false;
    }
    return true;
}



// Types {{{

struct LineBlock {
    CPUCell *cpu;
    GPUCell *gpu;
    LineAttrs *attrs;
    void *mem;
    size_t mem_size;
    index_type capacity, num_lines;
    uint64_t first_seq;
    // while pending
    struct CompressJob *job;
    CPUCell *shadow_cpu;
    GPUCell *shadow_gpu;
    bool shadow_modified;
    // the lines, changed by garbage collection, encoded, when there is no shadow
    EncodeBuf reenc;
    uint8_t *reenc_info;
    index_type blank_clear_lines;
};

typedef enum { JOB_BLOCK, JOB_ENCODED, JOB_DATA } JobKind;

// A run of a block's lines that went into one frame
typedef struct {
    index_type lines;
    size_t raw, out;
    bool starts, ends;
} JobSegment;

typedef struct CompressJob {
    struct CompressJob *queue_next; // in the compressor's queue, under W.lock
    struct CompressJob *next;       // in the buffer's list of jobs, main thread only
    HistoryCompression *z;
    JobKind kind;
    // Input, not changed once queued. JOB_BLOCK: the cells of a pending block.
    // JOB_ENCODED: encoded lines. JOB_DATA: a compressed frame to compress again.
    const CPUCell *cpu;
    const GPUCell *gpu;
    index_type num_lines, xnum;
    uint8_t *in;
    size_t in_len, raw_size;
    uint64_t frame_id, frame_version;
    size_t frame_bytes;
    int level;
    bool force_new_frame;
    LineBlock *block;
    // Output, written by the compressor before done is set
    EncodeBuf enc;
    uint8_t *info, has;
    uint8_t *out;
    size_t out_len, out_capacity;
    size_t *line_ends; // JOB_BLOCK: where each encoded line ends
    JobSegment *segs;
    size_t num_segs, segs_capacity;
    bool failed;
    // JOB_BLOCK: the lines are encoded, the compressor no longer reads the cells
    atomic_bool encoded;
    atomic_bool done;
    int queue; // the queue it is in, -1 for none, under W.lock
    bool cancelled; // under W.lock
} CompressJob;
// }}}

// The compressor thread {{{

// A frame ends after the line that brings its encoded size to this. Frames
// are the unit of random access and of eviction, and each starts without
// zstd's context; see the measurements in the docs of scrollback_compression.
#define DEFAULT_FRAME_BYTES (1024u * 1024u)

typedef struct {
    // compressor thread only, until in_flight is 0
    ZSTD_CCtx *cctx;
    size_t frame_raw;
    bool frame_open;
    // under W.lock
    size_t in_flight;
    atomic_size_t cctx_bytes;
} StreamState;

static struct {
    pthread_mutex_t lock;
    // work[0] wakes the encoder thread, work[1] the compressor thread
    pthread_cond_t work[2], idle;
    // Encoding blocks goes first, as it frees their cells (about 48 bytes a
    // cell) and is fast; then compressing the encoded blocks, in order; then
    // compressing frames again.
#define Q_ENCODE 0
#define Q_COMPRESS 1
#define Q_OTHER 2
#define NUM_QUEUES 3
    CompressJob *head[NUM_QUEUES], *tail[NUM_QUEUES];
    bool started, failed_to_start;
    ZSTD_CCtx *aux;
    ZSTD_DCtx *dctx;
    uint8_t *dec;
    size_t dec_capacity;
    uint64_t cpu_ns, jobs, bytes_in, bytes_out;
    void (*wakeup)(void);
} W = {.lock = PTHREAD_MUTEX_INITIALIZER, .work = {PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER}, .idle = PTHREAD_COND_INITIALIZER};

static StreamState *stream_of(HistoryCompression *z);

void
historybuf_set_wakeup(void (*wakeup)(void)) {
    pthread_mutex_lock(&W.lock);
    W.wakeup = wakeup;
    pthread_mutex_unlock(&W.lock);
}

static bool
grow_out(CompressJob *j, size_t extra) {
    size_t need;
    if (__builtin_add_overflow(j->out_len, extra, &need)) return false;
    return grow_buffer(&j->out, &j->out_capacity, need);
}

static bool
stream_compress(ZSTD_CCtx *c, CompressJob *j, const uint8_t *src, size_t sz, ZSTD_EndDirective d) {
    ZSTD_inBuffer in = {src, sz, 0};
    for (;;) {
        if (!grow_out(j, ZSTD_compressBound(in.size - in.pos) + 64)) return false;
        ZSTD_outBuffer out = {j->out, j->out_capacity, j->out_len};
        size_t r = ZSTD_compressStream2(c, &out, &in, d);
        j->out_len = out.pos;
        if (ZSTD_isError(r)) return false;
        if (d == ZSTD_e_continue ? in.pos == in.size : r == 0) return true;
    }
}

static ZSTD_CCtx *
new_cctx(void) {
    ZSTD_CCtx *c = ZSTD_createCCtx();
    // How far back zstd looks for matches, which sets most of the memory a
    // context uses: 64 KiB measured the same ratio as the level defaults on
    // terminal output (see the docs). zstd bounds its hash and chain tables by it.
    if (c) ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, 16);
    return c;
}

static void
set_level(ZSTD_CCtx *c, int level) {
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    // From level 4 up zstd's search tables are larger than a 64 KiB window can
    // use (tens of MB at level 18); bounding them by the window measured no
    // change in ratio, see the docs. 0 restores the level's own value.
    const int log = level >= 4 ? 17 : 0;
    ZSTD_CCtx_setParameter(c, ZSTD_c_hashLog, log);
    ZSTD_CCtx_setParameter(c, ZSTD_c_chainLog, log);
}

static bool
add_segment(CompressJob *j, index_type lines, size_t raw, size_t out, bool starts, bool ends) {
    if (j->num_segs == j->segs_capacity) {
        size_t cap = MAX(4u, j->segs_capacity * 2);
        JobSegment *n = realloc(j->segs, cap * sizeof(JobSegment));
        if (!n) return false;
        j->segs = n;
        j->segs_capacity = cap;
    }
    j->segs[j->num_segs++] = (JobSegment){.lines = lines, .raw = raw, .out = out, .starts = starts, .ends = ends};
    return true;
}

static bool
encode_block(CompressJob *j) {
    if (!(j->info = malloc(MAX(1u, j->num_lines))) || !(j->line_ends = malloc(MAX(1u, j->num_lines) * sizeof(size_t)))) return false;
    for (index_type i = 0; i < j->num_lines; i++) {
        if (!encode_line(&j->enc, j->cpu + (size_t)i * j->xnum, j->gpu + (size_t)i * j->xnum, j->xnum, j->info + i, &j->has)) return false;
        j->line_ends[i] = j->enc.enc_len;
    }
    // the encoded lines may wait a while for the compressor: keep only them
    free(j->enc.runs);
    j->enc.runs = NULL;
    j->enc.runs_capacity = 0;
    if (j->enc.enc_len && j->enc.enc_len < j->enc.enc_capacity) {
        uint8_t *n = realloc(j->enc.enc, j->enc.enc_len);
        if (n) {
            j->enc.enc = n;
            j->enc.enc_capacity = j->enc.enc_len;
        }
    }
    return true;
}

static bool
compress_block(CompressJob *j, StreamState *s) {
    if (!s->cctx && !(s->cctx = new_cctx())) return false;
    if (j->force_new_frame) s->frame_open = false;
    index_type seg_lines = 0;
    size_t seg_raw_start = 0, seg_out_start = 0;
    bool seg_starts = false;
    for (index_type i = 0; i < j->num_lines; i++) {
        const size_t before = i ? j->line_ends[i - 1] : 0;
        if (!s->frame_open) {
            ZSTD_CCtx_reset(s->cctx, ZSTD_reset_session_only);
            set_level(s->cctx, j->level);
            s->frame_open = true;
            s->frame_raw = 0;
            seg_starts = true;
        }
        s->frame_raw += j->line_ends[i] - before;
        seg_lines++;
        if (s->frame_raw >= j->frame_bytes) {
            // end the frame after this line
            if (!stream_compress(s->cctx, j, j->enc.enc + seg_raw_start, j->line_ends[i] - seg_raw_start, ZSTD_e_end)) return false;
            if (!add_segment(j, seg_lines, j->line_ends[i] - seg_raw_start, j->out_len - seg_out_start, seg_starts, true)) return false;
            s->frame_open = false;
            seg_lines = 0;
            seg_starts = false;
            seg_raw_start = j->line_ends[i];
            seg_out_start = j->out_len;
        }
    }
    if (seg_lines) {
        // flush, so that the block's lines are complete in the stream
        if (!stream_compress(s->cctx, j, j->enc.enc + seg_raw_start, j->enc.enc_len - seg_raw_start, ZSTD_e_flush)) return false;
        if (!add_segment(j, seg_lines, j->enc.enc_len - seg_raw_start, j->out_len - seg_out_start, seg_starts, false)) return false;
    }
    atomic_store_explicit(&s->cctx_bytes, ZSTD_sizeof_CCtx(s->cctx), memory_order_relaxed);
    return true;
}

// Run a job, or for JOB_BLOCK one of its stages. Returns true if the job is done.
static bool
run_job(CompressJob *j) {
    const uint8_t *src = j->in;
    size_t src_len = j->in_len;
    if (j->kind == JOB_BLOCK) {
        if (!atomic_load_explicit(&j->encoded, memory_order_relaxed)) {
            if (!encode_block(j)) {
                // the main thread keeps the lines from the cells, still there
                j->failed = true;
                free_encode_buf(&j->enc);
                return true;
            }
            atomic_store_explicit(&j->encoded, true, memory_order_release);
            return false;
        }
        StreamState *s = stream_of(j->z);
        if (!compress_block(j, s)) {
            // the lines stay encoded and the stream starts a new frame
            j->failed = true;
            j->num_segs = 0;
            s->frame_open = false;
        }
        return true;
    }
    if (j->kind == JOB_DATA) {
        if ((!W.dctx && !(W.dctx = ZSTD_createDCtx())) || !grow_buffer(&W.dec, &W.dec_capacity, j->raw_size)) {
            j->failed = true;
            return true;
        }
        ZSTD_DCtx_reset(W.dctx, ZSTD_reset_session_only);
        ZSTD_inBuffer in = {j->in, j->in_len, 0};
        ZSTD_outBuffer out = {W.dec, j->raw_size, 0};
        while (out.pos < out.size) {
            const size_t bi = in.pos, bo = out.pos;
            if (ZSTD_isError(ZSTD_decompressStream(W.dctx, &out, &in)) || (in.pos == bi && out.pos == bo)) break;
        }
        if (out.pos != j->raw_size) {
            j->failed = true;
            return true;
        }
        src = W.dec;
        src_len = j->raw_size;
    }
    if (!W.aux && !(W.aux = new_cctx())) {
        j->failed = true;
        return true;
    }
    ZSTD_CCtx_reset(W.aux, ZSTD_reset_session_only);
    set_level(W.aux, j->level);
    if (!stream_compress(W.aux, j, src, src_len, ZSTD_e_end)) j->failed = true;
    return true;
}

static uint64_t
thread_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void
enqueue(CompressJob *j, int q) {
    j->queue = q;
    j->queue_next = NULL;
    if (W.tail[q]) W.tail[q]->queue_next = j;
    else W.head[q] = j;
    W.tail[q] = j;
}

static void
unqueue(CompressJob *j) {
    const int q = j->queue;
    CompressJob *prev = NULL;
    for (CompressJob *x = W.head[q]; x; prev = x, x = x->queue_next) {
        if (x == j) {
            if (prev) prev->queue_next = x->queue_next;
            else W.head[q] = x->queue_next;
            if (W.tail[q] == x) W.tail[q] = prev;
            break;
        }
    }
    j->queue = -1;
}

static void
finish_job(CompressJob *j) {
    // under W.lock; after this the main thread may free the job
    stream_of(j->z)->in_flight--;
    atomic_store_explicit(&j->done, true, memory_order_release);
    pthread_cond_broadcast(&W.idle);
}

// The threads run at low priority, so that they use the CPU time the threads
// that parse and draw output, and other programs, leave: on Linux SCHED_IDLE,
// or the lowest nice value if that is refused; on macOS the utility QoS
// class. The background class starved them during a flood of output: the
// encoder fell behind, uncompressed blocks piled up to 30 GB and output
// slowed to 18 MB/s, against 2.9 GB and 70 MB/s with utility (see the docs of
// scrollback_compression).
static void
lower_priority(void) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
#ifndef SCHED_IDLE
#define SCHED_IDLE 5 // from linux/sched.h, glibc declares it only with _GNU_SOURCE
#endif
    struct sched_param sp = {0};
    // on Linux setpriority() with who 0 applies to the calling thread
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) != 0) setpriority(PRIO_PROCESS, 0, 19);
#endif
    // elsewhere (the BSDs) setpriority() would apply to the whole process: the threads keep the default priority
}

// Two threads: the encoder encodes blocks, which frees their cells, so it has
// a core of its own and keeps up with output; the compressor compresses the
// encoded blocks in order, and frames to compress again.
static void *
compressor_main(void *arg) {
    const bool is_encoder = arg != NULL;
    set_thread_name(is_encoder ? "ScrollbackEnc" : "ScrollbackZstd");
    lower_priority();
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    pthread_mutex_lock(&W.lock);
    for (;;) {
        CompressJob *j = NULL;
        for (int q = is_encoder ? Q_ENCODE : Q_COMPRESS; q < (is_encoder ? Q_ENCODE + 1 : NUM_QUEUES) && !j; q++) {
            if ((j = W.head[q])) unqueue(j);
        }
        if (!j) {
            pthread_cond_wait(&W.work[is_encoder ? 0 : 1], &W.lock);
            continue;
        }
        pthread_mutex_unlock(&W.lock);
        const uint64_t start = thread_cpu_ns();
        const bool done = run_job(j);
        const uint64_t used = thread_cpu_ns() - start;
        pthread_mutex_lock(&W.lock);
        W.cpu_ns += used;
        if (!done) {
            // encoded: compress it in its turn, unless cancelled meanwhile
            if (j->cancelled) finish_job(j);
            else {
                enqueue(j, Q_COMPRESS);
                pthread_cond_signal(&W.work[1]);
            }
            continue;
        }
        W.jobs++;
        W.bytes_in += j->kind == JOB_BLOCK ? j->enc.enc_len : j->in_len;
        W.bytes_out += j->out_len;
        finish_job(j);
        void (*wakeup)(void) = W.wakeup;
        if (!W.head[Q_ENCODE] && !W.head[Q_COMPRESS] && !W.head[Q_OTHER] && wakeup) {
            pthread_mutex_unlock(&W.lock);
            wakeup();
            pthread_mutex_lock(&W.lock);
        }
    }
    return NULL;
}

static bool
start_thread(bool is_encoder) {
    pthread_t t;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    const int ret = pthread_create(&t, &attr, compressor_main, is_encoder ? (void *)&W : NULL);
    pthread_attr_destroy(&attr);
    if (ret != 0) log_error("Could not start the scrollback compressor threads, compressing on the main thread: %s", strerror(ret));
    return ret == 0;
}

static bool
start_compressor(void) {
    if (W.started) return true;
    if (W.failed_to_start) return false;
    if (!start_thread(true) || !start_thread(false)) {
        // an encoder that did start never gets work
        W.failed_to_start = true;
        return false;
    }
    W.started = true;
    return true;
}

static void
submit_job(CompressJob *j) {
    j->queue = -1;
    atomic_init(&j->encoded, false);
    pthread_mutex_lock(&W.lock);
    if (start_compressor()) {
        stream_of(j->z)->in_flight++;
        enqueue(j, j->kind == JOB_BLOCK ? Q_ENCODE : Q_OTHER);
        pthread_cond_signal(&W.work[j->kind == JOB_BLOCK ? 0 : 1]);
        pthread_mutex_unlock(&W.lock);
        return;
    }
    // No thread: compress here, at a fast level for recompression. W's scratch
    // and the stream state are then only used by this thread.
    if (j->kind != JOB_BLOCK && j->level > MAIN_THREAD_LEVEL) j->level = MAIN_THREAD_LEVEL;
    while (!run_job(j));
    atomic_store_explicit(&j->done, true, memory_order_release);
    pthread_mutex_unlock(&W.lock);
}

// Cancel the jobs among these the threads have not started on, provided the
// stream can start a new frame at next, the job after them: one not yet
// started, or none (NULL). Returns false if nothing could be cancelled.
static bool
cancel_queued_jobs_before(CompressJob **jobs, size_t n, CompressJob *next) {
    pthread_mutex_lock(&W.lock);
    bool ans = !next || next->queue >= 0;
    if (ans) {
        if (next) next->force_new_frame = true;
        for (size_t i = 0; i < n; i++) {
            if (jobs[i]->queue >= 0) {
                jobs[i]->cancelled = true;
                unqueue(jobs[i]);
                finish_job(jobs[i]);
            }
        }
    }
    pthread_mutex_unlock(&W.lock);
    return ans;
}

// Cancel a job; with wait, also wait for the compressor to stop using its input
static void
cancel_job(CompressJob *j, bool wait) {
    pthread_mutex_lock(&W.lock);
    j->cancelled = true;
    if (j->queue >= 0) {
        unqueue(j);
        finish_job(j);
    } else if (wait) {
        while (!atomic_load_explicit(&j->done, memory_order_acquire)) pthread_cond_wait(&W.idle, &W.lock);
    }
    pthread_mutex_unlock(&W.lock);
}

static void
wait_for_compressor(HistoryCompression *z) {
    if (!z) return;
    pthread_mutex_lock(&W.lock);
    while (stream_of(z)->in_flight) pthread_cond_wait(&W.idle, &W.lock);
    pthread_mutex_unlock(&W.lock);
}

static void
free_job(CompressJob *j) {
    if (!j) return;
    free_encode_buf(&j->enc);
    free(j->info);
    free(j->out);
    free(j->in);
    free(j->segs);
    free(j->line_ends);
    free(j);
}
// }}}
// Blocks {{{

// The cells of a block, mmapped to avoid fragmenting the malloc pool, see
// https://github.com/kovidgoyal/kitty/pull/10254. Freed cells are kept for the
// next blocks, as new memory costs page faults on every line written: while
// output flows, blocks are freed as fast as they are made.
static size_t
cells_size(HistoryBuf *self) {
    return (size_t)self->block_lines * self->xnum * (sizeof(CPUCell) + sizeof(GPUCell));
}

static bool
alloc_cells(HistoryBuf *self, LineBlock *b) {
    if (b->capacity == self->block_lines && self->num_spare_cells) {
        b->mem = self->spare_cells[--self->num_spare_cells];
        b->mem_size = cells_size(self);
    } else {
        size_t cpu_sz, gpu_sz, total;
        if (__builtin_mul_overflow((size_t)b->capacity * self->xnum, sizeof(CPUCell), &cpu_sz) ||
            __builtin_mul_overflow((size_t)b->capacity * self->xnum, sizeof(GPUCell), &gpu_sz) || __builtin_add_overflow(cpu_sz, gpu_sz, &total))
            return false;
        char *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) return false;
        b->mem = mem;
        b->mem_size = total;
    }
    b->cpu = (CPUCell *)b->mem;
    b->gpu = (GPUCell *)((char *)b->mem + (size_t)b->capacity * self->xnum * sizeof(CPUCell));
    return true;
}

static void
unmap_cells(void *mem, size_t sz) {
    if (munmap(mem, sz) != 0) log_error("Failed to unmap history block: %s", strerror(errno));
}

static void
release_cells(HistoryBuf *self, LineBlock *b) {
    if (!b->mem) return;
    bool kept = false;
    if (b->mem_size == cells_size(self)) {
        if (self->num_spare_cells == self->spare_cells_capacity) {
            size_t cap = MAX(8u, self->spare_cells_capacity * 2);
            void **n = realloc(self->spare_cells, cap * sizeof(void *));
            if (n) {
                self->spare_cells = n;
                self->spare_cells_capacity = cap;
            }
        }
        if (self->num_spare_cells < self->spare_cells_capacity) {
            self->spare_cells[self->num_spare_cells++] = b->mem;
            kept = true;
        }
    }
    if (!kept) unmap_cells(b->mem, b->mem_size);
    b->mem = NULL;
    b->mem_size = 0;
    b->cpu = NULL;
    b->gpu = NULL;
}

static void
free_spare_cells(HistoryBuf *self, size_t keep) {
    while (self->num_spare_cells > keep) unmap_cells(self->spare_cells[--self->num_spare_cells], cells_size(self));
}

static LineBlock *
alloc_block(HistoryBuf *self) {
    LineBlock *b = calloc(1, sizeof(LineBlock));
    if (!b) return NULL;
    b->capacity = self->block_lines;
    b->first_seq = self->next_seq;
    if (!(b->attrs = malloc((size_t)b->capacity * sizeof(LineAttrs))) || !alloc_cells(self, b)) {
        free(b->attrs);
        free(b);
        return NULL;
    }
    return b;
}

static void
release_block_memory(LineBlock *b) {
    if (b->mem && munmap(b->mem, b->mem_size) != 0) log_error("Failed to unmap history block: %s", strerror(errno));
    free(b->shadow_cpu);
    free(b->shadow_gpu);
    free_encode_buf(&b->reenc);
    free(b->reenc_info);
    free(b->attrs);
    free(b);
}

static void
free_block(HistoryBuf *self, LineBlock *b) {
    release_cells(self, b);
    release_block_memory(b);
}

static inline LineBlock *
block_at(HistoryBuf *self, size_t i) {
    return self->blocks[(self->blocks_head + i) % self->blocks_capacity];
}

static bool
reserve_blocks(HistoryBuf *self, size_t extra) {
    if (self->num_blocks + extra <= self->blocks_capacity) return true;
    size_t cap = MAX(16u, self->blocks_capacity * 2);
    while (cap < self->num_blocks + extra) cap *= 2;
    if (cap > SIZE_MAX / sizeof(LineBlock *)) return false;
    LineBlock **n = malloc(cap * sizeof(LineBlock *));
    if (!n) return false;
    for (size_t i = 0; i < self->num_blocks; i++) n[i] = block_at(self, i);
    free(self->blocks);
    self->blocks = n;
    self->blocks_head = 0;
    self->blocks_capacity = cap;
    return true;
}

static bool
append_block(HistoryBuf *self, LineBlock *b) {
    if (!reserve_blocks(self, 1)) return false;
    self->blocks[(self->blocks_head + self->num_blocks++) % self->blocks_capacity] = b;
    return true;
}

static bool
prepend_block(HistoryBuf *self, LineBlock *b) {
    if (!reserve_blocks(self, 1)) return false;
    self->blocks_head = (self->blocks_head + self->blocks_capacity - 1) % self->blocks_capacity;
    self->blocks[self->blocks_head] = b;
    self->num_blocks++;
    self->block_hint = 0;
    return true;
}

static void
drop_front_block(HistoryBuf *self) {
    LineBlock *b = block_at(self, 0);
    self->blocks_head = (self->blocks_head + 1) % self->blocks_capacity;
    self->num_blocks--;
    self->block_hint = 0;
    if (self->num_pending) {
        self->num_pending--;
        self->pending_lines -= b->num_lines;
        if (self->pending_released) self->pending_released--;
    } else self->hot_lines -= b->num_lines;
    free_block(self, b);
}

static void
drop_back_block(HistoryBuf *self) {
    LineBlock *b = block_at(self, self->num_blocks - 1);
    self->num_blocks--;
    self->block_hint = 0;
    self->hot_lines -= b->num_lines;
    free_block(self, b);
}

static inline uint64_t
blocks_first_seq(HistoryBuf *self) {
    return self->num_blocks ? block_at(self, 0)->first_seq : self->next_seq;
}

static inline uint64_t
hot_first_seq(HistoryBuf *self) {
    return self->num_pending < self->num_blocks ? block_at(self, self->num_pending)->first_seq : self->next_seq;
}

// index of the block holding the line with sequence number seq, which must be in a block
static size_t
find_block(HistoryBuf *self, uint64_t seq, index_type *offset) {
    size_t i = self->block_hint;
    LineBlock *b;
    if (!(i < self->num_blocks && (b = block_at(self, i))->first_seq <= seq && seq < b->first_seq + b->num_lines)) {
        size_t lo = 0, hi = self->num_blocks;
        while (hi - lo > 1) {
            size_t mid = lo + (hi - lo) / 2;
            if (block_at(self, mid)->first_seq <= seq) lo = mid;
            else hi = mid;
        }
        i = self->block_hint = lo;
        b = block_at(self, i);
    }
    *offset = (index_type)(seq - b->first_seq);
    return i;
}

static bool decode_job_lines(HistoryBuf *self, struct CompressJob *j, index_type n, CPUCell *cpu, GPUCell *gpu);

static bool
ensure_shadow(HistoryBuf *self, LineBlock *b) {
    if (b->shadow_cpu) return true;
    const size_t n = (size_t)b->capacity * self->xnum;
    CPUCell *c = malloc(n * sizeof(CPUCell));
    GPUCell *g = malloc(n * sizeof(GPUCell));
    if (!c || !g) {
        free(c);
        free(g);
        return false;
    }
    if (b->reenc.enc) {
        // lines changed by garbage collection
        const uint8_t *p = b->reenc.enc, *end = p + b->reenc.enc_len;
        for (index_type i = 0; i < b->num_lines && p; i++) p = decode_line(p, end, c + (size_t)i * self->xnum, g + (size_t)i * self->xnum, self->xnum);
        free_encode_buf(&b->reenc);
        free(b->reenc_info);
        b->reenc_info = NULL;
        b->shadow_modified = true;
    } else if (b->cpu) {
        // reading the cells while the compressor reads them is fine, neither writes them
        memcpy(c, b->cpu, (size_t)b->num_lines * self->xnum * sizeof(CPUCell));
        memcpy(g, b->gpu, (size_t)b->num_lines * self->xnum * sizeof(GPUCell));
    } else if (!decode_job_lines(self, b->job, b->num_lines, c, g)) {
        // the cells were released once the compressor had encoded them
        free(c);
        free(g);
        return false;
    }
    for (index_type i = 0; i < b->blank_clear_lines; i++) clear_trailing_blanks(c + (size_t)i * self->xnum, g + (size_t)i * self->xnum, self->xnum);
    // marks and sprite positions are recomputed when the lines are rendered, as for compressed lines
    for (size_t i = 0; i < (size_t)b->num_lines * self->xnum; i++) g[i].attrs.mark = 0;
    for (index_type i = 0; i < b->num_lines; i++) b->attrs[i].has_dirty_text = true;
    b->shadow_cpu = c;
    b->shadow_gpu = g;
    return true;
}

// The cells the main thread may read and write for a block
static inline CPUCell *
block_cpu(LineBlock *b) {
    return b->shadow_cpu ? b->shadow_cpu : b->cpu;
}

static inline GPUCell *
block_gpu(LineBlock *b) {
    return b->shadow_gpu ? b->shadow_gpu : b->gpu;
}
// }}}

// Frames {{{

// Compressed lines are decoded into cells this many at a time: a frame can hold many thousands of lines
#define CHUNK_LINES 128u

typedef struct HistoryFrame {
    uint64_t id, first_seq;
    index_type num_lines;
    // lines at the start of the frame whose trailing blank cells are cleared when decoded
    index_type blank_clear_lines;
    uint8_t *data; // zstd frame, possibly not ended, possibly with lines past num_lines
    size_t size, capacity, data_raw;
    uint8_t *tail; // encoded lines the compressor could not compress, after those in data
    size_t tail_len;
    LineAttrs *attrs;
    uint8_t *info;
    index_type lines_capacity;
    // The encoded lines, when decompressed: raw_len bytes, chunk_offsets[c] is
    // where line c * CHUNK_LINES starts
    uint8_t *raw;
    size_t raw_len, raw_capacity;
    size_t *chunk_offsets;
    size_t num_chunk_offsets, chunk_offsets_capacity;
    uint64_t raw_last_used, version;
    // while not NULL the frame's content is this job's input, encoded lines: data is stale
    struct CompressJob *replacement;
    bool open, content_modified, raw_modified;
    uint8_t has; // HISTORY_HAS_*
} HistoryFrame;

typedef struct {
    uint64_t frame_id;
    index_type chunk, num_lines;
    CPUCell *cpu;
    GPUCell *gpu;
    uint64_t last_used;
    bool modified;
} CellChunk;

struct HistoryCompression {
    HistoryFrame *frames;
    size_t head, num, capacity, hint;
    CellChunk *chunks;
    size_t num_chunks, chunks_capacity, chunk_target;
    uint64_t clock, prev_trim_clock, next_frame_id;
    ZSTD_DCtx *dctx;
    EncodeBuf enc;
    bool force_new_frame;
    // jobs not yet retired: stream jobs in order, and recompression jobs
    CompressJob *jobs_head, *jobs_tail, *rjobs;
    StreamState stream;
    uint64_t lines_compressed, frames_decompressed, frames_recompressed;
};

static StreamState *
stream_of(HistoryCompression *z) {
    return &z->stream;
}

static inline HistoryFrame *
frame_at(HistoryCompression *z, size_t i) {
    return z->frames + (z->head + i) % z->capacity;
}

static inline HistoryFrame *
newest_frame(HistoryCompression *z) {
    return z && z->num ? frame_at(z, z->num - 1) : NULL;
}

static HistoryFrame *
frames_append(HistoryCompression *z) {
    if (z->num == z->capacity) {
        size_t cap = MAX(16u, z->capacity * 2);
        if (cap <= z->capacity || cap > SIZE_MAX / sizeof(HistoryFrame)) return NULL;
        HistoryFrame *n = malloc(cap * sizeof(HistoryFrame));
        if (!n) return NULL;
        for (size_t i = 0; i < z->num; i++) n[i] = *frame_at(z, i);
        free(z->frames);
        z->frames = n;
        z->head = 0;
        z->capacity = cap;
    }
    HistoryFrame *f = frame_at(z, z->num++);
    zero_at_ptr(f);
    return f;
}

static size_t
find_frame(HistoryCompression *z, uint64_t seq) {
    // index of the frame holding the line with sequence number seq, which must
    // be in a frame: frames are contiguous, so this searches their first lines
    if (z->hint < z->num) {
        HistoryFrame *f = frame_at(z, z->hint);
        if (f->first_seq <= seq && seq < f->first_seq + f->num_lines) return z->hint;
    }
    size_t lo = 0, hi = z->num;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (frame_at(z, mid)->first_seq <= seq) lo = mid;
        else hi = mid;
    }
    return z->hint = lo;
}

static HistoryFrame *
frame_for_id(HistoryCompression *z, uint64_t id) {
    // frame ids increase from the oldest frame to the newest
    size_t lo = 0, hi = z->num;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        HistoryFrame *f = frame_at(z, mid);
        if (f->id == id) return f;
        if (f->id < id) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static bool
grow_lines(HistoryFrame *f, index_type need) {
    if (need <= f->lines_capacity) return true;
    index_type cap = MAX(need, MAX(256u, f->lines_capacity + f->lines_capacity / 2));
    if (cap < need) cap = need;
    LineAttrs *a = realloc(f->attrs, (size_t)cap * sizeof(LineAttrs));
    if (!a) return false;
    f->attrs = a;
    uint8_t *i = realloc(f->info, cap);
    if (!i) return false;
    f->info = i;
    f->lines_capacity = cap;
    return true;
}

static void
drop_chunk_at(HistoryCompression *z, size_t i) {
    free(z->chunks[i].cpu);
    free(z->chunks[i].gpu);
    z->chunks[i] = z->chunks[--z->num_chunks];
}

static void
free_raw(HistoryFrame *f) {
    free(f->raw);
    free(f->chunk_offsets);
    f->raw = NULL;
    f->chunk_offsets = NULL;
    f->raw_len = f->raw_capacity = f->num_chunk_offsets = f->chunk_offsets_capacity = 0;
}

static void
free_frame(HistoryCompression *z, HistoryFrame *f) {
    // a replacement job is left in the job list, to be discarded when retired
    if (f->replacement) cancel_job(f->replacement, false);
    for (size_t i = 0; i < z->num_chunks;) {
        if (z->chunks[i].frame_id == f->id) drop_chunk_at(z, i);
        else i++;
    }
    free_raw(f);
    free(f->data);
    free(f->tail);
    free(f->attrs);
    free(f->info);
    zero_at_ptr(f);
}

static void
drop_oldest_frame(HistoryCompression *z) {
    free_frame(z, frame_at(z, 0));
    z->head = (z->head + 1) % z->capacity;
    z->num--;
    z->hint = 0;
}

static void
drop_newest_frame(HistoryCompression *z) {
    if (newest_frame(z)->open) z->force_new_frame = true;
    free_frame(z, frame_at(z, z->num - 1));
    z->num--;
    z->hint = 0;
}

static void
shrink_frame_data(HistoryFrame *f) {
    if (f->size && f->size < f->capacity) {
        uint8_t *n = realloc(f->data, f->size);
        if (n) {
            f->data = n;
            f->capacity = f->size;
        }
    }
}

static void
close_frame(HistoryFrame *f) {
    if (f && f->open) {
        f->open = false;
        shrink_frame_data(f);
    }
}

static HistoryFrame *
open_frame(HistoryCompression *z, uint64_t first_seq) {
    HistoryFrame *f = frames_append(z);
    if (f) {
        f->id = ++z->next_frame_id;
        f->first_seq = first_seq;
        f->open = true;
    }
    return f;
}

static HistoryCompression *
alloc_compression(void) {
    HistoryCompression *z = calloc(1, sizeof(HistoryCompression));
    if (!z) return NULL;
    z->chunk_target = MIN_CACHED_CHUNKS;
    atomic_init(&z->stream.cctx_bytes, 0);
    return z;
}
// }}}

// Encoded lines {{{

static bool
append_raw(HistoryFrame *f, index_type lines_before, const uint8_t *p, size_t len, index_type num_lines) {
    // Append encoded lines to the decompressed copy of a frame, recording where chunks start
    size_t need;
    if (__builtin_add_overflow(f->raw_len, len, &need) || !grow_buffer(&f->raw, &f->raw_capacity, need)) return false;
    const uint8_t *end = p + len;
    for (index_type i = 0; i < num_lines; i++) {
        const index_type lnum = lines_before + i;
        if (lnum % CHUNK_LINES == 0) {
            if (f->num_chunk_offsets == f->chunk_offsets_capacity) {
                size_t cap = MAX(8u, f->chunk_offsets_capacity * 2);
                size_t *n = realloc(f->chunk_offsets, cap * sizeof(size_t));
                if (!n) return false;
                f->chunk_offsets = n;
                f->chunk_offsets_capacity = cap;
            }
            f->chunk_offsets[f->num_chunk_offsets++] = f->raw_len + (size_t)(p - (end - len));
        }
        if (!(p = skip_line(p, end))) return false;
    }
    memcpy(f->raw + f->raw_len, end - len, len);
    f->raw_len += len;
    return true;
}

// The encoded lines of a frame, decompressed on demand and kept like the cell chunks
static bool
ensure_raw(HistoryBuf *self, HistoryFrame *f) {
    HistoryCompression *z = self->z;
    f->raw_last_used = ++z->clock;
    if (f->raw) return true;
    if (f->replacement) return append_raw(f, 0, f->replacement->in, f->replacement->in_len, f->num_lines) || (free_raw(f), false);
    uint8_t *dec = NULL;
    size_t dec_cap = 0;
    if (!grow_buffer(&dec, &dec_cap, MAX(1u, f->data_raw + f->tail_len))) return false;
    if (f->data_raw) {
        if (!z->dctx && !(z->dctx = ZSTD_createDCtx())) {
            free(dec);
            return false;
        }
        ZSTD_DCtx_reset(z->dctx, ZSTD_reset_session_only);
        ZSTD_inBuffer in = {f->data, f->size, 0};
        ZSTD_outBuffer out = {dec, f->data_raw, 0};
        while (out.pos < out.size) {
            const size_t bi = in.pos, bo = out.pos;
            size_t r = ZSTD_decompressStream(z->dctx, &out, &in);
            if (ZSTD_isError(r)) {
                log_error("Failed to decompress scrollback: %s", ZSTD_getErrorName(r));
                break;
            }
            if (in.pos == bi && out.pos == bo) break;
        }
        if (out.pos != f->data_raw) {
            log_error("Compressed scrollback frame is truncated");
            free(dec);
            return false;
        }
    }
    if (f->tail_len) memcpy(dec + f->data_raw, f->tail, f->tail_len);
    const bool ok = append_raw(f, 0, dec, f->data_raw + f->tail_len, f->num_lines);
    free(dec);
    if (!ok) free_raw(f);
    z->frames_decompressed++;
    return ok;
}

static const uint8_t *
raw_line(HistoryFrame *f, index_type lnum) {
    const uint8_t *p = f->raw + f->chunk_offsets[lnum / CHUNK_LINES], *end = f->raw + f->raw_len;
    for (index_type i = lnum - lnum % CHUNK_LINES; i < lnum && p; i++) p = skip_line(p, end);
    return p;
}

static bool
decode_frame_lines(HistoryBuf *self, HistoryFrame *f, index_type first, index_type n, CPUCell *cpu, GPUCell *gpu) {
    const uint8_t *p = raw_line(f, first), *end = f->raw + f->raw_len;
    for (index_type i = 0; i < n; i++) {
        CPUCell *c = cpu + (size_t)i * self->xnum;
        GPUCell *g = gpu + (size_t)i * self->xnum;
        if (!p || !(p = decode_line(p, end, c, g, self->xnum))) {
            log_error("Compressed scrollback frame is corrupt");
            return false;
        }
        if (first + i < f->blank_clear_lines) clear_trailing_blanks(c, g, self->xnum);
    }
    return true;
}

// Write the changed cells of a chunk back into its frame's encoded lines
static bool
splice_chunk(HistoryBuf *self, HistoryFrame *f, CellChunk *c) {
    HistoryCompression *z = self->z;
    if (!ensure_raw(self, f)) return false;
    z->enc.enc_len = 0;
    uint8_t has = 0;
    for (index_type i = 0; i < c->num_lines; i++) {
        uint8_t info;
        if (!encode_line(&z->enc, c->cpu + (size_t)i * self->xnum, c->gpu + (size_t)i * self->xnum, self->xnum, &info, &has)) return false;
        f->info[c->chunk * CHUNK_LINES + i] = info;
    }
    const index_type first = c->chunk * CHUNK_LINES;
    const uint8_t *start = raw_line(f, first), *stop = start, *end = f->raw + f->raw_len;
    for (index_type i = 0; i < c->num_lines && stop; i++) stop = skip_line(stop, end);
    if (!start || !stop) return false;
    const size_t a = start - f->raw, b = stop - f->raw, rest = f->raw_len - b;
    // rebuild from the splice: the encoded lines after it move
    uint8_t *n = malloc(a + z->enc.enc_len + rest);
    if (!n) return false;
    memcpy(n, f->raw, a);
    memcpy(n + a, z->enc.enc, z->enc.enc_len);
    memcpy(n + a + z->enc.enc_len, f->raw + b, rest);
    const size_t total = a + z->enc.enc_len + rest;
    free_raw(f);
    // lines past num_lines (left by a partial pull) are dropped here
    if (!append_raw(f, 0, n, total, f->num_lines)) {
        free(n);
        free_raw(f);
        return false;
    }
    free(n);
    // the trailing blanks of the spliced lines were cleared when they were decoded
    f->has |= has;
    f->raw_modified = true;
    f->version++;
    return true;
}

// Have the compressor compress a frame's changed encoded lines
static bool
recompress_frame(HistoryBuf *self, HistoryFrame *f) {
    HistoryCompression *z = self->z;
    if (f->open) {
        // Blocks with the compressor continue this frame's stream: it can only
        // be replaced once it has ended, or nothing more is coming for it
        if (z->jobs_head) return false;
        close_frame(f);
        z->force_new_frame = true;
    }
    CompressJob *j = calloc(1, sizeof(CompressJob));
    if (!j) return false;
    // the encoded lines up to num_lines
    size_t len = f->raw_len;
    if (f->num_lines) {
        const uint8_t *last = raw_line(f, f->num_lines - 1);
        const uint8_t *e = last ? skip_line(last, f->raw + f->raw_len) : NULL;
        if (e) len = e - f->raw;
    }
    if (!(j->in = malloc(MAX(1u, len)))) {
        free(j);
        return false;
    }
    memcpy(j->in, f->raw, len);
    j->in_len = len;
    j->kind = JOB_ENCODED;
    j->z = z;
    j->frame_id = f->id;
    j->level = self->level;
    atomic_init(&j->done, false);
    if (f->replacement) cancel_job(f->replacement, false);
    f->replacement = j;
    f->raw_modified = false;
    f->blank_clear_lines = 0; // the encoded lines have them cleared
    f->version++;
    j->next = z->rjobs;
    z->rjobs = j;
    z->frames_recompressed++;
    submit_job(j);
    return true;
}
// }}}

// The decompression cache {{{

static void
evict_chunk(HistoryBuf *self, size_t i) {
    HistoryCompression *z = self->z;
    CellChunk *c = z->chunks + i;
    HistoryFrame *f = frame_for_id(z, c->frame_id);
    if (f) {
        if (c->modified && !splice_chunk(self, f, c)) log_error("Out of memory compressing changed scrollback, the changes are lost");
        // rendering may have stored sprite positions in the cells being freed
        for (index_type l = 0; l < c->num_lines; l++) f->attrs[c->chunk * CHUNK_LINES + l].has_dirty_text = true;
    }
    drop_chunk_at(z, i);
}

static void
make_room_in_cache(HistoryBuf *self) {
    HistoryCompression *z = self->z;
    while (z->num_chunks >= z->chunk_target) {
        size_t lru = 0;
        for (size_t i = 1; i < z->num_chunks; i++)
            if (z->chunks[i].last_used < z->chunks[lru].last_used) lru = i;
        evict_chunk(self, lru);
    }
}

static CellChunk *
ensure_chunk(HistoryBuf *self, HistoryFrame *f, index_type chunk) {
    HistoryCompression *z = self->z;
    for (size_t i = 0; i < z->num_chunks; i++) {
        CellChunk *c = z->chunks + i;
        if (c->frame_id == f->id && c->chunk == chunk) {
            c->last_used = ++z->clock;
            f->raw_last_used = z->clock;
            return c;
        }
    }
    make_room_in_cache(self);
    if (!ensure_raw(self, f)) return NULL;
    const index_type first = chunk * CHUNK_LINES, n = MIN(CHUNK_LINES, f->num_lines - first);
    if (z->num_chunks == z->chunks_capacity) {
        size_t cap = MAX(8u, z->chunks_capacity * 2);
        CellChunk *nc = realloc(z->chunks, cap * sizeof(CellChunk));
        if (!nc) return NULL;
        z->chunks = nc;
        z->chunks_capacity = cap;
    }
    // room for a whole chunk, as the open frame grows
    CellChunk c = {.frame_id = f->id, .chunk = chunk, .num_lines = n, .last_used = ++z->clock};
    c.cpu = malloc((size_t)CHUNK_LINES * self->xnum * sizeof(CPUCell));
    c.gpu = malloc((size_t)CHUNK_LINES * self->xnum * sizeof(GPUCell));
    if (!c.cpu || !c.gpu || !decode_frame_lines(self, f, first, n, c.cpu, c.gpu)) {
        free(c.cpu);
        free(c.gpu);
        return NULL;
    }
    z->chunks[z->num_chunks++] = c;
    return z->chunks + z->num_chunks - 1;
}

static bool
frame_has_chunks(HistoryCompression *z, HistoryFrame *f) {
    for (size_t i = 0; i < z->num_chunks; i++)
        if (z->chunks[i].frame_id == f->id) return true;
    return false;
}

// Write back and free the cached cells of a frame
static void
flush_frame_chunks(HistoryBuf *self, HistoryFrame *f) {
    HistoryCompression *z = self->z;
    for (size_t i = 0; i < z->num_chunks;) {
        if (z->chunks[i].frame_id == f->id) evict_chunk(self, i);
        else i++;
    }
}

uint64_t
historybuf_cache_clock(HistoryBuf *self) {
    return self->z ? self->z->clock : 0;
}

static void retire(HistoryBuf *self);

static void
trim_raw(HistoryBuf *self, bool everything) {
    HistoryCompression *z = self->z;
    for (size_t i = 0; i < z->num; i++) {
        HistoryFrame *f = frame_at(z, i);
        if (!f->raw || frame_has_chunks(z, f)) continue;
        if (!everything && f->raw_last_used > z->prev_trim_clock) continue;
        if (f->raw_modified && !recompress_frame(self, f)) continue;
        free_raw(f);
    }
}

void
historybuf_cache_trim(HistoryBuf *self, uint64_t render_start) {
    // Free what was not used since the previous trim, and size the cache to
    // hold at least the chunks this render used
    retire(self);
    // no output since the previous trim: the spare cells are not needed
    if (self->pushes == self->pushes_at_trim) free_spare_cells(self, 1);
    self->pushes_at_trim = self->pushes;
    HistoryCompression *z = self->z;
    if (!z) return;
    size_t used_by_render = 0;
    for (size_t i = 0; i < z->num_chunks;) {
        CellChunk *c = z->chunks + i;
        if (c->last_used > render_start) used_by_render++;
        if (c->last_used <= z->prev_trim_clock) evict_chunk(self, i); // replaces chunks[i]
        else i++;
    }
    trim_raw(self, false);
    z->chunk_target = MAX(MIN_CACHED_CHUNKS, used_by_render + 2);
    z->prev_trim_clock = z->clock;
    if (!z->num_chunks) {
        ZSTD_freeDCtx(z->dctx);
        z->dctx = NULL;
    }
}
// }}}

// Line access {{{

static Line *
blank_line(HistoryBuf *self, Line *l) {
    zero_at_ptr_count(self->blank_cpu, self->xnum);
    zero_at_ptr_count(self->blank_gpu, self->xnum);
    l->cpu_cells = self->blank_cpu;
    l->gpu_cells = self->blank_gpu;
    return l;
}

typedef enum { LINE_NONE, LINE_HOT, LINE_PENDING, LINE_FRAME } LineKind;

typedef struct {
    LineKind kind;
    LineBlock *block;
    HistoryFrame *frame;
    CellChunk *chunk;
    index_type offset;
} LineLocation;

static LineLocation
locate(HistoryBuf *self, index_type lnum) {
    LineLocation ans = {0};
    if (!self->count) return ans;
    lnum = MIN(lnum, self->count - 1);
    const uint64_t seq = self->next_seq - 1 - lnum;
    if (seq >= blocks_first_seq(self)) {
        const size_t i = find_block(self, seq, &ans.offset);
        ans.block = block_at(self, i);
        ans.kind = i < self->num_pending ? LINE_PENDING : LINE_HOT;
    } else {
        ans.frame = frame_at(self->z, find_frame(self->z, seq));
        ans.offset = (index_type)(seq - ans.frame->first_seq);
        ans.kind = LINE_FRAME;
    }
    return ans;
}

static LineAttrs *
attrs_for_lnum(HistoryBuf *self, index_type lnum) {
    LineLocation loc = locate(self, lnum);
    switch (loc.kind) {
        case LINE_NONE: return &dummy_attrs;
        case LINE_FRAME: return loc.frame->attrs + loc.offset;
        default: return loc.block->attrs + loc.offset;
    }
}

// Point l at the cells of line lnum. Returns where it is.
static LineLocation
get_line(HistoryBuf *self, index_type lnum, Line *l) {
    LineLocation loc = locate(self, lnum);
    switch (loc.kind) {
        case LINE_NONE:
            l->attrs = (LineAttrs){0};
            blank_line(self, l);
            break;
        case LINE_PENDING:
            if (!ensure_shadow(self, loc.block)) {
                log_error("Out of memory reading scrollback, showing a blank line");
                l->attrs = loc.block->attrs[loc.offset];
                blank_line(self, l);
                loc.kind = LINE_NONE;
                break;
            }
            // fallthrough
        case LINE_HOT:
            l->attrs = loc.block->attrs[loc.offset];
            l->cpu_cells = block_cpu(loc.block) + (size_t)loc.offset * self->xnum;
            l->gpu_cells = block_gpu(loc.block) + (size_t)loc.offset * self->xnum;
            break;
        case LINE_FRAME:
            l->attrs = loc.frame->attrs[loc.offset];
            if ((loc.chunk = ensure_chunk(self, loc.frame, loc.offset / CHUNK_LINES))) {
                const size_t i = (size_t)(loc.offset % CHUNK_LINES) * self->xnum;
                l->cpu_cells = loc.chunk->cpu + i;
                l->gpu_cells = loc.chunk->gpu + i;
            } else {
                log_error("Out of memory decompressing scrollback, showing a blank line");
                blank_line(self, l);
                loc.kind = LINE_NONE;
            }
            break;
    }
    return loc;
}

// As get_line() for a line whose content the caller will change
static void
get_line_for_write(HistoryBuf *self, index_type lnum, Line *l) {
    LineLocation loc = get_line(self, lnum, l);
    if (loc.kind == LINE_FRAME) loc.chunk->modified = true;
    else if (loc.kind == LINE_PENDING) loc.block->shadow_modified = true;
}

static bool
line_wrapped(HistoryBuf *self, index_type lnum) {
    LineLocation loc = locate(self, lnum);
    switch (loc.kind) {
        case LINE_NONE: return false;
        case LINE_FRAME:
            for (size_t i = 0; i < self->z->num_chunks; i++) {
                CellChunk *c = self->z->chunks + i;
                if (c->modified && c->frame_id == loc.frame->id && c->chunk == loc.offset / CHUNK_LINES)
                    return c->cpu[(size_t)(loc.offset % CHUNK_LINES) * self->xnum + self->xnum - 1].next_char_was_wrapped;
            }
            return loc.frame->info[loc.offset] & INFO_WRAPPED;
        default:
            if (!block_cpu(loc.block))  // released once encoded
                return (loc.block->reenc_info ? loc.block->reenc_info : loc.block->job->info)[loc.offset] & INFO_WRAPPED;
            return block_cpu(loc.block)[(size_t)loc.offset * self->xnum + self->xnum - 1].next_char_was_wrapped;
    }
}

bool
historybuf_line_is_compressed(HistoryBuf *self, index_type lnum) {
    if (lnum >= self->count) return false;
    return self->next_seq - 1 - lnum < hot_first_seq(self);
}
// }}}

// Moving lines between the hot tail, the compressor and the frames {{{

static void register_active(HistoryBuf *self);
static void free_all_storage(HistoryBuf *self);
static void release_encoded_blocks(HistoryBuf *self);

static void
hand_off_oldest_hot_block(HistoryBuf *self) {
    if (!self->z && !(self->z = alloc_compression())) return; // the block stays hot
    HistoryCompression *z = self->z;
    LineBlock *b = block_at(self, self->num_pending);
    CompressJob *j = calloc(1, sizeof(CompressJob));
    if (!j) return;
    *j = (CompressJob){.z = z, .kind = JOB_BLOCK, .cpu = b->cpu, .gpu = b->gpu, .num_lines = b->num_lines, .xnum = self->xnum,
                       .level = self->level, .frame_bytes = self->frame_bytes, .force_new_frame = z->force_new_frame, .block = b};
    z->force_new_frame = false;
    atomic_init(&j->done, false);
    b->job = j;
    if (z->jobs_tail) z->jobs_tail->next = j;
    else z->jobs_head = j;
    z->jobs_tail = j;
    self->num_pending++;
    self->pending_lines += b->num_lines;
    self->hot_lines -= b->num_lines;
    register_active(self);
    submit_job(j);
}

static void
cancel_invisible_pending(HistoryBuf *self) {
    // Blocks past the scrollback limit that the compressor has not started on
    // need not be compressed. The first visible block, if any, must then start
    // a new frame, so this is only done while it has not been started on.
    // Cancelled blocks are dropped when they reach the front, see retire().
    const uint64_t oldest_visible = self->next_seq - self->count;
    size_t n = 0;
    while (n < self->num_pending && block_at(self, n)->first_seq + block_at(self, n)->num_lines <= oldest_visible) n++;
    if (!n || block_at(self, n - 1)->job->cancelled) return;
    RAII_ALLOC(CompressJob *, jobs, malloc(n * sizeof(CompressJob *)));
    if (!jobs) return;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!block_at(self, i)->job->cancelled) jobs[k++] = block_at(self, i)->job;
    CompressJob *next = n < self->num_pending ? block_at(self, n)->job : NULL;
    if (cancel_queued_jobs_before(jobs, k, next) && !next) self->z->force_new_frame = true;
}

static void
drop_invisible(HistoryBuf *self) {
    // Frames and hot blocks all of whose lines are past the scrollback limit.
    // Pending blocks become frames first: the stream continues through them.
    const uint64_t oldest_visible = self->next_seq - self->count;
    HistoryCompression *z = self->z;
    if (z && self->num_pending) cancel_invisible_pending(self);
    while (z && z->num) {
        HistoryFrame *f = frame_at(z, 0);
        if (f->first_seq + f->num_lines > oldest_visible) break;
        // pending blocks may continue its stream, it goes once it has ended
        if (f->open && z->jobs_head) break;
        drop_oldest_frame(z);
    }
    if ((!z || !z->num) && !self->num_pending) {
        while (self->num_blocks > 1) {
            LineBlock *b = block_at(self, 0);
            if (b->first_seq + b->num_lines > oldest_visible) break;
            drop_front_block(self);
        }
    }
}

// The lines of a block whose compression failed are kept encoded, or blank if even that failed
static bool
keep_encoded(HistoryFrame *f, CompressJob *j, index_type n) {
    if (!j->enc.enc_len) {
        free_encode_buf(&j->enc);
        if (!(j->enc.enc = calloc(n, 2))) return false; // flags 0, no cells: a blank line
        j->enc.enc_len = j->enc.enc_capacity = 2u * n;
        if (j->info) memset(j->info, 0, n);
    }
    uint8_t *t = realloc(f->tail, f->tail_len + j->enc.enc_len);
    if (!t) return false;
    memcpy(t + f->tail_len, j->enc.enc, j->enc.enc_len);
    f->tail = t;
    f->tail_len += j->enc.enc_len;
    return true;
}

static void
drop_through_block(HistoryBuf *self, LineBlock *b) {
    // Out of memory storing compressed lines: they and all older ones are lost
    HistoryCompression *z = self->z;
    log_error("Out of memory storing compressed scrollback, dropping the oldest lines");
    const uint64_t keep_from = b->first_seq + b->num_lines;
    while (z->num) drop_oldest_frame(z);
    z->force_new_frame = true;
    drop_front_block(self);
    if (self->next_seq - self->count < keep_from) self->count = (index_type)(self->next_seq - keep_from);
}

static void
retire_block_job(HistoryBuf *self, CompressJob *j) {
    HistoryCompression *z = self->z;
    LineBlock *b = j->block;
    if (!self->num_pending || block_at(self, 0) != b) {
        log_error("Scrollback compressor result out of order, dropping it"); // cannot happen
        free_job(j);
        return;
    }
    if (j->failed && !atomic_load_explicit(&j->encoded, memory_order_acquire)) {
        // the compressor could not even encode the lines: encode them here, from the cells it left
        free_encode_buf(&j->enc);
        free(j->info);
        j->info = malloc(MAX(1u, b->num_lines));
        for (index_type i = 0; i < b->num_lines && j->info; i++) {
            if (!encode_line(&j->enc, block_cpu(b) + (size_t)i * self->xnum, block_gpu(b) + (size_t)i * self->xnum, self->xnum, j->info + i, &j->has)) {
                free_encode_buf(&j->enc);
                break;
            }
        }
    }
    // the lines' cells are needed only to grow cached chunks or for changes made to the shadow
    if (b->reenc.enc || (!b->cpu && !b->shadow_cpu && z->num_chunks)) ensure_shadow(self, b);
    CPUCell *cells_cpu = block_cpu(b);
    GPUCell *cells_gpu = block_gpu(b);
    JobSegment failed_seg = {.lines = b->num_lines, .raw = j->enc.enc_len, .starts = false, .ends = true};
    const size_t nsegs = j->failed ? 1 : j->num_segs;
    index_type line = 0;
    size_t raw_off = 0, out_off = 0;
    for (size_t s = 0; s < nsegs; s++) {
        JobSegment seg = j->failed ? failed_seg : j->segs[s];
        HistoryFrame *f = newest_frame(z);
        // cannot happen: a segment continuing a frame that is gone cannot be decompressed, keep its lines encoded
        const bool orphan = !j->failed && !seg.starts && (!f || !f->open);
        if (seg.starts || !f || !f->open) {
            close_frame(f);
            if (!(f = open_frame(z, b->first_seq + line))) {
                drop_through_block(self, b);
                free_job(j);
                return;
            }
        }
        const index_type start = f->num_lines;
        if (!grow_lines(f, start + seg.lines)) {
            drop_through_block(self, b);
            free_job(j);
            return;
        }
        if (orphan) {
            log_error("Scrollback compressor output without its frame, keeping the lines uncompressed");
            uint8_t *t = realloc(f->tail, f->tail_len + seg.raw);
            if (!t) {
                drop_through_block(self, b);
                free_job(j);
                return;
            }
            memcpy(t + f->tail_len, j->enc.enc + raw_off, seg.raw);
            f->tail = t;
            f->tail_len += seg.raw;
            seg.ends = true;
        } else if (j->failed) {
            if (!keep_encoded(f, j, seg.lines)) {
                drop_through_block(self, b);
                free_job(j);
                return;
            }
        } else {
            size_t need;
            if (__builtin_add_overflow(f->size, seg.out, &need) || !grow_buffer(&f->data, &f->capacity, need)) {
                drop_through_block(self, b);
                free_job(j);
                return;
            }
            memcpy(f->data + f->size, j->out + out_off, seg.out);
            f->size += seg.out;
            f->data_raw += seg.raw;
        }
        if (f->raw && !append_raw(f, start, j->enc.enc + raw_off, j->failed ? j->enc.enc_len : seg.raw, seg.lines)) free_raw(f);
        for (index_type i = 0; i < seg.lines; i++) {
            LineAttrs a = b->attrs[line + i];
            a.has_dirty_text = true; // the sprite positions are not stored
            f->attrs[start + i] = a;
            f->info[start + i] = j->info ? j->info[line + i] : 0;
        }
        f->has |= j->has;
        f->num_lines += seg.lines;
        f->version++;
        if (b->blank_clear_lines > line) f->blank_clear_lines = MAX(f->blank_clear_lines, start + MIN(seg.lines, b->blank_clear_lines - line));
        // a cached chunk at the end of the open frame grows with it
        for (size_t c = 0; c < z->num_chunks; c++) {
            CellChunk *ch = z->chunks + c;
            if (ch->frame_id != f->id || ch->num_lines == CHUNK_LINES || (ch->chunk * CHUNK_LINES + ch->num_lines) != start) continue;
            if (!cells_cpu) {
                // out of memory decoding the lines: the chunk is decoded again when needed
                evict_chunk(self, c);
                break;
            }
            const index_type add = MIN(CHUNK_LINES - ch->num_lines, seg.lines);
            memcpy(ch->cpu + (size_t)ch->num_lines * self->xnum, cells_cpu + (size_t)line * self->xnum, (size_t)add * self->xnum * sizeof(CPUCell));
            memcpy(ch->gpu + (size_t)ch->num_lines * self->xnum, cells_gpu + (size_t)line * self->xnum, (size_t)add * self->xnum * sizeof(GPUCell));
            for (index_type k = 0; k < add && line + k < b->blank_clear_lines; k++)
                clear_trailing_blanks(ch->cpu + (size_t)(ch->num_lines + k) * self->xnum, ch->gpu + (size_t)(ch->num_lines + k) * self->xnum, self->xnum);
            ch->num_lines += add;
        }
        if (b->shadow_modified) {
            // the compressed lines lack the changes made to the shadow: put them in chunks, which are written back
            for (index_type i = 0; i < seg.lines; i++) {
                CellChunk *ch = ensure_chunk(self, f, (start + i) / CHUNK_LINES);
                if (!ch) {
                    log_error("Out of memory applying changes to compressed scrollback, they are lost");
                    break;
                }
                const size_t dst = (size_t)((start + i) % CHUNK_LINES) * self->xnum, src = (size_t)(line + i) * self->xnum;
                memcpy(ch->cpu + dst, cells_cpu + src, self->xnum * sizeof(CPUCell));
                memcpy(ch->gpu + dst, cells_gpu + src, self->xnum * sizeof(GPUCell));
                ch->modified = true;
            }
        }
        if (seg.ends) close_frame(f);
        line += seg.lines;
        raw_off += seg.raw;
        out_off += seg.out;
    }
    z->lines_compressed += b->num_lines;
    drop_front_block(self);
    free_job(j);
}

static void
retire_replacement_job(HistoryBuf *self, CompressJob *j) {
    HistoryCompression *z = self->z;
    HistoryFrame *f = j->cancelled ? NULL : frame_for_id(z, j->frame_id);
    if (f && j->kind == JOB_ENCODED && f->replacement == j) {
        f->replacement = NULL;
        free(f->data);
        free(f->tail);
        f->data = NULL;
        f->tail = NULL;
        f->size = f->capacity = f->data_raw = f->tail_len = 0;
        if (j->failed) {
            f->tail = j->in; // keep the content encoded
            f->tail_len = j->in_len;
            j->in = NULL;
        } else {
            f->data = j->out;
            f->size = f->capacity = j->out_len;
            f->data_raw = j->in_len;
            j->out = NULL;
        }
        f->open = false;
        f->version++;
    } else if (f && j->kind == JOB_DATA && !j->failed && !f->replacement && f->version == j->frame_version) {
        free(f->data);
        f->data = j->out;
        f->size = f->capacity = j->out_len;
        j->out = NULL;
        f->version++;
    }
    free_job(j);
}

static void
retire(HistoryBuf *self) {
    HistoryCompression *z = self->z;
    if (!z) return;
    release_encoded_blocks(self);
    while (z->jobs_head && atomic_load_explicit(&z->jobs_head->done, memory_order_acquire)) {
        CompressJob *j = z->jobs_head;
        z->jobs_head = j->next;
        if (!z->jobs_head) z->jobs_tail = NULL;
        if (j->cancelled) {
            if (self->num_pending && block_at(self, 0) == j->block) {
                j->block->job = NULL;
                drop_front_block(self);
            }
            free_job(j);
        } else retire_block_job(self, j);
    }
    for (CompressJob **p = &z->rjobs; *p;) {
        CompressJob *j = *p;
        if (atomic_load_explicit(&j->done, memory_order_acquire)) {
            *p = j->next;
            retire_replacement_job(self, j);
        } else p = &j->next;
    }
    drop_invisible(self);
}

// Buffers with jobs not yet retired, main thread only
static struct {
    HistoryBuf **items;
    size_t count, capacity;
} active;

static void
register_active(HistoryBuf *self) {
    if (self->on_active_list) return;
    if (active.count == active.capacity) {
        size_t cap = MAX(8u, active.capacity * 2);
        HistoryBuf **n = realloc(active.items, cap * sizeof(HistoryBuf *));
        if (!n) return; // retired when it next pushes a line or renders
        active.items = n;
        active.capacity = cap;
    }
    active.items[active.count++] = self;
    self->on_active_list = true;
}

static void
unregister_active(HistoryBuf *self) {
    if (!self->on_active_list) return;
    for (size_t i = 0; i < active.count; i++) {
        if (active.items[i] == self) {
            active.items[i] = active.items[--active.count];
            break;
        }
    }
    self->on_active_list = false;
}

void
historybuf_retire_all(void) {
    for (size_t i = 0; i < active.count;) {
        HistoryBuf *hb = active.items[i];
        retire(hb);
        if (!hb->z || (!hb->z->jobs_head && !hb->z->rjobs)) {
            active.items[i] = active.items[--active.count];
            hb->on_active_list = false;
        } else i++;
    }
}

static bool
decode_job_lines(HistoryBuf *self, CompressJob *j, index_type n, CPUCell *cpu, GPUCell *gpu) {
    if (!j || !atomic_load_explicit(&j->encoded, memory_order_acquire)) return false;
    const uint8_t *p = j->enc.enc, *end = p + j->enc.enc_len;
    for (index_type i = 0; i < n; i++)
        if (!(p = decode_line(p, end, cpu + (size_t)i * self->xnum, gpu + (size_t)i * self->xnum, self->xnum))) return false;
    return true;
}

// Release the cells of the pending blocks the compressor has encoded: until
// compressed, their lines are kept as the much smaller encoded lines
static void
release_encoded_blocks(HistoryBuf *self) {
    while (self->pending_released < self->num_pending) {
        LineBlock *b = block_at(self, self->pending_released);
        if (!b->job || !atomic_load_explicit(&b->job->encoded, memory_order_acquire)) break;
        release_cells(self, b);
        self->pending_released++;
    }
}

// Take the newest pending block back into the hot tail
static void
reclaim_newest_pending(HistoryBuf *self) {
    HistoryCompression *z = self->z;
    LineBlock *b = block_at(self, self->num_pending - 1);
    CompressJob *j = b->job;
    cancel_job(j, true);
    // it is the newest stream job
    CompressJob **p = &z->jobs_head, *prev = NULL;
    while (*p && *p != j) {
        prev = *p;
        p = &(*p)->next;
    }
    if (*p) {
        *p = j->next;
        if (z->jobs_tail == j) z->jobs_tail = prev;
    }
    if (!b->cpu) {
        // its cells were released: restore them from the shadow (copied below) or the encoded lines
        if (!alloc_cells(self, b)) {
            log_error("Out of memory restoring scrollback, dropping it");
            free_job(j);
            b->job = NULL;
            free_all_storage(self);
            return;
        }
        if (!b->shadow_cpu && !b->reenc.enc && !decode_job_lines(self, j, b->num_lines, b->cpu, b->gpu)) {
            log_error("Scrollback lines could not be restored, they will be blank");
            zero_at_ptr_count(b->cpu, (size_t)b->num_lines * self->xnum);
            zero_at_ptr_count(b->gpu, (size_t)b->num_lines * self->xnum);
        }
        if (self->pending_released >= self->num_pending) self->pending_released = self->num_pending - 1;
    }
    free_job(j);
    b->job = NULL;
    if (b->reenc.enc) {
        if (b->cpu) {
            const uint8_t *p = b->reenc.enc, *end = p + b->reenc.enc_len;
            for (index_type i = 0; i < b->num_lines && p; i++) p = decode_line(p, end, b->cpu + (size_t)i * self->xnum, b->gpu + (size_t)i * self->xnum, self->xnum);
        }
        free_encode_buf(&b->reenc);
        free(b->reenc_info);
        b->reenc_info = NULL;
    }
    if (b->shadow_cpu) {
        memcpy(b->cpu, b->shadow_cpu, (size_t)b->num_lines * self->xnum * sizeof(CPUCell));
        memcpy(b->gpu, b->shadow_gpu, (size_t)b->num_lines * self->xnum * sizeof(GPUCell));
        free(b->shadow_cpu);
        free(b->shadow_gpu);
        b->shadow_cpu = NULL;
        b->shadow_gpu = NULL;
    } else {
        for (index_type i = 0; i < b->blank_clear_lines; i++)
            clear_trailing_blanks(b->cpu + (size_t)i * self->xnum, b->gpu + (size_t)i * self->xnum, self->xnum);
    }
    b->blank_clear_lines = 0;
    b->shadow_modified = false;
    self->num_pending--;
    self->pending_lines -= b->num_lines;
    self->hot_lines += b->num_lines;
    // the stream holds the block just taken back: the next block starts a new
    // frame (older pending blocks still continue the current one)
    z->force_new_frame = true;
}

static index_type
visible_in_newest_frame(HistoryBuf *self) {
    HistoryFrame *f = newest_frame(self->z);
    const uint64_t from = MAX(f->first_seq, self->next_seq - self->count);
    return (index_type)(f->first_seq + f->num_lines - from);
}

// Drop the newest n lines of the newest frame, which must have more than n visible lines
static void
shorten_newest_frame(HistoryBuf *self, index_type n) {
    HistoryCompression *z = self->z;
    HistoryFrame *f = newest_frame(z);
    // the frame keeps their bytes, unread past num_lines, and takes no more lines
    flush_frame_chunks(self, f);
    if (f->open) z->force_new_frame = true;
    close_frame(f);
    f->num_lines -= n;
    if (f->blank_clear_lines > f->num_lines) f->blank_clear_lines = f->num_lines;
    f->version++;
    self->count -= n;
    self->next_seq -= n;
}

// Move up to a block of the newest lines of the newest frame into the hot tail. There must be no pending blocks.
static void
pull_from_newest_frame(HistoryBuf *self) {
    HistoryCompression *z = self->z;
    HistoryFrame *f = newest_frame(z);
    const index_type visible = visible_in_newest_frame(self), k = MIN(visible, self->block_lines);
    const index_type first = f->num_lines - k;
    flush_frame_chunks(self, f);
    LineBlock *b = alloc_block(self);
    if (!b || !prepend_block(self, b)) {
        if (b) release_block_memory(b);
        log_error("Out of memory restoring scrollback, dropping it");
        free_all_storage(self);
        return;
    }
    b->first_seq = f->first_seq + first;
    b->num_lines = k;
    if (!ensure_raw(self, f) || !decode_frame_lines(self, f, first, k, b->cpu, b->gpu)) {
        log_error("Out of memory decompressing scrollback, lines will be blank");
        zero_at_ptr_count(b->cpu, (size_t)k * self->xnum);
        zero_at_ptr_count(b->gpu, (size_t)k * self->xnum);
    }
    for (index_type i = 0; i < k; i++) b->attrs[i] = f->attrs[first + i];
    self->hot_lines += k;
    if (k == visible) drop_newest_frame(z);
    else {
        if (f->open) z->force_new_frame = true;
        close_frame(f);
        f->num_lines -= k;
        if (f->blank_clear_lines > f->num_lines) f->blank_clear_lines = f->num_lines;
        f->version++;
    }
}

static bool
have_compressed(HistoryBuf *self) {
    return self->num_pending || (self->z && self->z->num);
}

// Bring at least one line back into the hot tail
static void
restore_newest(HistoryBuf *self) {
    if (self->num_pending) reclaim_newest_pending(self);
    else if (self->z && self->z->num) pull_from_newest_frame(self);
}

static void
ensure_hot_min(HistoryBuf *self) {
    while (self->hot_lines < HOT_MIN_LINES && self->count > self->hot_lines && have_compressed(self)) restore_newest(self);
}

static void
drop_empty_back_blocks(HistoryBuf *self) {
    while (self->num_blocks > self->num_pending && !block_at(self, self->num_blocks - 1)->num_lines) drop_back_block(self);
}

// Drop the newest n lines
static void
truncate_newest(HistoryBuf *self, index_type n) {
    n = MIN(n, self->count);
    while (n) {
        drop_empty_back_blocks(self);
        if (self->num_blocks > self->num_pending) {
            LineBlock *b = block_at(self, self->num_blocks - 1);
            const index_type k = MIN(n, b->num_lines);
            b->num_lines -= k;
            self->hot_lines -= k;
            self->count -= k;
            self->next_seq -= k;
            n -= k;
            continue;
        }
        if (self->num_pending) {
            reclaim_newest_pending(self);
            continue;
        }
        const index_type visible = visible_in_newest_frame(self);
        if (n >= visible) {
            drop_newest_frame(self->z);
            self->count -= visible;
            self->next_seq -= visible;
            n -= visible;
        } else {
            shorten_newest_frame(self, n);
            n = 0;
        }
    }
    drop_empty_back_blocks(self);
    if (!self->count) free_all_storage(self);
    else ensure_hot_min(self);
}

static void
free_all_storage(HistoryBuf *self) {
    HistoryCompression *z = self->z;
    if (z) {
        for (CompressJob *j = z->jobs_head; j; j = j->next) cancel_job(j, true);
        for (CompressJob *j = z->rjobs; j; j = j->next) cancel_job(j, true);
        while (z->jobs_head) {
            CompressJob *j = z->jobs_head;
            z->jobs_head = j->next;
            free_job(j);
        }
        z->jobs_tail = NULL;
        while (z->rjobs) {
            CompressJob *j = z->rjobs;
            z->rjobs = j->next;
            free_job(j);
        }
        while (z->num) drop_newest_frame(z);
        z->force_new_frame = true;
    }
    self->num_pending = 0;
    while (self->num_blocks) drop_back_block(self);
    self->hot_lines = 0;
    self->pending_lines = 0;
    self->count = 0;
}

static void
free_compression(HistoryCompression *z) {
    if (!z) return;
    wait_for_compressor(z);
    free(z->frames);
    while (z->num_chunks) drop_chunk_at(z, z->num_chunks - 1);
    free(z->chunks);
    free_encode_buf(&z->enc);
    ZSTD_freeDCtx(z->dctx);
    ZSTD_freeCCtx(z->stream.cctx);
    free(z);
}
// }}}

static HistoryBuf *
create_historybuf(
    PyTypeObject *type, unsigned int xnum, unsigned int ynum, unsigned int pagerhist_sz, TextCache *tc, bool compress, int level,
    const HistoryCompressionParams *zp) {
    if (xnum == 0 || ynum == 0) {
        PyErr_SetString(PyExc_ValueError, "Cannot create an empty history buffer");
        return NULL;
    }
    init_masks();
    HistoryBuf *self = (HistoryBuf *)type->tp_alloc(type, 0);
    if (self == NULL) return NULL;
    self->xnum = xnum;
    // Screen code addresses history lines with negative ints, so the number
    // of lines must fit in an int: that is "unlimited".
    self->ynum = MIN(ynum, HISTORY_UNLIMITED);
    self->text_cache = tc_incref(tc);
    self->line = alloc_line(self->text_cache);
    self->blank_cpu = calloc(xnum, sizeof(CPUCell));
    self->blank_gpu = calloc(xnum, sizeof(GPUCell));
    if (!self->line || !self->blank_cpu || !self->blank_gpu) {
        Py_DECREF(self);
        return (HistoryBuf *)PyErr_NoMemory();
    }
    self->line->xnum = xnum;
    self->zparams = *zp;
    self->compress = compress;
    self->level = clamp_level(level);
    self->block_lines = MAX(1u, zp->block_lines ? zp->block_lines : DEFAULT_BLOCK_LINES);
    self->hot_target = MAX(HOT_MIN_LINES, zp->hot_lines ? zp->hot_lines : DEFAULT_HOT_LINES);
    self->frame_bytes = MAX(1u, zp->frame_bytes ? zp->frame_bytes : DEFAULT_FRAME_BYTES);
    self->pagerhist = alloc_pagerhist(pagerhist_sz);
    return self;
}

static PyObject *
new_history_object(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    unsigned int xnum = 1, ynum = 1, pagerhist_sz = 0, hot_lines = 0, block_lines = 0;
    unsigned long long frame_bytes = 0;
    int compress = 1, level = 0;
    static const char *kwlist[] = {"ynum", "xnum", "pagerhist_sz", "compress", "level", "hot_lines", "block_lines", "frame_bytes", NULL};
    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "II|IpiIIK", (char **)kwlist, &ynum, &xnum, &pagerhist_sz, &compress, &level, &hot_lines, &block_lines, &frame_bytes))
        return NULL;
    TextCache *tc = tc_alloc();
    if (!tc) return PyErr_NoMemory();
    HistoryCompressionParams p = compression_params;
    if (hot_lines) p.hot_lines = hot_lines;
    if (block_lines) p.block_lines = block_lines;
    if (frame_bytes) p.frame_bytes = (size_t)frame_bytes;
    HistoryBuf *ans = create_historybuf(type, xnum, ynum, pagerhist_sz, tc, compress, level, &p);
    tc_decref(tc);
    return (PyObject *)ans;
}

static void
dealloc(HistoryBuf *self) {
    unregister_active(self);
    Py_CLEAR(self->line);
    free_all_storage(self);
    free_compression(self->z);
    free(self->blocks);
    free_spare_cells(self, 0);
    free(self->spare_cells);
    free_pagerhist(self);
    free(self->blank_cpu);
    free(self->blank_gpu);
    tc_decref(self->text_cache);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static bool
hb_line_is_continued(HistoryBuf *self, index_type lnum) {
    if (!self->count) return false;
    lnum = MIN(lnum, self->count - 1);
    if (lnum == self->count - 1) {
        size_t sz;
        if (self->pagerhist && self->pagerhist->ringbuf && (sz = ringbuf_bytes_used(self->pagerhist->ringbuf)) > 0) {
            size_t pos = ringbuf_findchr(self->pagerhist->ringbuf, '\n', sz - 1);
            if (pos >= sz) return true; // ringbuf does not end with a newline
        }
        return false;
    }
    return line_wrapped(self, lnum + 1);
}

void
historybuf_init_line(HistoryBuf *self, index_type lnum, Line *l) {
    get_line(self, lnum, l);
}

bool
historybuf_is_line_continued(HistoryBuf *self, index_type lnum) {
    return hb_line_is_continued(self, lnum);
}

bool
history_buf_endswith_wrap(HistoryBuf *self) {
    return self->count ? line_wrapped(self, 0) : false;
}

void
historybuf_mark_line_clean(HistoryBuf *self, index_type y) {
    LineLocation loc = locate(self, y);
    // a line is clean once rendered, which stores sprite positions in the
    // cells it was rendered from: without those cells it has none
    if (loc.kind == LINE_PENDING && !loc.block->shadow_cpu) return;
    if (loc.kind == LINE_FRAME) {
        bool cached = false;
        for (size_t i = 0; i < self->z->num_chunks && !cached; i++)
            cached = self->z->chunks[i].frame_id == loc.frame->id && self->z->chunks[i].chunk == loc.offset / CHUNK_LINES;
        if (!cached) return;
    }
    attrs_for_lnum(self, y)->has_dirty_text = false;
}

void
historybuf_mark_line_dirty(HistoryBuf *self, index_type y) {
    attrs_for_lnum(self, y)->has_dirty_text = true;
}

void
historybuf_mark_all_dirty(HistoryBuf *self) {
    for (size_t i = 0; i < self->num_blocks; i++) {
        LineBlock *b = block_at(self, i);
        for (index_type j = 0; j < b->num_lines; j++) b->attrs[j].has_dirty_text = true;
    }
    if (self->z)
        for (size_t i = 0; i < self->z->num; i++) {
            HistoryFrame *f = frame_at(self->z, i);
            for (index_type j = 0; j < f->num_lines; j++) f->attrs[j].has_dirty_text = true;
        }
}

void
historybuf_set_line_has_image_placeholders(HistoryBuf *self, index_type y, bool val) {
    attrs_for_lnum(self, y)->has_image_placeholders = val;
}

void
historybuf_clear(HistoryBuf *self) {
    pagerhist_clear(self);
    free_all_storage(self);
    self->next_seq = 0;
}

static void
pagerhist_push(HistoryBuf *self, ANSIBuf *as_ansi_buf) {
    // push the oldest line into the pager history
    PagerHistoryBuf *ph = self->pagerhist;
    if (!ph) return;
    Line l = {.xnum = self->xnum, .text_cache = self->text_cache};
    get_line(self, self->count - 1, &l);
    ANSILineState s = {.output_buf = as_ansi_buf};
    as_ansi_buf->len = 0;
    line_as_ansi(&l, &s, 0, l.xnum, 0, true);
    pagerhist_write_bytes(ph, (const uint8_t *)"\x1b[m", 3);
    if (pagerhist_write_ucs4(ph, as_ansi_buf->buf, as_ansi_buf->len)) {
        char line_end[2];
        size_t num = 0;
        line_end[num++] = '\r';
        if (!l.cpu_cells[l.xnum - 1].next_char_was_wrapped) line_end[num++] = '\n';
        pagerhist_write_bytes(ph, (const uint8_t *)line_end, num);
    }
}

// Make room for a new line, the newest, pointing l at its cells
static bool
push_slot(HistoryBuf *self, ANSIBuf *as_ansi_buf, Line *l, LineAttrs **attrs) {
    if (self->z && self->z->jobs_head && atomic_load_explicit(&self->z->jobs_head->done, memory_order_acquire)) retire(self);
    else if (self->pending_released < self->num_pending) release_encoded_blocks(self);
    if (self->count >= self->ynum) {
        // the scrollback limit: the oldest line leaves the buffer
        pagerhist_push(self, as_ansi_buf);
        self->count--;
        drop_invisible(self);
    }
    LineBlock *b = self->num_blocks > self->num_pending ? block_at(self, self->num_blocks - 1) : NULL;
    if (!b || b->num_lines >= b->capacity) {
        if (!(b = alloc_block(self)) || !append_block(self, b)) {
            if (b) release_block_memory(b);
            log_error("Out of memory allocating scrollback, dropping a line");
            return false;
        }
    }
    if (!b->num_lines) b->first_seq = self->next_seq;
    const index_type idx = b->num_lines++;
    self->pushes++;
    self->hot_lines++;
    self->count++;
    self->next_seq++;
    l->cpu_cells = b->cpu + (size_t)idx * self->xnum;
    l->gpu_cells = b->gpu + (size_t)idx * self->xnum;
    *attrs = b->attrs + idx;
    if (self->compress && self->hot_lines >= self->hot_target + self->block_lines && self->num_blocks - self->num_pending >= 2)
        hand_off_oldest_hot_block(self);
    return true;
}

static void
prefetch_line_for_write(HistoryBuf *self) {
    // The history buffer is typically much larger than the CPU cache, so the
    // line copy in historybuf_add_line() stalls acquiring ownership of the
    // destination cache lines. Prefetching the slot that the next call will
    // write to overlaps those misses with the parsing/drawing of the next
    // line, speeding up bulk text throughput by ~35% (see benchmark.py).
    if (self->num_blocks <= self->num_pending) return;
    LineBlock *b = block_at(self, self->num_blocks - 1);
    if (b->num_lines >= b->capacity) return; // must not allocate
    const char *cpu = (const char *)(b->cpu + (size_t)b->num_lines * self->xnum);
    const char *gpu = (const char *)(b->gpu + (size_t)b->num_lines * self->xnum);
    const size_t cache_line_sz = 64;
    for (size_t i = 0; i < self->xnum * sizeof(CPUCell); i += cache_line_sz) __builtin_prefetch(cpu + i, 1, 0);
    for (size_t i = 0; i < self->xnum * sizeof(GPUCell); i += cache_line_sz) __builtin_prefetch(gpu + i, 1, 0);
}

void
historybuf_add_line(HistoryBuf *self, const Line *line, ANSIBuf *as_ansi_buf) {
    Line dest = {.xnum = self->xnum};
    LineAttrs *attrs;
    if (!push_slot(self, as_ansi_buf, &dest, &attrs)) return;
    copy_line(line, &dest);
    *attrs = line->attrs;
    prefetch_line_for_write(self);
}

bool
historybuf_pop_line(HistoryBuf *self, Line *line) {
    if (self->count <= 0) return false;
    drop_empty_back_blocks(self);
    while (self->num_blocks <= self->num_pending && have_compressed(self)) restore_newest(self);
    if (self->num_blocks <= self->num_pending) return false;
    LineBlock *b = block_at(self, self->num_blocks - 1);
    const index_type idx = b->num_lines - 1;
    line->cpu_cells = b->cpu + (size_t)idx * self->xnum;
    line->gpu_cells = b->gpu + (size_t)idx * self->xnum;
    line->attrs = b->attrs[idx];
    // the block is kept, even if empty, so that the line stays valid until the next push
    b->num_lines--;
    self->hot_lines--;
    self->count--;
    self->next_seq--;
    if (!self->count) {
        // keep the popped line's block
        HistoryCompression *z = self->z;
        if (z) {
            while (z->num) drop_newest_frame(z);
        }
    } else ensure_hot_min(self);
    return true;
}

void
historybuf_delete_newest_lines(HistoryBuf *self, index_type count) {
    if (!count) return;
    count = MIN(self->count, count);
    truncate_newest(self, count - 1);
    if (!self->count) return;
    // now nuke multi cell chars in the last line deleted that overlap onto the new newest lines
    const index_type xnum = self->xnum;
    Line l = {.xnum = xnum, .text_cache = self->text_cache};
    RAII_ALLOC(CPUCell, cells, malloc(xnum * sizeof(CPUCell)));
    if (cells) {
        get_line(self, 0, &l);
        memcpy(cells, l.cpu_cells, xnum * sizeof(CPUCell));
    }
    truncate_newest(self, 1);
    if (!cells) return;
    for (index_type x = 0; x < xnum; x++) {
        const CPUCell c = cells[x];
        if (c.is_multicell && c.y) {
            // the rows above, stopping at the first that does not continue the char, as it may be partial or torn
            for (index_type lnum = 0; lnum < c.y && lnum < self->count; lnum++) {
                get_line_for_write(self, lnum, &l);
                if (!continues_multicell_char(l.cpu_cells + x, &c, c.y - 1 - lnum)) break;
                CPUCell *cc = l.cpu_cells + x;
                cell_set_char(cc, 0);
                cell_clear_multicell(cc);
                clear_sprite_position(l.gpu_cells[x]);
            }
        }
    }
}

// Clear the part of a multicell char in cell x of line lnum
static void
clear_multicell_cell(HistoryBuf *self, index_type lnum, index_type x) {
    Line l = {.xnum = self->xnum, .text_cache = self->text_cache};
    get_line_for_write(self, lnum, &l);
    CPUCell *c = l.cpu_cells + x;
    cell_set_char(c, 0);
    cell_clear_multicell(c);
    clear_sprite_position(l.gpu_cells[x]);
}

void
historybuf_delete_lines(HistoryBuf *self, index_type num_newer, index_type count) {
    // Delete count lines that have num_newer more recently added lines after
    // them, i.e. the lines with line numbers [num_newer, num_newer + count),
    // moving the num_newer lines up to take their place.
    if (!count || num_newer >= self->count) return;
    count = MIN(count, self->count - num_newer);
    const index_type oldest_deleted = num_newer + count - 1, xnum = self->xnum;
    // The newer lines are copied out, the lines from the oldest deleted one on
    // are dropped and the copies added back. Without memory for the copies
    // nothing is deleted.
    size_t ncells;
    if (__builtin_mul_overflow((size_t)num_newer, (size_t)xnum, &ncells)) {
        log_error("Too many scrollback lines to delete from the history");
        return;
    }
    RAII_ALLOC(CPUCell, cpu, num_newer ? malloc(ncells * sizeof(CPUCell)) : NULL);
    RAII_ALLOC(GPUCell, gpu, num_newer ? malloc(ncells * sizeof(GPUCell)) : NULL);
    RAII_ALLOC(LineAttrs, attrs, num_newer ? malloc((size_t)num_newer * sizeof(LineAttrs)) : NULL);
    if (num_newer && (!cpu || !gpu || !attrs)) {
        log_error("Out of memory deleting lines from the scrollback");
        return;
    }
    Line l = {.xnum = xnum, .text_cache = self->text_cache};
    // Remove the parts of multicell chars that are cut by the deletion: those
    // that start above the deleted lines and extend into them, stopping at the
    // first row that does not continue the char, as it may be partial or torn...
    RAII_ALLOC(CPUCell, oldest, malloc((size_t)xnum * sizeof(CPUCell)));
    if (oldest) {
        get_line(self, oldest_deleted, &l);
        memcpy(oldest, l.cpu_cells, (size_t)xnum * sizeof(CPUCell));
        for (index_type x = 0; x < xnum; x++) {
            const CPUCell c = oldest[x];
            if (c.is_multicell && c.y) {
                const index_type limit = MIN(oldest_deleted + c.y + 1, self->count);
                for (index_type lnum = oldest_deleted + 1; lnum < limit; lnum++) {
                    get_line(self, lnum, &l);
                    if (!continues_multicell_char(l.cpu_cells + x, &c, c.y - (lnum - oldest_deleted))) break;
                    clear_multicell_cell(self, lnum, x);
                }
            }
        }
    }
    // ...and those that start in or above the deleted lines and extend below them
    for (index_type below = 0; below < num_newer; below++) {
        const index_type lnum = num_newer - 1 - below;
        bool found = false;
        for (index_type x = 0; x < xnum; x++) {
            get_line(self, lnum, &l);
            const CPUCell c = l.cpu_cells[x];
            if (c.is_multicell && c.y > below) {
                found = true;
                clear_multicell_cell(self, lnum, x);
            }
        }
        if (!found) break; // a multicell char cannot skip a line
    }
    // Copy the newer lines out, oldest first
    for (index_type i = 0; i < num_newer; i++) {
        get_line(self, num_newer - 1 - i, &l);
        memcpy(cpu + (size_t)i * xnum, l.cpu_cells, (size_t)xnum * sizeof(CPUCell));
        memcpy(gpu + (size_t)i * xnum, l.gpu_cells, (size_t)xnum * sizeof(GPUCell));
        attrs[i] = l.attrs;
    }
    truncate_newest(self, num_newer + count);
    // The buffer has room for them now, so nothing goes to the pager history
    ANSIBuf unused = {0};
    for (index_type i = 0; i < num_newer; i++) {
        Line dest = {.xnum = xnum};
        LineAttrs *a;
        if (!push_slot(self, &unused, &dest, &a)) break;
        Line src = {.xnum = xnum, .cpu_cells = cpu + (size_t)i * xnum, .gpu_cells = gpu + (size_t)i * xnum, .text_cache = self->text_cache};
        copy_line(&src, &dest);
        *a = attrs[i];
        a->has_dirty_text = true;
    }
    free(unused.buf);
}

GPUCell *
historybuf_gpu_cells(HistoryBuf *self, index_type lnum) {
    // Valid until the next access to the buffer, which can decompress other lines in its place
    Line l = {.xnum = self->xnum, .text_cache = self->text_cache};
    get_line(self, lnum, &l);
    return l.gpu_cells;
}

// Calls body with info set to the line info byte of each visible line, oldest first
#define FOR_EACH_LINE_INFO(self, info, body)                                                                                         \
    {                                                                                                                                \
        const uint64_t oldest_visible_ = (self)->next_seq - (self)->count;                                                           \
        if ((self)->z) {                                                                                                             \
            for (size_t fi_ = 0; fi_ < (self)->z->num; fi_++) {                                                                      \
                HistoryFrame *f_ = frame_at((self)->z, fi_);                                                                         \
                flush_frame_chunks((self), f_);                                                                                      \
                for (index_type j_ = oldest_visible_ > f_->first_seq ? (index_type)(oldest_visible_ - f_->first_seq) : 0;            \
                     j_ < f_->num_lines; j_++) {                                                                                     \
                    const uint8_t info = f_->info[j_];                                                                               \
                    body                                                                                                             \
                }                                                                                                                    \
            }                                                                                                                        \
        }                                                                                                                            \
        for (size_t bi_ = 0; bi_ < (self)->num_blocks; bi_++) {                                                                      \
            LineBlock *b_ = block_at((self), bi_);                                                                                   \
            for (index_type j_ = oldest_visible_ > b_->first_seq ? (index_type)MIN(oldest_visible_ - b_->first_seq, b_->num_lines) : 0; \
                 j_ < b_->num_lines; j_++) {                                                                                         \
                const uint8_t info = block_cpu(b_) ? line_info(block_cpu(b_) + (size_t)j_ * (self)->xnum, (self)->xnum, true)          \
                                                   : (b_->reenc_info ? b_->reenc_info : b_->job->info)[j_];                              \
                body                                                                                                                 \
            }                                                                                                                        \
        }                                                                                                                            \
    }

index_type
historybuf_reusable_oldest_lines(HistoryBuf *self) {
    // count of oldest lines that rewrap to themselves at the same width
    index_type ans = 0, y = 0;
    bool prev_wrapped = false, prev_full = false;
    FOR_EACH_LINE_INFO(self, info, {
        if (y + 1 >= self->count) return ans;
        if (prev_wrapped && (!prev_full || (info & INFO_EMPTY))) return ans;
        if (info & INFO_BLOCKS_REUSE) return ans;
        prev_wrapped = info & INFO_WRAPPED;
        prev_full = info & INFO_FULL;
        if (!prev_wrapped) ans = y + 1;
        y++;
    });
    return ans;
}

void
historybuf_clear_trailing_blanks_of_oldest(HistoryBuf *self, index_type count) {
    count = MIN(count, self->count);
    const uint64_t oldest_visible = self->next_seq - self->count, end = oldest_visible + count;
    if (self->z) {
        for (size_t i = 0; i < self->z->num; i++) {
            HistoryFrame *f = frame_at(self->z, i);
            if (f->first_seq >= end) break;
            const index_type n = (index_type)MIN((uint64_t)f->num_lines, end - f->first_seq);
            // applied when the lines are decoded, as changing them would cost a recompression
            f->blank_clear_lines = MAX(f->blank_clear_lines, n);
            for (size_t c = 0; c < self->z->num_chunks; c++) {
                CellChunk *ch = self->z->chunks + c;
                if (ch->frame_id != f->id) continue;
                for (index_type j = 0; j < ch->num_lines && ch->chunk * CHUNK_LINES + j < n; j++)
                    clear_trailing_blanks(ch->cpu + (size_t)j * self->xnum, ch->gpu + (size_t)j * self->xnum, self->xnum);
            }
        }
    }
    for (size_t i = 0; i < self->num_blocks; i++) {
        LineBlock *b = block_at(self, i);
        if (b->first_seq >= end) break;
        const index_type n = (index_type)MIN((uint64_t)b->num_lines, end - b->first_seq);
        if (i < self->num_pending) {
            b->blank_clear_lines = MAX(b->blank_clear_lines, n);
            if (!b->shadow_cpu) continue; // the compressor's copy is cleared when decoded
        }
        for (index_type j = 0; j < n; j++) clear_trailing_blanks(block_cpu(b) + (size_t)j * self->xnum, block_gpu(b) + (size_t)j * self->xnum, self->xnum);
    }
}

static void
rewrite_pending_block(HistoryBuf *self, LineBlock *b, historybuf_cells_visitor visitor, void *data) {
    const size_t n = (size_t)b->num_lines * self->xnum;
    RAII_ALLOC(CPUCell, cpu, malloc(MAX(1u, n) * sizeof(CPUCell)));
    RAII_ALLOC(GPUCell, gpu, malloc(MAX(1u, n) * sizeof(GPUCell)));
    if (!cpu || !gpu) {
        log_error("Out of memory reading scrollback to garbage collect it");
        return;
    }
    bool ok = true;
    if (b->reenc.enc) {
        const uint8_t *p = b->reenc.enc, *end = p + b->reenc.enc_len;
        for (index_type i = 0; i < b->num_lines && (ok = p != NULL); i++) p = decode_line(p, end, cpu + (size_t)i * self->xnum, gpu + (size_t)i * self->xnum, self->xnum);
    } else if (b->cpu) {
        memcpy(cpu, b->cpu, n * sizeof(CPUCell));
        memcpy(gpu, b->gpu, n * sizeof(GPUCell));
    } else ok = decode_job_lines(self, b->job, b->num_lines, cpu, gpu);
    if (!ok) return;
    bool changed = false;
    const uint64_t oldest_visible = self->next_seq - self->count;
    for (index_type j = oldest_visible > b->first_seq ? (index_type)(oldest_visible - b->first_seq) : 0; j < b->num_lines; j++)
        if (visitor(data, cpu + (size_t)j * self->xnum, self->xnum)) changed = true;
    if (!changed) return;
    EncodeBuf e = {0};
    uint8_t *info = malloc(MAX(1u, b->num_lines)), has = 0;
    for (index_type j = 0; j < b->num_lines && info; j++) {
        if (!encode_line(&e, cpu + (size_t)j * self->xnum, gpu + (size_t)j * self->xnum, self->xnum, info + j, &has)) {
            free(info);
            info = NULL;
        }
    }
    if (!info) {
        // keep the change in a shadow instead
        free_encode_buf(&e);
        if (ensure_shadow(self, b)) {
            memcpy(b->shadow_cpu, cpu, n * sizeof(CPUCell));
            b->shadow_modified = true;
        } else log_error("Out of memory garbage collecting scrollback, a change is lost");
        return;
    }
    free_encode_buf(&b->reenc);
    free(b->reenc_info);
    b->reenc = e;
    b->reenc_info = info;
}

void
historybuf_rewrite_cpu_cells(HistoryBuf *self, unsigned needs, historybuf_cells_visitor visitor, void *data) {
    HistoryCompression *z = self->z;
    const uint64_t oldest_visible = self->next_seq - self->count;
    if (z) {
        for (size_t i = 0; i < z->num; i++) {
            HistoryFrame *f = frame_at(z, i);
            // a frame whose cells were changed may have gained the properties
            if (!(f->has & needs) && !frame_has_chunks(z, f)) continue;
            const bool had_raw = f->raw != NULL;
            const index_type first = oldest_visible > f->first_seq ? (index_type)(oldest_visible - f->first_seq) : 0;
            for (index_type c = first / CHUNK_LINES; c * CHUNK_LINES < f->num_lines; c++) {
                bool was_cached = false;
                for (size_t k = 0; k < z->num_chunks && !was_cached; k++) was_cached = z->chunks[k].frame_id == f->id && z->chunks[k].chunk == c;
                CellChunk *ch = ensure_chunk(self, f, c);
                if (!ch) {
                    log_error("Out of memory decompressing scrollback to garbage collect it");
                    break;
                }
                for (index_type l = 0; l < ch->num_lines; l++)
                    if (c * CHUNK_LINES + l >= first && visitor(data, ch->cpu + (size_t)l * self->xnum, self->xnum)) ch->modified = true;
                if (!was_cached) evict_chunk(self, (size_t)(ch - z->chunks));
            }
            if (f->raw_modified && !frame_has_chunks(z, f) && recompress_frame(self, f) && !had_raw) free_raw(f);
        }
    }
    for (size_t i = 0; i < self->num_blocks; i++) {
        LineBlock *b = block_at(self, i);
        if (b->first_seq + b->num_lines <= oldest_visible) continue;
        if (i < self->num_pending && !b->shadow_cpu) {
            // Rewrite a copy, kept, if changed, encoded: copying every pending
            // block would hold the compressor's backlog uncompressed twice
            if (!b->reenc.enc && !b->cpu && !(b->job->has & needs)) continue;
            rewrite_pending_block(self, b, visitor, data);
            continue;
        }
        // lines past the scrollback limit are never shown again: leave them out, so that what only they use is freed
        for (index_type j = oldest_visible > b->first_seq ? (index_type)(oldest_visible - b->first_seq) : 0; j < b->num_lines; j++)
            if (visitor(data, block_cpu(b) + (size_t)j * self->xnum, self->xnum) && i < self->num_pending) b->shadow_modified = true;
    }
}

void
historybuf_apply_marker(HistoryBuf *self, PyObject *marker, ANSIBuf *buf) {
    Line l = {.xnum = self->xnum, .text_cache = self->text_cache};
    if (self->z) {
        // Lines not in cached chunks hold no marks: compressed lines are stored
        // without them and are dirty, so rendering will mark them.
        for (size_t i = 0; i < self->z->num_chunks; i++) {
            CellChunk *ch = self->z->chunks + i;
            for (index_type j = 0; j < ch->num_lines; j++) {
                l.cpu_cells = ch->cpu + (size_t)j * self->xnum;
                l.gpu_cells = ch->gpu + (size_t)j * self->xnum;
                mark_text_in_line(marker, &l, buf);
            }
        }
    }
    for (size_t i = 0; i < self->num_blocks; i++) {
        LineBlock *b = block_at(self, i);
        if (i < self->num_pending && !ensure_shadow(self, b)) continue;
        for (index_type j = 0; j < b->num_lines; j++) {
            l.cpu_cells = block_cpu(b) + (size_t)j * self->xnum;
            l.gpu_cells = block_gpu(b) + (size_t)j * self->xnum;
            mark_text_in_line(marker, &l, buf);
        }
    }
}

void
historybuf_set_mode(HistoryBuf *self, index_type lines, bool compress, int level, bool recompress) {
    retire(self);
    const index_type ynum = MIN(MAX(1u, lines), HISTORY_UNLIMITED);
    if (ynum < self->count) {
        self->count = ynum;
        drop_invisible(self);
    }
    self->ynum = ynum;
    self->compress = compress;
    self->level = clamp_level(level);
    if (compress) {
        // hand off what has accumulated in the hot tail while compression was off
        while (self->hot_lines >= self->hot_target + self->block_lines && self->num_blocks - self->num_pending >= 2) hand_off_oldest_hot_block(self);
    }
    HistoryCompression *z = self->z;
    if (!recompress || !z) return;
    for (size_t i = 0; i < z->num; i++) {
        HistoryFrame *f = frame_at(z, i);
        if (f->open || f->replacement || f->tail_len || f->raw_modified || frame_has_chunks(z, f) || !f->size) continue;
        CompressJob *j = calloc(1, sizeof(CompressJob));
        if (!j || !(j->in = malloc(f->size))) {
            free(j);
            log_error("Out of memory recompressing scrollback");
            return;
        }
        memcpy(j->in, f->data, f->size);
        j->in_len = f->size;
        j->raw_size = f->data_raw;
        j->kind = JOB_DATA;
        j->z = z;
        j->frame_id = f->id;
        j->frame_version = f->version;
        j->level = self->level;
        atomic_init(&j->done, false);
        j->next = z->rjobs;
        z->rjobs = j;
        submit_job(j);
    }
    register_active(self);
}

static PyObject *
line(HistoryBuf *self, PyObject *val) {
#define line_doc "Return the line with line number val. This buffer grows upwards, i.e. 0 is the most recently added line"
    if (self->count == 0) {
        PyErr_SetString(PyExc_IndexError, "This buffer is empty");
        return NULL;
    }
    index_type lnum = PyLong_AsUnsignedLong(val);
    if (lnum >= self->count) {
        PyErr_SetString(PyExc_IndexError, "Out of bounds");
        return NULL;
    }
    // Python may change the line, so a compressed line is compressed again when it leaves the cache
    get_line_for_write(self, lnum, self->line);
    Py_INCREF(self->line);
    return (PyObject *)self->line;
}

static PyObject *
__str__(HistoryBuf *self) {
    PyObject *lines = PyTuple_New(self->count);
    if (lines == NULL) return PyErr_NoMemory();
    RAII_ANSIBuf(buf);
    Line l = {.xnum = self->xnum, .text_cache = self->text_cache};
    for (index_type i = 0; i < self->count; i++) {
        get_line(self, i, &l);
        PyObject *t = line_as_unicode(&l, false, &buf);
        if (t == NULL) {
            Py_CLEAR(lines);
            return NULL;
        }
        PyTuple_SET_ITEM(lines, i, t);
    }
    PyObject *sep = PyUnicode_FromString("\n");
    PyObject *ans = PyUnicode_Join(sep, lines);
    Py_CLEAR(lines);
    Py_CLEAR(sep);
    return ans;
}

static PyObject *
push(HistoryBuf *self, PyObject *args) {
#define push_doc "Push a line into this buffer, removing the oldest line, if necessary"
    Line *line;
    if (!PyArg_ParseTuple(args, "O!", &Line_Type, &line)) return NULL;
    ANSIBuf as_ansi_buf = {0};
    historybuf_add_line(self, line, &as_ansi_buf);
    free(as_ansi_buf.buf);
    Py_RETURN_NONE;
}

static PyObject *
as_ansi(HistoryBuf *self, PyObject *callback) {
#define as_ansi_doc "as_ansi(callback) -> The contents of this buffer as ANSI escaped text, oldest line first. callback is called with each successive line."
    Line l = {.xnum = self->xnum, .text_cache = self->text_cache};
    ANSIBuf output = {0};
    ANSILineState s = {.output_buf = &output};
    for (index_type i = 0; i < self->count; i++) {
        get_line(self, self->count - 1 - i, &l);
        output.len = 0;
        line_as_ansi(&l, &s, 0, l.xnum, 0, true);
        if (!l.cpu_cells[l.xnum - 1].next_char_was_wrapped) {
            ensure_space_for(&output, buf, Py_UCS4, output.len + 1, capacity, 2048, false);
            output.buf[output.len++] = '\n';
        }
        PyObject *ans = PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, output.buf, output.len);
        if (ans == NULL) {
            PyErr_NoMemory();
            goto end;
        }
        PyObject *ret = PyObject_CallFunctionObjArgs(callback, ans, NULL);
        Py_CLEAR(ans);
        if (ret == NULL) goto end;
        Py_CLEAR(ret);
    }
end:
    free(output.buf);
    if (PyErr_Occurred()) return NULL;
    Py_RETURN_NONE;
}
static char_type
pagerhist_remove_char(PagerHistoryBuf *ph, unsigned *count, uint8_t record[8]) {
    uint32_t codep;
    UTF8State state = UTF8_ACCEPT;
    *count = 0;
    size_t num = ringbuf_bytes_used(ph->ringbuf);
    while (num--) {
        record[*count] = ringbuf_move_char(ph->ringbuf);
        decode_utf8(&state, &codep, record[*count]);
        *count += 1;
        if (state == UTF8_REJECT) {
            codep = 0;
            break;
        }
        if (state == UTF8_ACCEPT) break;
    }
    return codep;
}

static void
pagerhist_rewrap_to(HistoryBuf *self, index_type cells_in_line) {
    PagerHistoryBuf *ph = self->pagerhist;
    if (!ph->ringbuf || !ringbuf_bytes_used(ph->ringbuf)) return;
    PagerHistoryBuf *nph = calloc(1, sizeof(PagerHistoryBuf));
    if (!nph) return;
    nph->maximum_size = ph->maximum_size;
    nph->ringbuf = ringbuf_new(MIN(ph->maximum_size, ringbuf_capacity(ph->ringbuf) + 4096));
    if (!nph->ringbuf) {
        free(nph);
        return;
    }
    ssize_t ch_width = 0;
    unsigned count;
    uint8_t record[8];
    index_type num_in_current_line = 0;
    char_type ch;
    WCSState wcs_state;
    initialize_wcs_state(&wcs_state);

#define WRITE_CHAR()                                                                                 \
    {                                                                                                \
        if (num_in_current_line + ch_width > cells_in_line) {                                        \
            pagerhist_write_bytes(nph, (const uint8_t *)"\r", 1);                                    \
            num_in_current_line = 0;                                                                 \
        }                                                                                            \
        if (ch_width >= 0 || (int)num_in_current_line >= -ch_width) num_in_current_line += ch_width; \
        pagerhist_write_bytes(nph, record, count);                                                   \
    }

    while (ringbuf_bytes_used(ph->ringbuf)) {
        ch = pagerhist_remove_char(ph, &count, record);
        if (ch == '\n') {
            initialize_wcs_state(&wcs_state);
            ch_width = 1;
            WRITE_CHAR();
            num_in_current_line = 0;
        } else if (ch != '\r') {
            ch_width = wcswidth_step(&wcs_state, ch);
            WRITE_CHAR();
        }
    }
    free_pagerhist(self);
    self->pagerhist = nph;
#undef WRITE_CHAR
}

static PyObject *
pagerhist_write(HistoryBuf *self, PyObject *what) {
    if (self->pagerhist && self->pagerhist->maximum_size) {
        if (PyBytes_Check(what)) pagerhist_write_bytes(self->pagerhist, (const uint8_t *)PyBytes_AS_STRING(what), PyBytes_GET_SIZE(what));
        else if (PyUnicode_Check(what) && PyUnicode_READY(what) == 0) {
            Py_UCS4 *buf = PyUnicode_AsUCS4Copy(what);
            if (buf) {
                pagerhist_write_ucs4(self->pagerhist, buf, PyUnicode_GET_LENGTH(what));
                PyMem_Free(buf);
            }
        }
    }
    Py_RETURN_NONE;
}

static const uint8_t *
reverse_find(const uint8_t *haystack, size_t haystack_sz, const uint8_t *needle) {
    const size_t needle_sz = strlen((const char *)needle);
    if (!needle_sz || needle_sz > haystack_sz) return NULL;
    const uint8_t *p = haystack + haystack_sz - (needle_sz - 1);
    while (--p >= haystack) {
        if (*p == needle[0] && memcmp(p, needle, MIN(needle_sz, haystack_sz - (p - haystack))) == 0) return p;
    }
    return NULL;
}

static PyObject *
pagerhist_as_bytes(HistoryBuf *self, PyObject *args) {
    int upto_output_start = 0;
    if (!PyArg_ParseTuple(args, "|p", &upto_output_start)) return NULL;
#define ph self->pagerhist
    if (!ph || !ringbuf_bytes_used(ph->ringbuf)) return PyBytes_FromStringAndSize("", 0);
    pagerhist_ensure_start_is_valid_utf8(ph);
    if (ph->rewrap_needed) pagerhist_rewrap_to(self, self->xnum);

    size_t sz = ringbuf_bytes_used(ph->ringbuf);
    PyObject *ans = PyBytes_FromStringAndSize(NULL, sz);
    if (!ans) return NULL;
    uint8_t *buf = (uint8_t *)PyBytes_AS_STRING(ans);
    ringbuf_memcpy_from(buf, ph->ringbuf, sz);
    if (upto_output_start) {
        const uint8_t *p = reverse_find(buf, sz, (const uint8_t *)"\x1b]133;C\x1b\\");
        if (p) {
            PyObject *t = PyBytes_FromStringAndSize((const char *)p, sz - (p - buf));
            Py_DECREF(ans);
            ans = t;
        }
    }
    return ans;
#undef ph
}

static PyObject *
pagerhist_as_text(HistoryBuf *self, PyObject *args) {
    PyObject *ans = NULL;
    PyObject *bytes = pagerhist_as_bytes(self, args);
    if (bytes) {
        ans = PyUnicode_DecodeUTF8(PyBytes_AS_STRING(bytes), PyBytes_GET_SIZE(bytes), "ignore");
        Py_DECREF(bytes);
    }
    return ans;
}

typedef struct {
    Line line;
    HistoryBuf *self;
} GetLineWrapper;

static Line *
get_line_wrapper(void *x, int y) {
    GetLineWrapper *glw = x;
    get_line(glw->self, glw->self->count - y - 1, &glw->line);
    return &glw->line;
}

PyObject *
as_text_history_buf(HistoryBuf *self, PyObject *args, ANSIBuf *output) {
    GetLineWrapper glw = {.self = self};
    glw.line.xnum = self->xnum;
    glw.line.text_cache = self->text_cache;
    PyObject *ans = as_text_generic(args, &glw, get_line_wrapper, self->count, output, true);
    return ans;
}

static PyObject *
dirty_lines(HistoryBuf *self, PyObject *a UNUSED) {
#define dirty_lines_doc "dirty_lines() -> Line numbers of all lines that have dirty text."
    PyObject *ans = PyList_New(0);
    if (!ans) return NULL;
    for (index_type i = 0; i < self->count; i++) {
        if (attrs_for_lnum(self, i)->has_dirty_text) {
            RAII_PyObject(n, PyLong_FromUnsignedLong(i));
            if (!n || PyList_Append(ans, n) != 0) {
                Py_DECREF(ans);
                return NULL;
            }
        }
    }
    return ans;
}

static PyObject *
pagerhist_rewrap(HistoryBuf *self, PyObject *xnum) {
    if (self->pagerhist) { pagerhist_rewrap_to(self, PyLong_AsUnsignedLong(xnum)); }
    Py_RETURN_NONE;
}

static PyObject *
is_continued(HistoryBuf *self, PyObject *val) {
#define is_continued_doc "is_continued(y) -> Whether the line y is continued or not"
    unsigned long y = PyLong_AsUnsignedLong(val);
    if (y >= self->count) {
        PyErr_SetString(PyExc_ValueError, "Out of bounds.");
        return NULL;
    }
    if (hb_line_is_continued(self, self->count - y - 1)) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static PyObject *
endswith_wrap(HistoryBuf *self, PyObject *val UNUSED) {
#define endswith_wrap_doc "endswith_wrap() -> Whether the last line is wrapped at the end of the buffer"
    if (history_buf_endswith_wrap(self)) { Py_RETURN_TRUE; }
    Py_RETURN_FALSE;
}

static PyObject *
compression_stats(HistoryBuf *self, PyObject *a UNUSED) {
#define compression_stats_doc "compression_stats() -> A dict describing the compressed part of the buffer and its memory use"
    retire(self);
    size_t frames = 0, compressed = 0, raw = 0, side = 0, raw_cache = 0, chunks = 0, chunk_bytes = 0, contexts = 0, scratch = 0, jobs = 0;
    size_t lines_in_frames = 0, max_frame_lines = 0;
    unsigned long long lines_compressed = 0, frames_decompressed = 0, frames_recompressed = 0;
    HistoryCompression *z = self->z;
    if (z) {
        frames = z->num;
        for (size_t i = 0; i < z->num; i++) {
            HistoryFrame *f = frame_at(z, i);
            compressed += f->capacity + f->tail_len;
            raw += f->data_raw + f->tail_len;
            side += (size_t)f->lines_capacity * (sizeof(LineAttrs) + 1);
            raw_cache += f->raw_capacity + f->chunk_offsets_capacity * sizeof(size_t);
            lines_in_frames += f->num_lines;
            max_frame_lines = MAX(max_frame_lines, (size_t)f->num_lines);
            if (f->replacement) jobs++;
        }
        chunks = z->num_chunks;
        chunk_bytes = z->num_chunks * (size_t)CHUNK_LINES * self->xnum * (sizeof(CPUCell) + sizeof(GPUCell));
        contexts = atomic_load_explicit(&z->stream.cctx_bytes, memory_order_relaxed) + ZSTD_sizeof_DCtx(z->dctx);
        scratch = z->enc.enc_capacity + z->enc.runs_capacity + z->capacity * sizeof(HistoryFrame) + z->chunks_capacity * sizeof(CellChunk);
        for (CompressJob *j = z->jobs_head; j; j = j->next) jobs++;
        lines_compressed = z->lines_compressed;
        frames_decompressed = z->frames_decompressed;
        frames_recompressed = z->frames_recompressed;
    }
    size_t hot_bytes = 0, pending_bytes = 0;
    for (size_t i = 0; i < self->num_blocks; i++) {
        LineBlock *b = block_at(self, i);
        size_t sz = (b->mem ? b->mem_size : 0) + (b->shadow_cpu ? (size_t)b->capacity * self->xnum * (sizeof(CPUCell) + sizeof(GPUCell)) : 0);
        // a pending block whose cells were released holds its lines encoded
        if (!b->mem && b->job) sz += b->job->enc.enc_capacity;
        if (i < self->num_pending) pending_bytes += sz;
        else hot_bytes += sz;
    }
    hot_bytes += self->num_spare_cells * cells_size(self);
    const size_t total = hot_bytes + pending_bytes + compressed + side + raw_cache + chunk_bytes + contexts + scratch;
    return Py_BuildValue("{sO sI sI sI sI sI sI sn sn sn sn sn sn sn sn sn sn sn sn sn sK sK sK si sn sI}", "compress", self->compress ? Py_True : Py_False,
        "count", self->count, "ynum", self->ynum, "hot_lines", self->hot_lines, "pending_lines", self->pending_lines, "compressed_lines",
        (unsigned)lines_in_frames, "max_frame_lines", (unsigned)max_frame_lines, "frames", (Py_ssize_t)frames, "compressed_bytes", (Py_ssize_t)compressed,
        "encoded_bytes", (Py_ssize_t)raw, "side_bytes", (Py_ssize_t)side, "raw_cache_bytes", (Py_ssize_t)raw_cache, "cached_chunks", (Py_ssize_t)chunks,
        "chunk_bytes", (Py_ssize_t)chunk_bytes, "context_bytes", (Py_ssize_t)contexts, "scratch_bytes", (Py_ssize_t)scratch, "hot_bytes",
        (Py_ssize_t)hot_bytes, "pending_bytes", (Py_ssize_t)pending_bytes, "jobs", (Py_ssize_t)jobs, "total_bytes", (Py_ssize_t)total,
        "lines_compressed", lines_compressed, "frames_decompressed", frames_decompressed, "frames_recompressed", frames_recompressed, "level",
        self->level, "frame_bytes", (Py_ssize_t)self->frame_bytes, "block_lines", self->block_lines);
}

static PyObject *
compressor_stats(PyObject *self UNUSED, PyObject *a UNUSED) {
#define compressor_stats_doc "compressor_stats() -> CPU time and work of the compressor thread, shared by all buffers"
    pthread_mutex_lock(&W.lock);
    size_t queued = 0;
    for (int q = 0; q < NUM_QUEUES; q++)
        for (CompressJob *j = W.head[q]; j; j = j->queue_next) queued++;
    PyObject *ans = Py_BuildValue("{sd sK sK sK sn sO}", "cpu_seconds", (double)W.cpu_ns / 1e9, "jobs", (unsigned long long)W.jobs, "bytes_in",
        (unsigned long long)W.bytes_in, "bytes_out", (unsigned long long)W.bytes_out, "queued", (Py_ssize_t)queued, "thread", W.started ? Py_True : Py_False);
    pthread_mutex_unlock(&W.lock);
    return ans;
}

static PyObject *
zstd_levels(PyObject *self UNUSED, PyObject *a UNUSED) {
#define zstd_levels_doc "zstd_levels() -> (min, max, default) compression levels of the zstd library in use"
    return Py_BuildValue("iii", ZSTD_minCLevel(), ZSTD_maxCLevel(), ZSTD_CLEVEL_DEFAULT);
}

static PyObject *
cache_trim(HistoryBuf *self, PyObject *args) {
#define cache_trim_doc "cache_trim(render_start=0) -> Trim the decompression cache as after a render, for tests"
    unsigned long long render_start = 0;
    if (!PyArg_ParseTuple(args, "|K", &render_start)) return NULL;
    historybuf_cache_trim(self, render_start);
    Py_RETURN_NONE;
}

static PyObject *
wait_for_compression(HistoryBuf *self, PyObject *a UNUSED) {
#define wait_for_compression_doc "wait_for_compression() -> Wait for the compressor thread to finish this buffer's work and take it in"
    Py_BEGIN_ALLOW_THREADS;
    wait_for_compressor(self->z);
    Py_END_ALLOW_THREADS;
    retire(self);
    Py_RETURN_NONE;
}

static PyObject *
set_mode(HistoryBuf *self, PyObject *args) {
#define set_mode_doc "set_mode(lines, compress, level, recompress=False) -> Change the line limit and compression"
    unsigned int lines;
    int compress, level, recompress = 0;
    if (!PyArg_ParseTuple(args, "Ipi|p", &lines, &compress, &level, &recompress)) return NULL;
    historybuf_set_mode(self, lines, compress, level, recompress);
    Py_RETURN_NONE;
}


// Boilerplate {{{
static PyObject *rewrap(HistoryBuf *self, PyObject *args);
#define rewrap_doc ""

static PyMethodDef methods[] = {
    METHOD(line, METH_O) METHOD(is_continued, METH_O) METHOD(endswith_wrap, METH_NOARGS) METHOD(as_ansi, METH_O) METHODB(pagerhist_write, METH_O),
    METHODB(pagerhist_rewrap, METH_O),
    METHODB(pagerhist_as_text, METH_VARARGS),
    METHODB(pagerhist_as_bytes, METH_VARARGS),
    METHOD(compression_stats, METH_NOARGS) METHOD(cache_trim, METH_VARARGS) METHOD(wait_for_compression, METH_NOARGS) METHOD(set_mode, METH_VARARGS)
        {"compressor_stats", (PyCFunction)compressor_stats, METH_NOARGS | METH_STATIC, compressor_stats_doc},
    {"zstd_levels", (PyCFunction)zstd_levels, METH_NOARGS | METH_STATIC, zstd_levels_doc},
    METHOD(dirty_lines, METH_NOARGS) METHOD(push, METH_VARARGS) METHOD(rewrap, METH_VARARGS){NULL, NULL, 0, NULL} /* Sentinel */
};

static PyMemberDef members[] = {
    {"xnum", T_UINT, offsetof(HistoryBuf, xnum), READONLY, "xnum"},
    {"ynum", T_UINT, offsetof(HistoryBuf, ynum), READONLY, "ynum"},
    {"count", T_UINT, offsetof(HistoryBuf, count), READONLY, "count"},
    {NULL} /* Sentinel */
};

PyTypeObject HistoryBuf_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "fast_data_types.HistoryBuf",
    .tp_basicsize = sizeof(HistoryBuf),
    .tp_dealloc = (destructor)dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "History buffers",
    .tp_methods = methods,
    .tp_members = members,
    .tp_str = (reprfunc)__str__,
    .tp_new = new_history_object};

INIT_TYPE(HistoryBuf)

HistoryBuf *
alloc_historybuf(unsigned int lines, unsigned int columns, unsigned int pagerhist_sz, TextCache *tc, bool compress, int level) {
    return create_historybuf(&HistoryBuf_Type, columns, lines, pagerhist_sz, tc, compress, level, &compression_params);
}

HistoryBuf *
historybuf_alloc_like(HistoryBuf *like, unsigned int lines, unsigned int columns, unsigned int pagerhist_sz) {
    // with the compression settings of like
    return create_historybuf(&HistoryBuf_Type, columns, lines, pagerhist_sz, like->text_cache, like->compress, like->level, &like->zparams);
}

// }}}

static void
history_buf_set_last_char_as_continuation(HistoryBuf *self, index_type y, bool wrapped) {
    if (self->count > 0) {
        Line l = {.xnum = self->xnum};
        get_line_for_write(self, y, &l);
        l.cpu_cells[self->xnum - 1].next_char_was_wrapped = wrapped;
    }
}

index_type
historybuf_next_dest_line(HistoryBuf *self, ANSIBuf *as_ansi_buf, Line *src_line, index_type dest_y, Line *dest_line, bool continued) {
    history_buf_set_last_char_as_continuation(self, 0, continued);
    LineAttrs *attrs;
    if (push_slot(self, as_ansi_buf, dest_line, &attrs)) {
        *attrs = src_line->attrs;
        dest_line->attrs = src_line->attrs;
        // the slot may hold a stale line
        zero_at_ptr_count(dest_line->cpu_cells, dest_line->xnum);
        zero_at_ptr_count(dest_line->gpu_cells, dest_line->xnum);
    } else blank_line(self, dest_line); // out of memory: the line is written to scratch and lost
    return dest_y + 1;
}

HistoryBuf *
historybuf_alloc_for_rewrap(unsigned int columns, HistoryBuf *self) {
    if (!self) return NULL;
    return historybuf_alloc_like(self, self->ynum, columns, 0);
}

void
historybuf_finish_rewrap(HistoryBuf *dest, HistoryBuf *src) {
    historybuf_mark_all_dirty(dest);
    dest->pagerhist = src->pagerhist;
    src->pagerhist = NULL;
    if (dest->pagerhist && dest->xnum != src->xnum && ringbuf_bytes_used(dest->pagerhist->ringbuf)) dest->pagerhist->rewrap_needed = true;
}

static bool
copy_history(HistoryBuf *dest, HistoryBuf *src) {
    // dest is empty, with the width and settings of src
    wait_for_compressor(src->z);
    retire(src);
    dest->next_seq = src->next_seq;
    HistoryCompression *sz = src->z;
    if (sz && sz->num) {
        if (!dest->z && !(dest->z = alloc_compression())) return false;
        for (size_t i = 0; i < sz->num; i++) {
            HistoryFrame *s = frame_at(sz, i);
            flush_frame_chunks(src, s);
            HistoryFrame *d = frames_append(dest->z);
            if (!d) return false;
            *d = (HistoryFrame){.id = ++dest->z->next_frame_id, .first_seq = s->first_seq, .num_lines = s->num_lines,
                                .blank_clear_lines = s->blank_clear_lines, .has = s->has};
            if (!grow_lines(d, MAX(1u, s->num_lines))) return false;
            memcpy(d->attrs, s->attrs, s->num_lines * sizeof(LineAttrs));
            memcpy(d->info, s->info, s->num_lines);
            if (s->raw_modified || s->replacement) {
                // the content is in the encoded lines: copy them as such
                if (!ensure_raw(src, s) || !(d->tail = malloc(MAX(1u, s->raw_len)))) return false;
                memcpy(d->tail, s->raw, s->raw_len);
                d->tail_len = s->raw_len;
                d->blank_clear_lines = s->replacement ? 0 : s->blank_clear_lines;
            } else {
                if (!(d->data = malloc(MAX(1u, s->size))) || (s->tail_len && !(d->tail = malloc(s->tail_len)))) return false;
                memcpy(d->data, s->data, s->size);
                d->size = d->capacity = s->size;
                d->data_raw = s->data_raw;
                if (s->tail_len) memcpy(d->tail, s->tail, s->tail_len);
                d->tail_len = s->tail_len;
            }
        }
        dest->z->force_new_frame = true;
    }
    for (size_t i = 0; i < src->num_blocks; i++) {
        LineBlock *s = block_at(src, i), *d = alloc_block(dest);
        if (!d || !append_block(dest, d)) {
            if (d) release_block_memory(d);
            return false;
        }
        d->first_seq = s->first_seq;
        d->num_lines = s->num_lines;
        memcpy(d->cpu, block_cpu(s), (size_t)s->num_lines * src->xnum * sizeof(CPUCell));
        memcpy(d->gpu, block_gpu(s), (size_t)s->num_lines * src->xnum * sizeof(GPUCell));
        memcpy(d->attrs, s->attrs, s->num_lines * sizeof(LineAttrs));
        dest->hot_lines += s->num_lines;
    }
    dest->count = src->count;
    return true;
}

void
historybuf_fast_rewrap(HistoryBuf *dest, HistoryBuf *src, index_type count, bool steal) {
    // dest must have been created by historybuf_alloc_for_rewrap() with the same width as src
    if (steal) {
#define S(x) SWAP(dest->x, src->x)
        S(blocks);
        S(blocks_head);
        S(num_blocks);
        S(blocks_capacity);
        S(num_pending);
        S(hot_lines);
        S(pending_lines);
        S(count);
        S(next_seq);
        S(z);
#undef S
        dest->block_hint = src->block_hint = 0;
        if (dest->z && (dest->z->jobs_head || dest->z->rjobs)) register_active(dest);
    } else if (!copy_history(dest, src)) {
        log_error("Out of memory copying the scrollback, dropping it");
        free_all_storage(dest);
    }
    if (count < dest->count) truncate_newest(dest, dest->count - count);
}


static PyObject *
rewrap(HistoryBuf *self, PyObject *args) {
    unsigned xnum;
    if (!PyArg_ParseTuple(args, "I", &xnum)) return NULL;
    ANSIBuf as_ansi_buf = {0};
    LineBuf *dummy = alloc_linebuf(4, self->xnum, self->text_cache);
    if (!dummy) return PyErr_NoMemory();
    RAII_PyObject(cleanup, (PyObject *)dummy);
    (void)cleanup;
    TrackCursor cursors[1] = {{.is_sentinel = true}};
    ResizeResult r = resize_screen_buffers(dummy, self, HISTORY_PRESERVE, 8, xnum, &as_ansi_buf, cursors);
    free(as_ansi_buf.buf);
    if (!r.ok) return PyErr_NoMemory();
    Py_CLEAR(r.lb);
    return (PyObject *)r.hb;
}
