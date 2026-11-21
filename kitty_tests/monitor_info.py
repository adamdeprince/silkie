#!/usr/bin/env python
# License: GPLv3

import base64
import os
import shutil
import tempfile
from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty.config import load_config
from kitty.fast_data_types import (
    Color,
    ColorProfile,
    intern_monitor_identity,
    known_e_paper_monitors,
    monitor_matches,
    parse_edid,
    set_eink_mode,
    set_monitor_sysfs_root,
    set_test_monitor_identity,
    simulate_monitor_changes,
)

from .base import BaseTest, parse_bytes

here = os.path.dirname(os.path.abspath(__file__))


def fixture(name: str) -> bytes:
    # The EDIDs of the two monitors of a Linux machine, as /sys/class/drm/card*-DP-*/edid gave them, with the serial
    # numbers taken out: the header's, and the LG's serial descriptor. The BOOX's holds its model, BOOX MIRA253.
    with open(os.path.join(here, 'edid', name), 'rb') as f:
        return f.read()


BOOX = fixture('boox-mira-pro.edid')
LG = fixture('lg-ultrafine-pro.edid')
# What mutter said of the two monitors, recorded from its wl_output and xdg_output events
BOOX_WAYLAND = {'connector': 'DP-7', 'make': 'OBX', 'model': 'MIRA253', 'description': 'OBX 25"'}
LG_WAYLAND = {'connector': 'DP-8', 'make': 'GSM', 'model': 'UltraFine Pro', 'description': 'LG Electronics 27"'}
BOOX_X11 = {'connector': 'DP-7', 'edid': BOOX}
LG_X11 = {'connector': 'DP-8', 'edid': LG}


def b64(x: str) -> str:
    return base64.standard_b64encode(x.encode()).decode()


class TestMonitorInfo(BaseTest):
    def tearDown(self):
        set_test_monitor_identity(None, False)
        set_monitor_sysfs_root(None)
        super().tearDown()

    def test_edid_parsing(self):
        boox = parse_edid(BOOX)
        self.ae(boox['mfg'], 'OBX')
        self.ae(boox['product'], 0x0011)
        self.ae((boox['version'], boox['revision'], boox['year']), (1, 3, 2021))
        self.ae(boox['name'], 'MIRA253')
        self.assertNotIn('serial', boox)
        # It says it is an RGB color display, so nothing in it says gray
        self.assertFalse(boox['grayscale'])
        lg = parse_edid(LG)
        self.ae((lg['mfg'], lg['product'], lg['name'], lg['version'], lg['revision']), ('GSM', 0x5BCD, 'UltraFine Pro', 1, 4))
        self.assertFalse(lg['grayscale'])
        # Not an EDID, or too short
        self.assertIsNone(parse_edid(b''))
        self.assertIsNone(parse_edid(BOOX[:127]))
        self.assertIsNone(parse_edid(b'\x01' + BOOX[1:]))
        # An analog display of EDID 1.3 that says it is monochrome
        gray = bytearray(BOOX)
        gray[20] = 0x0F
        gray[24] &= ~0x18
        self.assertTrue(parse_edid(bytes(gray))['grayscale'])
        # A digital one of EDID 1.4 uses those bits for its encodings
        gray[20], gray[19] = 0x80, 4
        self.assertFalse(parse_edid(bytes(gray))['grayscale'])
        # Unprintable bytes in a descriptor are dropped, bits that do not spell letters leave the ID unknown
        bad = bytearray(BOOX)
        bad[0x5F:0x63] = b'\x01\x7fA\xff'
        bad[8], bad[9] = 0, 0
        info = parse_edid(bytes(bad))
        self.ae(info['name'], 'A253')
        self.assertIsNone(info['mfg'])

    def test_identities_and_table(self):
        # Wayland: what the compositor says, no EDID
        m = intern_monitor_identity(BOOX_WAYLAND)
        self.ae((m['connector'], m['make'], m['model'], m['description'], m['mfg'], m['product']), ('DP-7', 'OBX', 'MIRA253', 'OBX 25"', None, None))
        self.ae(m['known'], 'ONYX BOOX Mira Pro')
        self.assertIsNone(intern_monitor_identity(LG_WAYLAND)['known'])
        # X11: the EDID of the RandR output
        m = intern_monitor_identity(BOOX_X11)
        self.ae((m['make'], m['model'], m['mfg'], m['product'], m['edid_source']), ('OBX', 'MIRA253', 'OBX', '0x0011', 'system'))
        self.ae(m['known'], 'ONYX BOOX Mira Pro')
        # No serial is kept
        self.assertNotIn('serial', m)
        # The text of the serial descriptor recognizes the model, where that is all there is
        only_serial = bytearray(BOOX)
        only_serial[10:12] = b'\x99\x99'
        only_serial[0x5F:0x6C] = b'Other\n       '
        self.ae(intern_monitor_identity({'edid': bytes(only_serial)})['known'], 'ONYX BOOX Mira Pro')
        m = intern_monitor_identity(LG_X11)
        self.ae((m['mfg'], m['product'], m['model'], m['known']), ('GSM', '0x5BCD', 'UltraFine Pro', None))
        # macOS: the name of the screen and the vendor and model numbers of Core Graphics, the 16 bits of the EDID
        m = intern_monitor_identity({'model': 'MIRA253', 'ids': (0x3C58, 0x11)})
        self.ae((m['mfg'], m['product'], m['known']), ('OBX', '0x0011', 'ONYX BOOX Mira Pro'))
        m = intern_monitor_identity({'model': 'Some name', 'ids': (0x3C58, 0x11)})
        self.ae(m['known'], 'ONYX BOOX Mira Pro')
        m = intern_monitor_identity({'model': 'LG UltraFine', 'ids': (0x1E6D, 0x5B71)})
        self.ae((m['mfg'], m['known']), ('GSM', None))
        # The strings of another manufacturer's monitor do not match
        self.assertIsNone(intern_monitor_identity({'model': 'MIRA253', 'ids': (0x1E6D, 0x11)})['known'])
        self.assertIsNone(intern_monitor_identity({'make': 'GSM', 'model': 'MIRA253'})['known'])
        # nor the product code of another monitor of the manufacturer
        self.assertIsNone(intern_monitor_identity({'model': 'Other', 'ids': (0x3C58, 0x12)})['known'])
        # A compositor that names the manufacturer rather than giving its ID: the strings alone
        self.ae(intern_monitor_identity({'make': 'Onyx International', 'model': 'MIRA253'})['known'], 'ONYX BOOX Mira Pro')
        # A shared or borrowed manufacturer ID needs the strings
        mira133 = bytearray(BOOX)
        mira133[8:12] = bytes((0x3D, 0xD8, 0x01, 0x00))
        mira133[0x5F:0x6C] = b'Mira133\n     '
        self.ae(intern_monitor_identity({'edid': bytes(mira133)})['known'], 'ONYX BOOX Mira')
        mira133[0x5F:0x6C] = b'Somelec\n     '
        self.assertIsNone(intern_monitor_identity({'edid': bytes(mira133)})['known'])
        # Control characters from the system are dropped
        self.ae(intern_monitor_identity({'model': 'a\x1b[1mb\n', 'connector': 'X'})['model'], 'a[1mb')
        # The same description is the same identity
        self.ae(intern_monitor_identity(dict(BOOX_X11)), intern_monitor_identity(dict(BOOX_X11)))
        table = {e['name']: e for e in known_e_paper_monitors()}
        self.ae((table['ONYX BOOX Mira Pro']['kind'], table['ONYX BOOX Mira Pro']['color'], table['ONYX BOOX Mira Pro']['gray_bits']), ('eink', 'gray', 4))
        # e-paper is not always gray: a model sold with either panel is of unknown color
        self.ae((table['DASUNG Paperlike 13K']['kind'], table['DASUNG Paperlike 13K']['color']), ('eink', None))

    def test_sysfs_edid_for_wayland(self):
        root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, root)

        def connector(name, edid):
            os.makedirs(os.path.join(root, name))
            with open(os.path.join(root, name, 'edid'), 'wb') as f:
                f.write(edid)

        connector('card2-DP-7', BOOX)
        connector('card2-DP-8', LG)
        connector('card1-DP-1', b'')
        set_monitor_sysfs_root(root)
        m = intern_monitor_identity(dict(BOOX_WAYLAND, description='sysfs 1'))
        self.ae((m['edid_source'], m['product'], m['make'], m['known']), ('sysfs', '0x0011', 'OBX', 'ONYX BOOX Mira Pro'))
        m = intern_monitor_identity(dict(LG_WAYLAND, description='sysfs 1'))
        self.ae((m['edid_source'], m['product'], m['make']), ('sysfs', '0x5BCD', 'GSM'))
        # An EDID that disagrees with the compositor, as for a nested one with similar names, is not used
        m = intern_monitor_identity(dict(BOOX_WAYLAND, model='Other', description='sysfs 2'))
        self.ae((m['edid_source'], m['product']), (None, None))
        # nor is one without a model to check it against
        self.assertIsNone(intern_monitor_identity({'connector': 'DP-7', 'description': 'sysfs 3'})['edid_source'])
        # nor when two cards have the connector
        connector('card3-DP-7', BOOX)
        self.assertIsNone(intern_monitor_identity(dict(BOOX_WAYLAND, description='sysfs 4'))['edid_source'])
        # nor a path
        self.assertIsNone(intern_monitor_identity(dict(BOOX_WAYLAND, connector='../card2-DP-8', description='sysfs 5'))['edid_source'])
        # An EDID from the system is used as it is
        self.ae(intern_monitor_identity(dict(BOOX_X11, description='sysfs 6'))['edid_source'], 'system')

    def test_matching(self):
        for match, expected in (
            ('MIRA253', True), ('mira253', True), ('DP-7', True), ('dp-7', True), ('BOOX MIRA253', False), ('OBX', True),
            ('OBX:0x0011', True), ('obx:17', True), ('OBX:0X11', True), ('OBX:*', True), ('OBX:0x0012', False), ('GSM:*', False),
            ('MIRA', False), ('DP-8', False), ('OBX:0x10000', False), ('OBX:', False), ('OB1:17', False), ('', False),
        ):
            with self.subTest(match=match):
                self.ae(monitor_matches(dict(BOOX_X11, description='Onyx 25"'), match), expected)
        # A part of the description
        self.assertTrue(monitor_matches(LG_WAYLAND, 'lg electronics'))
        self.assertTrue(monitor_matches(LG_WAYLAND, 'UltraFine Pro'))
        self.assertFalse(monitor_matches(LG_WAYLAND, 'UltraFine'))
        # The make of a compositor is the manufacturer ID when it is three capital letters, without the product code
        self.assertTrue(monitor_matches(BOOX_WAYLAND, 'OBX:*'))
        self.assertFalse(monitor_matches(BOOX_WAYLAND, 'OBX:17'))

    def test_config(self):
        bad = []
        opts = load_config(overrides=[
            'monitor_info MIRA253 kind=eink color=gray',
            'monitor_info "OBX:17" bits=4',
            "monitor_info 'OBX:0x0011' ppi=145.6",
            'monitor_info MIRA253 color=mono',
            'monitor_info DP-8 kind=nonsense',
            'monitor_info DP-8 nokey=1',
            'monitor_info DP-8 bits=-1',
            'monitor_info DP-8',
            'monitor_info "DP-8 kind=eink',
            'monitor_info DP-8 kind',
        ], accumulate_bad_lines=bad)
        self.ae(len(bad), 6)
        self.ae(opts.monitor_info, {'MIRA253': 'kind=eink color=gray color=mono', 'OBX:17': 'bits=4', 'OBX:0x0011': 'ppi=145.6'})

    def states(self, ticks, **options):
        self.set_options(options)
        return simulate_monitor_changes(ticks)

    def test_precedence(self):
        def one(identity, **options):
            s = self.states([(0, identity)], **options)[0]
            return s['kind'], s['color'], s['gray_bits'], s['eink'], s['gray']

        # The OS says an RGB display; the table knows the BOOX
        self.ae(one(LG_X11), ('unknown', 'rgb', 0, False, False))
        self.ae(one(BOOX_X11), ('eink', 'gray', 4, True, True))
        self.ae(one(BOOX_WAYLAND), ('eink', 'gray', 4, True, True))
        # monitor_info overrides the table, the later lines winning
        self.ae(one(BOOX_X11, monitor_info={'MIRA253': 'kind=lcd color=rgb'}), ('lcd', 'rgb', 0, False, False))
        self.ae(one(BOOX_X11, monitor_info={'MIRA253': 'kind=lcd', 'OBX:*': 'kind=eink'}), ('eink', 'gray', 4, True, True))
        # Color e-paper: e-paper behavior, its colors kept
        self.ae(one(LG_X11, monitor_info={'DP-8': 'kind=eink color=rgb'}), ('eink', 'rgb', 0, True, False))
        self.ae(one(LG_X11, monitor_info={'DP-8': 'kind=eink'}), ('eink', 'rgb', 0, True, False))
        # A gray display that is not e-paper: black on white, ordinary refresh
        self.ae(one(LG_X11, monitor_info={'GSM:0x5BCD': 'color=gray gray_bits=8'}), ('unknown', 'gray', 8, False, True))
        # A monitor_info line for another monitor changes nothing
        self.ae(one(LG_X11, monitor_info={'MIRA253': 'kind=eink color=gray'}), ('unknown', 'rgb', 0, False, False))
        # eink_mode: every display is e-paper, gray unless something says what colors it has
        set_eink_mode(True)
        try:
            self.ae(one(LG_X11, eink_mode=True), ('eink', 'gray', 0, True, True))
            self.ae(one(None, eink_mode=True), ('eink', 'gray', 0, True, True))
            self.ae(one(LG_X11, eink_mode=True, display_info='color=rgb'), ('eink', 'rgb', 0, True, False))
            self.ae(one(LG_X11, eink_mode=True, monitor_info={'DP-8': 'color=rgb'}), ('eink', 'rgb', 0, True, False))
            self.ae(one(LG_X11, eink_mode=True, monitor_info={'DP-8': 'kind=lcd'}), ('eink', 'gray', 0, True, True))
        finally:
            set_eink_mode(False)
        # display_info changes only what programs are told
        self.ae(one(LG_X11, display_info='kind=eink color=gray bits=4'), ('eink', 'gray', 4, False, False))
        self.ae(one(BOOX_X11, display_info='kind=lcd color=rgb'), ('lcd', 'rgb', 0, True, True))

    def test_moves_between_monitors(self):
        # What is done follows the window from monitor to monitor, once a move has lasted 300ms
        states = self.states([
            (0, LG_X11), (100, BOOX_X11), (350, BOOX_X11), (400, BOOX_X11), (500, LG_X11), (600, BOOX_X11), (700, LG_X11),
            (1000, LG_X11), (1100, None), (1500, None), (1600, BOOX_WAYLAND), (1900, BOOX_WAYLAND),
        ])
        self.ae([(s['eink'], s['gray'], s['changed']) for s in states], [
            (False, False, False),  # acted on at once, ordinary as before
            (False, False, False), (False, False, False), (True, True, True),
            # dragged back and forth across the boundary: nothing changes until it stays
            (True, True, False), (True, True, False), (True, True, False), (False, False, True),
            (False, False, False), (False, False, False),
            (False, False, False), (True, True, True),
        ])
        # Starting on e-paper is acted on at once
        self.ae(self.states([(0, BOOX_X11)])[0]['changed'], True)
        # A long session of moves keeps working
        ticks, t = [], 0
        for i in range(500):
            ticks += [(t, BOOX_X11 if i % 2 else LG_X11), (t + 400, BOOX_X11 if i % 2 else LG_X11)]
            t += 1000
        states = self.states(ticks)
        # every move but the first, which starts where the window already was
        self.ae(sum(s['changed'] for s in states), 499)

    def test_display_report(self):
        set_test_monitor_identity(BOOX_X11)
        s = self.create_screen()
        c = s.callbacks
        parse_bytes(s, b'\x1b]777;display;v=1:op=query:id=3\x1b\\')
        self.ae(c.wtcbuf, (
            '\x1b]777;display;v=1:op=cap:id=3:mode=0:kind=eink:color=gray:bits=4:gamut=unknown:headroom=1:scale=0:period=0:ppi=0'
            ':red_bits=0:green_bits=0:blue_bits=0:gray_bits=4:precision=model:output_bits=0:output_type=unknown:output_dither=0'
            ':appearance=no_preference:high_contrast=1:differentiate_without_color=1:languages=und'
            f':eink_scheme=light:mfg=OBX:product=17:make={b64("OBX")}:model={b64("MIRA253")}:connector={b64("DP-7")}\x1b\\').encode())
        # Never a serial number
        self.assertNotIn(b':serial=', c.wtcbuf)
        self.assertNotIn(b64('BOOX MIRA253').encode(), c.wtcbuf)
        s = self.create_screen(options={'display_info': 'kind=lcd color=rgb'})
        parse_bytes(s, b'\x1b]777;display;v=1:op=query:id=3\x1b\\')
        self.assertIn(b':kind=lcd:color=rgb:', s.callbacks.wtcbuf)
        self.assertIn(b':high_contrast=1:differentiate_without_color=1:languages=und:eink_scheme=light:', s.callbacks.wtcbuf)
        # Strings from the system are UTF-8, fields of unknown things are left out
        set_test_monitor_identity({'model': 'Écran: 1', 'description': 'x' * 5000})
        s = self.create_screen()
        c = s.callbacks
        parse_bytes(s, b'\x1b]777;display;v=1:op=query:id=3\x1b\\')
        reply = c.wtcbuf.decode()
        self.assertIn(f':model={b64("Écran: 1")}:description={b64("x" * 5000)}\x1b\\', reply)
        self.assertNotIn(':mfg=', reply)
        # No monitor: as before
        set_test_monitor_identity(None)
        s = self.create_screen()
        c = s.callbacks
        parse_bytes(s, b'\x1b]777;display;v=1:op=query:id=3\x1b\\')
        self.assertTrue(c.wtcbuf.endswith(b':output_dither=0:appearance=no_preference:high_contrast=0:differentiate_without_color=0:languages=und\x1b\\'))

    def test_eink_colors_layer(self):
        opts = self.set_options({'foreground': Color(1, 2, 3), 'background': Color(4, 5, 6)})
        cp = ColorProfile(opts)
        spec = {'foreground': 0, 'background': 0xFFFFFF, 'cursor': 0, 'cursor_text_color': 0xFFFFFF, 'selection_foreground': 0xFFFFFF,
                'selection_background': 0}
        for _ in range(64):
            # a program's colors
            cp.default_bg = Color(0x11, 0x22, 0x33)
            cp.cursor_color = Color(9, 9, 9)
            self.assertTrue(cp.set_eink_colors(spec))
            self.assertTrue(cp.eink_colors_active())
            self.ae((int(cp.default_fg), int(cp.default_bg), int(cp.cursor_color)), (0, 0xFFFFFF, 0))
            # what get-colors and the change notifications go by
            self.ae((cp.as_dict()['foreground'], cp.as_dict()['background']), (0, 0xFFFFFF))
            self.assertFalse(cp.set_eink_colors(spec))
            # the program's colors come back
            self.assertTrue(cp.set_eink_colors(None))
            self.ae((int(cp.default_fg), int(cp.default_bg), int(cp.cursor_color)), (0x010203, 0x112233, 0x090909))
            self.assertFalse(cp.set_eink_colors(None))
            # unless the program set others meanwhile
            cp.set_eink_colors(spec)
            cp.default_bg = Color(0x44, 0x44, 0x44)
            cp.set_eink_colors(None)
            self.ae(int(cp.default_bg), 0x444444)
        # The palette is left alone
        before = cp.as_dict()['color1']
        cp.set_eink_colors(spec)
        self.ae(cp.as_dict()['color1'], before)
        cp.set_eink_colors(None)

    def test_per_os_window_colors(self):
        # Two OS windows: what is done follows each one's display
        from kitty.colors import sync_eink_colors

        windows = []
        for os_window_id in (1, 1, 2):
            s = self.create_screen(options={'foreground': Color(1, 2, 3), 'background': Color(4, 5, 6)})
            w = SimpleNamespace(id=len(windows) + 1, os_window_id=os_window_id, screen=s, refresh=Mock(), notify_child_of_color_change=Mock())
            windows.append(w)
        tab_bars = {1: Mock(), 2: Mock()}
        tms = [SimpleNamespace(os_window_id=i, tab_bar=tab_bars[i], mark_tab_bar_dirty=Mock(), active_tab=None) for i in (1, 2)]
        boss = SimpleNamespace(all_windows=windows, all_tab_managers=tms, default_bg_changed_for=Mock())
        gray = {1: False, 2: False}
        chrome = {}
        with (
            patch('kitty.colors.get_boss', return_value=boss),
            patch('kitty.fast_data_types.os_window_eink_state', side_effect=lambda i: (gray[i], gray[i])),
            patch('kitty.fast_data_types.set_os_window_eink_chrome', side_effect=lambda i, spec: chrome.__setitem__(i, spec)),
        ):
            for i in range(32):
                gray[1] = i % 2 == 0
                sync_eink_colors()
                bgs = [int(w.screen.color_profile.default_bg) for w in windows]
                self.ae(bgs, [0xFFFFFF, 0xFFFFFF, 0x040506] if gray[1] else [0x040506] * 3)
                self.ae(chrome[1] is not None, gray[1])
                self.assertIsNone(chrome[2])
                self.ae(tab_bars[1].patch_colors.call_args[0][0]['tab_bar_background'], 0xFFFFFF if gray[1] else None)
            # Only the windows whose colors changed are redrawn and told
            for w in windows:
                w.notify_child_of_color_change.reset_mock()
            gray[1] = True
            sync_eink_colors((1,))
            sync_eink_colors((1,))
            self.ae([w.notify_child_of_color_change.call_count for w in windows], [1, 1, 0])
