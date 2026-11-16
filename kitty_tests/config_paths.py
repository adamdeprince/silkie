#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import json
import os
import subprocess
import tempfile
from pathlib import Path

from kitty.constants import kitty_exe

from .base import BaseTest


class TestConfigPaths(BaseTest):

    def test_silkie_configuration_isolation(self):
        with tempfile.TemporaryDirectory() as tdir:
            root = Path(tdir)
            env = {k: v for k, v in os.environ.items() if k not in (
                'SILKIE_CONFIG_DIRECTORY', 'KITTY_CONFIG_DIRECTORY', 'KITTY_LAUNCHED_BY_LAUNCH_SERVICES',
            )}
            env.update(HOME=tdir, XDG_CONFIG_HOME=str(root / 'xdg'), XDG_CONFIG_DIRS='')

            def write(path, text):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)

            def read(*args):
                code = '''
import json
from kitty.constants import config_dir, defconf
from kitty.cli import SYSTEM_CONF, create_opts, default_config_paths, parse_args
args, _ = parse_args(ARGS)
print(json.dumps([config_dir, defconf, SYSTEM_CONF, default_config_paths(args.config), create_opts(args).scrollback_lines]))
'''
                code = code.replace('ARGS', repr(list(args)))
                cp = subprocess.run([kitty_exe(), '+runpy', code], env=env, check=True, capture_output=True, text=True)
                return json.loads(cp.stdout)

            kitty_dir = root / 'xdg' / 'kitty'
            write(kitty_dir / 'kitty.conf', 'scrollback_lines 1111\n')
            env['KITTY_CONFIG_DIRECTORY'] = str(kitty_dir)
            silkie_dir = root / 'xdg' / 'silkie'
            result = read()
            self.ae(result[0], str(silkie_dir))
            self.ae(result[1], str(silkie_dir / 'silkie.conf'))
            self.ae(result[2], '/etc/xdg/silkie/silkie.conf')
            self.assertIn(str(silkie_dir / 'settings.conf'), result[3])
            self.assertNotEqual(result[4], 1111)

            # The old filename in the new directory must also be ignored.
            write(silkie_dir / 'kitty.conf', 'scrollback_lines 1111\n')
            write(silkie_dir / 'silkie.conf', 'include extra.conf\n')
            write(silkie_dir / 'extra.conf', 'scrollback_lines 2222\n')
            self.ae(read()[4], 2222)
            write(silkie_dir / 'settings.conf', 'scrollback_lines 3333\n')
            self.ae(read()[4], 3333)

            custom = root / 'custom'
            env['SILKIE_CONFIG_DIRECTORY'] = str(custom)
            write(custom / 'silkie.conf', 'scrollback_lines 4444\n')
            result = read()
            self.ae(result[0], str(custom))
            self.ae(result[4], 4444)
            self.ae(read('--config', str(kitty_dir / 'kitty.conf'))[4], 1111)
            result = read('--config', 'NONE')
            self.ae(result[3], [])
            self.assertNotEqual(result[4], 4444)

    def test_config_directory_search(self):
        with tempfile.TemporaryDirectory() as tdir:
            root = Path(tdir)
            env = {k: v for k, v in os.environ.items() if k not in (
                'SILKIE_CONFIG_DIRECTORY', 'KITTY_LAUNCHED_BY_LAUNCH_SERVICES',
            )}
            env.update(HOME=tdir, XDG_CONFIG_HOME=str(root / 'xdg'), XDG_CONFIG_DIRS=str(root / 'system'))

            def check(expected):
                cp = subprocess.run([kitty_exe(), '+runpy', 'from kitty.constants import config_dir; print(config_dir)'],
                                    env=env, check=True, capture_output=True, text=True)
                self.ae(cp.stdout.strip(), str(expected))

            def config(base):
                path = root / base / 'silkie'
                path.mkdir(parents=True, exist_ok=True)
                (path / 'silkie.conf').touch()
                return path

            system = config('system')
            # Directory lists can exceed PATH_MAX; individual paths remain short.
            env['XDG_CONFIG_DIRS'] = ':'.join([str(root / 'missing')] * 1024 + [str(system.parent)])
            check(system)
            home = config('.config')
            check(home)
            check(config('xdg'))
            env['SILKIE_CONFIG_DIRECTORY'] = '~/custom'
            check(root / 'custom')
