#!/usr/bin/env python
# License: GPL v3 Copyright: 2016, Kovid Goyal <kovid at kovidgoyal.net>

import re
from base64 import standard_b64encode
from functools import partial
from time import sleep

from kitty.config import load_config
from kitty.fast_data_types import (
    GLFW_MOD_ALT,
    GLFW_MOD_CAPS_LOCK,
    GLFW_MOD_CONTROL,
    GLFW_MOD_SHIFT,
    GLFW_MOD_SUPER,
    GLFW_MOUSE_BUTTON_LEFT,
    GLFW_MOUSE_BUTTON_RIGHT,
    MOUSE_SELECTION_WORD_AND_LINE_FROM_POINT,
    create_mock_window,
    mock_mouse_selection,
    monotonic,
    send_gesture_report,
    send_mock_mouse_event_to_window,
    send_pointer_action,
    send_pointer_device,
    send_pointer_pad,
    send_pointer_report,
    take_pointer_pad_label,
    send_mock_scroll_event_to_window,
    set_options,
    update_mock_pointer,
)

from .base import BaseTest, parse_bytes


def send_mouse_event(
    window,
    button=-1,
    modifiers=0,
    is_release=False,
    x=0.0,
    y=0,
    clear_click_queue=False,
    pixel_x=None,
    pixel_y=None,
):
    ix = int(x)
    in_left_half_of_cell = x - ix < 0.5
    return send_mock_mouse_event_to_window(window, button, modifiers, is_release, ix, y, clear_click_queue, in_left_half_of_cell, pixel_x, pixel_y)


class TestMouse(BaseTest):
    def test_link_hover_modifiers(self):
        s = self.create_screen(cols=60)
        s.draw('https://example.invalid/')
        parse_bytes(s, b'\r\n\x1b]8;;https://example.invalid/\x1b\\hyperlink\x1b]8;;\x1b\\\r\nsrc/file.py:42')
        w = create_mock_window(s)
        hover = partial(send_mouse_event, w, x=2)
        normal_shape = send_mouse_event(w, x=50)
        link_shape = hover()
        self.assertNotEqual(normal_shape, link_shape)

        def configure(*lines):
            set_options(load_config(overrides=('clear_all_mouse_actions yes', 'detect_url_regex [[:alnum:]_./-]+:[0-9]+', *lines)))

        def check(mods, clickable, y=0):
            self.ae(hover(modifiers=mods, y=y), link_shape if clickable else normal_shape)
            expected = ('https://example.invalid/', 'hyperlink', 'src/file.py:42')[y] if clickable else ''
            self.ae(''.join(s.text_for_marked_url()), expected)
            # The renderer uses bit 1 of the selection buffer for URL underlines.
            self.ae(sum(bool(cell & 2) for cell in s.current_selections()), len(expected))

        mods = GLFW_MOD_CONTROL | GLFW_MOD_SHIFT
        configure('mouse_map ctrl+shift+left release grabbed,ungrabbed mouse_handle_click link')
        for y in (0, 1, 2):
            for held, clickable in (
                (0, False),
                (GLFW_MOD_CONTROL, False),
                (mods, True),
                (mods | GLFW_MOD_ALT, False),
                (mods | GLFW_MOD_CAPS_LOCK, True),
                (0, False),
            ):
                # Repeated events in the same cell must respond to modifier changes.
                check(held, clickable, y)

        configure('mouse_map left click ungrabbed mouse_handle_click selection link prompt')
        check(0, True)
        check(mods, False)
        parse_bytes(s, b'\x1b[?1000h')
        normal_shape = send_mouse_event(w, x=50)
        check(0, False)
        configure('mouse_map alt+left release grabbed mouse_click_url')
        check(GLFW_MOD_ALT, True)
        check(0, False)
        parse_bytes(s, b'\x1b[?1000l')
        normal_shape = send_mouse_event(w, x=50)
        check(GLFW_MOD_ALT, False)

        configure('action_alias follow_link mouse_handle_click link', 'mouse_map alt+left release ungrabbed follow_link')
        check(GLFW_MOD_ALT, True)
        check(0, False)
        configure('mouse_map left click ungrabbed combine : mouse_handle_click link : discard_event')
        check(0, True)
        configure('mouse_map left click ungrabbed mouse_click_url_or_select')
        check(0, True)
        configure('mouse_map left click ungrabbed mouse_click_url', 'mouse_map left click ungrabbed')
        check(0, False)

        for event in ('press', 'release', 'click'):
            configure(f'mouse_map cmd+left {event} ungrabbed mouse_handle_click link')
            check(GLFW_MOD_SUPER, True)
            check(0, False)
        for event in ('doublepress', 'triplepress', 'doubleclick'):
            configure(f'mouse_map left {event} ungrabbed mouse_click_url')
            check(0, False)

    def test_link_hover_key_events(self):
        s = self.create_screen(cols=60)
        s.draw('https://example.invalid/')
        w = create_mock_window(s)
        link_shape = send_mouse_event(w, x=2)
        mods = GLFW_MOD_CONTROL | GLFW_MOD_SHIFT
        set_options(
            load_config(
                overrides=(
                    'clear_all_mouse_actions yes',
                    'mouse_map ctrl+shift+left release grabbed,ungrabbed mouse_handle_click link',
                    'mouse_map cmd+left release grabbed,ungrabbed mouse_handle_click link',
                )
            )
        )
        for tracking in (False, True):
            parse_bytes(s, b'\x1b[?1000h' if tracking else b'\x1b[?1000l')
            normal_shape = send_mouse_event(w, x=50)
            send_mouse_event(w, x=2)
            for held in (mods, GLFW_MOD_SUPER):
                for old, new, highlighted in ((0, held, True), (held, 0, False)):
                    self.ae(update_mock_pointer(w, old, new), (link_shape if highlighted else normal_shape, True))
                    self.ae(bool(''.join(s.text_for_marked_url())), highlighted)
                self.ae(update_mock_pointer(w, 0, held, False)[1], False)
                self.ae(''.join(s.text_for_marked_url()), '')
            for old, new in ((0, GLFW_MOD_SHIFT), (mods, GLFW_MOD_SUPER), (mods, mods | GLFW_MOD_CAPS_LOCK)):
                self.ae(update_mock_pointer(w, old, new)[1], False)
            for drag in (1, 2):
                self.ae(update_mock_pointer(w, 0, mods, True, drag)[1], False)

    def test_link_hover_preserves_border_cursor(self):
        s = self.create_screen(cols=60)
        s.draw('https://example.invalid/')
        w = create_mock_window(s)
        send_mouse_event(w, x=2)
        mods = GLFW_MOD_CONTROL | GLFW_MOD_SHIFT
        for border in (1, 2):
            shape, updated = update_mock_pointer(w, 0, 0, True, 0, border)
            self.ae(updated, False)
            self.ae(update_mock_pointer(w, 0, mods, True, 0, border), (shape, False))

    def test_link_hover_cleared_by_output(self):
        s = self.create_screen(cols=60)
        w = create_mock_window(s)
        mods = GLFW_MOD_CONTROL | GLFW_MOD_SHIFT
        set_options(load_config(overrides=('clear_all_mouse_actions yes', 'mouse_map ctrl+shift+left release ungrabbed mouse_handle_click link')))

        def marked():
            return ''.join(s.text_for_marked_url())

        def setup():
            s.reset()
            s.draw('https://example.invalid/')
            return send_mouse_event(w, x=2, modifiers=mods)

        parse_bytes(s, b'\x1b[?1000h')
        grabbed_shape = send_mouse_event(w, x=50)
        s.reset()
        normal_shape = send_mouse_event(w, x=50)
        for output, expected in (
            (b' suffix', normal_shape),
            (b'\r\x1b[Kplain text', normal_shape),
            (b'\x1b[2J', normal_shape),
            (b'\x1b[?1000h\r\x1b[K', grabbed_shape),
            (b'\x1b[?1000h', None),
            (b'\x1b[3;1Hother line', None),
        ):
            with self.subTest(output=output):
                link_shape = setup()
                self.assertNotIn(link_shape, (normal_shape, grabbed_shape))
                self.ae(marked(), 'https://example.invalid/')
                # Output alone, with the modifiers still held, must update the pointer
                shape = update_mock_pointer(w, mods, mods, True, 0, 0, None, False, partial(parse_bytes, s, output))[0]
                self.ae(shape, expected or link_shape)
                self.ae(bool(marked()), expected is None)
                if expected is not None:
                    # Releasing the modifiers after the output changes nothing
                    setup()
                    self.ae(update_mock_pointer(w, mods, 0, True, 0, 0, None, False, partial(parse_bytes, s, output)), (expected, False))
                    self.ae(marked(), '')

    def test_link_hover_stays_consistent(self):
        s = self.create_screen(cols=30)
        s.draw('x' * 6 + 'https://example.invalid/')
        w = create_mock_window(s)
        normal_shape = send_mouse_event(w, x=2)
        link_shape = send_mouse_event(w, x=29)
        mods = GLFW_MOD_CONTROL | GLFW_MOD_SHIFT

        def configure(*lines):
            set_options(load_config(overrides=('clear_all_mouse_actions yes', *lines)))

        def marked():
            return ''.join(s.text_for_marked_url())

        # The cell under the mouse is stale once it moves into the padding
        configure('mouse_map ctrl+shift+left release ungrabbed mouse_handle_click link')
        send_mouse_event(w, x=29)
        self.ae(update_mock_pointer(w, 0, mods, True, 0, 0, None, True), (normal_shape, False))
        self.ae(marked(), '')
        send_mouse_event(w, x=29)
        self.ae(update_mock_pointer(w, mods, 0, True, 0, 0, None, True), (normal_shape, True))
        self.ae(marked(), '')

        # The mouse tracking mode can change without the mouse moving
        configure('mouse_map left click ungrabbed mouse_click_url')
        send_mouse_event(w, x=10)
        self.ae(update_mock_pointer(w, 0, GLFW_MOD_ALT, True, 0, 0, None, False, partial(parse_bytes, s, b'\x1b[?1000h'))[1], True)
        self.ae(marked(), '')
        parse_bytes(s, b'\x1b[?1000l')

        # Pointer shape changes from the program keep the hand over a highlighted link
        send_mouse_event(w, x=10)
        self.ae(update_mock_pointer(w, 0, 0, True, 0, 0, 'wait')[0], link_shape)
        self.ae(marked(), 'https://example.invalid/')

    def test_pointer_shape_during_mouse_drag(self):
        s = self.create_screen(cols=60)
        parse_bytes(s, b'\x1b[?1000h')
        w = create_mock_window(s)
        for drag in (1, 2):
            for name in ('grabbing', 'ew-resize'):
                s.change_pointer_shape('=', name)
                expected = send_mouse_event(w, x=2)
                s.change_pointer_shape('=', 'text')
                self.ae(update_mock_pointer(w, 0, 0, True, drag, 0, name)[0], expected)

    def test_mouse_selection(self):
        s = self.create_screen(options=dict(rectangle_select_modifiers=GLFW_MOD_ALT | GLFW_MOD_CONTROL))
        w = create_mock_window(s)
        ev = partial(send_mouse_event, w)

        def mouse_selection(code: int) -> None:
            mock_mouse_selection(w, s.callbacks.current_mouse_button, code)

        s.callbacks.mouse_selection = mouse_selection

        def sel():
            return ''.join(s.text_for_selection())

        def init():
            s.reset()
            s.draw('pqrst')
            s.draw('uvwxy')
            s.draw('ABCDE')
            s.draw('FGHIJ')
            s.draw('KLMNO')
            s.draw('12345')
            s.draw('67890')
            s.draw('abcde')
            s.draw('fghij')
            s.draw('klmno')

        def press(x=0, y=0, modifiers=0, button=GLFW_MOUSE_BUTTON_LEFT):
            ev(button, x=x, y=y, modifiers=modifiers)

        def release(x=0, y=0, button=GLFW_MOUSE_BUTTON_LEFT):
            ev(button, x=x, y=y, is_release=True, clear_click_queue=True)

        def move(x=0, y=0, button=-1, q=None):
            ev(x=x, y=y, button=button)
            if q is not None:
                sl = sel()
                from kitty.window import as_text

                self.ae(sl, q, f'{sl!r} != {q!r} after movement to x={x} y={y}. Screen contents: {as_text(s)!r}')

        def multi_click(x=0, y=0, count=2, modifiers=0):
            clear_click_queue = True
            while count > 0:
                count -= 1
                ev(GLFW_MOUSE_BUTTON_LEFT, x=x, y=y, modifiers=modifiers, clear_click_queue=clear_click_queue)
                clear_click_queue = False

        def scroll(x=0, y=0, up=True):
            move(x=x, y=y, button=-2 if up else -3)

        # Single line click, move, release test
        init()
        press()
        move(x=3.6, q='1234')
        release(x=3.6)
        self.ae(sel(), '1234')
        press(x=4), release(x=0.6)
        self.ae(sel(), '234')

        # Shift-drag starts a new selection when there is nothing to extend.
        init()
        press(modifiers=GLFW_MOD_SHIFT)
        move(x=3.6, q='1234')
        release(x=3.6)
        self.ae(sel(), '1234')
        # With a selection present, Shift-click still extends the nearest end.
        press(x=4.6, modifiers=GLFW_MOD_SHIFT)
        release(x=4.6)
        self.ae(sel(), '12345')

        # multi line movement
        init()
        press(x=2, y=2)
        move(x=2, y=1, q='890ab')
        move(x=2.6, y=1, q='90ab')
        move(y=1, q='67890ab')
        move(x=4, y=1, q='0ab')
        move(x=4.6, y=1, q='ab')
        move(q='1234567890ab')
        move(x=2, y=3, q='cdefg')
        move(y=3, q='cde')
        move(x=0.6, y=3, q='cdef')
        move(x=2.6, y=3, q='cdefgh')
        move(x=4.6, y=3, q='cdefghij')

        # Single cell select
        init()
        press(), release(1)
        self.ae(sel(), '1')
        press(3), release(2)
        self.ae(sel(), '3')

        # Multi-line click release
        init()
        press(1, 1), release(3.6, 2)
        self.ae(sel(), '7890abcd')
        press(1.6, 1), release(3, 2)
        self.ae(sel(), '890abc')
        press(3.6, 4), release(2, 2)
        self.ae(sel(), 'cdefghijklmn')
        press(3, 4), release(2.6, 2)
        self.ae(sel(), 'defghijklm')

        # Word select with drag
        s.reset()
        s.draw('ab cd')
        s.draw(' f gh')
        s.draw(' stuv')
        s.draw('X Y')
        multi_click(x=1.4)
        self.ae(sel(), 'ab')
        move(2.6)
        self.ae(sel(), 'ab ')
        move(3.6)
        self.ae(sel(), 'ab cd')
        move(2.6)
        self.ae(sel(), 'ab ')
        release(3.6, 1)
        self.ae(sel(), 'ab cd f gh')
        multi_click(x=1, y=2)
        self.ae(sel(), 'stuvX')
        release()
        multi_click(x=3.6)
        self.ae(sel(), 'cd')
        move(0.2)
        release()
        self.ae(sel(), 'ab cd')
        multi_click(x=4.4)
        self.ae(sel(), 'cd')
        move(x=4.4, y=1)
        self.ae(sel(), 'cd f gh')
        move(x=4.4, y=0)
        self.ae(sel(), 'cd')
        release()
        multi_click(x=4.4, y=1)
        self.ae(sel(), 'gh')
        move(x=4.4, y=0)
        self.ae(sel(), 'cd f gh')
        move(x=4.4, y=1)
        self.ae(sel(), 'gh')
        release()
        multi_click(x=4.4)
        self.ae(sel(), 'cd')
        move()
        self.ae(sel(), 'ab cd')
        move(x=1, y=1)
        self.ae(sel(), 'ab cd f')
        move()
        self.ae(sel(), 'ab cd')
        release()
        multi_click(x=1.4)
        self.ae(sel(), 'ab')
        move(x=4.4)
        self.ae(sel(), 'ab cd')
        move(x=4.4, y=1)
        self.ae(sel(), 'ab cd f gh')
        move(x=4.4)
        self.ae(sel(), 'ab cd')

        # Line select with drag
        s.reset()
        s.draw('1 2 3')
        s.linefeed(), s.carriage_return()
        s.draw('4 5 6')
        s.linefeed(), s.carriage_return()
        s.draw('7 8 9X')
        multi_click(x=1, count=3)
        self.ae(sel(), str(s.line(0)))
        move(y=1)
        self.ae(sel(), '1 2 3\n4 5 6')
        move(y=2)
        self.ae(sel(), '1 2 3\n4 5 6\n7 8 9X')
        move(y=1)
        self.ae(sel(), '1 2 3\n4 5 6')
        move()
        self.ae(sel(), str(s.line(0)))
        release()
        multi_click(y=1, count=3)
        self.ae(sel(), '4 5 6')
        move(y=0)
        self.ae(sel(), '1 2 3\n4 5 6')
        move(y=1)
        self.ae(sel(), '4 5 6')
        move(y=2)
        self.ae(sel(), '4 5 6\n7 8 9X')
        release()
        s.reset()
        s.draw(' 123')
        s.linefeed(), s.carriage_return()
        s.draw(' 456')
        s.linefeed(), s.carriage_return()
        multi_click(x=1, count=3)
        self.ae(sel(), '123')
        move(x=2, y=1)
        self.ae(sel(), '123\n 456')
        release()
        press(x=2, y=1, button=GLFW_MOUSE_BUTTON_RIGHT)
        release(x=2, y=1, button=GLFW_MOUSE_BUTTON_RIGHT)
        self.ae(sel(), '123\n 456')
        press(button=GLFW_MOUSE_BUTTON_RIGHT)
        self.ae(sel(), ' 123\n 456')
        release(button=GLFW_MOUSE_BUTTON_RIGHT)

        # line select for wrapped lines in scrollback
        s.reset()
        s.draw('ABCDE12345')
        s.linefeed(), s.carriage_return()
        s.draw(('X' * s.columns) * (s.lines - 1))
        multi_click(x=1, count=3)
        self.ae(sel(), 'ABCDE12345')
        s.reset()
        s.draw('ABCDE12345')
        s.linefeed(), s.carriage_return()
        s.draw('678')
        s.linefeed(), s.carriage_return()
        s.draw(('X' * s.columns) * (s.lines - 2))
        multi_click(x=1, y=1, count=3)
        self.ae(sel(), '678')
        press(x=2, button=GLFW_MOUSE_BUTTON_RIGHT)
        release(x=2, button=GLFW_MOUSE_BUTTON_RIGHT)
        self.ae(sel(), 'ABCDE12345\n678')

        # line select for wrapped lines below viewport
        s.reset()
        s.scroll(100, False)
        # draw enough content to enable scrolling: fill lines-1 visual lines, then draw a long wrapped line
        s.draw(('X' * s.columns) * (s.lines - 1))
        s.linefeed(), s.carriage_return()
        s.draw('ABCDE12345')  # wraps to 2 visual lines, second one pushed below viewport
        # scroll up by 1 so top visual line of the wrapped text is visible but bottom wraps off-screen
        s.scroll(1, True)
        multi_click(x=1, y=s.lines - 1, count=3)
        self.ae(sel(), 'ABCDE12345')
        # extending selection to a line that wraps below viewport
        s.reset()
        s.scroll(100, False)
        s.draw(('X' * s.columns) * (s.lines - 2))
        s.linefeed(), s.carriage_return()
        s.draw('678')
        s.linefeed(), s.carriage_return()
        s.draw('ABCDE12345')
        s.scroll(1, True)
        multi_click(x=1, y=3, count=3)
        self.ae(sel(), '678')
        press(x=2, y=s.lines - 1, button=GLFW_MOUSE_BUTTON_RIGHT)
        release(x=2, y=s.lines - 1, button=GLFW_MOUSE_BUTTON_RIGHT)
        self.ae(sel(), '678\nABCDE12345')
        s.scroll(100, False)

        # word select for wrapped words in scrollback
        s.reset()
        s.draw('abcde12345')
        s.linefeed(), s.carriage_return()
        s.draw(('X' * s.columns) * (s.lines - 1))
        multi_click(x=1)
        self.ae(sel(), 'abcde12345')

        # word select for wrapped words below viewport
        s.reset()
        s.scroll(100, False)
        s.draw(('X' * s.columns) * (s.lines - 1))
        s.linefeed(), s.carriage_return()
        s.draw('abcde12345')
        s.scroll(1, True)
        multi_click(x=1, y=s.lines - 1)
        self.ae(sel(), 'abcde12345')
        s.scroll(100, False)

        # Rectangle select
        init()
        press(x=1, y=1, modifiers=GLFW_MOD_ALT | GLFW_MOD_CONTROL)
        move(x=3.6, y=3)
        self.ae(sel(), '789bcdghi')
        release(x=3, y=3)
        self.ae(sel(), '78bcgh')
        press(x=3.6, y=1, modifiers=GLFW_MOD_ALT | GLFW_MOD_CONTROL)
        self.ae(sel(), '')
        move(x=1, y=3)
        self.ae(sel(), '789bcdghi')
        release(x=1.6)
        self.ae(sel(), '3489')

        # scrolling
        init()
        press(x=1.6)
        scroll(x=1)
        self.ae(sel(), 'LMNO12')
        scroll(x=1)
        self.ae(sel(), 'GHIJKLMNO12')
        scroll(x=1, up=False)
        self.ae(sel(), 'LMNO12')
        scroll(x=2.6, up=False)
        self.ae(sel(), '3')
        release()
        # fractional scrolling
        init()
        s.fractional_scroll(-0.5)
        press()
        move(x=3.6, q='1234')
        release(x=3.6)
        self.ae(sel(), '1234')

        # extending selections
        init()
        press()
        move(x=3.6, q='1234')
        release(x=3.6)
        self.ae(sel(), '1234')
        press(x=1, y=1, button=GLFW_MOUSE_BUTTON_RIGHT)
        self.ae(sel(), '123456')
        move(x=2, y=1, q='1234567')
        release(x=3, y=1, button=GLFW_MOUSE_BUTTON_RIGHT)
        self.ae(sel(), '12345678')
        init()
        press(y=2)
        move(x=3.6, y=2, q='abcd')
        press(x=3, y=0, button=GLFW_MOUSE_BUTTON_RIGHT)
        self.ae(sel(), '4567890abcd')

        # blank line select
        s.reset()
        s.draw('abcde')
        s.linefeed(), s.carriage_return()
        s.linefeed(), s.carriage_return()
        s.draw('12345')
        press(x=0, y=0)
        move(x=2, y=2, q='abcde\n\n12')

        # Line from begin select (alt+triple click): selects from column 0
        # (including any leading whitespace), unlike normal triple click which strips it
        s.reset()
        s.draw(' 123')
        s.linefeed(), s.carriage_return()
        s.draw(' 456')
        s.linefeed(), s.carriage_return()
        multi_click(x=1, count=3, modifiers=GLFW_MOD_ALT)
        self.ae(sel(), ' 123')
        multi_click(x=1, count=3)
        self.ae(sel(), '123')
        multi_click(x=1, count=3, modifiers=GLFW_MOD_ALT)
        move(y=1)
        self.ae(sel(), ' 123\n 456')
        release()

        # Line from point select (ctrl+alt+triple click): selects from click
        # position to end of line; cannot start before the first non-blank char
        s.reset()
        s.draw(' 123')
        s.linefeed(), s.carriage_return()
        s.draw(' 456')
        s.linefeed(), s.carriage_return()
        multi_click(x=2, count=3, modifiers=GLFW_MOD_ALT | GLFW_MOD_CONTROL)
        self.ae(sel(), '23')
        # click before first non-blank: selection starts at first non-blank
        multi_click(x=0, count=3, modifiers=GLFW_MOD_ALT | GLFW_MOD_CONTROL)
        self.ae(sel(), '123')
        # click past last non-blank char: no selection created
        multi_click(x=4, count=3, modifiers=GLFW_MOD_ALT | GLFW_MOD_CONTROL)
        self.ae(sel(), '')
        # drag to next line
        multi_click(x=2, count=3, modifiers=GLFW_MOD_ALT | GLFW_MOD_CONTROL)
        move(y=1)
        self.ae(sel(), '23\n 456')
        release()

        # Word and line from point: selects the word at the click point and
        # extends the selection to the end of the line
        sel_word_and_line = MOUSE_SELECTION_WORD_AND_LINE_FROM_POINT
        s.reset()
        s.draw('ab cd')
        press(x=0)
        mock_mouse_selection(w, GLFW_MOUSE_BUTTON_LEFT, sel_word_and_line)
        self.ae(sel(), 'ab cd')
        press(x=3)
        mock_mouse_selection(w, GLFW_MOUSE_BUTTON_LEFT, sel_word_and_line)
        self.ae(sel(), 'cd')
        # click on space: no word, selects from space to end of line
        press(x=2)
        mock_mouse_selection(w, GLFW_MOUSE_BUTTON_LEFT, sel_word_and_line)
        self.ae(sel(), ' cd')
        # click past last non-blank char: no selection created
        press(x=4)
        mock_mouse_selection(w, GLFW_MOUSE_BUTTON_LEFT, sel_word_and_line)
        self.ae(sel(), '')

        # Double-click on whitespace: no word found, no selection created
        s.reset()
        s.draw('ab cd')
        multi_click(x=2)
        self.ae(sel(), '')
        multi_click(x=2.4)
        self.ae(sel(), '')

    def test_scroll_protocol(self):
        from kitty.fast_data_types import send_scroll_report

        self.set_options()  # scroll_delta reads the scroll multipliers
        s = self.create_screen()
        c = s.callbacks

        def send(body):
            c.clear()
            parse_bytes(s, f'\x1b]777;scroll;{body}\x1b\\'.encode())

        def report(x_offset, y_offset, **kw):
            c.clear()
            return send_scroll_report(s, x_offset, y_offset, **kw)

        send('v=1:op=query:id=7')
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=cap:id=7:caps=1:mode=0\x1b\\')
        self.assertFalse(report(0, 3))
        send('v=1:op=mode:mode=1')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=mode:mode=1\x1b\\')
        send('v=1:op=query:id=8')
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=cap:id=8:caps=1:mode=1\x1b\\')
        # a trackpad: precise pixels, a finger gesture in progress, shift held
        self.assertTrue(report(0, -2.5, offset_type=2, phase=3, mods=1, x=33.4, y=12, t=408211, tu=73))
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=event:t=408211:tu=73:dx=0:dy=250:x=8550:y=3072:mods=1:src=1:phase=2\x1b\\')
        # the fingers land, lift without a distance, and are cancelled
        report(1.25, 0, offset_type=2, phase=1)
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=event:t=0:dx=-125:dy=0:x=0:y=0:mods=0:src=1:phase=1\x1b\\')
        self.assertTrue(report(0, 0, offset_type=2, phase=4))
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=event:t=0:dx=0:dy=0:x=0:y=0:mods=0:src=1:phase=3\x1b\\')
        report(0, 0, offset_type=2, phase=5)
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=event:t=0:dx=0:dy=0:x=0:y=0:mods=0:src=1:phase=7\x1b\\')
        # coasting takes precedence over the finger phase of the same event
        report(0, 4, offset_type=2, momentum=1, phase=4)
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=event:t=0:dx=0:dy=-400:x=0:y=0:mods=0:src=1:phase=4\x1b\\')
        # a wheel: one notch left converted to pixels, momentum ended
        report(120, 0, offset_type=1, momentum=4, t=408300, x=-0.5, y=1000.25)
        self.ae(c.wtcbuf, b'\x1b]777;scroll;v=1:op=event:t=408300:dx=-5000:dy=0:x=-128:y=256064:mods=0:src=0:phase=6\x1b\\')
        send('v=1:op=mode:mode=0')
        self.assertFalse(report(0, 1))
        send('v=1:op=mode:mode=1')
        s.reset()
        self.assertFalse(report(0, 1))

    def test_scroll_frames(self):
        # A frame from a program follows the rules every extension shares, see docs/extension-framing.rst
        s = self.create_screen()
        c = s.callbacks

        def mode():
            c.clear()
            parse_bytes(s, b'\x1b]777;scroll;v=1:op=mode\x1b\\')
            return c.wtcbuf

        def send(body):
            parse_bytes(s, f'\x1b]777;scroll;{body}\x1b\\'.encode())
            return mode()

        on = b'\x1b]777;scroll;v=1:op=mode:mode=1\x1b\\'
        off = b'\x1b]777;scroll;v=1:op=mode:mode=0\x1b\\'
        # A key the terminal does not know is ignored, so that fields can be added
        self.ae(send('v=1:op=mode:mode=1:later=x-y'), on)
        # A frame is rejected as a whole for a duplicate key, a value that is not a decimal integer in range, a field
        # that is not key=value, a missing v or op, or a version the terminal does not speak
        for body in (
            'v=1:op=mode:mode=0:mode=0', 'v=1:op=mode:mode=0:later=1:later=2', 'v=1:v=1:op=mode:mode=0', 'v=1:op=mode:mode=+0',
            'v=1:op=mode:mode=0x', 'v=1:op=mode:mode=', 'v=1:op=mode:mode=-1', 'v=1:op=mode:mode=99999999999', 'v=1:op=mode:mode=0:',
            'v=1::op=mode:mode=0', 'v=1:op=mode:mode=0:junk', 'v=1:op=mode:=0', 'op=mode:mode=0', 'v=1:mode=0', 'v=2:op=mode:mode=0',
        ):
            self.ae(send(body), on, body)
        # Unknown fields grow with the frame, including beyond the old buffer size.
        body = 'v=1:op=mode:mode=0:p='
        for size in (127, 128, 1024, 8192):
            self.ae(send('v=1:op=mode:mode=1'), on)
            self.ae(send(body + 'x' * (size - len(body))), off)
        # and a query with an id out of range is not answered
        c.clear()
        parse_bytes(s, b'\x1b]777;scroll;v=1:op=query:id=-1\x1b\\')
        parse_bytes(s, b'\x1b]777;scroll;v=1:op=query:id=2147483648\x1b\\')
        self.ae(c.wtcbuf, b'')

    def test_v120_scroll_direction_reversal(self):
        # A high resolution wheel delivers one physical detent as several
        # VALUE120 events, and the fractional remainder is carried between them.
        # Detents do not reliably total 120 units, so that carry is what lets an
        # undersized detent still scroll a line. But when the direction changes
        # the residual has the wrong sign, and the first detent of the new
        # direction gets spent cancelling it instead of scrolling.
        # The fragment sequences below were captured from a Logitech MX Master 3.
        from kitty.fast_data_types import test_scale_scroll

        self.set_options()  # scale_scroll reads wheel_scroll_multiplier
        cell = 40

        def run(*v120):
            # scale_scroll is exercised in mouse tracking mode, which reduces
            # wheel_scroll_multiplier to its sign, so the result does not depend
            # on the configured multiplier.
            return test_scale_scroll([float(v) for v in v120], cell)

        down = (-48, -32, -24, -24)  # 128 units
        up = (48, 32, 24, 24)

        # Five detents down, then one up. Every detent must scroll, including
        # the one that reverses direction. Before the residual was discarded on
        # a reversal, that last detent scrolled nothing.
        res = run(*(down * 5), *up)
        self.ae(-5, sum(res[:20]), f'detents before the reversal: {res}')
        self.ae(1, sum(res[20:]), f'detent that reversed direction: {res}')

        # Repeated reversals keep working.
        res = run(*(down * 2), *(up * 2), *(down * 2))
        self.ae([-2, 2, -2], [sum(res[i : i + 8]) for i in range(0, 24, 8)])

        # Scrolling in one direction is unaffected: the carry is retained, so a
        # run of undersized detents still scrolls about one line each.
        self.ae(-9, sum(run(*((-48, -24, -24, -16) * 10))))

        # A fragment too small to be a detent does not scroll.
        self.ae(0, sum(run(8)))

    def test_gesture_protocol(self):
        s = self.create_screen()
        c = s.callbacks

        def send(body):
            c.clear()
            parse_bytes(s, f'\x1b]777;gesture;{body}\x1b\\'.encode())

        def report(kind, phase, **kw):
            c.clear()
            return send_gesture_report(s, kind, phase, 256, 512, **kw)

        def ev(fields, t='0'):
            return f'\x1b]777;gesture;v=1:op=event:{fields}:t={t}:x=256:y=512:'.encode()

        def made():
            # The terminal cancels at the time it decides to, which is not known here
            return re.sub(rb':t=[0-9]+:', b':t=T:', c.wtcbuf)

        # caps is what the window system delivers, and the tests have none
        send('v=1:op=query:id=42')
        self.ae(c.wtcbuf, b'\x1b]777;gesture;v=1:op=cap:id=42:caps=0:mode=0\x1b\\')
        self.assertFalse(report(1, 1))
        send('v=1:op=mode:mode=1')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;gesture;v=1:op=mode:mode=1\x1b\\')
        send('v=1:op=query:id=43')
        self.ae(c.wtcbuf, b'\x1b]777;gesture;v=1:op=cap:id=43:caps=0:mode=1\x1b\\')
        self.assertTrue(report(1, 1, mods=3))
        self.ae(c.wtcbuf, ev('kind=1:phase=1') + b'mods=3:scale=0:total=1000\x1b\\')
        report(1, 2, delta=0.5)
        self.ae(c.wtcbuf, ev('kind=1:phase=2') + b'mods=0:scale=500:total=1500\x1b\\')
        report(1, 2, delta=-0.5)
        self.ae(c.wtcbuf, ev('kind=1:phase=2') + b'mods=0:scale=-500:total=750\x1b\\')
        report(2, 1)
        report(2, 2, delta=-12.5)
        self.ae(c.wtcbuf, ev('kind=2:phase=2') + b'mods=0:angle=-1250:total=-1250\x1b\\')
        # Turning the mode off cancels the gestures in progress, whose remaining reports are then dropped
        send('v=1:op=mode:mode=0')
        self.ae(made(), ev('kind=1:phase=4', 'T') + b'mods=0:scale=0:total=750\x1b\\' + ev('kind=2:phase=4', 'T') + b'mods=0:angle=0:total=-1250\x1b\\')
        send('v=1:op=mode:mode=1')
        self.assertFalse(report(1, 3))
        report(3, 1, dx=1)
        self.ae(c.wtcbuf, ev('kind=3:phase=1') + b'mods=0:dx=1:dy=0\x1b\\')
        report(3, 3, dx=1)
        self.ae(c.wtcbuf, ev('kind=3:phase=3') + b'mods=0:dx=1:dy=0\x1b\\')
        # The number of fingers is told when a gesture begins and is in every report of it
        report(3, 1, dy=-1, fingers=3)
        self.ae(c.wtcbuf, ev('kind=3:phase=1') + b'mods=0:dx=0:dy=-1:fingers=3\x1b\\')
        report(3, 3, dy=-1)
        self.ae(c.wtcbuf, ev('kind=3:phase=3') + b'mods=0:dx=0:dy=-1:fingers=3\x1b\\')
        report(1, 1, fingers=2)
        report(1, 2, delta=0.5)
        self.ae(c.wtcbuf, ev('kind=1:phase=2') + b'mods=0:scale=500:total=1500:fingers=2\x1b\\')
        report(1, 3)
        self.ae(c.wtcbuf, ev('kind=1:phase=3') + b'mods=0:scale=0:total=1500:fingers=2\x1b\\')
        report(1, 1)
        self.ae(c.wtcbuf, ev('kind=1:phase=1') + b'mods=0:scale=0:total=1000\x1b\\')
        # A hold has a mode bit of its own, so a program that asked for mode 1 never sees one
        self.assertFalse(report(5, 1, fingers=4))
        send('v=1:op=mode:mode=2')
        self.ae(made(), ev('kind=1:phase=4', 'T') + b'mods=0:scale=0:total=1000\x1b\\')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;gesture;v=1:op=mode:mode=2\x1b\\')
        self.assertFalse(report(1, 1))
        self.assertTrue(report(5, 1, fingers=4))
        self.ae(c.wtcbuf, ev('kind=5:phase=1') + b'mods=0:fingers=4\x1b\\')
        report(5, 4)
        self.ae(c.wtcbuf, ev('kind=5:phase=4') + b'mods=0:fingers=4\x1b\\')
        self.assertFalse(report(5, 3))
        send('v=1:op=mode:mode=3')
        send('v=1:op=query:id=44')
        self.ae(c.wtcbuf, b'\x1b]777;gesture;v=1:op=cap:id=44:caps=0:mode=3\x1b\\')
        self.assertTrue(report(5, 1))
        self.assertTrue(report(2, 1))
        report(5, 3)
        self.ae(c.wtcbuf, ev('kind=5:phase=3') + b'mods=0\x1b\\')
        # Bits the terminal does not know are dropped, and a read says what is left
        send('v=1:op=mode:mode=13')
        self.ae(made(), ev('kind=2:phase=4', 'T') + b'mods=0:angle=0:total=0\x1b\\')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;gesture;v=1:op=mode:mode=1\x1b\\')
        report(4, 1)
        self.ae(c.wtcbuf, ev('kind=4:phase=1') + b'mods=0\x1b\\')
        c.clear()
        s.reset()
        self.ae(made(), ev('kind=4:phase=4', 'T') + b'mods=0\x1b\\')
        self.assertFalse(report(4, 3))

        # Every report has the time the system gave the event, on the clock of every extension, with the microseconds
        # past the millisecond where the system stamps that finely
        send('v=1:op=mode:mode=1')
        report(1, 1, t=408211, tu=73)
        self.ae(c.wtcbuf, ev('kind=1:phase=1', '408211:tu=73') + b'mods=0:scale=0:total=1000\x1b\\')
        report(1, 2, delta=0.5, t=408215, tu=0)
        self.ae(c.wtcbuf, ev('kind=1:phase=2', '408215:tu=0') + b'mods=0:scale=500:total=1500\x1b\\')
        # A begin for a gesture still open cancels it at the time of the begin
        report(1, 1, t=408300)
        self.ae(c.wtcbuf, ev('kind=1:phase=4', '408300') + b'mods=0:scale=0:total=1500\x1b\\' + ev('kind=1:phase=1', '408300') + b'mods=0:scale=0:total=1000\x1b\\')
        # The terminal's own cancel is stamped when it is made, on the same clock
        c.clear()
        before = int(monotonic() * 1000)
        send('v=1:op=mode:mode=0')
        t = int(re.search(rb':t=([0-9]+):', c.wtcbuf).group(1))
        self.assertGreaterEqual(t, before - 1)
        self.assertLessEqual(t, int(monotonic() * 1000) + 1)

    def test_gesture_frames(self):
        # A frame from a program follows the rules every extension shares, see docs/extension-framing.rst
        s = self.create_screen()
        c = s.callbacks

        def mode():
            c.clear()
            parse_bytes(s, b'\x1b]777;gesture;v=1:op=mode\x1b\\')
            return c.wtcbuf

        def send(body):
            parse_bytes(s, f'\x1b]777;gesture;{body}\x1b\\'.encode())
            return mode()

        on = b'\x1b]777;gesture;v=1:op=mode:mode=1\x1b\\'
        off = b'\x1b]777;gesture;v=1:op=mode:mode=0\x1b\\'
        # A key the terminal does not know is ignored, so that fields can be added
        self.ae(send('v=1:op=mode:mode=1:later=x-y'), on)
        # A frame is rejected as a whole for a duplicate key, a value that is not a decimal integer in range, a field
        # that is not key=value, a missing v or op, or a version the terminal does not speak
        for body in (
            'v=1:op=mode:mode=0:mode=0', 'v=1:op=mode:mode=0:later=1:later=2', 'v=1:v=1:op=mode:mode=0', 'v=1:op=mode:mode=+0',
            'v=1:op=mode:mode=0x', 'v=1:op=mode:mode=', 'v=1:op=mode:mode=-1', 'v=1:op=mode:mode=99999999999', 'v=1:op=mode:mode=0:',
            'v=1::op=mode:mode=0', 'v=1:op=mode:mode=0:junk', 'v=1:op=mode:=0', 'op=mode:mode=0', 'v=1:mode=0', 'v=2:op=mode:mode=0',
            'v=01x:op=mode:mode=0',
        ):
            self.ae(send(body), on, body)
        # Unknown fields grow with the frame, including beyond the old buffer size.
        body = 'v=1:op=mode:mode=0:p='
        for size in (127, 128, 1024, 8192):
            self.ae(send('v=1:op=mode:mode=1'), on)
            self.ae(send(body + 'x' * (size - len(body))), off)
        # and a query with an id out of range is not answered
        c.clear()
        parse_bytes(s, b'\x1b]777;gesture;v=1:op=query:id=-1\x1b\\')
        parse_bytes(s, b'\x1b]777;gesture;v=1:op=query:id=2147483648\x1b\\')
        self.ae(c.wtcbuf, b'')

    def test_tablet_protocol(self):
        from kitty.constants import is_macos

        s = self.create_screen(options={'tablet_hardware_ids': True})
        c = s.callbacks
        caps = 101 if is_macos else 65

        def send(body):
            c.clear()
            parse_bytes(s, f'\x1b]777;tablet-pointer;{body}\x1b\\'.encode())

        def report(kind, phase, contact=0, **kw):
            c.clear()
            kw.setdefault('t', 1000)
            return send_pointer_report(s, kind, phase, 7, contact, 256, 512, **kw)

        def ev(fields):
            return f'\x1b]777;tablet-pointer;v=1:op=event:{fields}\x1b\\'.encode()

        send('v=1:op=query:id=42')
        self.ae(c.wtcbuf, f'\x1b]777;tablet-pointer;v=1:op=cap:id=42:caps={caps}:mode=0:w=50:h=100:cw=10:ch=20\x1b\\'.encode())
        send('v=2:op=query:id=42')
        self.ae(c.wtcbuf, b'')
        self.assertFalse(report(1, 1))
        send('v=1:op=mode:mode=1')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;tablet-pointer;v=1:op=mode:mode=1\x1b\\')
        send('v=1:op=query:id=43')
        self.ae(c.wtcbuf, f'\x1b]777;tablet-pointer;v=1:op=cap:id=43:caps={caps}:mode=1:w=50:h=100:cw=10:ch=20\x1b\\'.encode())
        self.assertTrue(report(1, 1, buttons=1, tool=2, valid=3, p=100, tx=-10, ty=20))
        self.ae(c.wtcbuf, ev('kind=1:phase=1:device=7:t=1000:contact=0:x=256:y=512:buttons=1:mods=0:tool=2:valid=3:p=100:tx=-10:ty=20'))
        # Fingers are not reported while only tool reports are on
        self.assertFalse(report(2, 1, 3, tool=0))
        # Turning the mode off cancels the stroke, whose remaining reports are then dropped
        send('v=1:op=mode:mode=0')
        self.ae(c.wtcbuf, ev('kind=1:phase=4:device=7:t=1000:contact=0:x=256:y=512:buttons=0:mods=0:tool=2:valid=3:p=0:tx=-10:ty=20'))
        send('v=1:op=mode:mode=1')
        self.assertFalse(report(1, 3))
        # Finger reports have their own mode bit
        send('v=1:op=mode:mode=2')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;tablet-pointer;v=1:op=mode:mode=2\x1b\\')
        self.assertFalse(report(1, 0))
        self.assertTrue(report(2, 1, 3, tool=0, buttons=1, mods=3))
        self.ae(c.wtcbuf, ev('kind=2:phase=1:device=7:t=1000:contact=3:x=256:y=512:buttons=1:mods=3:tool=0:valid=0'))
        report(2, 2, 3, tool=0, buttons=1, t=1016, valid=128, major=1280, minor=768, angle=4500)
        self.ae(c.wtcbuf, ev('kind=2:phase=2:device=7:t=1016:contact=3:x=256:y=512:buttons=1:mods=0:tool=0:valid=128:major=1280:minor=768:angle=4500'))
        report(2, 6, t=1016)
        self.ae(c.wtcbuf, ev('kind=2:phase=6:device=7:t=1016'))
        report(2, 1, 0, tool=0, buttons=1, t=2000, src=1, w=40960, h=25600)
        self.ae(c.wtcbuf, ev('kind=2:phase=1:device=7:t=2000:contact=0:x=256:y=512:buttons=1:mods=0:tool=0:valid=0:src=1:w=40960:h=25600'))
        # Changing the mode cancels every contact
        send('v=1:op=mode:mode=3')
        self.ae(
            c.wtcbuf,
            ev('kind=2:phase=4:device=7:t=1016:contact=3:x=256:y=512:buttons=0:mods=0:tool=0:valid=128:major=1280:minor=768:angle=4500')
            + ev('kind=2:phase=4:device=7:t=2000:contact=0:x=256:y=512:buttons=0:mods=0:tool=0:valid=0:src=1:w=40960:h=25600'),
        )
        self.assertTrue(report(1, 0, valid=1, p=5))
        self.ae(c.wtcbuf, ev('kind=1:phase=0:device=7:t=1000:contact=0:x=256:y=512:buttons=0:mods=0:tool=1:valid=1:p=5'))
        # A time finer than a millisecond is followed by its microseconds, zero included
        report(1, 0, t=1001, tu=0)
        self.ae(c.wtcbuf, ev('kind=1:phase=0:device=7:t=1001:tu=0:contact=0:x=256:y=512:buttons=0:mods=0:tool=1:valid=0'))
        # A frame closes finger reports, and there is none where there was nothing to close
        self.assertFalse(report(2, 6, t=1001, tu=999))
        self.assertTrue(report(2, 1, 5, tool=0, buttons=1, t=1001, tu=999))
        self.assertTrue(report(2, 6, t=1001, tu=999))
        self.ae(c.wtcbuf, ev('kind=2:phase=6:device=7:t=1001:tu=999'))
        self.assertFalse(report(2, 6, t=1001, tu=999))
        s.reset()
        self.assertFalse(report(1, 1))

        # A tool is described before its reports, and only to a program that takes tools
        def device(**kw):
            c.clear()
            return send_pointer_device(s, 7, **kw)

        def frame(fields):
            return f'\x1b]777;tablet-pointer;v=1:op=device:{fields}\x1b\\'.encode()

        self.assertFalse(device(tool=2))
        send('v=1:op=mode:mode=2')
        self.assertFalse(device(tool=2))
        send('v=1:op=mode:mode=1')
        self.assertTrue(device(tool=2, axes=3, name='Wacom Intuos Pro M', serial=99, hwid=2082, vid=1386, pid=855))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=2:axes=3:name=V2Fjb20gSW50dW9zIFBybyBN'))
        self.assertTrue(device())
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=0'))
        # The hardware identifiers are sent only when asked for
        send('v=1:op=mode:mode=9')
        self.assertTrue(device(tool=2, axes=3, name='Wacom Intuos Pro M', serial=99, hwid=2082, vid=1386, pid=855))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=2:axes=3:name=V2Fjb20gSW50dW9zIFBybyBN:serial=99:hwid=2082:vid=1386:pid=855'))
        self.assertTrue(device(serial=99))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=0:serial=99'))
        # A name is cut between characters, never inside one
        self.assertTrue(device(name='é' * 64))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=0:name=' + standard_b64encode(('é' * 63).encode()).decode()))
        # Bits of the mode this terminal does not know are dropped, so reading it back shows what took
        send('v=1:op=mode:mode=1023')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;tablet-pointer;v=1:op=mode:mode=95\x1b\\')
        send('v=1:op=mode:mode=0')

        # Tool actions are a kind of report with a mode bit of its own
        def action(*a, **kw):
            c.clear()
            return send_pointer_action(s, *a, **kw)

        self.assertFalse(action(1))
        send('v=1:op=mode:mode=3')
        self.assertFalse(action(1))
        send('v=1:op=mode:mode=16')
        self.assertFalse(report(1, 0))
        self.assertTrue(action(1, t=1000, x=256, y=512, mods=1))
        self.ae(c.wtcbuf, ev('kind=4:device=0:t=1000:action=1:x=256:y=512:mods=1'))
        self.assertTrue(action(2, device=7, state=1, t=1000, tu=5))
        self.ae(c.wtcbuf, ev('kind=4:device=7:t=1000:tu=5:action=2:state=1:x=0:y=0:mods=0'))
        self.assertTrue(action(2, device=7, state=0, t=1200))
        self.ae(c.wtcbuf, ev('kind=4:device=7:t=1200:action=2:state=0:x=0:y=0:mods=0'))
        # The force on a touchpad is carried by the fingers on it, with its stage
        send('v=1:op=mode:mode=2')
        report(2, 1, 0, tool=0, buttons=1, src=1, valid=1, p=30000, stage=1)
        self.ae(c.wtcbuf, ev('kind=2:phase=1:device=7:t=1000:contact=0:x=256:y=512:buttons=1:mods=0:tool=0:valid=1:p=30000:stage=1:src=1'))
        report(2, 2, 0, tool=0, buttons=1, src=1, valid=1, p=0, stage=0)
        self.ae(c.wtcbuf, ev('kind=2:phase=2:device=7:t=1000:contact=0:x=256:y=512:buttons=1:mods=0:tool=0:valid=1:p=0:stage=0:src=1'))
        send('v=1:op=mode:mode=0')
        # The contact table grows, so a touchscreen with many fingers down is not truncated
        send('v=1:op=mode:mode=2')
        for i in range(40):
            self.assertTrue(report(2, 1, i, tool=0, buttons=1))
            self.ae(c.wtcbuf, ev(f'kind=2:phase=1:device=7:t=1000:contact={i}:x=256:y=512:buttons=1:mods=0:tool=0:valid=0'))
        self.assertTrue(report(2, 2, 39, tool=0, buttons=1, t=1001))
        self.ae(c.wtcbuf, ev('kind=2:phase=2:device=7:t=1001:contact=39:x=256:y=512:buttons=1:mods=0:tool=0:valid=0'))
        send('v=1:op=mode:mode=0')
        self.ae(c.wtcbuf.count(b':phase=4:'), 40)
        self.assertIn(ev('kind=2:phase=4:device=7:t=1001:contact=39:x=256:y=512:buttons=0:mods=0:tool=0:valid=0'), c.wtcbuf)

    def test_tablet_framing_and_consent(self):
        # Frames from a program follow docs/extension-framing.rst, and the user can refuse the hardware identifiers
        s = self.create_screen()
        c = s.callbacks

        def send(body):
            c.clear()
            parse_bytes(s, f'\x1b]777;tablet-pointer;{body}\x1b\\'.encode())

        def mode():
            send('v=1:op=mode')
            return c.wtcbuf

        def is_mode(m):
            self.ae(mode(), f'\x1b]777;tablet-pointer;v=1:op=mode:mode={m}\x1b\\'.encode())

        # By default the identifiers are refused: the bit reads back as off and none is sent
        send('v=1:op=mode:mode=9')
        is_mode(1)
        c.clear()
        self.assertTrue(send_pointer_device(s, 7, tool=2, serial=99, hwid=2082, vid=1386, pid=855))
        self.ae(c.wtcbuf, b'\x1b]777;tablet-pointer;v=1:op=device:device=7:kind=1:tool=2:axes=0\x1b\\')
        # A frame with a duplicate key, a value out of range or one that is not a plain decimal number changes nothing
        for bad in (
            'v=1:op=mode:mode=2:mode=2', 'v=1:op=mode:op=mode:mode=2', 'v=1:op=mode:mode=+2', 'v=1:op=mode:mode=-2', 'v=1:op=mode:mode=2x',
            'v=1:op=mode:mode=', 'v=1:op=mode:mode=2147483648', 'v=+1:op=mode:mode=2', 'v=1:op=mode:mode=2:junk',
            'v=1:op=mode:mode=2:zz=1:zz=1', 'v=2:op=mode:mode=2',
        ):
            send(bad)
            self.ae(c.wtcbuf, b'', bad)
            is_mode(1)
        # A leading zero is still a decimal number, of version 1
        send('v=01:op=mode:mode=2')
        is_mode(2)
        # A key the terminal does not know is ignored
        send('v=1:op=mode:mode=1:later=7')
        is_mode(1)
        send('v=1:op=query:id=2147483647:later=7')
        self.assertIn(b':id=2147483647:', c.wtcbuf)
        send('v=1:op=query:id=2147483648')
        self.ae(c.wtcbuf, b'')
        # A frame with a field that is not key=value, an empty one among them, changes nothing
        for bad in ('v=1::op=mode:mode=2', 'v=1:op=mode:mode=2:', 'v=1:op=mode:mode=2:=x'):
            send(bad)
            is_mode(1)
        # Storage grows with the frame: the fields and the number of keys have no limit
        pad = 'v=1:op=mode:mode=2:x='
        for size in (127, 128, 1024, 8192):
            send('v=1:op=mode:mode=1')
            send(pad + 'a' * (size - len(pad)))
            is_mode(2)
        send('v=1:op=mode:mode=1')
        send('v=1:op=mode:mode=2:' + ':'.join(f'k{i}=1' for i in range(200)))
        is_mode(2)
        # A label names one control with an index from 0 to 255
        send('v=1:op=mode:mode=4')
        send('v=1:op=label:device=3:button=256:text=')
        send('v=1:op=label:device=3:button=1:button=2:text=')
        self.ae(c.wtcbuf, b'')

    def test_tablet_pads(self):
        s = self.create_screen(options={'tablet_hardware_ids': True})
        c = s.callbacks
        BUTTON, RING, STRIP, DIAL, GROUP = 1, 2, 3, 4, 5

        def send(body):
            c.clear()
            parse_bytes(s, f'\x1b]777;tablet-pointer;{body}\x1b\\'.encode())

        def pad(control, index, value=0, **kw):
            c.clear()
            kw.setdefault('t', 1000)
            return send_pointer_pad(s, 9, control, index, value, **kw)

        def describe(**kw):
            c.clear()
            return send_pointer_device(s, 9, kind=3, **kw)

        def ev(fields):
            return f'\x1b]777;tablet-pointer;v=1:op=event:kind=3:device=9:{fields}\x1b\\'.encode()

        def frame(fields):
            return f'\x1b]777;tablet-pointer;v=1:op=device:device=9:kind=3:{fields}\x1b\\'.encode()

        def b64(text):
            return standard_b64encode(text if isinstance(text, bytes) else text.encode()).decode()

        def label(body):
            take_pointer_pad_label()
            send(f'v=1:op=label:{body}')
            self.ae(c.wtcbuf, b'')
            return take_pointer_pad_label()

        # Pads have a mode bit of their own, and nothing of a pad goes to a program that did not set it
        send('v=1:op=mode:mode=27')
        self.assertFalse(pad(BUTTON, 2, 1))
        self.assertFalse(describe(buttons=9))
        self.assertIsNone(label(f'device=9:button=2:text={b64("Undo")}'))
        send('v=1:op=mode:mode=4')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;tablet-pointer;v=1:op=mode:mode=4\x1b\\')
        # Tools are not reported while only pads are on
        c.clear()
        self.assertFalse(send_pointer_report(s, 1, 0, 7, 0, 256, 512))

        # A pad is described before its reports, with the modes of each of its groups
        name = 'Wacom Intuos5 touch M Pad'
        self.assertTrue(describe(name=name, buttons=9, rings=1, num_modes=(4,), modes=(1,), vid=0x56A, pid=0x27))
        self.ae(c.wtcbuf, frame(f'buttons=9:rings=1:strips=0:dials=0:groups=1:modes=4:mode=1:name={b64(name)}'))
        # The mode the groups are in is left out when it is not known, and a pad need not have groups
        describe(buttons=4, strips=2, dials=1, num_modes=(3, 2))
        self.ae(c.wtcbuf, frame('buttons=4:rings=0:strips=2:dials=1:groups=2:modes=3,2'))
        describe(buttons=1)
        self.ae(c.wtcbuf, frame('buttons=1:rings=0:strips=0:dials=0:groups=0'))
        # The ids of the tablet are hardware identifiers
        send('v=1:op=mode:mode=12')
        describe(buttons=9, vid=0x56A, pid=0x27)
        self.ae(c.wtcbuf, frame('buttons=9:rings=0:strips=0:dials=0:groups=0:vid=1386:pid=39'))

        # Every form of report
        self.assertTrue(pad(BUTTON, 2, 1, mods=5))
        self.ae(c.wtcbuf, ev('t=1000:mods=5:button=2:state=1'))
        self.assertTrue(pad(BUTTON, 2, 0, t=1040, tu=250))
        self.ae(c.wtcbuf, ev('t=1040:tu=250:mods=0:button=2:state=0'))
        pad(RING, 0, 9000, src=True)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:ring=0:angle=9000:src=1'))
        pad(RING, 1, 0)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:ring=1:angle=0'))
        pad(RING, 0, stop=True, src=True)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:ring=0:stop=1:src=1'))
        pad(STRIP, 1, 65535, src=True)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:strip=1:pos=65535:src=1'))
        pad(STRIP, 1, stop=True)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:strip=1:stop=1'))
        pad(DIAL, 0, -120)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:dial=0:delta=-120'))
        pad(DIAL, 1, 15)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:dial=1:delta=15'))
        pad(GROUP, 0, 2)
        self.ae(c.wtcbuf, ev('t=1000:mods=0:group=0:mode=2'))
        self.assertRaises(ValueError, pad, 6, 0)

        # A release whose press the program did not see is not sent
        self.assertFalse(pad(BUTTON, 5, 0))
        # A button held twice is one button
        pad(BUTTON, 5, 1)
        pad(BUTTON, 5, 1, t=1001)
        self.assertTrue(pad(BUTTON, 5, 0))
        self.assertFalse(pad(BUTTON, 5, 0))

        # Changing the mode lets go of the held buttons, whose releases are then dropped
        pad(BUTTON, 1, 1, mods=2)
        pad(BUTTON, 3, 1, t=1010)
        send('v=1:op=mode:mode=5')
        self.ae(c.wtcbuf, ev('t=1000:mods=2:button=1:state=0') + ev('t=1010:mods=0:button=3:state=0'))
        self.assertFalse(pad(BUTTON, 1, 0))
        self.assertFalse(pad(BUTTON, 3, 0))
        # So does a reset
        pad(BUTTON, 1, 1)
        c.clear()
        s.reset()
        self.ae(c.wtcbuf, ev('t=1000:mods=0:button=1:state=0'))
        self.assertFalse(pad(BUTTON, 1, 1))
        # A resize cancels contacts but not the buttons of a pad, which are not at any position
        send('v=1:op=mode:mode=5')
        pad(BUTTON, 1, 1)
        send_pointer_report(s, 1, 1, 7, 0, 256, 512, buttons=1)
        c.clear()
        s.resize(s.lines, s.columns + 2)
        self.assertIn(b':kind=1:phase=4:', c.wtcbuf)
        self.assertNotIn(b':kind=3:', c.wtcbuf)
        self.assertTrue(pad(BUTTON, 1, 0))

        # Labels: what a control does at the moment, for the window system to show
        self.ae(label(f'device=9:button=2:text={b64("Undo")}'), (9, BUTTON, 2, 'Undo'))
        self.ae(label(f'text={b64("Größe")}:ring=0:device=9'), (9, RING, 0, 'Größe'))
        # A label is as long as the program makes it
        for size in (60, 61, 10000):
            self.ae(label(f'device=12:strip=1:text={b64("é" * size)}'), (12, STRIP, 1, 'é' * size))
        # No text, or an empty one, takes the label away
        self.ae(label('device=9:dial=255'), (9, DIAL, 255, ''))
        self.ae(label('device=9:dial=0:text='), (9, DIAL, 0, ''))
        for bad in (
            f'button=2:text={b64("Undo")}',  # no device
            f'device=9:text={b64("Undo")}',  # no control
            f'device=9:button=2:ring=0:text={b64("Undo")}',  # two controls
            f'device=9:button=256:text={b64("Undo")}',
            f'device=9:button=-1:text={b64("Undo")}',
            f'device=x:button=1:text={b64("Undo")}',
            f'device=9:group=0:text={b64("Undo")}',  # a group is not a control
            'device=9:button=2:text=VW5kbw',  # not padded
            'device=9:button=2:text=!!!!',
            f'device=9:button=2:text={b64("a" + chr(10) + "b")}',  # a control character
            f'device=9:button=2:text={b64(bytes((0xC3, 0x28)))}',  # not UTF-8
            f'device=9:button=2:text={b64(bytes((0xE2, 0x82)))}',  # cut short
        ):
            self.assertIsNone(label(bad), bad)
        send(f'v=2:op=label:device=9:button=2:text={b64("Undo")}')
        self.assertIsNone(take_pointer_pad_label())

    def test_tablet_accidental_contacts(self):
        # A finger the system takes for an accident, a palm or a thumb at rest, is for a program that asked for those
        s = self.create_screen()
        c = s.callbacks

        def send(body):
            c.clear()
            parse_bytes(s, f'\x1b]777;tablet-pointer;{body}\x1b\\'.encode())

        def report(phase, contact=0, **kw):
            c.clear()
            kw.setdefault('t', 1000)
            if phase != 6:
                kw.setdefault('tool', 0)
                kw.setdefault('buttons', 1)
            return send_pointer_report(s, 2, phase, 7, contact, 256, 512, **kw)

        def ev(phase, contact=0, conf=None, buttons=None, t=1000, extra=''):
            if phase == 6:
                return f'\x1b]777;tablet-pointer;v=1:op=event:kind=2:phase=6:device=7:t={t}\x1b\\'.encode()
            buttons = int(phase in (1, 2)) if buttons is None else buttons
            conf = '' if conf is None else f':conf={conf}'
            return (f'\x1b]777;tablet-pointer;v=1:op=event:kind=2:phase={phase}:device=7:t={t}:contact={contact}'
                    f':x=256:y=512:buttons={buttons}:mods=0:tool=0:valid=0{extra}{conf}\x1b\\').encode()

        def nothing(phase, contact=0, **kw):
            self.assertFalse(report(phase, contact, **kw))
            self.ae(c.wtcbuf, b'')

        def frame():
            ans = report(6)
            self.ae(c.wtcbuf, ev(6) if ans else b'')
            return ans

        # Without the mode bit a program is shown only the contacts that are meant
        send('v=1:op=mode:mode=2')
        # One that lands as an accident is not there, and neither is the frame of a group nothing was reported of
        nothing(1, conf=0)
        nothing(2, conf=0)
        self.assertFalse(frame())
        # Once the system takes it to be meant it begins, with a down
        self.assertTrue(report(2, conf=1, t=1001))
        self.ae(c.wtcbuf, ev(1, conf=1, t=1001))
        self.assertTrue(frame())
        self.assertFalse(frame())
        self.assertTrue(report(2, conf=1))
        self.ae(c.wtcbuf, ev(2, conf=1))
        # One that is being reported and turns into an accident is cancelled, as by any other cancel
        self.assertTrue(report(2, conf=0, valid=1, p=77, t=1002))
        self.ae(c.wtcbuf, ev(4, conf=0, t=1002, extra=':p=0').replace(b'valid=0', b'valid=1'))
        self.assertTrue(frame())
        nothing(2, conf=0)
        self.assertFalse(frame())
        # and begins anew if it is taken to be meant again before it lifts
        self.assertTrue(report(2, conf=1))
        self.ae(c.wtcbuf, ev(1, conf=1))
        self.assertTrue(report(3, conf=1))
        self.ae(c.wtcbuf, ev(3, conf=1))
        self.assertTrue(frame())
        # One that lifts while it is hidden reports nothing, whatever it is taken for as it lifts
        nothing(1, conf=0)
        nothing(3, conf=0)
        nothing(1, conf=0)
        nothing(3, conf=1)
        nothing(1, conf=0)
        nothing(4, conf=0)
        self.assertFalse(frame())
        # nor is it there to be lifted a second time
        nothing(3, conf=1)
        # One that lifts as an accident did not tap: it is cancelled
        self.assertTrue(report(1, conf=1))
        self.ae(c.wtcbuf, ev(1, conf=1))
        self.assertTrue(report(3, conf=0))
        self.ae(c.wtcbuf, ev(4, conf=0))
        self.assertTrue(frame())
        # A system that only marks the accidents says nothing of any other contact, which then has no conf. Such a
        # contact is shown, and one that stops being an accident begins with a down as one that is found meant does
        nothing(1, conf=0)
        self.assertFalse(frame())
        self.assertTrue(report(2, t=1003))
        self.ae(c.wtcbuf, ev(1, t=1003))
        self.assertTrue(report(2))
        self.ae(c.wtcbuf, ev(2))
        self.assertTrue(report(2, conf=0))
        self.ae(c.wtcbuf, ev(4, conf=0))
        nothing(2, conf=0)
        self.assertTrue(report(2))
        self.ae(c.wtcbuf, ev(1))
        self.assertTrue(report(3))
        self.ae(c.wtcbuf, ev(3))
        self.assertTrue(frame())
        # A frame closes a group in which one finger was reported and another was not
        nothing(1, 1, conf=0)
        self.assertTrue(report(1, 2, conf=1))
        self.assertTrue(frame())
        nothing(2, 1, conf=0)
        self.assertFalse(frame())
        self.assertTrue(report(2, 2, conf=1))
        nothing(2, 1, conf=0)
        self.assertTrue(frame())
        # A fresh down replaces a contact, hidden or not
        self.assertTrue(report(1, 1, conf=1))
        self.ae(c.wtcbuf, ev(1, 1, conf=1))
        self.assertTrue(report(2, 1, conf=1))
        self.ae(c.wtcbuf, ev(2, 1, conf=1))
        self.assertTrue(report(1, 2, conf=0))  # the stroke the program has is gone, and it is told so
        self.ae(c.wtcbuf, ev(4, 2, conf=0))
        self.assertTrue(report(2, 2, conf=1))
        self.ae(c.wtcbuf, ev(1, 2, conf=1))
        # A resize cancels the contacts the program knows of and forgets the hidden ones
        nothing(1, 3, conf=0)
        self.assertTrue(frame())
        c.clear()
        s.resize(s.lines, s.columns + 2)
        self.ae(c.wtcbuf, ev(4, 1, conf=1) + ev(4, 2, conf=1))
        # What is left of a cancelled contact stays dropped whatever it is taken for, and a hidden one does not come back
        for contact in (1, 2, 3):
            nothing(2, contact, conf=1)
            nothing(2, contact, conf=0)
            nothing(2, contact, conf=1)
            nothing(3, contact, conf=1)
        self.assertFalse(frame())
        # A change of mode and a reset say nothing of a hidden contact, and forget it
        nothing(1, conf=0)
        send('v=1:op=mode:mode=2')
        self.ae(c.wtcbuf, b'')
        nothing(2, conf=1)
        nothing(3, conf=1)
        nothing(1, conf=0)
        s.reset()
        self.ae(c.wtcbuf, b'')
        send('v=1:op=mode:mode=2')
        nothing(2, conf=1)
        # A system that does not judge reports every finger, as before
        self.assertTrue(report(1))
        self.ae(c.wtcbuf, ev(1))
        self.assertTrue(report(3))
        self.ae(c.wtcbuf, ev(3))
        self.assertTrue(frame())
        # Only a finger is ever hidden
        send('v=1:op=mode:mode=3')
        c.clear()
        self.assertTrue(send_pointer_report(s, 1, 1, 7, 0, 256, 512, t=1000, buttons=1, conf=0))
        self.assertIn(b':phase=1:', c.wtcbuf)

        # With the mode bit every contact is reported as it happens, and none is cancelled for being an accident
        send('v=1:op=mode:mode=66')
        send('v=1:op=mode')
        self.ae(c.wtcbuf, b'\x1b]777;tablet-pointer;v=1:op=mode:mode=66\x1b\\')
        self.assertTrue(report(1, conf=0))
        self.ae(c.wtcbuf, ev(1, conf=0))
        self.assertTrue(frame())
        self.assertTrue(report(2, conf=0))
        self.ae(c.wtcbuf, ev(2, conf=0))
        self.assertTrue(report(2, conf=1))
        self.ae(c.wtcbuf, ev(2, conf=1))
        self.assertTrue(report(2, conf=0))
        self.ae(c.wtcbuf, ev(2, conf=0))
        self.assertTrue(report(3, conf=0))
        self.ae(c.wtcbuf, ev(3, conf=0))
        self.assertTrue(frame())
        # Taking the bit away cancels what is down, as every change of mode does, and the rest of it is dropped
        self.assertTrue(report(1, conf=0))
        send('v=1:op=mode:mode=2')
        self.ae(c.wtcbuf, ev(4, conf=0))
        nothing(2, conf=0)
        nothing(2, conf=1)

    def test_tablet_hardware_facts(self):
        # What hardware reports and the protocol had no place for at first: every one of these is left out when it is not known
        s = self.create_screen(options={'tablet_hardware_ids': True})
        c = s.callbacks

        def send(body):
            c.clear()
            parse_bytes(s, f'\x1b]777;tablet-pointer;{body}\x1b\\'.encode())

        def report(kind, phase, contact=0, **kw):
            c.clear()
            kw.setdefault('t', 1000)
            return send_pointer_report(s, kind, phase, 7, contact, 256, 512, **kw)

        def ev(fields):
            return f'\x1b]777;tablet-pointer;v=1:op=event:{fields}\x1b\\'.encode()

        def device(dev=7, **kw):
            c.clear()
            return send_pointer_device(s, dev, **kw)

        def frame(fields):
            return f'\x1b]777;tablet-pointer;v=1:op=device:{fields}\x1b\\'.encode()

        def action(*a, **kw):
            c.clear()
            kw.setdefault('t', 1000)
            return send_pointer_action(s, *a, **kw)

        send('v=1:op=mode:mode=87')
        # Whether a finger is meant: said only when the system judges, and it can change while the finger is down. A program
        # that asked for the contacts that are not meant is told of them as of any other
        self.assertTrue(report(2, 1, 3, tool=0, buttons=1))
        self.ae(c.wtcbuf, ev('kind=2:phase=1:device=7:t=1000:contact=3:x=256:y=512:buttons=1:mods=0:tool=0:valid=0'))
        self.assertTrue(report(2, 2, 3, tool=0, buttons=1, conf=1))
        self.ae(c.wtcbuf, ev('kind=2:phase=2:device=7:t=1000:contact=3:x=256:y=512:buttons=1:mods=0:tool=0:valid=0:conf=1'))
        self.assertTrue(report(2, 2, 3, tool=0, buttons=1, conf=0, src=1, w=100, h=50))
        self.ae(c.wtcbuf, ev('kind=2:phase=2:device=7:t=1000:contact=3:x=256:y=512:buttons=1:mods=0:tool=0:valid=0:conf=0:src=1:w=100:h=50'))
        # A contact that is not meant is still one until it lifts or is cancelled, and a cancel says what was last said of it
        send('v=1:op=mode:mode=21')
        self.ae(c.wtcbuf, ev('kind=2:phase=4:device=7:t=1000:contact=3:x=256:y=512:buttons=0:mods=0:tool=0:valid=0:conf=0:src=1:w=100:h=50'))

        # Where a tool is on its tablet and how hard its barrel is held, each with a bit of valid
        self.assertTrue(report(1, 1, buttons=1, valid=1 | 256, p=100, rx=65535, ry=0, grip=9))
        self.ae(c.wtcbuf, ev('kind=1:phase=1:device=7:t=1000:contact=0:x=256:y=512:buttons=1:mods=0:tool=1:valid=257:p=100:rx=65535:ry=0'))
        self.assertTrue(report(1, 2, buttons=1, valid=1 | 512, p=100, rx=1, ry=2, grip=65535))
        self.ae(c.wtcbuf, ev('kind=1:phase=2:device=7:t=1000:contact=0:x=256:y=512:buttons=1:mods=0:tool=1:valid=513:p=100:grip=65535'))
        self.assertTrue(report(1, 3, valid=64 | 256 | 512, wheel=-1500, wclicks=-1, rx=32768, ry=40000, grip=0))
        self.ae(c.wtcbuf, ev(
            'kind=1:phase=3:device=7:t=1000:contact=0:x=256:y=512:buttons=0:mods=0:tool=1:valid=832:wheel=-1500:wclicks=-1:rx=32768:ry=40000:grip=0'))
        # The longest report there is fits
        self.assertTrue(report(
            1, 1, 2147483647, tool=7, buttons=4294967295, mods=15, valid=1023, p=65535, tx=-9000, ty=-9000, twist=35999, tangent=-65535, z=-1000000,
            dist=65535, wheel=-2147483647, wclicks=-2147483647, major=2147483647, minor=2147483647, angle=-2147483647, rx=65535, ry=65535, grip=65535,
            stage=2, conf=1, src=1, w=2147483647, h=2147483647, t=18446744073709551615, tu=999))
        self.assertTrue(c.wtcbuf.endswith(b':rx=65535:ry=65535:grip=65535:stage=2:conf=1:src=1:w=2147483647:h=2147483647\x1b\\'), c.wtcbuf)
        send('v=1:op=mode:mode=21')

        # The tablet a tool is on, its surface, and what is stored in the tool
        self.assertTrue(device(axes=1 | 256, tablet=2, w=57344, h=37888, direct=1))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=257:w=57344:h=37888:direct=1:tablet=2'))
        self.assertTrue(device(tablet=1, w=57344, direct=0))  # a size is both lengths or neither
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=0:direct=0:tablet=1'))
        self.assertTrue(device(bat=0, pcolor=0, pwidth=0, pstyle=3))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=0:bat=0:pcolor=000000:pwidth=0:pstyle=3'))
        self.assertTrue(device(bat=250, pcolor=0xff8000, pwidth=128, name='USI', serial=5))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=0:bat=100:pcolor=ff8000:pwidth=128:name=VVNJ'))
        # The tablet is not a hardware identifier, and comes before those
        send('v=1:op=mode:mode=29')
        self.assertTrue(device(tablet=3, serial=5, vid=1386, pid=855))
        self.ae(c.wtcbuf, frame('device=7:kind=1:tool=1:axes=0:tablet=3:serial=5:vid=1386:pid=855'))
        send('v=1:op=mode:mode=21')

        # A pad says which tablet it belongs to and which group each of its controls is in
        self.assertTrue(device(9, kind=3, buttons=6, rings=1, strips=1, num_modes=(3, 2), modes=(0, 1), tablet=2,
                               bgroups=(0, 0, 0, None, 1, 1), rgroups=(0,), sgroups=(1,)))
        self.ae(c.wtcbuf, frame('device=9:kind=3:buttons=6:rings=1:strips=1:dials=0:groups=2:modes=3,2:mode=0,1:bgroups=0,0,0,-,1,1:rgroups=0:sgroups=1:tablet=2'))
        # A kind of control the pad has none of has no list, nor has one whose groups are not known
        self.assertTrue(device(9, kind=3, buttons=2, dials=2, num_modes=(1,), dgroups=(0, None)))
        self.ae(c.wtcbuf, frame('device=9:kind=3:buttons=2:rings=0:strips=0:dials=2:groups=1:modes=1:dgroups=0,-'))
        # A control in a group the pad does not have is in none
        self.assertTrue(device(9, kind=3, buttons=3, num_modes=(1,), bgroups=(0, 1, -5)))
        self.ae(c.wtcbuf, frame('device=9:kind=3:buttons=3:rings=0:strips=0:dials=0:groups=1:modes=1:bgroups=0,-,-'))
        self.assertRaises(ValueError, device, 9, kind=3, buttons=3, num_modes=(1,), bgroups=(0, 0))
        # The longest device frame there is fits
        many = tuple(15 for i in range(64))
        self.assertTrue(device(18446744073709551615, kind=3, buttons=64, rings=64, strips=64, dials=64, num_modes=(4294967295,) * 16, modes=(4294967295,) * 16,
                               bgroups=many, rgroups=many, sgroups=many, dgroups=many, tablet=18446744073709551615, name='é' * 64,
                               serial=18446744073709551615, hwid=18446744073709551615, vid=4294967295, pid=4294967295))
        self.assertIn(b':dgroups=' + b','.join(b'15' for i in range(64)) + b':tablet=18446744073709551615:name=', c.wtcbuf)
        self.assertTrue(c.wtcbuf.endswith(b'\x1b\\'))
        self.assertLess(len(c.wtcbuf), 2048)

        # A tool put into its holder and taken out of it does not last, and has no position worth more than the pointer's
        self.assertTrue(action(6, 7))
        self.ae(c.wtcbuf, ev('kind=4:device=7:t=1000:action=6:x=0:y=0:mods=0'))
        self.assertTrue(action(7, 7))
        self.ae(c.wtcbuf, ev('kind=4:device=7:t=1000:action=7:x=0:y=0:mods=0'))
        # A tool moved through the air with its button held: how far is said only between the beginning and the end
        self.assertTrue(action(8, 7, state=1, x=256, y=512, dx=5, dy=5))
        self.ae(c.wtcbuf, ev('kind=4:device=7:t=1000:action=8:state=1:x=256:y=512:mods=0'))
        self.assertTrue(action(8, 7, state=2, x=256, y=512, dx=-120, dy=40000, t=1008, tu=250))
        self.ae(c.wtcbuf, ev('kind=4:device=7:t=1008:tu=250:action=8:state=2:dx=-120:dy=32767:x=256:y=512:mods=0'))
        self.assertTrue(action(8, 7, state=0, x=256, y=512, t=1016))
        self.ae(c.wtcbuf, ev('kind=4:device=7:t=1016:action=8:state=0:x=256:y=512:mods=0'))
    def test_mapped_scroll_steps(self):
        from kitty.fast_data_types import test_mapped_scroll_steps as steps

        LINES, V120, HIGHRES = 0, 1, 2  # GLFWOffsetType
        self.set_options()
        # Classic wheel: one step per click, whatever wheel_scroll_multiplier is
        self.ae(steps(LINES, [1.0, 1.0, -1.0]), [1, 1, -1])
        # High resolution wheel: one step per 120 units, however they are split
        self.ae(sum(steps(V120, [15.0] * 16)), 2)
        # Touchpad: one step per 100 logical pixels, independent of the font size
        self.ae(steps(HIGHRES, [30.0] * 10), [0, 0, 0, 1, 0, 0, 1, 0, 0, 1])
        # Reversing direction starts a fresh step instead of first cancelling the remainder
        self.ae(steps(HIGHRES, [150.0, -60.0, -40.0]), [1, 0, -1])
        # touch_scroll_multiplier scales touchpad steps and its sign flips them
        self.set_options({'touch_scroll_multiplier': -2.0})
        self.ae(steps(HIGHRES, [30.0, 30.0]), [0, -1])
        # Wheels only take the sign of wheel_scroll_multiplier and every click is a step
        self.set_options({'wheel_scroll_multiplier': -3.0})
        self.ae(steps(LINES, [1.0, 0.1]), [-1, -1])
        self.ae(steps(V120, [120.0, 240.0]), [-1, -2])

    def test_mapped_scroll(self):
        from kitty.config import load_config

        LINES, HIGHRES = 0, 2  # GLFWOffsetType
        MOMENTUM_BEGAN, MOMENTUM_ACTIVE, MOMENTUM_ENDED = 1, 3, 4  # GLFWMomentumType
        s = self.create_screen(lines=5, scrollback=50)
        errors = []
        opts = load_config(
            overrides=(
                'pixel_scroll no',
                'wheel_scroll_multiplier 1',
                'mouse_map ctrl+wheel_up press ungrabbed change_font_size all +1',
                'mouse_map ctrl+wheel_down press ungrabbed change_font_size all -1',
                'mouse_map alt+wheel_up press ungrabbed mouse_selection normal',
                'mouse_map ctrl+wheel_left press ungrabbed previous_tab',
                'mouse_map shift+wheel_left press ungrabbed previous_tab',
                'mouse_map shift+wheel_right press ungrabbed next_tab',
            ),
            accumulate_bad_lines=errors,
        )
        self.ae(errors, [])
        set_options(opts)
        w = create_mock_window(s)
        for i in range(50):
            s.draw(f'{i}')
            s.carriage_return()
            s.linefeed()
        calls = []
        s.callbacks.change_font_size = lambda *a: calls.append(a[1])
        s.callbacks.mouse_selection = lambda code: mock_mouse_selection(w, s.callbacks.current_mouse_button, code)
        s.callbacks.previous_tab = lambda: calls.append('left')
        s.callbacks.next_tab = lambda: calls.append('right')

        def scroll(y, mods=0, offset_type=LINES, momentum=0, x=0.0):
            send_mock_scroll_event_to_window(w, offset_type, x, float(y), mods, momentum)

        def ac(expected_calls, scrolled_by):
            self.ae(calls, expected_calls)
            self.ae(s.scrolled_by, scrolled_by)
            del calls[:]

        # Unmapped scrolling still scrolls the screen
        scroll(2)
        ac([], 2)
        # Mapped scrolling fires the action once per step instead
        scroll(1, GLFW_MOD_CONTROL)
        ac(['+'], 2)
        scroll(3, GLFW_MOD_CONTROL)
        ac(['+'] * 3, 2)
        scroll(-1, GLFW_MOD_CONTROL)
        ac(['-'], 2)
        # Touchpad events that do not complete a step are consumed
        scroll(60, GLFW_MOD_CONTROL, HIGHRES)
        ac([], 2)
        scroll(40, GLFW_MOD_CONTROL, HIGHRES)
        ac(['+'], 2)
        # A partial step does not survive unmapped scrolling in between
        scroll(60, GLFW_MOD_CONTROL, HIGHRES)
        scroll(1)
        ac([], 3)
        scroll(40, GLFW_MOD_CONTROL, HIGHRES)
        ac([], 3)
        # Momentum after a mapped gesture neither fires the action nor scrolls
        scroll(100, GLFW_MOD_CONTROL, HIGHRES)
        scroll(200, GLFW_MOD_CONTROL, HIGHRES, MOMENTUM_BEGAN)
        scroll(200, GLFW_MOD_CONTROL, HIGHRES, MOMENTUM_ACTIVE)
        ac(['+'], 3)
        # A horizontal unmapped gesture ends the mapped gesture, so its momentum scrolls
        scroll(0, x=10, offset_type=HIGHRES)
        scroll(-40, 0, HIGHRES, MOMENTUM_BEGAN)
        ac([], 1)
        # Modifiers or modes without a mapping scroll normally
        scroll(1, GLFW_MOD_ALT | GLFW_MOD_CONTROL)
        ac([], 2)
        parse_bytes(s, b'\x1b[?1000h')  # grab the mouse
        scroll(1, GLFW_MOD_CONTROL)
        ac([], 2)
        parse_bytes(s, b'\x1b[?1000l')
        # The wheel cannot start a selection, it never releases
        scroll(1, GLFW_MOD_ALT)
        self.assertFalse(s.has_selection())
        ac([], 2)
        # Horizontal scrolling, positive offsets are leftwards
        scroll(0, GLFW_MOD_SHIFT, x=1)
        ac(['left'], 2)
        scroll(0, GLFW_MOD_SHIFT, x=-2)
        ac(['right'] * 2, 2)
        scroll(0, GLFW_MOD_SHIFT, HIGHRES, x=60)
        ac([], 2)
        scroll(0, GLFW_MOD_SHIFT, HIGHRES, x=40)
        ac(['left'], 2)
        # A diagonal event fires the mappings of both axes
        scroll(1, GLFW_MOD_CONTROL, x=1)
        ac(['+', 'left'], 2)
        # Horizontal jitter, without horizontal mappings, continues a mapped
        # vertical gesture, so its momentum is still swallowed
        scroll(100, GLFW_MOD_ALT, HIGHRES)
        scroll(0, GLFW_MOD_ALT, HIGHRES, x=10)
        scroll(-200, GLFW_MOD_ALT, HIGHRES, MOMENTUM_BEGAN)
        ac([], 2)
        # But horizontal scrolling in an unmapped direction, with horizontal
        # mappings, ends the gesture, so its momentum scrolls
        scroll(100, GLFW_MOD_CONTROL, HIGHRES)
        scroll(0, GLFW_MOD_CONTROL, HIGHRES, x=-10)
        scroll(-20, GLFW_MOD_CONTROL, HIGHRES, MOMENTUM_BEGAN)
        ac(['+'], 1)
        # Spent momentum ends the gesture, so a later unmapped event in the same
        # window with the same modifiers scrolls instead of being swallowed.
        # shift is mapped horizontally only, so vertical scrolling is unmapped.
        scroll(0, GLFW_MOD_SHIFT, HIGHRES, x=100)
        scroll(0, GLFW_MOD_SHIFT, HIGHRES, MOMENTUM_BEGAN, x=-200)
        scroll(0, GLFW_MOD_SHIFT, HIGHRES, MOMENTUM_ENDED, x=-100)
        ac(['left'], 1)
        scroll(1, GLFW_MOD_SHIFT)
        ac([], 2)

        # A classic wheel has no end-of-gesture event, so a gesture is also
        # bounded by the time between its events and by the mouse mode, which
        # changes the set of mapped wheel buttons. Without that, an unmapped
        # direction would stay swallowed indefinitely. Reload the mappings so
        # that ctrl is mapped horizontally only, as in the mouse_map docs.
        opts = load_config(
            overrides=(
                'pixel_scroll no',
                'wheel_scroll_multiplier 1',
                'mouse_map ctrl+wheel_left press ungrabbed,grabbed previous_tab',
                'mouse_map ctrl+wheel_right press ungrabbed,grabbed next_tab',
                'mouse_map alt+wheel_up press ungrabbed change_font_size all +1',
            ),
            accumulate_bad_lines=errors,
        )
        self.ae(errors, [])
        set_options(opts)
        # Vertical is unmapped for ctrl now, so it scrolls
        scroll(1, GLFW_MOD_CONTROL)
        ac([], 3)
        # A mapped horizontal event starts a gesture, during which off-axis
        # jitter is still swallowed
        scroll(0, GLFW_MOD_CONTROL, x=1)
        ac(['left'], 3)
        scroll(1, GLFW_MOD_CONTROL)
        ac([], 3)
        # But once the gesture times out, vertical scrolling works again
        sleep(0.3)
        scroll(1, GLFW_MOD_CONTROL)
        ac([], 4)
        # Changing the mouse mode ends the gesture too. alt+wheel_up is mapped
        # only when ungrabbed, so once grabbed the wheel goes to the program
        # instead of being swallowed as a continuation of the mapped gesture.
        scroll(1, GLFW_MOD_ALT)
        ac(['+'], 4)
        parse_bytes(s, b'\x1b[?1000h')
        s.callbacks.wtcbuf = b''
        scroll(1, GLFW_MOD_ALT)
        ac([], 4)
        self.assertTrue(s.callbacks.wtcbuf, 'the wheel event was not reported to the program')
        parse_bytes(s, b'\x1b[?1000l')

    def test_mapped_scroll_with_scroll_reports(self):
        from kitty.config import load_config

        HIGHRES = 2  # GLFWOffsetType
        BEGAN, ACTIVE, ENDED = 1, 3, 4  # GLFWMomentumType, also the finger phase
        s = self.create_screen(lines=5, scrollback=50)
        errors = []
        opts = load_config(
            overrides=(
                'pixel_scroll no',
                'mouse_map ctrl+wheel_up press grabbed change_font_size all +1',
                'mouse_map alt+wheel_up press ungrabbed change_font_size all -1',
            ),
            accumulate_bad_lines=errors,
        )
        self.ae(errors, [])
        set_options(opts)
        w = create_mock_window(s)
        calls = []
        s.callbacks.change_font_size = lambda *a: calls.append(a[1])
        parse_bytes(s, b'\x1b]777;scroll;v=1:op=mode:mode=1\x1b\\')

        def scroll(y, mods=0, phase=0, momentum=0):
            s.callbacks.clear()
            send_mock_scroll_event_to_window(w, HIGHRES, 0.0, float(y), mods, momentum, phase)
            return re.findall(rb':phase=(\d)', s.callbacks.wtcbuf)

        # Scroll reports grab the wheel, so ungrabbed mappings do not apply
        self.ae(scroll(100, GLFW_MOD_ALT), [b'0'])
        self.ae(calls, [])
        # A grabbed mapping takes over a finger gesture the program saw begin,
        # and the program is told the gesture was cancelled
        self.ae(scroll(10, phase=BEGAN), [b'1'])
        self.ae(scroll(10, phase=ACTIVE), [b'2'])
        self.ae(scroll(100, GLFW_MOD_CONTROL, phase=ACTIVE), [b'7'])
        self.ae(calls, ['+'])
        # The rest of the mapped gesture and its coasting are not reported
        self.ae(scroll(100, GLFW_MOD_CONTROL, phase=ACTIVE), [])
        self.ae(scroll(100, GLFW_MOD_CONTROL, momentum=BEGAN), [])
        self.ae(scroll(0, GLFW_MOD_CONTROL, momentum=ENDED), [])
        self.ae(calls, ['+', '+'])
        # A gesture that ended is not cancelled when a mapping fires later
        self.ae(scroll(10, phase=BEGAN), [b'1'])
        self.ae(scroll(0, phase=ENDED), [b'3'])
        self.ae(scroll(100, GLFW_MOD_CONTROL, phase=BEGAN), [])
        self.ae(calls, ['+', '+', '+'])
