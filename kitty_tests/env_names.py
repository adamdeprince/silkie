#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import json
import os
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch

from kitty.constants import kitty_base_dir, kitty_exe, silkie_env

from .base import BaseTest

# Every variable renamed from KITTY_<name> to SILKIE_<name>, with the old name still read
RENAMED = (
    'CACHE_DIRECTORY', 'RUNTIME_DIRECTORY', 'DISABLE_WAYLAND', 'WAYLAND_DETECT_MODIFIERS', 'SIMD', 'RC_PASSWORD', 'DEVELOP_FROM',
    'CLONE_SOURCE_CODE', 'CLONE_SOURCE_PATH', 'NO_LTO', 'EGL_LIBRARY', 'STARTUP_NOTIFICATION_LIBRARY', 'CANBERRA_LIBRARY',
    'SYSTEMD_LIBRARY', 'FONTCONFIG_LIBRARY',
)


def clean_env(**kw: str) -> dict[str, str]:
    ans = {k: v for k, v in os.environ.items() if not any(k in (f'SILKIE_{n}', f'KITTY_{n}') for n in RENAMED)}
    ans.update(kw)
    return ans


class TestEnvNames(BaseTest):

    def run_python(self, code: str, env: dict[str, str], cwd: str | None = None) -> str:
        cp = subprocess.run([kitty_exe(), '+runpy', code], env=env, cwd=cwd, capture_output=True, text=True)
        self.assertEqual(cp.returncode, 0, cp.stderr)
        return cp.stdout.strip()

    def test_silkie_env_precedence(self):
        env = clean_env()
        with patch.dict(os.environ, env, clear=True):
            self.assertIsNone(silkie_env('CACHE_DIRECTORY'))
            os.environ['KITTY_CACHE_DIRECTORY'] = 'kitty'
            self.ae(silkie_env('CACHE_DIRECTORY'), 'kitty')
            os.environ['SILKIE_CACHE_DIRECTORY'] = 'silkie'
            self.ae(silkie_env('CACHE_DIRECTORY'), 'silkie')
            # An empty SILKIE_ value is still set, and overrides an inherited KITTY_ one
            os.environ['SILKIE_CACHE_DIRECTORY'] = ''
            self.ae(silkie_env('CACHE_DIRECTORY'), '')
            del os.environ['KITTY_CACHE_DIRECTORY']
            self.ae(silkie_env('CACHE_DIRECTORY'), '')

    def test_cache_and_runtime_directories(self):
        code = 'import json; from kitty.constants import cache_dir, runtime_dir; print(json.dumps([cache_dir(), runtime_dir()]))'
        with tempfile.TemporaryDirectory() as tdir:
            d = {name: os.path.join(tdir, name) for name in ('kc', 'sc', 'kr', 'sr')}

            def check(expected_cache: str, expected_runtime: str, **kw: str) -> None:
                result = json.loads(self.run_python(code, clean_env(**kw)))
                self.ae(result, [expected_cache, expected_runtime], kw)

            check(d['sc'], d['sr'], KITTY_CACHE_DIRECTORY=d['kc'], KITTY_RUNTIME_DIRECTORY=d['kr'],
                  SILKIE_CACHE_DIRECTORY=d['sc'], SILKIE_RUNTIME_DIRECTORY=d['sr'])
            # The old names were not used at all, not even to create the directories
            self.assertFalse(os.path.exists(d['kc']) or os.path.exists(d['kr']))
            check(d['kc'], d['kr'], KITTY_CACHE_DIRECTORY=d['kc'], KITTY_RUNTIME_DIRECTORY=d['kr'])
            check(d['sc'], d['sr'], SILKIE_CACHE_DIRECTORY=d['sc'], SILKIE_RUNTIME_DIRECTORY=d['sr'])

    def test_disable_wayland(self):
        code = 'from kitty.constants import detect_if_wayland_ok; print(detect_if_wayland_ok())'
        base = clean_env(WAYLAND_DISPLAY='silkie-test-no-such-display')
        # Without either variable the result depends on whether this build has the Wayland backend. Disabling must
        # always give False, and an empty SILKIE_ value must override KITTY_ and give the result without it.
        baseline = self.run_python(code, base)
        self.ae(self.run_python(code, dict(base, KITTY_DISABLE_WAYLAND='1')), 'False')
        self.ae(self.run_python(code, dict(base, SILKIE_DISABLE_WAYLAND='1')), 'False')
        self.ae(self.run_python(code, dict(base, SILKIE_DISABLE_WAYLAND='1', KITTY_DISABLE_WAYLAND='')), 'False')
        self.ae(self.run_python(code, dict(base, SILKIE_DISABLE_WAYLAND='', KITTY_DISABLE_WAYLAND='1')), baseline)

    def test_simd(self):
        code = 'import json; from kitty import fast_data_types as f; print(json.dumps([f.has_sse4_2, f.has_avx2]))'

        def check(expected: list[bool], **kw: str) -> None:
            self.ae(json.loads(self.run_python(code, clean_env(**kw))), expected, kw)

        check([True, False], KITTY_SIMD='128')
        check([False, True], SILKIE_SIMD='256')
        check([True, False], SILKIE_SIMD='128', KITTY_SIMD='256')
        check([False, False], SILKIE_SIMD='none', KITTY_SIMD='128')

    def test_rc_password(self):
        code = '''
from kitty.remote_control import get_password, parse_rc_args
opts, _ = parse_rc_args(['@', 'ls'])
print(repr(get_password(opts)))
'''
        self.ae(self.run_python(code, clean_env(KITTY_RC_PASSWORD='kitty')), "'kitty'")
        self.ae(self.run_python(code, clean_env(SILKIE_RC_PASSWORD='silkie')), "'silkie'")
        self.ae(self.run_python(code, clean_env(SILKIE_RC_PASSWORD='silkie', KITTY_RC_PASSWORD='kitty')), "'silkie'")

    def test_os_name_in_includes(self):
        from kitty.conf.utils import os_name
        from kitty.config import load_config
        with tempfile.TemporaryDirectory() as tdir:
            root = Path(tdir)
            (root / f'{os_name()}.conf').write_text('scrollback_lines 4321\n')
            for name in ('SILKIE_OS', 'KITTY_OS'):
                conf = root / f'{name}.conf'
                conf.write_text(f'include ${{{name}}}.conf\n')
                self.ae(load_config(str(conf)).scrollback_lines, 4321, name)
            gen = root / 'gen.py'
            gen.write_text('import os\nprint("scrollback_lines", 1000 + (os.environ["SILKIE_OS"] == os.environ["KITTY_OS"]))\n')
            conf = root / 'geninclude.conf'
            conf.write_text(f'geninclude {gen}\n')
            self.ae(load_config(str(conf)).scrollback_lines, 1001)

    def test_build_options(self):
        code = '''
import json, setup
o = setup.Options()
print(json.dumps([o.link_time_optimization, o.egl_library, o.startup_notification_library, o.canberra_library,
                  o.systemd_library, o.fontconfig_library]))
'''

        def check(expected: list[object], **kw: str) -> None:
            self.ae(json.loads(self.run_python(code, clean_env(**kw), cwd=kitty_base_dir)), expected, kw)

        libs = ('EGL', 'STARTUP_NOTIFICATION', 'CANBERRA', 'SYSTEMD', 'FONTCONFIG')
        check([True, None, None, None, None, None])
        check([False, 'k', 'k', 'k', 'k', 'k'], KITTY_NO_LTO='', **{f'KITTY_{x}_LIBRARY': 'k' for x in libs})
        check([False, 's', 's', 's', 's', 's'], SILKIE_NO_LTO='', **{f'SILKIE_{x}_LIBRARY': 's' for x in libs})
        check([False, 's', 's', 's', 's', 's'], **{f'KITTY_{x}_LIBRARY': 'k' for x in libs}, **{f'SILKIE_{x}_LIBRARY': 's' for x in libs},
              SILKIE_NO_LTO='', KITTY_NO_LTO='')
