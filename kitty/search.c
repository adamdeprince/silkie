/*
 * search.c
 * Copyright (C) 2026 Adam DePrince
 *
 * Distributed under terms of the GPL3 license.
 */

/*
 * Find
 * ====
 *
 * Searches the text of a screen, and on the main screen its scrollback, for a
 * literal string or a Python regular expression and keeps the matches as cell
 * ranges, which the renderer highlights through two bits of the per cell
 * selection byte (SEARCH_MATCH_MASK, SEARCH_CURRENT_MASK). Cell colors are
 * never changed, so the marks of create_marker and the colors programs chose
 * are untouched.
 *
 * Lines are named by an absolute number: on the main screen the sequence
 * number of a scrollback line (see history.c), and for screen row y
 * historybuf->next_seq + y, which a line keeps when it scrolls into the
 * scrollback. On the alternate screen the row. Text is searched by logical
 * line, joined across soft wraps, so matches can span rows.
 *
 * The lines are in three regions:
 *
 *   [back_pos, frozen_end)   scrollback lines that were searched, whose
 *                            matches are in older (descending) and newer
 *                            (ascending). Scrollback does not change, so
 *                            these are searched once.
 *   [frozen_end, end)        the rest: the screen and the scrollback lines of
 *                            the logical line that continues on the screen.
 *                            Searched again into live whenever output changes
 *                            the screen.
 *   older than back_pos      not searched yet. The search works backwards from
 *                            the newest scrollback line to the oldest.
 *
 * Work is done on the main thread in slices from the main loop (see
 * screen_search_tick_all()) so that searching an unlimited, compressed
 * scrollback never blocks input or rendering. As lines scroll into the
 * scrollback frozen_end moves forward over the logical lines that are
 * complete. Compressed lines are read through the decompression cache, which
 * keeps only a few chunks for a sequential read. Clearing or rewrapping the
 * scrollback changes its generation or the screen size, and the search starts
 * again.
 *
 * Nothing limits the number of matches or the length of a logical line: the
 * buffers grow from the data.
 */

#include "search.h"
#include "screen.h"
#include "state.h"
#include "lineops.h"
#include "history.h"

typedef struct {
    uint64_t start_line, end_line;
    index_type start_x, end_x; // end_x is exclusive, on end_line
} SearchMatch;

typedef struct {
    SearchMatch *items;
    size_t head, count, capacity; // the valid matches are items[head, head + count)
} MatchList;

// Where each code point of the searched text came from. 32 bits are enough for the line, which is counted from
// the first line of the text and so is less than the number of lines of the scrollback (an index_type), and for
// the columns, as a line of 2^31 cells would need 100 GB.
typedef struct {
    uint32_t line;       // from the first line of the text, see SearchText.first_line
    uint32_t x : 31;     // the first cell of the char
    uint32_t first : 1;  // the first code point of its cell
    index_type x_end;    // the cell after the char
} CharSource;

typedef enum { NAV_NONE, NAV_MOVED, NAV_WRAPPED, NAV_PENDING } NavResult;

struct ScreenSearch {
    PyObject *query, *needle, *pattern, *search_method;
    bool case_sensitive;
    // what the matches are for
    HistoryBuf *hb;
    uint64_t hb_generation;
    LineBuf *linebuf;
    index_type columns, lines;
    bool on_main, auto_scroll;
    // the regions, see above
    uint64_t back_pos, frozen_end;
    bool back_done;
    // a logical line that ends after the last scrollback line: lines (chain_start, chain_checked) continue chain_start
    uint64_t chain_start, chain_checked;
    bool live_dirty, live_valid;
    monotonic_t next_live_scan_at;
    MatchList older, newer, live;
    // the current match
    bool has_current, choose_current;
    SearchMatch current;
    uint64_t anchor; // the bottom row of the viewport when the search started, for choosing the first current match
    int pending;     // a move that waits for more of the scrollback to be searched: -1 to older, +1 to wrap to the oldest
    bool wrapped;
    // rendering
    uint64_t version, rendered_version, rendered_top;
    index_type rendered_lines;
    bool rendered;
    // telling the window
    uint64_t notified_version;
    monotonic_t notified_at;
    // the text of the lines being searched, from line text_first
    uint64_t text_first;
    Py_UCS4 *text;
    CharSource *src;
    size_t len, capacity;
    Py_ssize_t *fold;
    size_t fold_capacity;
    MatchList found;
    // statistics
    uint64_t lines_searched;
    monotonic_t started_at, finished_at, busy;
    bool failed;
};

// Match lists {{{
static bool
list_reserve(MatchList *l, size_t extra) {
    if (l->head && l->head + l->count + extra > l->capacity) {
        memmove(l->items, l->items + l->head, l->count * sizeof(l->items[0]));
        l->head = 0;
    }
    if (l->count + extra <= l->capacity) return true;
    if (extra > SIZE_MAX / sizeof(SearchMatch) - l->count) return false;
    size_t cap = MAX(l->capacity, (size_t)64);
    while (cap < l->count + extra) cap = cap > SIZE_MAX / sizeof(SearchMatch) / 2 ? l->count + extra : cap * 2;
    SearchMatch *n = realloc(l->items, cap * sizeof(SearchMatch));
    if (!n) return false;
    l->items = n;
    l->capacity = cap;
    return true;
}

static bool
list_append(MatchList *l, const SearchMatch *m) {
    if (!list_reserve(l, 1)) return false;
    l->items[l->head + l->count++] = *m;
    return true;
}

static inline SearchMatch *
list_at(const MatchList *l, size_t i) {
    return l->items + l->head + i;
}

static void
list_clear(MatchList *l) {
    l->head = 0;
    l->count = 0;
}

static void
list_free(MatchList *l) {
    free(l->items);
    zero_at_ptr(l);
}

// The matches in order: older reversed, then newer, then live
static inline size_t
total_matches(const ScreenSearch *s) {
    return s->older.count + s->newer.count + s->live.count;
}

static inline const SearchMatch *
match_at(const ScreenSearch *s, size_t i) {
    if (i < s->older.count) return list_at(&s->older, s->older.count - 1 - i);
    i -= s->older.count;
    if (i < s->newer.count) return list_at(&s->newer, i);
    return list_at(&s->live, i - s->newer.count);
}

static inline int
cmp_pos(uint64_t la, index_type xa, uint64_t lb, index_type xb) {
    if (la != lb) return la < lb ? -1 : 1;
    if (xa != xb) return xa < xb ? -1 : 1;
    return 0;
}

// The index of the first match that starts at or after (line, x)
static size_t
first_starting_at_or_after(const ScreenSearch *s, uint64_t line, index_type x) {
    size_t lo = 0, hi = total_matches(s);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const SearchMatch *m = match_at(s, mid);
        if (cmp_pos(m->start_line, m->start_x, line, x) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// The index of the first match that ends on or after line. Matches do not overlap so their ends are in order too.
static size_t
first_ending_on_or_after(const ScreenSearch *s, uint64_t line) {
    size_t lo = 0, hi = total_matches(s);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (match_at(s, mid)->end_line < line) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool
current_index(const ScreenSearch *s, size_t *idx) {
    if (!s->has_current) return false;
    const size_t i = first_starting_at_or_after(s, s->current.start_line, s->current.start_x);
    if (i >= total_matches(s)) return false;
    const SearchMatch *m = match_at(s, i);
    if (m->start_line != s->current.start_line || m->start_x != s->current.start_x) return false;
    *idx = i;
    return true;
}
// }}}

// Lines {{{
static inline uint64_t
base_line(const ScreenSearch *s) {
    return s->on_main ? s->hb->next_seq : 0;
}

static inline uint64_t
oldest_line(const ScreenSearch *s) {
    return s->on_main ? s->hb->next_seq - s->hb->count : 0;
}

static inline uint64_t
end_line(const ScreenSearch *s) {
    return base_line(s) + s->lines;
}

static Line *
line_at(ScreenSearch *s, Screen *screen, uint64_t a) {
    const uint64_t base = base_line(s);
    if (a < base) {
        historybuf_init_line(s->hb, (index_type)(base - 1 - a), s->hb->line);
        return s->hb->line;
    }
    linebuf_init_line(screen->linebuf, (index_type)(a - base));
    return screen->linebuf->line;
}

// Whether line a continues on line a + 1
static bool
wraps(ScreenSearch *s, uint64_t a) {
    const uint64_t base = base_line(s);
    if (a + 1 < base) return historybuf_is_line_continued(s->hb, (index_type)(base - 2 - a));
    if (a + 1 == base) return history_buf_endswith_wrap(s->hb);
    return linebuf_line_ends_with_continuation(s->linebuf, (index_type)(a - base));
}

// The first line of the logical line that contains line a, no older than oldest
static uint64_t
logical_start(ScreenSearch *s, uint64_t a, uint64_t oldest) {
    while (a > oldest && wraps(s, a - 1)) a--;
    return a;
}

// The first line after the logical line that contains line a - 1, at most limit
static uint64_t
logical_end(ScreenSearch *s, uint64_t a, uint64_t limit) {
    while (a < limit && wraps(s, a - 1)) a++;
    return a;
}

// The newest scrollback line that starts a logical line which ends in the scrollback: the end of what can be
// frozen. The lines that continue a logical line are remembered, so that a line that never ends is walked once.
static uint64_t
freeze_limit(ScreenSearch *s) {
    if (!s->on_main) return 0;
    const uint64_t base = base_line(s), oldest = oldest_line(s);
    if (base <= oldest || !wraps(s, base - 1)) {
        s->chain_start = s->chain_checked = base;
        return base;
    }
    // Line base - 1 continues on the screen, find where its logical line starts. The lines
    // (chain_start, chain_checked) were found to continue chain_start before.
    const bool known = s->chain_checked > s->chain_start;
    uint64_t a = base - 1;
    while (a > oldest && wraps(s, a - 1)) {
        if (known && a <= s->chain_checked) {
            a = MAX(s->chain_start, oldest);
            break;
        }
        a--;
    }
    s->chain_start = a;
    s->chain_checked = base;
    return a;
}
// }}}

// Text {{{
static bool
reserve_text(ScreenSearch *s, size_t extra) {
    if (s->len + extra <= s->capacity) return true;
    if (extra > SIZE_MAX / sizeof(CharSource) - s->len) return false;
    size_t cap = MAX(s->capacity, (size_t)4096);
    while (cap < s->len + extra) cap = cap > SIZE_MAX / sizeof(CharSource) / 2 ? s->len + extra : cap * 2;
    Py_UCS4 *t = realloc(s->text, cap * sizeof(Py_UCS4));
    if (!t) return false;
    s->text = t;
    CharSource *src = realloc(s->src, cap * sizeof(CharSource));
    if (!src) return false;
    s->src = src;
    s->capacity = cap;
    return true;
}

// Append the text of a line, as its text would be copied: empty cells inside the text are spaces, the cells a
// tab or a multicell char covers are part of it and the rows below the first of a multiline char have no text.
static bool
append_line(ScreenSearch *s, const Line *line, uint64_t a) {
    const index_type limit = xlimit_for_line(line);
    const uint32_t lnum = (uint32_t)(a - s->text_first);
    // at least a code point per cell, chars made of more are made room for as they come
    if (!reserve_text(s, (size_t)limit + 1)) return false;
    for (index_type x = 0; x < limit; x++) {
        const CPUCell *c = line->cpu_cells + x;
        if (!c->ch_is_idx && !c->is_multicell) {
            // the common case: a cell with one code point
            const char_type ch = c->ch_or_idx;
            if (ch == '\t') goto slow;
            s->text[s->len] = ch ? ch : ' ';
            s->src[s->len++] = (CharSource){.line = lnum, .x = x, .x_end = x + 1, .first = 1};
            continue;
        }
slow:
        if (c->is_multicell && (c->x || c->y)) continue;
        ListOfChars lc;
        for (;;) {
            lc.chars = s->text + s->len;
            lc.capacity = s->capacity - s->len;
            if (text_in_cell_without_alloc(c, line->text_cache, &lc)) break;
            if (!reserve_text(s, lc.count + (size_t)(limit - x))) return false;
        }
        if (!lc.count) continue;
        index_type x_end = x + 1;
        if (!lc.chars[0]) {
            lc.chars[0] = ' ';
            lc.count = 1;
        } else if (lc.chars[0] == '\t') {
            unsigned skip = lc.count > 1 ? lc.chars[1] : 0;
            lc.count = 1;
            while (skip && x_end < line->xnum && cell_is_char(line->cpu_cells + x_end, ' ')) {
                x_end++;
                skip--;
            }
        } else if (c->is_multicell) x_end = MIN(line->xnum, x + MAX(1u, mcd_x_limit(c)));
        for (size_t i = 0; i < lc.count; i++) s->src[s->len + i] = (CharSource){.line = lnum, .x = x, .x_end = x_end, .first = i == 0};
        s->len += lc.count;
        x = x_end - 1;
        // keep room for a code point per remaining cell
        if (!reserve_text(s, (size_t)(limit - x) + 1)) return false;
    }
    return true;
}

static bool
append_separator(ScreenSearch *s, uint64_t a, index_type x) {
    if (!reserve_text(s, 1)) return false;
    s->text[s->len] = '\n';
    s->src[s->len++] = (CharSource){.line = (uint32_t)(a - s->text_first), .x = x, .x_end = x, .first = 1};
    return true;
}

// Build the text of the logical lines [first, limit)
static bool
build_text(ScreenSearch *s, Screen *screen, uint64_t first, uint64_t limit) {
    s->len = 0;
    s->text_first = first;
    for (uint64_t a = first; a < limit; a++) {
        const Line *line = line_at(s, screen, a);
        if (!append_line(s, line, a)) return false;
        if (a + 1 == limit || !line->cpu_cells[line->xnum - 1].next_char_was_wrapped) {
            if (!append_separator(s, a, line->xnum)) return false;
        }
    }
    s->lines_searched += limit - first;
    return true;
}
// }}}

// Matching {{{
static inline bool
at_cell_boundary(const ScreenSearch *s, size_t i) {
    return i >= s->len || s->text[i] == '\n' || s->src[i].first;
}

static inline bool
has_separator(const ScreenSearch *s, size_t a, size_t b) {
    for (size_t i = a; i < b; i++)
        if (s->text[i] == '\n') return true;
    return false;
}

static bool
record(ScreenSearch *s, size_t a, size_t b) {
    const CharSource *first = s->src + a, *last = s->src + b - 1;
    SearchMatch m = {.start_line = s->text_first + first->line, .start_x = first->x, .end_line = s->text_first + last->line, .end_x = last->x_end};
    return list_append(&s->found, &m);
}

// The offsets in the case folded text of the original code points, NULL when they are the same. The folding of a
// code point does not depend on its neighbors, so the folded text is the concatenation of the folded code points.
static bool
build_fold_map(ScreenSearch *s, Py_ssize_t folded_len) {
    if ((size_t)folded_len + 1 > s->fold_capacity) {
        Py_ssize_t *f = realloc(s->fold, ((size_t)folded_len + 1) * sizeof(Py_ssize_t));
        if (!f) return false;
        s->fold = f;
        s->fold_capacity = (size_t)folded_len + 1;
    }
    // fold[j] is the original offset of folded code point j
    Py_ssize_t j = 0;
    for (size_t i = 0; i < s->len; i++) {
        Py_ssize_t n = 1;
        const Py_UCS4 ch = s->text[i];
        if (ch >= 0x80) {
            RAII_PyObject(c, PyUnicode_FromOrdinal((int)ch));
            if (!c) return false;
            RAII_PyObject(f, PyObject_CallMethod(c, "casefold", NULL));
            if (!f) return false;
            n = PyUnicode_GET_LENGTH(f);
        }
        if (j + n > folded_len) return false;
        for (Py_ssize_t k = 0; k < n; k++) s->fold[j++] = (Py_ssize_t)i;
    }
    if (j != folded_len) return false;
    s->fold[j] = (Py_ssize_t)s->len;
    return true;
}

static bool
find_literal(ScreenSearch *s, PyObject *text) {
    RAII_PyObject(folded, NULL);
    PyObject *hay = text;
    bool mapped = false;
    if (!s->case_sensitive) {
        folded = PyObject_CallMethod(text, "casefold", NULL);
        if (!folded) return false;
        hay = folded;
        if ((size_t)PyUnicode_GET_LENGTH(folded) != s->len) {
            if (!build_fold_map(s, PyUnicode_GET_LENGTH(folded))) {
                if (!PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "Case folding changed the text unexpectedly");
                return false;
            }
            mapped = true;
        }
    }
    const Py_ssize_t n = PyUnicode_GET_LENGTH(s->needle), hay_len = PyUnicode_GET_LENGTH(hay);
    if (!n) return true;
    Py_ssize_t pos = 0;
    while (pos + n <= hay_len) {
        const Py_ssize_t r = PyUnicode_Find(hay, s->needle, pos, hay_len, 1);
        if (r == -2) return false;
        if (r < 0) break;
        size_t a = (size_t)r, b = (size_t)(r + n);
        bool aligned = true;
        if (mapped) {
            // a match must start and end on code points of the original text
            aligned = (r == 0 || s->fold[r - 1] != s->fold[r]) && s->fold[r + n] != s->fold[r + n - 1];
            a = (size_t)s->fold[r];
            b = (size_t)s->fold[r + n - 1] + 1;
        }
        if (aligned && at_cell_boundary(s, a) && s->src[a].first && at_cell_boundary(s, b) && !has_separator(s, a, b)) {
            if (!record(s, a, b)) {
                PyErr_NoMemory();
                return false;
            }
            pos = r + n;
        } else pos = r + 1;
    }
    return true;
}

static PyObject *
interned(const char *name, PyObject **cache) {
    if (!*cache) *cache = PyUnicode_InternFromString(name);
    return *cache;
}

static bool
match_span(PyObject *m, Py_ssize_t *a, Py_ssize_t *b) {
    static PyObject *name = NULL;
    if (!interned("span", &name)) return false;
    RAII_PyObject(span, PyObject_CallMethodNoArgs(m, name));
    if (!span) return false;
    return PyArg_ParseTuple(span, "nn", a, b);
}

static bool
find_regex(ScreenSearch *s, PyObject *text) {
    const Py_ssize_t len = (Py_ssize_t)s->len;
    Py_ssize_t pos = 0;
    while (pos < len) {
        RAII_PyObject(m, PyObject_CallFunction(s->search_method, "Onn", text, pos, len));
        if (!m) return false;
        if (m == Py_None) break;
        Py_ssize_t a, b;
        if (!match_span(m, &a, &b)) return false;
        if (has_separator(s, (size_t)a, (size_t)b)) {
            // matches stay in a logical line: search the rest of the line it starts in by itself
            Py_ssize_t le = a;
            while (le < len && s->text[le] != '\n') le++;
            RAII_PyObject(m2, PyObject_CallFunction(s->search_method, "Onn", text, a, le));
            if (!m2) return false;
            if (m2 == Py_None) {
                pos = le + 1;
                continue;
            }
            if (!match_span(m2, &a, &b)) return false;
        }
        if (b <= a) {
            pos = a + 1;
            continue;
        }
        if (s->src[a].first && s->text[a] != '\n' && at_cell_boundary(s, (size_t)b)) {
            if (!record(s, (size_t)a, (size_t)b)) {
                PyErr_NoMemory();
                return false;
            }
            pos = b;
        } else pos = a + 1;
    }
    return true;
}

// Search the logical lines [first, limit) into s->found, in order
static bool
search_lines(ScreenSearch *s, Screen *screen, uint64_t first, uint64_t limit) {
    list_clear(&s->found);
    if (first >= limit) return true;
    if (!build_text(s, screen, first, limit)) {
        PyErr_NoMemory();
        return false;
    }
    RAII_PyObject(text, PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, s->text, (Py_ssize_t)s->len));
    if (!text) return false;
    return s->pattern ? find_regex(s, text) : find_literal(s, text);
}
// }}}

// State {{{
static void
mark_changed(ScreenSearch *s) {
    s->version++;
}

static void
reset_regions(ScreenSearch *s, Screen *screen) {
    s->hb = screen->historybuf;
    s->hb_generation = s->hb->generation;
    s->linebuf = screen->linebuf;
    s->on_main = screen->linebuf == screen->main_linebuf;
    s->columns = screen->columns;
    s->lines = screen->lines;
    list_clear(&s->older);
    list_clear(&s->newer);
    list_clear(&s->live);
    s->chain_start = s->chain_checked = 0;
    const uint64_t limit = freeze_limit(s);
    s->back_pos = s->frozen_end = limit;
    s->back_done = !s->on_main || limit <= oldest_line(s);
    s->live_dirty = true;
    s->live_valid = false;
    s->next_live_scan_at = 0;
    s->anchor = base_line(s) + screen->lines - 1 - (s->on_main ? MIN((uint64_t)screen->scrolled_by, base_line(s)) : 0);
    s->choose_current = true;
    s->has_current = false;
    s->pending = 0;
    s->wrapped = false;
    s->lines_searched = 0;
    s->started_at = monotonic();
    s->finished_at = 0;
    s->busy = 0;
    mark_changed(s);
}

static bool
is_stale(const ScreenSearch *s, const Screen *screen) {
    return s->hb != screen->historybuf || s->hb_generation != screen->historybuf->generation || s->linebuf != screen->linebuf ||
           s->columns != screen->columns || s->lines != screen->lines;
}

void
screen_search_invalidate(ScreenSearch *s) {
    if (s) s->columns = 0;
}

void
screen_search_free(ScreenSearch *s) {
    if (!s) return;
    Py_CLEAR(s->query);
    Py_CLEAR(s->needle);
    Py_CLEAR(s->pattern);
    Py_CLEAR(s->search_method);
    list_free(&s->older);
    list_free(&s->newer);
    list_free(&s->live);
    list_free(&s->found);
    free(s->text);
    free(s->src);
    free(s->fold);
    free(s);
}

static inline bool
scan_finished(const ScreenSearch *s) {
    return s->back_done && s->live_valid && !s->live_dirty;
}

// Drop the matches of lines that left the scrollback
static void
drop_lost_lines(ScreenSearch *s) {
    if (!s->on_main) return;
    const uint64_t oldest = oldest_line(s);
    bool changed = false;
    while (s->older.count && list_at(&s->older, s->older.count - 1)->start_line < oldest) {
        s->older.count--;
        changed = true;
    }
    if (!s->older.count) {
        while (s->newer.count && list_at(&s->newer, 0)->start_line < oldest) {
            s->newer.head++;
            s->newer.count--;
            changed = true;
        }
    }
    if (s->back_pos < oldest) {
        s->back_pos = oldest;
        if (s->frozen_end < oldest) s->frozen_end = oldest;
    }
    if (!s->back_done && s->back_pos <= oldest) {
        s->back_done = true;
        changed = true;
    }
    if (changed) mark_changed(s);
}
// }}}

// Navigation {{{
static void
scroll_to_match(Screen *screen, ScreenSearch *s, const SearchMatch *m) {
    if (!s->on_main) return;
    const uint64_t base = base_line(s);
    const int64_t first_row = (int64_t)m->start_line - (int64_t)base + (int64_t)screen->scrolled_by;
    const int64_t last_row = (int64_t)m->end_line - (int64_t)base + (int64_t)screen->scrolled_by;
    if (first_row >= 0 && last_row < (int64_t)screen->lines) return; // visible
    // put the match in the middle of the screen
    const int64_t height = last_row - first_row + 1;
    int64_t target = (int64_t)screen->lines / 2 - height / 2 - ((int64_t)m->start_line - (int64_t)base);
    if (height >= (int64_t)screen->lines) target = -((int64_t)m->start_line - (int64_t)base);
    target = MAX((int64_t)0, MIN(target, (int64_t)screen->historybuf->count));
    screen_history_scroll_to_absolute(screen, (double)target);
}

static void
set_current(Screen *screen, ScreenSearch *s, size_t idx, bool scroll) {
    s->current = *match_at(s, idx);
    s->has_current = true;
    s->choose_current = false;
    s->pending = 0;
    mark_changed(s);
    if (scroll) scroll_to_match(screen, s, &s->current);
}

// The first current match: the last that starts at or above the bottom of the viewport, else the first below it
static void
choose_current(Screen *screen, ScreenSearch *s) {
    if (!s->choose_current) return;
    const size_t total = total_matches(s);
    if (!total) return;
    const size_t below = first_starting_at_or_after(s, s->anchor + 1, 0);
    if (below > 0) set_current(screen, s, below - 1, s->auto_scroll);
    else if (s->back_done) set_current(screen, s, 0, s->auto_scroll);
}

// Keep the current match after the matches changed: the same match, else the nearest after it
static void
refresh_current(Screen *screen, ScreenSearch *s) {
    if (!s->has_current) {
        choose_current(screen, s);
        return;
    }
    size_t idx;
    if (current_index(s, &idx)) return;
    const size_t total = total_matches(s);
    if (!total) {
        s->has_current = false;
        mark_changed(s);
        return;
    }
    size_t i = first_starting_at_or_after(s, s->current.start_line, s->current.start_x);
    if (i >= total) i = total - 1;
    s->current = *match_at(s, i);
    mark_changed(s);
}

static NavResult
navigate(Screen *screen, ScreenSearch *s, int direction) {
    const size_t total = total_matches(s);
    s->wrapped = false;
    if (!total) {
        if (!scan_finished(s)) {
            s->pending = direction < 0 ? -1 : 1;
            return NAV_PENDING;
        }
        return NAV_NONE;
    }
    size_t idx;
    if (!current_index(s, &idx)) {
        s->choose_current = true;
        s->has_current = false;
        choose_current(screen, s);
        if (s->has_current) {
            scroll_to_match(screen, s, &s->current);
            return NAV_MOVED;
        }
        s->pending = direction < 0 ? -1 : 1;
        return NAV_PENDING;
    }
    if (direction < 0) { // to older matches, up
        if (idx > 0) {
            set_current(screen, s, idx - 1, true);
            return NAV_MOVED;
        }
        if (!s->back_done) {
            s->pending = -1;
            return NAV_PENDING;
        }
        set_current(screen, s, total - 1, true);
        s->wrapped = true;
        return NAV_WRAPPED;
    }
    if (idx + 1 < total) {
        set_current(screen, s, idx + 1, true);
        return NAV_MOVED;
    }
    if (!s->back_done) {
        s->pending = 1;
        return NAV_PENDING;
    }
    set_current(screen, s, 0, true);
    s->wrapped = true;
    return NAV_WRAPPED;
}

static void
resolve_pending(Screen *screen, ScreenSearch *s) {
    if (!s->pending) return;
    if (s->pending < 0) {
        size_t idx;
        if (!s->has_current) {
            if (total_matches(s)) {
                s->choose_current = true;
                choose_current(screen, s);
                if (s->has_current) {
                    s->pending = 0;
                    scroll_to_match(screen, s, &s->current);
                }
            } else if (scan_finished(s)) s->pending = 0;
        } else if (current_index(s, &idx) && idx > 0) {
            set_current(screen, s, idx - 1, true);
        } else if (s->back_done) {
            s->pending = 0;
            if (total_matches(s)) {
                set_current(screen, s, total_matches(s) - 1, true);
                s->wrapped = true;
            }
        }
    } else if (s->back_done) {
        s->pending = 0;
        if (total_matches(s)) {
            size_t idx;
            if (s->has_current && current_index(s, &idx) && idx + 1 < total_matches(s)) set_current(screen, s, idx + 1, true);
            else {
                set_current(screen, s, 0, true);
                s->wrapped = s->has_current;
            }
        }
    }
}
// }}}

// Doing the work {{{
#define SEARCH_CHUNK_LINES 256u

static bool
fail(ScreenSearch *s) {
    PyErr_Print();
    s->failed = true;
    s->back_done = true;
    s->live_valid = true;
    s->live_dirty = false;
    mark_changed(s);
    return false;
}

// Search newly complete logical lines of the scrollback, oldest first
static bool
search_forward(Screen *screen, ScreenSearch *s, uint64_t limit) {
    const uint64_t first = s->frozen_end;
    const uint64_t last = logical_end(s, MIN(limit, first + SEARCH_CHUNK_LINES), limit);
    if (!search_lines(s, screen, first, last)) return fail(s);
    if (!list_reserve(&s->newer, s->found.count)) {
        PyErr_NoMemory();
        return fail(s);
    }
    for (size_t i = 0; i < s->found.count; i++) list_append(&s->newer, list_at(&s->found, i));
    s->frozen_end = last;
    // the lines that were live are now searched
    list_clear(&s->live);
    s->live_valid = false;
    s->live_dirty = true;
    mark_changed(s);
    return true;
}

static bool
search_backward(Screen *screen, ScreenSearch *s) {
    const uint64_t oldest = oldest_line(s), limit = s->back_pos;
    const uint64_t first = logical_start(s, limit > oldest + SEARCH_CHUNK_LINES ? limit - SEARCH_CHUNK_LINES : oldest, oldest);
    if (!search_lines(s, screen, first, limit)) return fail(s);
    if (!list_reserve(&s->older, s->found.count)) {
        PyErr_NoMemory();
        return fail(s);
    }
    for (size_t i = s->found.count; i-- > 0;) list_append(&s->older, list_at(&s->found, i));
    s->back_pos = first;
    if (first <= oldest) s->back_done = true;
    if (s->found.count || s->back_done) mark_changed(s);
    return true;
}

static bool
search_live(Screen *screen, ScreenSearch *s) {
    if (!search_lines(s, screen, s->frozen_end, end_line(s))) return fail(s);
    bool same = s->live_valid && s->found.count == s->live.count;
    for (size_t i = 0; same && i < s->found.count; i++) same = memcmp(list_at(&s->found, i), list_at(&s->live, i), sizeof(SearchMatch)) == 0;
    if (!same) {
        list_clear(&s->live);
        if (!list_reserve(&s->live, s->found.count)) {
            PyErr_NoMemory();
            return fail(s);
        }
        for (size_t i = 0; i < s->found.count; i++) list_append(&s->live, list_at(&s->found, i));
        mark_changed(s);
    }
    if (!s->live_valid) mark_changed(s);
    s->live_valid = true;
    s->live_dirty = false;
    return true;
}

static void
notify(Screen *screen, ScreenSearch *s, monotonic_t now, bool force) {
    if (s->notified_version == s->version) return;
    // the counter changes many times a second while the scrollback is searched, it is shown at most ten times a second
    if (!force && now - s->notified_at < ms_to_monotonic_t(100) && !scan_finished(s)) return;
    s->notified_version = s->version;
    s->notified_at = now;
    if (screen->callbacks && screen->callbacks != Py_None) {
        RAII_PyObject(ret, PyObject_CallMethod(screen->callbacks, "on_search_status_changed", NULL));
        if (!ret) PyErr_Print();
    }
}

// Do the pending work of a search for at most budget. Returns the time until work is next due, -1 when none is.
static monotonic_t
tick(Screen *screen, ScreenSearch *s, monotonic_t now, monotonic_t budget, bool notify_window) {
    if (s->failed || !s->query) return -1;
    if (is_stale(s, screen)) {
        reset_regions(s, screen);
        s->auto_scroll = false;
    }
    if (screen->is_dirty || screen->input_parsed) s->live_dirty = true;
    drop_lost_lines(s);
    const monotonic_t deadline = now + budget;
    const monotonic_t start = monotonic();
    monotonic_t t = start;
    // new scrollback lines and the screen first, they are what is seen, then the rest of the scrollback
    const uint64_t limit = freeze_limit(s);
    bool more = false;
    while (s->frozen_end < limit && t < deadline) {
        if (!search_forward(screen, s, limit)) return -1;
        t = monotonic();
    }
    if (s->frozen_end < limit) more = true;
    monotonic_t wait = -1;
    if (s->frozen_end >= limit && s->live_dirty) {
        if (t >= s->next_live_scan_at || !s->live_valid) {
            const monotonic_t ls = monotonic();
            if (!search_live(screen, s)) return -1;
            t = monotonic();
            // an endless logical line makes the live region long, keep the time spent searching it again small
            s->next_live_scan_at = t + 4 * (t - ls);
        } else wait = s->next_live_scan_at - t;
    }
    while (!s->back_done && t < deadline) {
        if (!search_backward(screen, s)) return -1;
        t = monotonic();
    }
    if (!s->back_done) more = true;
    s->busy += t - start;
    refresh_current(screen, s);
    resolve_pending(screen, s);
    const bool first_finished = scan_finished(s) && !s->finished_at;
    if (first_finished) {
        // the whole scrollback has been searched, the counter is final
        s->finished_at = t;
        mark_changed(s);
    }
    if (notify_window) notify(screen, s, t, first_finished);
    if (more) return 0;
    return wait;
}

monotonic_t
screen_search_tick_all(monotonic_t now, monotonic_t budget) {
    monotonic_t ans = -1;
    size_t n = 0;
    for (size_t o = 0; o < global_state.num_os_windows; o++) {
        OSWindow *w = global_state.os_windows + o;
        for (size_t t = 0; t < w->num_tabs; t++)
            for (size_t i = 0; i < w->tabs[t].num_windows; i++)
                if (w->tabs[t].windows[i].render_data.screen && w->tabs[t].windows[i].render_data.screen->search &&
                    w->tabs[t].windows[i].render_data.screen->search->query)
                    n++;
    }
    // stopped searches are visited too, the next render removes their highlights
    const monotonic_t share = MAX(budget / (monotonic_t)MAX(n, (size_t)1), ms_to_monotonic_t(1));
    for (size_t o = 0; o < global_state.num_os_windows; o++) {
        OSWindow *w = global_state.os_windows + o;
        for (size_t t = 0; t < w->num_tabs; t++) {
            for (size_t i = 0; i < w->tabs[t].num_windows; i++) {
                Screen *screen = w->tabs[t].windows[i].render_data.screen;
                if (!screen || !screen->search) continue;
                if (!screen->search->query) {
                    if (screen_search_render_dirty(screen)) w->needs_render = true;
                    continue;
                }
                const monotonic_t wait = tick(screen, screen->search, now, share, true);
                if (wait >= 0 && (ans < 0 || wait < ans)) ans = wait;
                // the window may have stopped the search when told about it
                if (screen->search && screen->search->version != screen->search->rendered_version) w->needs_render = true;
            }
        }
    }
    return ans;
}
// }}}

// Rendering {{{
static inline uint64_t
viewport_top(const ScreenSearch *s, const Screen *screen) {
    const uint64_t base = base_line(s);
    return s->on_main ? base - MIN((uint64_t)screen->scrolled_by, base) : 0;
}

bool
screen_search_render_dirty(Screen *self) {
    ScreenSearch *s = self->search;
    if (!s) return false;
    if (!s->query) return s->rendered;
    return !s->rendered || s->version != s->rendered_version || viewport_top(s, self) != s->rendered_top || s->rendered_lines != self->lines;
}

// Set mask on the cells of m in the rows of data, row 0 being line row_base
static void
set_bits(Screen *self, ScreenSearch *s, uint8_t *data, int64_t row_base, int64_t rows, const SearchMatch *m, uint8_t mask) {
    const int64_t from = MAX((int64_t)m->start_line, MAX(row_base, (int64_t)0)), to = MIN((int64_t)m->end_line + 1, row_base + rows);
    for (int64_t a = from; a < to; a++) {
        const int64_t row = a - row_base;
        const index_type x0 = (uint64_t)a == m->start_line ? m->start_x : 0;
        const index_type x1 = MIN(self->columns, (uint64_t)a == m->end_line ? m->end_x : self->columns);
        uint8_t *line_start = data + (size_t)row * self->columns;
        const Line *line = (uint64_t)a < end_line(s) && (uint64_t)a >= oldest_line(s) ? line_at(s, self, (uint64_t)a) : NULL;
        for (index_type x = x0; x < x1; x++) {
            line_start[x] |= mask;
            // every row of a multiline char is highlighted
            if (line && line->cpu_cells[x].is_multicell && line->cpu_cells[x].scale > 1) {
                const CPUCell *c = line->cpu_cells + x;
                for (int64_t r = MAX((int64_t)0, row - (int64_t)c->y); r < MIN(rows, row - (int64_t)c->y + (int64_t)c->scale); r++)
                    data[(size_t)r * self->columns + x] |= mask;
            }
        }
    }
}

void
screen_search_apply_highlights(Screen *self, uint8_t *data, size_t size, int extra_leading_rows, unsigned scrolled_by) {
    ScreenSearch *s = self->search;
    if (!s) return;
    s->rendered = s->query != NULL;
    s->rendered_version = s->version;
    s->rendered_top = viewport_top(s, self);
    s->rendered_lines = self->lines;
    if (!s->query || is_stale(s, self) || !self->columns) return;
    (void)scrolled_by;
    // row 0 of data is extra_leading_rows above the top of the viewport
    const int64_t rows = (int64_t)(size / self->columns);
    const int64_t row_base = (int64_t)viewport_top(s, self) - extra_leading_rows;
    const uint64_t top = (uint64_t)MAX(row_base, (int64_t)0), bottom = (uint64_t)MAX(row_base + rows, (int64_t)0); // bottom is exclusive
    const size_t total = total_matches(s);
    size_t cur = SIZE_MAX;
    if (s->has_current) {
        size_t idx;
        if (current_index(s, &idx)) cur = idx;
    }
    for (size_t i = first_ending_on_or_after(s, top); i < total; i++) {
        const SearchMatch *m = match_at(s, i);
        if (m->start_line >= bottom) break;
        set_bits(self, s, data, row_base, rows, m, i == cur ? (SEARCH_MATCH_MASK | SEARCH_CURRENT_MASK) : SEARCH_MATCH_MASK);
    }
}
// }}}

// Python API {{{
static void
search_wakeup(void) {
    // there is no main loop without OS windows, as in the tests
    if (global_state.num_os_windows) request_tick_callback();
}

static Screen *
as_screen(PyObject *self) {
    return (Screen *)self;
}

static ScreenSearch *
ensure_search(Screen *screen) {
    if (!screen->search) {
        screen->search = calloc(1, sizeof(ScreenSearch));
        if (!screen->search) PyErr_NoMemory();
    }
    return screen->search;
}

PyObject *
screen_search_start_py(PyObject *self, PyObject *args, PyObject *kw) {
    static const char *kwlist[] = {"query", "case_sensitive", "pattern", "auto_scroll", "budget_ms", NULL};
    PyObject *query, *pattern = Py_None;
    int case_sensitive = 0, auto_scroll = 1;
    double budget_ms = 4;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "U|pOpd", (char **)kwlist, &query, &case_sensitive, &pattern, &auto_scroll, &budget_ms)) return NULL;
    Screen *screen = as_screen(self);
    ScreenSearch *s = ensure_search(screen);
    if (!s) return NULL;
    RAII_PyObject(needle, NULL);
    RAII_PyObject(method, NULL);
    if (pattern != Py_None) {
        method = PyObject_GetAttrString(pattern, "search");
        if (!method) return NULL;
    } else if (case_sensitive) {
        needle = Py_NewRef(query);
    } else {
        needle = PyObject_CallMethod(query, "casefold", NULL);
        if (!needle) return NULL;
    }
    Py_XSETREF(s->query, Py_NewRef(query));
    Py_XSETREF(s->needle, Py_XNewRef(needle));
    Py_XSETREF(s->pattern, pattern == Py_None ? NULL : Py_NewRef(pattern));
    Py_XSETREF(s->search_method, Py_XNewRef(method));
    s->case_sensitive = case_sensitive;
    s->failed = false;
    const bool empty = PyUnicode_GET_LENGTH(query) == 0;
    reset_regions(s, screen);
    s->auto_scroll = auto_scroll;
    if (empty) {
        // nothing to find
        s->back_done = true;
        s->live_valid = true;
        s->live_dirty = false;
        s->frozen_end = end_line(s);
    }
    // the screen is searched now, so that its matches show without waiting for the main loop
    tick(screen, s, monotonic(), ms_double_to_monotonic_t(MAX(0., budget_ms)), false);
    search_wakeup();
    Py_RETURN_NONE;
}

PyObject *
screen_search_stop_py(PyObject *self, PyObject *args UNUSED) {
    ScreenSearch *s = as_screen(self)->search;
    if (s && s->query) {
        // the struct stays until the window goes, so that the next render removes the highlights
        Py_CLEAR(s->query);
        Py_CLEAR(s->needle);
        Py_CLEAR(s->pattern);
        Py_CLEAR(s->search_method);
        list_clear(&s->older);
        list_clear(&s->newer);
        list_clear(&s->live);
        s->has_current = false;
        s->pending = 0;
        mark_changed(s);
    }
    Py_RETURN_NONE;
}

PyObject *
screen_search_tick_py(PyObject *self, PyObject *args) {
    double budget_ms = 4;
    if (!PyArg_ParseTuple(args, "|d", &budget_ms)) return NULL;
    Screen *screen = as_screen(self);
    if (!screen->search) Py_RETURN_FALSE;
    const monotonic_t wait = tick(screen, screen->search, monotonic(), ms_double_to_monotonic_t(budget_ms), false);
    if (screen->search && screen->search->failed) Py_RETURN_FALSE;
    if (wait == 0) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

PyObject *
screen_search_run_to_completion_py(PyObject *self, PyObject *args UNUSED) {
    Screen *screen = as_screen(self);
    ScreenSearch *s = screen->search;
    if (!s) Py_RETURN_NONE;
    s->next_live_scan_at = 0;
    while (screen->search == s && !s->failed && s->query) {
        s->next_live_scan_at = 0;
        if (tick(screen, s, monotonic(), s_to_monotonic_t(3600), false) != 0 && scan_finished(s)) break;
    }
    Py_RETURN_NONE;
}

PyObject *
screen_search_navigate_py(PyObject *self, PyObject *args) {
    int direction;
    if (!PyArg_ParseTuple(args, "i", &direction)) return NULL;
    Screen *screen = as_screen(self);
    ScreenSearch *s = screen->search;
    if (!s || !s->query) return PyUnicode_FromString("none");
    if (is_stale(s, screen)) tick(screen, s, monotonic(), ms_to_monotonic_t(4), false);
    const char *ans = "none";
    switch (navigate(screen, s, direction)) {
        case NAV_NONE: ans = "none"; break;
        case NAV_MOVED: ans = "moved"; break;
        case NAV_WRAPPED: ans = "wrapped"; break;
        case NAV_PENDING: ans = "pending"; search_wakeup(); break;
    }
    return PyUnicode_FromString(ans);
}

PyObject *
screen_search_status_py(PyObject *self, PyObject *args UNUSED) {
    Screen *screen = as_screen(self);
    ScreenSearch *s = screen->search;
    if (!s || !s->query) Py_RETURN_NONE;
    size_t idx = 0;
    const bool has_current = current_index(s, &idx);
    const uint64_t oldest = oldest_line(s), end = end_line(s);
    const uint64_t to_search = s->back_done ? 0 : s->back_pos - MIN(s->back_pos, oldest);
    const monotonic_t elapsed = (s->finished_at ? s->finished_at : monotonic()) - s->started_at;
    return Py_BuildValue(
        "{sO sn sn sO sO sO sO sK sK sK sd sd}",
        "query", s->query,
        "total", (Py_ssize_t)total_matches(s),
        "current", has_current ? (Py_ssize_t)idx + 1 : (Py_ssize_t)0,
        "done", scan_finished(s) ? Py_True : Py_False,
        "wrapped", s->wrapped ? Py_True : Py_False,
        "pending", s->pending ? Py_True : Py_False,
        "failed", s->failed ? Py_True : Py_False,
        "lines", (unsigned long long)(end - oldest),
        "lines_to_search", (unsigned long long)to_search,
        "lines_searched", (unsigned long long)s->lines_searched,
        "elapsed", monotonic_t_to_s_double(elapsed),
        "busy", monotonic_t_to_s_double(s->busy));
}

static PyObject *
match_as_tuple(const ScreenSearch *s, const SearchMatch *m) {
    // rows as in the rest of the Screen API: 0 is the top of the screen, the scrollback is negative
    const int64_t base = (int64_t)base_line(s);
    return Py_BuildValue("LILI", (long long)((int64_t)m->start_line - base), m->start_x, (long long)((int64_t)m->end_line - base), m->end_x);
}

PyObject *
screen_search_matches_py(PyObject *self, PyObject *args UNUSED) {
    ScreenSearch *s = as_screen(self)->search;
    const size_t total = s && s->query ? total_matches(s) : 0;
    RAII_PyObject(ans, PyList_New((Py_ssize_t)total));
    if (!ans) return NULL;
    for (size_t i = 0; i < total; i++) {
        PyObject *t = match_as_tuple(s, match_at(s, i));
        if (!t) return NULL;
        PyList_SET_ITEM(ans, (Py_ssize_t)i, t);
    }
    return Py_NewRef(ans);
}

PyObject *
screen_search_current_py(PyObject *self, PyObject *args UNUSED) {
    ScreenSearch *s = as_screen(self)->search;
    size_t idx;
    if (!s || !s->query || !current_index(s, &idx)) Py_RETURN_NONE;
    return match_as_tuple(s, match_at(s, idx));
}

PyObject *
screen_search_highlights_py(PyObject *self, PyObject *args UNUSED) {
    // The match bits of the cells of the viewport, a row of bytes per line: 0, 1 for a match, 3 for the current match
    Screen *screen = as_screen(self);
    const size_t sz = (size_t)screen->lines * screen->columns;
    RAII_ALLOC(uint8_t, buf, calloc(MAX(sz, (size_t)1), 1));
    if (!buf) return PyErr_NoMemory();
    screen_search_apply_highlights(screen, buf, sz, 0, screen->scrolled_by);
    RAII_PyObject(ans, PyTuple_New(screen->lines));
    if (!ans) return NULL;
    for (index_type y = 0; y < screen->lines; y++) {
        uint8_t *row = buf + (size_t)y * screen->columns;
        for (index_type x = 0; x < screen->columns; x++) row[x] = (row[x] & SEARCH_CURRENT_MASK) ? 3 : (row[x] & SEARCH_MATCH_MASK) ? 1 : 0;
        PyObject *b = PyBytes_FromStringAndSize((const char *)row, screen->columns);
        if (!b) return NULL;
        PyTuple_SET_ITEM(ans, y, b);
    }
    return Py_NewRef(ans);
}
// }}}
