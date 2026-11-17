#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from .base import BaseTest, parse_bytes


class TestDisplayPrecision(BaseTest):

    def report(self, screen, operation='query:id=7'):
        screen.callbacks.clear()
        parse_bytes(screen, f'\x1b]777;display;v=1:op={operation}\x1b\\'.encode())
        response = screen.callbacks.wtcbuf
        self.assertTrue(response.startswith(b'\x1b]777;display;v=1:'), response)
        self.assertTrue(response.endswith(b'\x1b\\'), response)
        return dict(item.split('=', 1) for item in response[len(b'\x1b]777;display;'):-2].decode().split(':'))

    def test_rgb_and_gray_channel_widths(self):
        cases = (
            ('', 'rgb', (0, 0, 0, 0, 0), 'unknown'),
            ('color=rgb bits=10', 'rgb', (10, 10, 10, 10, 0), 'configured'),
            ('color=rgb red_bits=5 green_bits=6 blue_bits=5', 'rgb', (0, 5, 6, 5, 0), 'configured'),
            ('bits=8 red_bits=10 color=rgb', 'rgb', (0, 10, 8, 8, 0), 'configured'),
            ('red_bits=10 color=rgb bits=8', 'rgb', (0, 10, 8, 8, 0), 'configured'),
            ('color=gray bits=4', 'gray', (4, 0, 0, 0, 4), 'configured'),
            ('gray_bits=6 color=gray bits=4', 'gray', (6, 0, 0, 0, 6), 'configured'),
            ('color=gray', 'gray', (0, 0, 0, 0, 0), 'unknown'),
            ('color=mono', 'mono', (1, 0, 0, 0, 1), 'configured'),
            ('color=mono bits=4 gray_bits=8', 'mono', (1, 0, 0, 0, 1), 'configured'),
            ('color=rgb bits=0', 'rgb', (0, 0, 0, 0, 0), 'configured'),
            ('color=rgb bits=8 red_bits=-1 green_bits=abc blue_bits=99999999999999999999',
             'rgb', (8, 8, 8, 8, 0), 'configured'),
        )
        keys = ('bits', 'red_bits', 'green_bits', 'blue_bits', 'gray_bits')
        for overrides, color, widths, precision in cases:
            with self.subTest(overrides=overrides):
                screen = self.create_screen(options={'display_info': overrides})
                report = self.report(screen)
                self.ae(report['color'], color)
                self.ae(tuple(int(report[key]) for key in keys), widths)
                self.ae(report['precision'], precision)

    def test_reports_grow_and_channel_changes_survive_repeated_events(self):
        screen = self.create_screen()
        for iteration in range(128):
            # All widths fit the protocol's integer representation. This also
            # produces a report longer than the old fixed 256-byte buffer.
            red = 2147483647 - iteration
            self.set_options({'display_info': ('ignored=value ' * 1024 +
                f'color=rgb red_bits={red} green_bits=2147483647 blue_bits=2147483647 '
                'period=2147483647 ppi=123456789 headroom=123456789 scale=123456789')})
            report = self.report(screen, 'mode:mode=1')
            self.ae(report['op'], 'event')
            self.ae(report['red_bits'], str(red))
            self.ae(report['green_bits'], '2147483647')
            self.ae(report['blue_bits'], '2147483647')
            self.ae(report['precision'], 'configured')
        report = self.report(screen, 'query:id=2147483647')
        self.ae(report['precision'], 'configured')
        self.assertGreater(len(screen.callbacks.wtcbuf), 256)

    def test_output_fields_without_a_window(self):
        # Silkie's own surface is known only once a window has been drawn; until then nothing is guessed
        screen = self.create_screen(options={'display_info': 'color=rgb bits=10'})
        report = self.report(screen)
        self.ae((report['output_bits'], report['output_type'], report['output_dither']), ('0', 'unknown', '0'))
        report = self.report(screen, 'mode:mode=1')
        self.ae((report['output_bits'], report['output_type'], report['output_dither']), ('0', 'unknown', '0'))
