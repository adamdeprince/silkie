# License: GPLv3
from itertools import product

from kitty.accessibility import pane_text
from kitty.fast_data_types import SCROLL_LINE, send_gesture_report, send_pointer_action, send_pointer_report, send_scroll_report
from kitty.options.utils import text_flow

from .base import BaseTest, parse_bytes
from .graphics import parse_response, send_command


def osc(screen, selector, fields):
    screen.callbacks.clear()
    parse_bytes(screen, f'\x1b]777;{selector};v=1:{fields}\x1b\\'.encode())
    return screen.callbacks.wtcbuf


class TestWritingDirections(BaseTest):
    def test_flow_axes(self):
        axes = (('right', 'left'), ('down', 'up'))
        for vertical in range(2):
            for inline, line in product(axes[vertical], axes[1 - vertical]):
                with self.subTest(inline=inline, line=line):
                    s = self.create_screen(cols=12, lines=4, cell_width=10, cell_height=20)
                    cmd = f'op=set:inline={inline}:line={line}'
                    self.assertIn(f'inline={inline}:line={line}:next=auto'.encode(), osc(s, 'flow', cmd + ':id=42'))
                    self.ae(s.flow, text_flow(inline + ' ' + line))
                    self.ae((s.lines, s.columns), (6, 8) if vertical else (4, 12))
                    self.ae(osc(s, 'flow', f'op=set:inline={inline}:line={inline}:id=43'), b'')
                    # Place a native-size, asymmetric raster at logical home.
                    self.ae(parse_response(send_command(s, 'a=T,f=24,i=1,s=8,v=3,C=1', b'xyz' * 24)), 'OK')
                    ref, = s.update_graphics_layers()
                    dest = ref['dest_rect']
                    self.assertAlmostEqual((dest['right'] - dest['left']) * 60, 8, places=4)
                    self.assertAlmostEqual((dest['top'] - dest['bottom']) * 40, 3, places=4)
                    self.assertAlmostEqual(dest['right'] if 'left' in (inline, line) else dest['left'],
                                           1 if 'left' in (inline, line) else -1)
                    self.assertAlmostEqual(dest['bottom'] if 'up' in (inline, line) else dest['top'],
                                           -1 if 'up' in (inline, line) else 1)
                    parse_bytes(s, '第一\r\n第二'.encode())
                    self.ae(str(s.line(0)), '第一')
                    self.ae(str(s.line(1)), '第二')
                    for i in range(300):
                        parse_bytes(s, f'\r\n{i}'.encode())
                    self.ae(str(s.line(s.lines - 1)), '299')
                    s.scroll(SCROLL_LINE, True)
                    self.ae(str(s.visual_line(s.lines - 1)), '298')
                    s.resize(6, 14)
                    self.ae((s.lines, s.columns), (7, 12) if vertical else (6, 14))
                    s.reset()
                    self.ae((s.flow, s.lines, s.columns), (0, 6, 14))

    def test_flow_pointer_interfaces(self):
        s = self.create_screen(cols=12, lines=4)
        osc(s, 'flow', 'op=set:inline=up:line=right')
        osc(s, 'gesture', 'op=mode:mode=1')
        osc(s, 'scroll', 'op=mode:mode=1')
        osc(s, 'tablet-pointer', 'op=mode:mode=19')
        # Pane is 120 x 80 physical pixels. (3, 7) addresses (72.996..., 3).
        expected = b':x=18687:y=768:'
        calls = (
            lambda: send_scroll_report(s, 0, 1, x=3, y=7),
            lambda: send_gesture_report(s, 1, 1, 768, 1792),
            lambda: send_pointer_report(s, 1, 1, 1, 0, 768, 1792),
            lambda: send_pointer_action(s, 1, x=768, y=1792),
        )
        for call in calls:
            s.callbacks.clear()
            self.assertTrue(call())
            self.assertIn(expected, s.callbacks.wtcbuf)
        s.callbacks.clear()
        self.assertTrue(send_pointer_report(s, 2, 1, 2, 1, 768, 1792, src=1))
        self.assertIn(b':x=768:y=1792:', s.callbacks.wtcbuf)

    def test_character_reading_paths(self):
        s = self.create_screen(cols=2, lines=2)
        osc(s, 'flow', 'op=reading:next=down')
        s.draw('AB')
        osc(s, 'flow', 'op=reading:next=auto')
        parse_bytes(s, b'\r\nCD')
        result = pane_text(s)
        self.ae(result['text'], 'AC\nBD')
        self.ae(result['cells'], [[(0, 1), (3, 4)], [(1, 2), (4, 5)]])
        self.ae(result['caret'], 5)
        osc(s, 'flow', 'op=reading:next=up')
        s.cursor_position(2, 1)
        s.draw('C')
        self.ae(sorted(pane_text(s)['text'].split()), ['AC', 'BD'])
        osc(s, 'flow', 'op=reading:next=auto')
        s.cursor_position(1, 1)
        s.draw('EF')
        self.ae(s.line(0).reading_directions(), None)
        self.ae(s.line(1).reading_directions(), b'\x04\0')

    def test_reading_across_wide_character(self):
        s = self.create_screen(cols=8, lines=2)
        s.draw('水')
        osc(s, 'flow', 'op=reading:next=left')
        s.draw('A')
        snapshot = pane_text(s)
        self.ae(snapshot['text'], 'A水')
        self.ae(snapshot['cells'][0][:3], [(1, 2), (1, 2), (0, 1)])

    def test_reading_end_marker(self):
        s = self.create_screen(cols=12, lines=2)
        s.draw('start')
        self.assertIn(b':next=end', osc(s, 'flow', 'op=reading:next=end:id=1'))
        s.draw('水')
        self.assertIn(b':next=end', osc(s, 'flow', 'op=query:id=2'))
        self.ae(osc(s, 'flow', 'op=set:inline=end:line=down:id=3'), b'')
        osc(s, 'flow', 'op=reading:next=auto')
        parse_bytes(s, b'extra\r\nmore')
        result = pane_text(s)
        self.ae(result['text'], 'start水')
        self.ae(result['cells'][0][5:8], [(5, 6), (5, 6), (6, 6)])
        self.ae(result['caret'], 6)
        self.ae(str(s.line(1)), 'more')
        # Overwriting the marked character makes the rest readable again.
        s.cursor_position(1, 6)
        s.draw('火')
        self.ae(pane_text(s)['text'], 'start火extra\nmore')

    def test_reading_stops_at_first_revisit(self):
        # Enter a two-cell cycle after a long run of identical characters.
        # Distinct positions must all be read, and later text must be omitted.
        count = 4096
        s = self.create_screen(cols=count + 2, lines=2)
        osc(s, 'flow', 'op=reading:next=right')
        s.draw('a' * count)
        osc(s, 'flow', 'op=reading:next=left')
        s.draw('b')
        osc(s, 'flow', 'op=reading:next=auto')
        s.draw('X')
        self.ae(pane_text(s)['text'], 'a' * count + 'b')

        # A second head that joins an already-read path also ends the read.
        s = self.create_screen(cols=3, lines=2)
        osc(s, 'flow', 'op=reading:next=down')
        s.draw('A')
        osc(s, 'flow', 'op=reading:next=auto')
        parse_bytes(s, b'\r\nB')
        osc(s, 'flow', 'op=reading:next=left')
        s.draw('C')
        osc(s, 'flow', 'op=reading:next=auto')
        s.draw('D')
        self.ae(pane_text(s)['text'], 'ABC')

    def test_default_flow_and_reading_lifetime(self):
        s = self.create_screen(cols=10, lines=3, scrollback=600)
        s.set_default_flow(text_flow('left up'))
        osc(s, 'flow', 'op=set:inline=right:line=down')
        osc(s, 'flow', 'op=reading:next=left')
        for _ in range(400):
            parse_bytes(s, '水a\u0301\r\n'.encode())
        osc(s, 'flow', 'op=reading:next=auto')
        s.scroll(SCROLL_LINE, True)
        self.ae(s.visual_line(0).reading_directions()[:3], b'\x02' * 3)
        self.assertIn('a\u0301水', pane_text(s)['text'])
        osc(s, 'caps', 'op=reset')
        self.ae(s.flow, text_flow('left up'))
        s.reset()
        self.ae(s.line(0).reading_directions(), None)
