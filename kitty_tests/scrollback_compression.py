#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

# The zstd compressed scrollback (kitty/history.c): a compressed history must
# behave exactly like an uncompressed one.

import random

from kitty.fast_data_types import SCROLL_FULL, HistoryBuf, test_set_history_compression
from kitty.logout_session import scrollback_for_restore
from kitty.options.utils import SCROLLBACK_COMPRESSION_NONE

from .base import BaseTest, draw_multicell, filled_cursor, filled_line_buf, parse_bytes

# The smallest hot tail (8 lines), blocks and frames, so that a few lines exercise the compressed paths
TINY = {'hot_lines': 1, 'block_lines': 2, 'frame_bytes': 64}
TINY_PARAMS = (1, 2, 64)


def push_numbered(hb, n, xnum=10, start=0):
    lb = filled_line_buf(5, xnum)
    c = filled_cursor()
    for i in range(start, start + n):
        line = lb.line(1)
        line.set_text(str(i).ljust(xnum), 0, xnum, c)
        hb.push(line)


def ops_for(seed, cols):
    rng = random.Random(seed)
    chars = 'abc 日😀' if seed % 2 else 'abcdef '
    ops = []
    for i in range(rng.randint(30, 120)):
        r = rng.random()
        if r < 0.35:
            ops.append(('draw', ''.join(rng.choice(chars) for _ in range(rng.randint(0, 3 * cols)))))
        elif r < 0.45:
            ops.append(('bytes', f'\x1b[4{rng.randint(1, 7)}m{rng.choice("xyz")}\x1b[K\x1b[m'.encode()))
        elif r < 0.5:
            ops.append(('mc', rng.choice('AB日'), rng.randint(2, 3)))
        elif r < 0.55:
            ops.append(('bytes', rng.choice((b'\x1b]133;A\x07', b'\x1b]133;A;k=s\x07', b'\x1b]133;C\x07'))))
        elif r < 0.58:
            # reverse index at the top pulls lines back from the scrollback, delete lines and clear to prompt
            ops.append(('bytes', rng.choice((b'\x1b[H\x1bM\x1bM', b'\x1b[2T', b'\x1b[3S', b'\x1b[H\x1b[2L'))))
        elif r < 0.6:
            ops.append(('resize', rng.randint(1, 9), rng.randint(2, 14)))
        elif r < 0.62:
            ops.append(('scroll_prompt',))
        else:
            ops.append(('nl',))
    return ops


def run(s, op):
    if op[0] == 'draw':
        s.draw(op[1])
    elif op[0] == 'bytes':
        parse_bytes(s, op[1])
    elif op[0] == 'mc':
        draw_multicell(s, op[1], scale=op[2])
    elif op[0] == 'resize':
        s.resize(op[1], op[2])
    elif op[0] == 'scroll_prompt':
        s.scroll_prompt_to_bottom()
    else:
        s.carriage_return(), s.linefeed()


class TestScrollbackCompression(BaseTest):

    def setUp(self):
        super().setUp()
        test_set_history_compression(*TINY_PARAMS)

    def tearDown(self):
        test_set_history_compression()
        super().tearDown()

    def screen_pair(self, cols=12, lines=4, scrollback=60, options=None, params=None, level=3):
        test_set_history_compression(*(params or TINY_PARAMS))
        a = self.create_screen(cols=cols, lines=lines, scrollback=scrollback, options=dict(options or {}, scrollback_compression=level))
        b = self.create_screen(cols=cols, lines=lines, scrollback=scrollback, options=dict(options or {}, scrollback_compression=SCROLLBACK_COMPRESSION_NONE))
        self.assertTrue(a.historybuf.compression_stats()['compress'])
        self.assertFalse(b.historybuf.compression_stats()['compress'])
        return a, b

    def assert_same(self, a, b, msg=''):
        # once with lines still pending with the compressor, once after it is done
        self.assert_same_(a, b, msg)
        a.historybuf.wait_for_compression()
        self.assert_same_(a, b, msg + ' (compressed)')

    def assert_same_(self, a, b, msg=''):
        ha, hb = a.historybuf, b.historybuf
        self.ae(ha.count, hb.count, msg)
        for i in range(ha.count):
            self.assertTrue(ha.line(i) == hb.line(i), f'{msg} history line {i}: {str(ha.line(i))!r} != {str(hb.line(i))!r} {ha.line(i).as_ansi()!r} {hb.line(i).as_ansi()!r}')
            self.ae(ha.line(i).as_ansi(), hb.line(i).as_ansi(), f'{msg} history line {i}')
            self.ae(ha.is_continued(i), hb.is_continued(i), f'{msg} history line {i}')
        self.ae(ha.endswith_wrap(), hb.endswith_wrap(), msg)
        for i in range(a.lines):
            self.assertTrue(a.linebuf.line(i) == b.linebuf.line(i), f'{msg} line {i}')
        self.ae(str(ha), str(hb), msg)
        for args in ((False, False), (True, False), (True, True), (False, True)):
            ta, tb = [], []
            a.as_text_for_history_buf(ta.append, *args)
            b.as_text_for_history_buf(tb.append, *args)
            self.ae(ta, tb, f'{msg} as_text {args}')
            ta, tb = [], []
            a.as_text_non_visual(ta.append, *args)
            b.as_text_non_visual(tb.append, *args)
            self.ae(ta, tb, f'{msg} as_text_non_visual {args}')
        la, lb = [], []
        ha.as_ansi(la.append)
        hb.as_ansi(lb.append)
        self.ae(la, lb, msg)
        self.ae(ha.pagerhist_as_text(), hb.pagerhist_as_text(), msg)
        self.ae(scrollback_for_restore(a), scrollback_for_restore(b), msg)

    def test_erase_last_command_with_compressed_history(self):
        # Erasing the last command while the live prompt has scrolled into the
        # scrollback deletes lines from the middle of the history, which may be
        # compressed or waiting to be, multicell chars cut by it included
        a, b = self.screen_pair(cols=10, lines=5, scrollback=40)
        for s in (a, b):
            for i in range(12):
                s.draw(f'h{i}\r\n')
            draw_multicell(s, 'A', scale=2), s.draw('a\r\n')
            parse_bytes(s, b'\x1b]133;A\a'), s.draw('$ p1\r\n'), parse_bytes(s, b'\x1b]133;C\a')
            for i in range(6):
                s.draw(f'o{i}\r\n')
            draw_multicell(s, 'B', scale=2), s.draw('\r\n')
            parse_bytes(s, b'\x1b]133;A\a')
            for i in range(8):
                s.draw(('\r\n' if i else '') + (f'$ l{i}' if i == 0 else f'l{i}'))
        a.historybuf.wait_for_compression()
        self.assertGreater(a.historybuf.compression_stats()['frames'], 0)
        self.assert_same(a, b, 'before')
        self.assertTrue(a.erase_last_command())
        self.assertTrue(b.erase_last_command())
        self.assert_same(a, b, 'after erasing')
        self.assertIn('$ l0', str(a.historybuf))

    def test_deep_colors_in_compressed_history(self):
        # Garbage collecting the 16 bit colors keeps those of compressed lines
        a, b = self.screen_pair(cols=10, lines=5, scrollback=40)
        for s in (a, b):
            for i in range(30 * 10):
                parse_bytes(s, f'\x1b[38:0:0:{i}:1:2m'.encode())
                s.draw('x')
        a.historybuf.wait_for_compression()
        self.assertGreater(a.historybuf.compression_stats()['frames'], 0)
        for s in (a, b):
            s.garbage_collect_deep_colors()
        self.ae(a.deep_colors_stats()[0], b.deep_colors_stats()[0])
        self.ae(a.deep_colors_stats()[0], 30 * 10)
        self.assert_same(a, b, 'after garbage collection')

    def test_deep_color_gc_skips_frames_without_deep_colors(self):
        # Compressed frames with no 16 bit colors are not decompressed by the
        # garbage collection of those colors, the ones with them are, and the
        # colors they use are kept
        a, b = self.screen_pair(cols=10, lines=5, scrollback=400)
        for s in (a, b):
            for i in range(150):
                s.draw(f'plain{i}\r\n')
            for i in range(10):
                parse_bytes(s, f'\x1b[38:0:0:{i}:1:2m'.encode())
                s.draw(f'deep{i}')
                parse_bytes(s, b'\x1b[m')
                s.draw('\r\n')
            for i in range(150):
                s.draw(f'plain{i}\r\n')
        hb = a.historybuf
        hb.wait_for_compression()
        stats = hb.compression_stats()
        deep_frames = stats['deep_color_frames']
        self.assertGreater(deep_frames, 0)
        self.assertLess(deep_frames, stats['frames'] // 4)
        before = stats['frames_decompressed']
        for s in (a, b):
            s.garbage_collect_deep_colors()
        self.ae(hb.compression_stats()['frames_decompressed'] - before, deep_frames, 'only the frames with deep colors were decompressed')
        self.ae(a.deep_colors_stats()[0], 10)
        self.ae(a.deep_colors_stats()[0], b.deep_colors_stats()[0])
        self.assert_same(a, b, 'after garbage collection')
        # scrollback with no deep colors at all is not decompressed
        c = self.create_screen(cols=10, lines=5, scrollback=100, options={'scrollback_compression': 3})
        # a deep color table, with a color used only by a line that leaves the scrollback
        parse_bytes(c, b'\x1b[38:0:0:1:1:2mx\x1b[m\r\n')
        for i in range(300):
            c.draw(f'plain{i}\r\n')
        c.historybuf.wait_for_compression()
        stats = c.historybuf.compression_stats()
        self.ae(stats['deep_color_frames'], 0)
        c.garbage_collect_deep_colors()
        self.ae(c.historybuf.compression_stats()['frames_decompressed'], stats['frames_decompressed'])
        self.ae(c.deep_colors_stats()[0], 0)

    def test_historybuf_round_trip(self):
        hb = HistoryBuf(100, 10, **TINY)
        ref = HistoryBuf(100, 10, compress=False)
        push_numbered(hb, 300)
        push_numbered(ref, 300)
        # readable while the compressor works
        self.ae(str(hb), str(ref))
        hb.wait_for_compression()
        st = hb.compression_stats()
        self.assertTrue(st['compress'])
        self.ae(st['count'], 100)
        self.ae(st['pending_lines'], 0)
        self.assertGreaterEqual(st['compressed_lines'], 92)
        self.ae(st['hot_lines'], 8)
        # frames end at a line boundary once they hold frame_bytes of encoded lines
        self.assertGreater(st['frames'], 3)
        for i in range(hb.count):
            self.ae(str(hb.line(i)).rstrip(), str(299 - i))
            self.assertTrue(hb.line(i) == ref.line(i))
        # random access across frames
        rng = random.Random(7)
        for _ in range(500):
            i = rng.randrange(hb.count)
            self.ae(str(hb.line(i)).rstrip(), str(299 - i))
        # the cache holds a few chunks while lines are read, and none once trimmed after renders that did not use them
        st = hb.compression_stats()
        self.assertGreater(st['cached_chunks'], 0)
        self.assertLessEqual(st['cached_chunks'], 4)
        hb.cache_trim(), hb.cache_trim()
        self.ae(hb.compression_stats()['cached_chunks'], 0)
        hb.wait_for_compression()
        self.ae(str(hb), str(ref))
        a, b = [], []
        hb.as_ansi(a.append)
        ref.as_ansi(b.append)
        self.ae(a, b)
        # rewrap of a compressed buffer
        for width in (5, 10, 17):
            h2, r2 = hb.rewrap(width), ref.rewrap(width)
            self.ae(str(h2), str(r2))
            self.ae([h2.is_continued(i) for i in range(h2.count)], [r2.is_continued(i) for i in range(r2.count)])
        # compression off
        off = HistoryBuf(100, 10, compress=False)
        push_numbered(off, 300)
        self.assertFalse(off.compression_stats()['compress'])
        self.ae(off.compression_stats()['frames'], 0)
        self.ae(str(off), str(ref))
        # a buffer that fits in the hot tail never compresses
        small = HistoryBuf(8, 10, **TINY)
        push_numbered(small, 50)
        small.wait_for_compression()
        self.ae(small.compression_stats()['frames'], 0)

    def test_frames_by_size_and_bounded_eviction(self):
        # frames end after the line that brings them to frame_bytes; with a line
        # limit, lines past it are dropped a whole frame at a time and hidden until then
        lb = filled_line_buf(5, 40)
        c = filled_cursor()
        for limit in (30, 100, 1000):
            for frame_bytes in (1, 200, 2000, 10**6):
                hb = HistoryBuf(limit, 40, hot_lines=1, block_lines=4, frame_bytes=frame_bytes)
                ref = HistoryBuf(limit, 40, compress=False)
                for i in range(3000):
                    line = lb.line(1)
                    line.set_text((f'{i} ' + 'x' * (i % 30)).ljust(40), 0, 40, c)
                    hb.push(line), ref.push(line)
                    if i % 500 == 0:
                        self.ae(str(hb), str(ref), f'{limit=} {frame_bytes=} {i=}')
                hb.wait_for_compression()
                self.ae(hb.count, min(limit, 3000))
                self.ae(str(hb), str(ref), f'{limit=} {frame_bytes=}')
                st = hb.compression_stats()
                if frame_bytes == 1:
                    self.ae(st['max_frame_lines'], 1)
                # at most one frame of hidden lines is kept
                self.assertLessEqual(st['compressed_lines'] + st['hot_lines'] + st['pending_lines'], hb.count + st['max_frame_lines'] + 4)

    def test_block_and_level_settings(self):
        ref = HistoryBuf(500, 10, compress=False)
        push_numbered(ref, 900)
        lo, hi, default = HistoryBuf.zstd_levels()
        self.assertLess(lo, 0)
        self.assertGreaterEqual(hi, 19)
        for kw in ({'block_lines': 1}, {'block_lines': 3}, {'block_lines': 128}, {'level': lo}, {'level': -5}, {'level': 9}, {'level': hi},
                   {'frame_bytes': 1}, {'frame_bytes': 10**7}):
            hb = HistoryBuf(500, 10, **dict(TINY, **kw))
            push_numbered(hb, 900)
            self.ae(str(hb), str(ref), str(kw))
            hb.wait_for_compression()
            self.ae(str(hb), str(ref), str(kw))

    def test_mode_changes(self):
        # level changes apply to newly compressed lines, recompress also to existing frames, the limit can drop lines
        hb = HistoryBuf(2**31 - 1, 10, level=1, **TINY)
        ref = HistoryBuf(2**31 - 1, 10, compress=False)
        push_numbered(hb, 400), push_numbered(ref, 400)
        hb.set_mode(2**31 - 1, True, 19)
        push_numbered(hb, 400, start=400), push_numbered(ref, 400, start=400)
        self.ae(hb.compression_stats()['level'], 19)
        hb.set_mode(2**31 - 1, True, -3, True)
        hb.wait_for_compression()
        self.ae(str(hb), str(ref))
        self.ae(HistoryBuf.compressor_stats()['queued'], 0)
        # compression off, then on again: the hot tail holds everything meanwhile
        hb.set_mode(2**31 - 1, False, 3)
        push_numbered(hb, 300, start=800), push_numbered(ref, 300, start=800)
        hb.wait_for_compression()
        self.assertGreaterEqual(hb.compression_stats()['hot_lines'], 300)
        hb.set_mode(2**31 - 1, True, 3)
        hb.wait_for_compression()
        self.ae(hb.compression_stats()['hot_lines'], 8)
        self.ae(str(hb), str(ref))
        # a lower limit drops the oldest lines
        hb.set_mode(250, True, 3)
        self.ae(hb.count, 250)
        self.ae(str(hb), '\n'.join(str(ref).split('\n')[:250]))
        # a level outside the library's range is clamped
        hb.set_mode(250, True, 10**6)
        self.ae(hb.compression_stats()['level'], HistoryBuf.zstd_levels()[1])

    def test_threading(self):
        # Shutdown with pending work: buffers and screens freed while the compressor has their blocks
        for _ in range(20):
            hb = HistoryBuf(10**6, 80, level=19, hot_lines=1, block_lines=16)
            push_numbered(hb, 2000, xnum=80)
            self.assertGreater(hb.compression_stats()['pending_lines'] + hb.compression_stats()['compressed_lines'], 0)
            del hb
        for _ in range(5):
            s = self.create_screen(cols=80, lines=5, scrollback=10**6, options={'scrollback_compression': 19})
            for i in range(3000):
                s.draw(f'line {i} ' * 5), s.carriage_return(), s.linefeed()
            del s
        # resize, reset and clearing while lines are pending
        a, b = self.screen_pair(cols=30, lines=5, scrollback=10**6, level=19, params=(1, 8, 512))
        for i in range(2000):
            t = f'\x1b[3{i % 8}mrow {i} ' + 'z' * (i % 41) + '\r\n'
            parse_bytes(a, t.encode()), parse_bytes(b, t.encode())
            if i % 400 == 399:
                for s in (a, b):
                    s.resize(5 + (i // 400) % 3, 30 + (i // 400) % 2)
                self.assert_same_(a, b, f'resize at {i}')
        for s in (a, b):
            s.reset()
        self.assert_same(a, b, 'reset')
        for i in range(500):
            parse_bytes(a, f'again {i}\r\n'.encode()), parse_bytes(b, f'again {i}\r\n'.encode())
        for s in (a, b):
            parse_bytes(s, b'\x1b[3J')
        self.ae(a.historybuf.count, 0)
        self.assert_same(a, b, 'cleared')
        # reverse scrolling takes lines back from the compressor and the frames
        for i in range(300):
            parse_bytes(a, f'back {i}\r\n'.encode()), parse_bytes(b, f'back {i}\r\n'.encode())
        for s in (a, b):
            s.reverse_scroll(200, True)
        self.assert_same(a, b, 'reverse scroll')

    def test_cells_and_attributes(self):
        # colours, SGR attributes, Silkie's per-cell fields (gamut, HDR, text
        # style, depth, phase), hyperlinks, wrapping, prompt marks
        a, b = self.screen_pair(cols=12, lines=3, scrollback=80)
        data = [
            '\x1b[31mred\x1b[42mgreen-bg\x1b[m plain',
            '\x1b[1;3;4;9mstyled\x1b[m \x1b[2mdim\x1b[m \x1b[7mrev\x1b[m',
            '\x1b[38:2::10:20:30mtrue\x1b[48;5;200mcolour\x1b[58:5:9m\x1b[4:3mcurly\x1b[m',
            '\x1b]8;;https://example.com/a\x1b\\link\x1b]8;;\x1b\\ after \x1b]8;id=x;https://example.com/b\x1b\\two\x1b]8;;\x1b\\',
            '\x1b]133;A\x07$ prompt\r\n\x1b]133;C\x07output',
            'wrapped ' * 4,
            '日本語 😀 é à́',
            '\x1b[44m\x1b[Kbg to the end\x1b[m',
        ]
        # Silkie extensions, those the build has: unknown sequences are ignored by both screens alike
        data += ['\x1b[38:2:1:255:0:0m\x1b[48:2:2:0:255:0mgamut\x1b[m', '\x1b[58:6:1:2:3mhdr\x1b[m', '\x1b]66;s=2;big\x07 text sized']
        for i in range(30):
            for d in data:
                parse_bytes(a, f'{i} {d}\r\n'.encode())
                parse_bytes(b, f'{i} {d}\r\n'.encode())
        # the compressor runs at low priority, on a busy machine it may not have caught up yet
        a.historybuf.wait_for_compression()
        self.assertGreater(a.historybuf.compression_stats()['compressed_lines'], 40)
        self.assert_same(a, b)
        # the hyperlinks survive compression
        texts = []
        a.as_text_for_history_buf(texts.append, True)
        self.assertIn('https://example.com/a', ''.join(texts))

    def test_multicell_in_history(self):
        a, b = self.screen_pair(cols=10, lines=5, scrollback=60)
        for i in range(40):
            for s in (a, b):
                s.draw(f'{i}x')
                draw_multicell(s, 'AB日'[i % 3], scale=1 + i % 3)
                s.draw('y')
                s.carriage_return(), s.linefeed()
        self.assert_same(a, b)
        for h, w in ((5, 7), (3, 13), (8, 10), (5, 10)):
            a.resize(h, w), b.resize(h, w)
            self.assert_same(a, b, f'resize {h}x{w}')

    def test_resize_and_random_operations(self):
        from kitty.fast_data_types import test_set_history_reuse

        try:
            for seed in range(120):
                rng = random.Random(seed)
                cols, lines = rng.randint(3, 12), rng.randint(2, 8)
                scrollback = rng.choice((20, 30, 200))
                params = rng.choice(((1, 1, 1), (1, 1, 64), (1, 2, 64), (1, 3, 200), (30, 4, 1000)))
                options = {
                    'scrollback_fill_enlarged_window': rng.random() < 0.5,
                    'scrollback_pager_history_size': rng.choice((0, 1024)),
                }
                test_set_history_reuse(rng.random() < 0.5)
                a, b = self.screen_pair(cols, lines, scrollback, options, params)
                for i, op in enumerate(ops_for(seed, cols)):
                    run(a, op), run(b, op)
                    if i % 10 == 0:
                        a.historybuf.cache_trim()
                    if i % 17 == 0:
                        a.historybuf.wait_for_compression()
                self.assert_same(a, b, f'seed={seed}')
        finally:
            test_set_history_reuse(True)

    def test_selection_spanning_compressed_history(self):
        a, b = self.screen_pair(cols=10, lines=4, scrollback=200)
        for i in range(150):
            line = f'\x1b[3{i % 8}m{i:03d} sel\x1b]8;;http://h/{i % 5}\x1b\\L\x1b]8;;\x1b\\ é́\r\n'
            parse_bytes(a, line.encode()), parse_bytes(b, line.encode())
        draw_multicell(a, 'X', scale=3), draw_multicell(b, 'X', scale=3)
        for s in (a, b):
            s.scroll(SCROLL_FULL, True)
            s.start_selection(2, 0, False)
            s.scroll(SCROLL_FULL, False)
            s.update_selection(5, s.lines - 1, True)
        for ansi in (False, True):
            for strip in (False, True):
                self.ae(a.text_for_selection(ansi, strip), b.text_for_selection(ansi, strip))
        self.assertIn('001 sel', ''.join(a.text_for_selection(False, False)))

    def test_marker_and_scroll_to_mark(self):
        from kitty.marks import marker_from_regex
        a, b = self.screen_pair(cols=10, lines=4, scrollback=100)
        for i in range(80):
            t = f'{"target" if i in (3, 40) else "line"} {i}\r\n'.encode()
            parse_bytes(a, t), parse_bytes(b, t)
        for s in (a, b):
            s.set_marker(marker_from_regex('target', 1))
        for _ in range(3):
            self.ae(a.scroll_to_next_mark(0, True), b.scroll_to_next_mark(0, True))
            self.ae(a.scrolled_by, b.scrolled_by)
        for _ in range(3):
            self.ae(a.scroll_to_next_mark(0, False), b.scroll_to_next_mark(0, False))
            self.ae(a.scrolled_by, b.scrolled_by)
        a.set_marker(), b.set_marker()
        self.ae(a.scroll_to_next_mark(0, True), b.scroll_to_next_mark(0, True))

    def test_garbage_collection_of_compressed_lines(self):
        # Enough hyperlinks and multi codepoint cells to trigger the garbage
        # collection of the hyperlink pool and the text cache, which rewrite
        # the compressed frames
        a, b = self.screen_pair(cols=20, lines=3, scrollback=10000, params=(1, 16, 4096))
        n = 9000
        chunk = []
        for i in range(n):
            chunk.append(f'\x1b]8;;http://example.com/{i}\x1b\\L{i}\x1b]8;;\x1b\\ a{chr(0x300 + i % 100)}{chr(0x300 + i // 100 % 100)}\r\n')
            if len(chunk) == 500:
                d = ''.join(chunk).encode()
                parse_bytes(a, d), parse_bytes(b, d)
                a.historybuf.wait_for_compression()
                chunk = []
        st = a.historybuf.compression_stats()
        self.assertGreater(st['frames_recompressed'], 0)
        ta, tb = [], []
        a.as_text_for_history_buf(ta.append, True)
        b.as_text_for_history_buf(tb.append, True)
        self.ae(ta, tb)
        self.assertIn('http://example.com/0\x1b', ''.join(ta))

    def test_pager_history_from_compressed_lines(self):
        a, b = self.screen_pair(cols=10, lines=3, scrollback=30, options={'scrollback_pager_history_size': 1024})
        for i in range(200):
            t = f'\x1b[3{i % 7}mp{i} ' + ('w' * (i % 25)) + '\x1b[m\r\n'
            parse_bytes(a, t.encode()), parse_bytes(b, t.encode())
        self.ae(a.historybuf.pagerhist_as_text(), b.historybuf.pagerhist_as_text())
        self.assertIn('p170 ', a.historybuf.pagerhist_as_text())
        self.assert_same(a, b)

    def test_clear_and_reset(self):
        a, b = self.screen_pair(cols=10, lines=3, scrollback=50)
        for s in (a, b):
            for i in range(100):
                s.draw(f'c{i}'), s.carriage_return(), s.linefeed()
        self.assert_same(a, b)
        for s in (a, b):
            parse_bytes(s, b'\x1b[3J')
        self.ae(a.historybuf.count, 0)
        self.ae(a.historybuf.compression_stats()['frames'], 0)
        for s in (a, b):
            for i in range(100):
                s.draw(f'd{i}'), s.carriage_return(), s.linefeed()
        self.assert_same(a, b)
        for s in (a, b):
            s.reset()
        self.assert_same(a, b)

    def test_unlimited_scrollback(self):
        s = self.create_screen(cols=10, lines=3, scrollback=2**32 - 1, options={'scrollback_compression': 9})
        hb = s.historybuf
        self.ae(hb.ynum, 2**31 - 1)
        for i in range(5000):
            s.draw(f'u{i}'), s.carriage_return(), s.linefeed()
        self.ae(hb.count, 4998)
        self.ae(str(hb.line(hb.count - 1)).rstrip(), 'u0')
        hb.wait_for_compression()
        st = hb.compression_stats()
        self.assertTrue(st['compress'])
        self.ae(st['level'], 9)
        self.assertLess(st['compressed_bytes'], 4998 * 30)


class FakeTab:
    scrollback_settings = None

    def __init__(self, profile=None):
        self.profile = profile
        self.windows = []

    def __iter__(self):
        return iter(self.windows)

    @property
    def active_window(self):
        return self.windows[0] if self.windows else None

    def set_scrollback_settings(self, *a, **kw):
        from kitty.tabs import Tab
        Tab.set_scrollback_settings(self, *a, **kw)


class TestScrollbackSettings(BaseTest):
    # The two settings, scrollback_lines and scrollback_compression, globally,
    # per profile, per tab (launch, session files, remote control) and per window

    def window(self, tab, options=None):
        from kitty.window import Window
        w = Window.__new__(Window)
        w.tabref = lambda: tab
        w.scrollback_override = getattr(tab, 'scrollback_settings', None)
        lines = w.scrollback_override[0] if w.scrollback_override else w.default_scrollback_settings(tab.profile)[0]
        w.screen = self.create_screen(cols=10, lines=3, scrollback=lines, options=options)
        w.apply_scrollback_settings(profile=tab.profile)
        tab.windows.append(w)
        return w

    def test_option_parsing(self):
        from kitty.options.utils import SCROLLBACK_UNLIMITED, scrollback_compression, scrollback_lines, scrollback_settings_from
        lo, hi, _ = HistoryBuf.zstd_levels()
        self.ae(scrollback_lines('unlimited'), SCROLLBACK_UNLIMITED)
        self.ae(scrollback_lines('-1'), SCROLLBACK_UNLIMITED)
        self.ae(scrollback_lines('5000'), 5000)
        self.ae(scrollback_compression('none'), SCROLLBACK_COMPRESSION_NONE)
        self.ae(scrollback_compression('9'), 9)
        self.ae(scrollback_compression(str(lo)), lo)
        self.ae(scrollback_compression(str(hi)), hi)
        for bad in (str(hi + 1), str(lo - 1), 'fast'):
            with self.assertRaises(ValueError):
                scrollback_compression(bad)
        self.ae(scrollback_settings_from(-1, 'zstd:19'), (SCROLLBACK_UNLIMITED, 19))
        self.ae(scrollback_settings_from('300', 'none'), (300, SCROLLBACK_COMPRESSION_NONE))
        self.ae(scrollback_settings_from(300, -5), (300, -5))
        from kitty.config import defaults
        self.ae((defaults.scrollback_lines, defaults.scrollback_compression), (SCROLLBACK_UNLIMITED, 9))

    def test_window_and_tab_settings(self):
        tab = FakeTab()
        self.set_options()
        w = self.window(tab)
        self.ae(w.scrollback_settings, (-1, 9))
        st = w.screen.historybuf.compression_stats()
        self.ae((st['ynum'], st['compress'], st['level']), (2**31 - 1, True, 9))
        # every combination
        for lines, compression in ((-1, 'none'), (500, 'none'), (500, 3), (-1, 'zstd:-7'), (2000, '19')):
            w.set_scrollback_settings(lines, compression)
            st = w.screen.historybuf.compression_stats()
            want_level = 'none' if compression == 'none' else int(str(compression).removeprefix('zstd:'))
            self.ae(w.scrollback_settings, (lines, want_level))
            self.ae(st['ynum'], 2**31 - 1 if lines < 0 else lines)
            self.ae(st['compress'], compression != 'none')
            if compression != 'none':
                self.ae(st['level'], want_level)
        # a tab's settings apply to its windows and are inherited by new ones
        w2 = self.window(tab)
        tab_obj = tab
        from kitty.tabs import Tab
        Tab.set_scrollback_settings(tab_obj, 1000, 'zstd:5')
        for x in (w, w2):
            self.ae(x.scrollback_settings, (1000, 5))
        w3 = self.window(tab)
        self.ae(w3.scrollback_settings, (1000, 5))
        self.ae(w3.screen.historybuf.compression_stats()['ynum'], 1000)
        # a config reload changes the compression of windows that follow the global options
        other = self.window(FakeTab())
        self.set_options({'scrollback_compression': 2})
        other.apply_scrollback_settings()
        self.ae(other.scrollback_settings, (-1, 2))
        self.ae(w3.scrollback_settings, (1000, 5))

    def test_profile_settings(self):
        from kitty.profiles import Profile, clear_caches
        clear_caches()
        p = Profile(id='zstd-test', name='zstd test', options={'scrollback_lines': ['250'], 'scrollback_compression': ['none']})
        self.set_options()
        w = self.window(FakeTab(p))
        self.ae(w.scrollback_settings, (250, 'none'))
        self.assertFalse(w.screen.historybuf.compression_stats()['compress'])
        clear_caches()
        p = Profile(id='zstd-test2', name='zstd test 2', options={'scrollback_compression': ['15']})
        w = self.window(FakeTab(p))
        self.ae(w.scrollback_settings, (-1, 15))
        clear_caches()

    def test_launch_and_session(self):
        from kitty.launch import parse_launch_args
        spec = parse_launch_args(['--scrollback-lines', 'unlimited', '--scrollback-compression', 'zstd:12', 'echo'])
        self.ae((spec.opts.scrollback_lines, spec.opts.scrollback_compression), ('unlimited', 'zstd:12'))
        from kitty.session import parse_session
        self.set_options()
        from kitty.config import defaults
        sessions = list(parse_session('new_tab one\nlaunch --scrollback-lines 400 --scrollback-compression none\nlaunch\n', defaults))
        windows = sessions[0].tabs[0].windows
        self.ae((windows[0].launch_spec.opts.scrollback_lines, windows[0].launch_spec.opts.scrollback_compression), ('400', 'none'))

    def test_remote_control(self):
        from kitty.rc.base import PayloadGetter, command_for_name
        self.set_options()
        tab_a, tab_b = FakeTab(), FakeTab()
        wa, wb = self.window(tab_a), self.window(tab_b)
        cmd = command_for_name('set_scrollback_compression')

        class Boss:
            active_window = wa

            @property
            def all_windows(self):
                return tab_a.windows + tab_b.windows

            def match_windows(self, expr, self_window=None):
                return [w for w in self.all_windows if (w is wb) == (expr == 'b')]

            def match_tabs(self, expr):
                return [tab_b] if expr == 'b' else [tab_a]

        def run(payload):
            cmd.response_from_kitty(Boss(), None, PayloadGetter(cmd, payload))

        run({'compression': '7', 'lines': '', 'recompress': False, 'match_window': '', 'match_tab': '', 'all': False})
        self.ae((wa.scrollback_settings, wb.scrollback_settings), ((-1, 7), (-1, 9)))
        run({'compression': 'none', 'lines': '300', 'recompress': False, 'match_window': 'b', 'match_tab': '', 'all': False})
        self.ae(wb.scrollback_settings, (300, 'none'))
        run({'compression': 'zstd:4', 'lines': 'unlimited', 'recompress': True, 'match_window': '', 'match_tab': 'b', 'all': False})
        self.ae(wb.scrollback_settings, (-1, 4))
        self.ae(tab_b.scrollback_settings[1], 4)
        wc = self.window(tab_b)
        self.ae(wc.scrollback_settings, (-1, 4))
        run({'compression': '1', 'lines': '', 'recompress': False, 'match_window': '', 'match_tab': '', 'all': True})
        for w in (wa, wb, wc):
            self.ae(w.scrollback_settings[1], 1)
