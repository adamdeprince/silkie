#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince
import math
import struct
from unittest.mock import patch

from kitty.spatial import IDENTITY, Controller, IMUDecoder, Presentation, inverse, matrix, multiply

from .base import BaseTest, parse_bytes
from .graphics import parse_full_response, send_command


def depth(screen, value):
    parse_bytes(screen, f'\x1b]777;depth;v=1:op=set:d={value}\x1b\\'.encode())


def report(stamp, gyro=(0., 0., 0.), accel=(0., -9.81, 0.)):
    body = bytearray(128) # One-series wire format, not a buffering limit
    struct.pack_into('<Q', body, 8, stamp)
    struct.pack_into('<I', body, 0x18, 0x0b)
    struct.pack_into('<3f', body, 0x1c, *gyro)
    struct.pack_into('<3f', body, 0x28, *accel)
    return b'\x28\x36' + struct.pack('>I', len(body)) + body


class TestSpatial(BaseTest):
    def test_text_depth(self):
        s = self.create_screen(cols=40, lines=3)
        for value in (-2147483648, -250, 0, 250, 2147483647):
            depth(s, value)
            parse_bytes(s, b'x')
            self.ae(s.cursor.depth, value)
            self.ae(s.line(0).cursor_from(s.cursor.x-1).depth, value)
        parse_bytes(s, b'\x1b[31m')
        self.ae(s.cursor.depth, 2147483647)
        parse_bytes(s, b'\x1b[0m')
        self.ae(s.cursor.depth, 0)
        for fields in ('d=+1', 'd=1x', 'd=2147483648', 'd=-2147483649', 'd=', 'd=1:d=2', 'd=1:future=a:future=b'):
            depth(s, -170)
            parse_bytes(s, f'\x1b]777;depth;v=1:op=set:{fields}\x1b\\'.encode())
            self.ae(s.cursor.depth, -170, fields)
        parse_bytes(s, b'\x1b]777;depth;v=2:op=set:d=1\x1b\\')
        self.ae(s.cursor.depth, -170)
        parse_bytes(s, b'\x1b]777;depth;v=1:op=reset\a')
        self.ae(s.cursor.depth, 0)

    def test_depth_round_trip_and_scrollback(self):
        s = self.create_screen(cols=12, lines=3, scrollback=2048)
        depth(s, -250)
        parse_bytes(s, b'negative\x1b[0mflat')
        ansi = s.line(0).as_ansi()
        copy = self.create_screen(cols=12, lines=3)
        parse_bytes(copy, ansi.encode())
        self.ae(str(copy.line(0)), str(s.line(0)))
        self.ae([copy.line(0).cursor_from(i).depth for i in range(12)], [s.line(0).cursor_from(i).depth for i in range(12)])
        s.reset()
        for n in range(1200):
            depth(s, (n % 2 * 2-1)*250)
            parse_bytes(s, b'line\r\n')
        self.assertGreater(s.historybuf.count, 1000)
        self.ae(abs(s.historybuf.line(0).cursor_from(0).depth), 250)
        self.ae(s.cursor.depth, 250)
        s.resize(6, 6)
        self.ae(s.cursor.depth, 250)
        parse_bytes(s, b'\x1b7')
        depth(s, -100)
        parse_bytes(s, b'\x1b8')
        self.ae(s.cursor.depth, 250)

    def test_vertical_phase(self):
        s = self.create_screen(cols=20, lines=3)
        parse_bytes(s, b'\x1b]777;depth;v=1:op=set:d=-150:y=-4\aA')
        self.ae((s.cursor.depth, s.cursor.phase), (-150, -4))
        parse_bytes(s, b'\x1b]777;depth;v=1:op=set:y=6\aB')
        self.ae((s.cursor.depth, s.cursor.phase), (-150, 6))
        parse_bytes(s, b'\x1b]777;depth;v=1:op=set:d=250:y=inf\a')
        self.ae((s.cursor.depth, s.cursor.phase), (-150, 6))
        copy = self.create_screen(cols=20, lines=3)
        parse_bytes(copy, s.line(0).as_ansi().encode())
        self.ae(copy.line(0).cursor_from(0).phase, -4)
        self.ae(copy.line(0).cursor_from(1).phase, 6)
        parse_bytes(s, b'\x1b[0m')
        self.ae((s.cursor.depth, s.cursor.phase), (0, 0))

    def test_fractional_pixel_attributes(self):
        s = self.create_screen(cols=20, lines=3, scrollback=2048)
        parse_bytes(s, b'\x1b]777;depth;v=1:op=set:p=0.25:y=-0.125\aA')
        self.ae((s.cursor.depth_pixels, s.cursor.phase), (.25, -.125))
        self.ae(s.line(0).cursor_from(0).depth_pixels, .25)
        copy = self.create_screen(cols=20, lines=3)
        parse_bytes(copy, s.line(0).as_ansi().encode())
        self.ae(copy.line(0).cursor_from(0).depth_pixels, .25)
        self.ae(copy.line(0).cursor_from(0).phase, -.125)
        for fields in ('p=nan', 'p=inf', 'p=1e100', 'p=1:d=250', 'p=1:y=nan', 'p=1x', 'y='):
            parse_bytes(s, f'\x1b]777;depth;v=1:op=set:{fields}\a'.encode())
            self.ae((s.cursor.depth_pixels, s.cursor.phase), (.25, -.125), fields)
        depth(s, -250)
        self.ae((s.cursor.depth, s.cursor.depth_pixels), (-250, 0.))
        parse_bytes(s, b'\x1b]777;depth;v=1:op=set:p=-.5\a')
        self.ae((s.cursor.depth, s.cursor.depth_pixels), (0, -.5))
        parse_bytes(s, b'\x1b[0m')
        self.ae((s.cursor.depth_pixels, s.cursor.phase), (0., 0.))
        for unused in range(1200):
            parse_bytes(s, b'\x1b]777;depth;v=1:op=set:p=.25:y=-.125\aline\r\n')
        self.assertGreater(s.historybuf.count, 1000)
        cell = s.historybuf.line(0).cursor_from(0)
        self.ae((cell.depth_pixels, cell.phase), (.25, -.125))
        s.resize(6, 6)
        parse_bytes(s, b'\x1b7\x1b[0m\x1b8')
        self.ae((s.cursor.depth_pixels, s.cursor.phase), (.25, -.125))

    def test_vector_vertical_phase_limit(self):
        s = self.create_screen()
        records = b'g id=1 phase=20\np id=2 g=1 d=M10,20h10v10h-10Z fill=#ffffff'
        self.ae(parse_full_response(send_command(s, 'a=t,f=200,i=1,s=40,v=60', records)).code, 'OK')
        def center(eye, limit):
            pixels = memoryview(s.grman.render_vector_linear(1, 40, 60, 0., 1., 1., 52., eye*.5, limit)).cast('f')
            weights = [(y, pixels[(y*40+15)*4+3]) for y in range(60)]
            return sum(y*a for y, a in weights)/sum(a for unused, a in weights)
        flat = center(-1, 0)
        self.ae(center(1, 0), flat)
        self.assertAlmostEqual(center(1, 4)-center(-1, 4), 4.)

    def test_vector_fractional_depth_and_multiplier(self):
        s = self.create_screen()
        records = b'g id=1 depth_px=0.5\np id=2 g=1 d=M10,10h10v10h-10Z fill=#ffffff'
        self.ae(parse_full_response(send_command(s, 'a=t,f=200,i=1,s=40,v=30', records)).code, 'OK')
        def center(eye, multiplier, limit=5.):
            pixels = memoryview(s.grman.render_vector_linear(1, 40, 30, eye*960., limit, limit, 52., 0., 0., multiplier)).cast('f')
            weights = [(x, pixels[(15*40+x)*4+3]) for x in range(40)]
            return sum(x*a for x, a in weights)/sum(a for unused, a in weights)
        # Left/right shifts of +/-0.25px are anti-aliased, preserving the centroid.
        self.assertAlmostEqual(center(1, 1)-center(-1, 1), .5, places=3)
        self.assertAlmostEqual(center(1, 2)-center(-1, 2), 1., places=3)
        self.assertAlmostEqual(center(1, 64, 0), center(-1, 64, 0), places=6)

    def test_vector_depth_and_limits(self):
        s = self.create_screen()
        def render(value, eye, positive=5., negative=5.):
            records = f'g id=1 depth={value}\np id=2 g=1 d=M45,10h10v10h-10Z fill=#ffffff'
            response = parse_full_response(send_command(s, 'a=t,f=200,i=1,s=100,v=30', records.encode()))
            self.ae(response.code, 'OK', response.msg)
            pixels = memoryview(s.grman.render_vector_linear(1, 100, 30, eye*50., positive, negative, 52.)).cast('f')
            weights = [(x, pixels[(15*100+x)*4+3]) for x in range(100)]
            return sum(x*a for x, a in weights)/sum(a for _, a in weights)
        flat = render(0, -1)
        self.assertLess(render(5000, -1), flat)
        self.assertGreater(render(5000, 1), flat)
        self.assertGreater(render(-5000, -1), flat)
        self.assertLess(render(-5000, 1), flat)
        self.ae(render(5000, 1, positive=0), flat)
        self.ae(render(-5000, 1, negative=0), flat)
        self.assertAlmostEqual(render(50000, 1, positive=1), render(1000, 1, positive=1), places=5)
        self.assertAlmostEqual(render(-50000, 1, negative=1), render(-1000, 1, negative=1), places=5)

    def test_sustained_imu_stream(self):
        decoder = IMUDecoder()
        data = b''.join(report(1_000_000*n, (0., .5, 0.)) for n in range(20000))
        # Split headers, lengths, and bodies at different boundaries, including
        # chunks far larger than previous fixed buffers in source implementations.
        for start in range(0, len(data), 65537):
            decoder.feed(data[start:start+65537])
        self.ae(decoder.samples, 20000)
        self.ae(decoder.pending, b'')
        self.assertAlmostEqual(math.hypot(*decoder.orientation), 1., places=12)
        self.assertGreater(abs(decoder.orientation[2]), .1)
        before = decoder.orientation
        decoder.feed(b'junk\x28\x36\x00\x00\x00\x01' + report(20_000_000_000, (float('nan'), 0., 0.)))
        self.ae(decoder.orientation, before)
        decoder.feed(report(20_000_000_000))
        self.ae(decoder.pending, b'')

    def test_pose_and_tracking_loss(self):
        q = (math.cos(.2), 0., math.sin(.2), 0.)
        self.assertAlmostEqual(multiply(q, inverse(q))[0], 1.)
        self.ae(matrix(IDENTITY), (1., 0., 0., 0., 1., 0., 0., 0., 1.))
        controller = Controller(windows={1: Presentation('anchored')})
        class Tracker:
            def pose(self):
                return q, True
        class Boss:
            os_window_map = {1: None}
        controller.tracker = Tracker()
        with patch('kitty.fast_data_types.get_boss', return_value=Boss()), patch('kitty.fast_data_types.spatial_pose') as submit, \
                patch('kitty.native_ui.update_native_menus'):
            controller.tick(1)
            self.ae(controller.windows[1].reference, q)
            self.ae(submit.call_args.args[1], matrix(IDENTITY))
            with patch.object(controller.tracker, 'pose', return_value=(q, False)):
                controller.tick(1)
            self.assertFalse(controller.windows[1].tracking)
            self.assertIsNone(controller.windows[1].reference)

    def test_spatial_configuration(self):
        from kitty.config import load_config
        self.ae(load_config().spatial_negative_divergence, 0)
        for name, value in (('spatial_field_of_view', '180'), ('spatial_field_of_view', '0'),
                            ('spatial_negative_divergence', '-1'), ('spatial_positive_divergence', 'nan'),
                            ('spatial_wall_scale', '0.5'), ('spatial_wall_scale', 'inf')):
            bad = []
            load_config(overrides=(f'{name} {value}',), accumulate_bad_lines=bad)
            self.ae(len(bad), 1, name)
        opts = load_config(overrides=('spatial_positive_divergence 0', 'spatial_negative_divergence 2.25', 'spatial_wall_scale 128'))
        self.ae(opts.spatial_positive_divergence, 0)
        self.ae(opts.spatial_negative_divergence, 2.25)
        self.ae(opts.spatial_wall_scale, 128)

    def test_xreal_requires_explicit_opt_in(self):
        from kitty.config import load_config
        opts = load_config()
        self.assertFalse(opts.experimental_xreal_1s)
        controller = Controller()
        with patch('kitty.constants.is_macos', True), patch('kitty.fast_data_types.get_options', return_value=opts), \
                patch('kitty.fast_data_types.spatial_configure', return_value=True) as native, \
                patch('kitty.fast_data_types.add_timer', return_value=7), patch('kitty.fast_data_types.remove_timer'), \
                patch('kitty.spatial.Tracker') as tracker:
            for mode, output in (('follow', 'sbs'), ('anchored', 'sbs'), ('anchored', 'red-blue')):
                with self.assertRaisesRegex(ValueError, 'experimental and untested'):
                    controller.configure(1, mode, output)
            native.assert_not_called()
            tracker.assert_not_called()
            controller.configure(1, 'follow', 'red-blue')
            self.ae(controller.windows[1].mode, 'follow')
            tracker.assert_not_called()
            opts.experimental_xreal_1s = True
            controller.configure(1, 'anchored', 'sbs')
            tracker.assert_called_once_with(opts.spatial_tracking_address)
            opts.experimental_xreal_1s = False
            controller.configure(1, 'off')
            self.assertIsNone(controller.tracker)
            self.ae(controller.timer, 0)
            tracker.return_value.close.assert_called_once()

    def test_depth_large_frames_and_reset(self):
        s = self.create_screen(cols=10)
        for size in (4096, 65536, 131072):
            parse_bytes(s, ('\x1b]777;depth;v=1:op=set:d=-321:future=' + 'x'*size + '\a').encode())
            self.ae(s.cursor.depth, -321)
        parse_bytes(s, b'\x1b]777;caps;v=1:op=reset\x1b\\')
        self.ae(s.cursor.depth, 0)
