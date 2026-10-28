#!/usr/bin/env python
# License: GPLv3

from kitty.config import load_config
from kitty.fast_data_types import set_options, simulate_hdr_render_ticks
from kitty.options.types import defaults

from .base import BaseTest


class TestHDR(BaseTest):
    def test_hdr_default_and_overrides(self):
        self.ae(defaults.hdr, 'detect')
        self.ae(load_config().hdr, 'detect')
        for mode in ('detect', 'no', 'auto', 'always'):
            with self.subTest(mode=mode):
                bad_lines = []
                opts = load_config(overrides=[f'hdr {mode}'], accumulate_bad_lines=bad_lines)
                self.ae(bad_lines, [])
                self.ae(opts.hdr, mode)

    def test_hdr_display_changes(self):
        # An EDR display starts with no headroom until we request EDR. Keep it
        # enabled while idle and at maximum SDR brightness. Moving to an SDR
        # display must disable it, even if its previous headroom was larger.
        states = [
            (True, 1.0, False),
            (True, 2.0, False),
            (True, 8.0, True),
            (True, 1.0, True),
            (False, 8.0, True),
            (False, 1.0, False),
            (False, 1.0, True),
            (True, 2.0, True),
            (True, 2.0, False),
        ]
        expected = {
            'detect': [(True, 1.0), (True, 2.0), (True, 4.0), (True, 1.0), (False, 1.0), (False, 1.0), (False, 1.0), (True, 2.0), (True, 2.0)],
            'no': [(False, 1.0)] * len(states),
            'auto': [(False, 1.0), (False, 2.0), (True, 4.0), (True, 1.0), (True, 4.0), (False, 1.0), (True, 1.0), (True, 2.0), (False, 2.0)],
            'always': [(True, 1.0), (True, 2.0), (True, 4.0), (True, 1.0), (True, 4.0), (True, 1.0), (True, 1.0), (True, 2.0), (True, 2.0)],
        }
        for mode, result in expected.items():
            with self.subTest(mode=mode):
                opts = load_config(overrides=[f'hdr {mode}', 'hdr_max_headroom 4'])
                set_options(opts)
                self.ae(simulate_hdr_render_ticks(states * 256), result * 256)
