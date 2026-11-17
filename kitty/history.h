/*
 * history.h
 * Copyright (C) 2024 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#pragma once

#include "line.h"

typedef struct {
    void *ringbuf;
    size_t maximum_size;
    bool rewrap_needed;
} PagerHistoryBuf;

// The blocks of uncompressed lines and the compressed frames, see history.c
typedef struct LineBlock LineBlock;
typedef struct HistoryCompression HistoryCompression;

typedef struct {
    // 0 means the built in default for each
    unsigned hot_lines, block_lines;
    size_t frame_bytes;
} HistoryCompressionParams;

typedef struct {
    PyObject_HEAD

        // ynum is the scrollback limit: the maximum number of lines kept,
        // HISTORY_UNLIMITED for unlimited scrollback
        index_type xnum,
        ynum;
    PagerHistoryBuf *pagerhist;
    Line *line;
    TextCache *text_cache;
    // number of lines in the buffer: compressed, waiting for the compressor and uncompressed
    index_type count;
    // sequence number the next line pushed will get; line numbers count back
    // from the newest line, which has sequence number next_seq - 1
    uint64_t next_seq;
    // changed whenever lines other than the oldest leave the buffer or the
    // sequence numbers are reused (clear, delete, pop), see kitty/search.c
    uint64_t generation;
    // The uncompressed lines, oldest first, in blocks of block_lines lines:
    // the first num_pending blocks are being compressed, the rest are the hot tail.
    LineBlock **blocks;
    size_t blocks_head, num_blocks, blocks_capacity, num_pending, block_hint;
    // the first pending_released pending blocks have been encoded by the compressor and their cells released
    size_t pending_released;
    index_type hot_lines, pending_lines, block_lines, hot_target;
    size_t frame_bytes;
    // cell memory of freed blocks, for reuse: freed when output stops, see historybuf_cache_trim()
    void **spare_cells;
    size_t num_spare_cells, spare_cells_capacity, spare_cells_size;
    uint64_t pushes, pushes_at_trim;
    bool compress;
    int level;
    HistoryCompression *z; // the compressed frames, NULL until the first line is compressed
    HistoryCompressionParams zparams;
    bool on_active_list;
    // returned, re-zeroed, when a line cannot be produced (empty buffer or a
    // compressed frame that cannot be decompressed for want of memory)
    CPUCell *blank_cpu;
    GPUCell *blank_gpu;
} HistoryBuf;

// scrollback_lines at or above this is unlimited: screen code addresses history lines with ints
#define HISTORY_UNLIMITED ((index_type)INT_MAX)

HistoryBuf *alloc_historybuf(unsigned int lines, unsigned int columns, unsigned int pagerhist_sz, TextCache *tc, bool compress, int level);
HistoryBuf *historybuf_alloc_like(HistoryBuf *like, unsigned int lines, unsigned int columns, unsigned int pagerhist_sz);
HistoryBuf *historybuf_alloc_for_rewrap(unsigned int columns, HistoryBuf *self);
void historybuf_finish_rewrap(HistoryBuf *dest, HistoryBuf *src);
void historybuf_fast_rewrap(HistoryBuf *dest, HistoryBuf *src, index_type count, bool steal);
index_type historybuf_next_dest_line(HistoryBuf *self, ANSIBuf *as_ansi_buf, Line *src_line, index_type dest_y, Line *dest_line, bool continued);
bool historybuf_is_line_continued(HistoryBuf *self, index_type lnum);
void historybuf_delete_newest_lines(HistoryBuf *self, index_type count);
void historybuf_delete_lines(HistoryBuf *self, index_type num_newer, index_type count);
// count of the oldest lines that rewrap to themselves at the same width
index_type historybuf_reusable_oldest_lines(HistoryBuf *self);
// clear the blank cells after the last non-blank char of the oldest count lines
void historybuf_clear_trailing_blanks_of_oldest(HistoryBuf *self, index_type count);
// whether line lnum is compressed or waiting to be: its cells are a copy, marks are computed when it is rendered
bool historybuf_line_is_compressed(HistoryBuf *self, index_type lnum);
// Rewrite the CPU cells of every line in place. visitor returns true if it
// changed the cells. Compressed frames are skipped unless they hold cells with
// one of the given properties (HISTORY_HAS_*), then decompressed, visited and
// recompressed if changed.
#define HISTORY_HAS_HYPERLINKS 1u
#define HISTORY_HAS_TEXT_IDX 2u
// a foreground, background or decoration color word of DEEP_COLOR_TYPE, see deep-colors.h
#define HISTORY_HAS_DEEP_COLORS 4u
typedef bool (*historybuf_cells_visitor)(void *data, CPUCell *cells, index_type xnum);
void historybuf_rewrite_cpu_cells(HistoryBuf *self, unsigned needs, historybuf_cells_visitor visitor, void *data);
// Call visitor with the GPU cells of every visible line, read only. Compressed
// frames and encoded blocks that hold no cells with one of the given properties
// (HISTORY_HAS_*) are skipped without being decompressed.
typedef void (*historybuf_gpu_cells_visitor)(void *data, const GPUCell *cells, index_type xnum);
void historybuf_visit_gpu_cells(HistoryBuf *self, unsigned needs, historybuf_gpu_cells_visitor visitor, void *data);
// Apply a marker to the lines whose cells are in memory and drop the marks the
// compressed lines carry, which are recomputed when those lines are rendered.
void historybuf_apply_marker(HistoryBuf *self, PyObject *marker, ANSIBuf *buf);
void historybuf_mark_all_dirty(HistoryBuf *self);
// The decompression cache: call historybuf_cache_clock() before rendering and
// historybuf_cache_trim() with its value after, see history.c
uint64_t historybuf_cache_clock(HistoryBuf *self);
void historybuf_cache_trim(HistoryBuf *self, uint64_t render_start);
void historybuf_set_compression_params(const HistoryCompressionParams *p);
// Change the scrollback limit (lines past a lower limit are dropped), whether
// lines are compressed and at which level. Compression applies to lines
// compressed from now on, and with recompress to the frames compressed already.
void historybuf_set_mode(HistoryBuf *self, index_type lines, bool compress, int level, bool recompress);
// Take in the work the compressor thread has finished, for every buffer. Called from the main loop.
void historybuf_retire_all(void);
// Called by the compressor thread when it runs out of work, to wake the main loop
void historybuf_set_wakeup(void (*wakeup)(void));
