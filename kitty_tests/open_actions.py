#!/usr/bin/env python
# License: GPL v3 Copyright: 2018, Kovid Goyal <kovid at kovidgoyal.net>


import os
from contextlib import contextmanager

from kitty.utils import get_editor

from .base import BaseTest


@contextmanager
def patch_env(**kw):
    orig = os.environ.copy()
    for k, v in kw.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v
    yield
    os.environ.clear()
    os.environ.update(orig)


class TestOpenActions(BaseTest):
    def test_parsing_of_open_actions(self):
        from kitty.open_actions import KeyAction, actions_for_url

        self.set_options()
        spec = """
protocol file
mime text/*
fragment_matches .
AcTion launch $EDITOR $FILE_PATH $FRAGMENT
action

protocol file
mime text/*
action ignored

ext py,txt
action one
action two
"""

        def actions(url):
            with patch_env(FILE_PATH='notgood'):
                return tuple(actions_for_url(url, spec))

        def single(url, func, *args):
            acts = actions(url)
            self.ae(len(acts), 1)
            self.ae(acts[0].func, func)
            self.ae(acts[0].args, args)

        single('file://hostname/tmp/moo.txt#23', 'launch', *get_editor(), '/tmp/moo.txt', '23')
        single('some thing.txt', 'ignored')
        self.ae(actions('x:///a.txt'), (KeyAction('one', ()), KeyAction('two', ())))

    def test_config_dir_files_ignored(self):
        import tempfile
        from unittest.mock import patch

        from kitty.colors import ThemeColors, ThemeFile
        from kitty.constants import config_dir_files_ignored, set_ignore_config_dir_files
        from kitty.guess_mime_type import clear_mime_cache, user_mime_map
        from kitty.open_actions import clear_caches, load_launch_actions, load_open_actions

        self.set_options()
        with tempfile.TemporaryDirectory() as tdir:
            for name in (ThemeFile.dark.value, ThemeFile.light.value):
                with open(os.path.join(tdir, name), 'w') as f:
                    f.write('background #123456\n')
            for name in ('open-actions.conf', 'launch-actions.conf'):
                with open(os.path.join(tdir, name), 'w') as f:
                    f.write('protocol file\naction one\n')
            with open(os.path.join(tdir, 'mime.types'), 'w') as f:
                f.write('text/x-kitty-test kittytest\n')

            def check(ignored: bool) -> None:
                clear_caches()
                clear_mime_cache()
                set_ignore_config_dir_files(ignored)
                try:
                    self.ae(config_dir_files_ignored(), ignored)
                    tc = ThemeColors()
                    self.ae(tc.refresh(), not ignored)
                    self.ae(tc.has_dark_theme, not ignored)
                    self.ae(tc.has_light_theme, not ignored)
                    self.ae(bool(load_open_actions()), not ignored)
                    self.ae(bool(load_launch_actions()), not ignored)
                    self.ae('.kittytest' in user_mime_map(), not ignored)
                finally:
                    set_ignore_config_dir_files(False)
                    clear_caches()
                    clear_mime_cache()

            with patch('kitty.colors.config_dir', tdir), patch('kitty.open_actions.config_dir', tdir), patch('kitty.constants.config_dir', tdir):
                check(False)
                check(True)

    def test_auto_theme_file_deletion(self):
        import shutil
        import tempfile
        from types import SimpleNamespace
        from unittest.mock import patch

        from kitty.colors import ThemeColors, ThemeFile, theme_file_schemes

        self.set_options()
        boss = SimpleNamespace(args=SimpleNamespace(debug_rendering=False))
        for theme in ThemeFile:
            scheme = theme_file_schemes[theme.value]
            for remove_directory in (False, True):
                with self.subTest(theme=theme.name, remove_directory=remove_directory), tempfile.TemporaryDirectory() as tdir:
                    config_dir = os.path.join(tdir, 'kitty')
                    os.mkdir(config_dir)
                    for other in ThemeFile:
                        with open(os.path.join(config_dir, other.value), 'w') as f:
                            f.write('background #123456\nbackground_image /tmp/kitty-auto-theme.png\ntransparent_background_colors #234567@0.5\n')
                    with (
                        patch('kitty.colors.config_dir', config_dir),
                        patch('kitty.colors.config_dir_files_ignored', return_value=False),
                        patch('kitty.colors.get_boss', return_value=boss),
                        patch('kitty.colors.patch_colors') as apply,
                    ):
                        tc = ThemeColors()
                        self.assertTrue(tc.refresh())
                        self.assertTrue(tc.apply_theme(scheme))
                        mtime = tc.themes[scheme].mtime
                        apply.reset_mock()
                        if remove_directory:
                            shutil.rmtree(config_dir)
                        else:
                            os.unlink(os.path.join(config_dir, theme.value))
                        changed = tc.refresh()
                        self.assertFalse(tc.apply_theme(scheme), 'Deleted automatic theme was applied again')
                        self.assertFalse(tc.has_applied_theme)
                        self.ae(tc.applied_theme, '')
                        for other in ThemeFile:
                            self.ae(getattr(tc, f'has_{other.name}_theme'), not remove_directory and other is not theme)
                        self.assertFalse(tc.refresh())
                        self.assertFalse(tc.on_system_color_scheme_change(scheme))
                        apply.assert_not_called()
                        self.assertTrue(changed)
                        os.makedirs(config_dir, exist_ok=True)
                        path = os.path.join(config_dir, theme.value)
                        with open(path, 'w') as f:
                            f.write('background #abcdef\n')
                        os.utime(path, ns=(mtime, mtime))
                        self.assertTrue(tc.refresh())
                        self.assertTrue(tc.apply_theme(scheme))
                        apply.assert_called_once()
                        self.ae(apply.call_args.args[0]['background'], 0xABCDEF)
                        self.ae(apply.call_args.args[1], ())
                        self.ae(apply.call_args.kwargs['background_image_options']['background_image'], ())
                        self.assertFalse(tc.refresh())

    def test_auto_theme_config_reload(self):
        import tempfile
        from types import SimpleNamespace
        from unittest.mock import patch

        from kitty.colors import ThemeColors, ThemeFile

        self.set_options()
        boss = SimpleNamespace(args=SimpleNamespace(debug_rendering=False))
        with tempfile.TemporaryDirectory() as config_dir:
            path = os.path.join(config_dir, ThemeFile.dark.value)
            with (
                patch('kitty.colors.config_dir', config_dir),
                patch('kitty.colors.config_dir_files_ignored', return_value=False),
                patch('kitty.colors.get_boss', return_value=boss),
                patch('kitty.colors.glfw_get_system_color_theme', return_value='dark'),
                patch('kitty.colors.patch_colors') as apply,
            ):
                tc = ThemeColors()
                # No theme file: reloading applies nothing
                tc.on_config_reload()
                apply.assert_not_called()
                self.ae(tc.applied_theme, '')
                # A theme file created after startup is applied on reload
                with open(path, 'w') as f:
                    f.write('background #111111\n')
                tc.on_config_reload()
                apply.assert_called_once()
                self.ae(apply.call_args.args[0]['background'], 0x111111)
                self.ae(tc.applied_theme, 'dark')
                # Deleting it stops it from being applied
                apply.reset_mock()
                os.unlink(path)
                tc.on_config_reload()
                apply.assert_not_called()
                self.ae(tc.applied_theme, '')
                # Re-creating it applies it again
                with open(path, 'w') as f:
                    f.write('background #222222\n')
                tc.on_config_reload()
                apply.assert_called_once()
                self.ae(apply.call_args.args[0]['background'], 0x222222)
                self.ae(tc.applied_theme, 'dark')
                # A theme only for a different color scheme is not applied
                apply.reset_mock()
                os.rename(path, os.path.join(config_dir, ThemeFile.light.value))
                tc.on_config_reload()
                apply.assert_not_called()
                self.ae(tc.applied_theme, '')
                self.assertTrue(tc.has_light_theme)
                self.assertFalse(tc.has_dark_theme)

    def test_builtin_light_theme_palette(self):
        from kitty.colors import ThemeColors, builtin_light_theme_text, parse_colors
        from kitty.config import parse_config

        def luminance(c: int) -> float:
            def lin(v: int) -> float:
                x = v / 255
                return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4

            return 0.2126 * lin((c >> 16) & 0xFF) + 0.7152 * lin((c >> 8) & 0xFF) + 0.0722 * lin(c & 0xFF)

        def contrast(a: int, b: int) -> float:
            la, lb = sorted((luminance(a), luminance(b)), reverse=True)
            return (la + 0.05) / (lb + 0.05)

        self.set_options()
        text = builtin_light_theme_text()
        # every line in the file is a valid color setting
        lines = [x for x in text.splitlines() if x.strip() and not x.lstrip().startswith('#')]
        conf = parse_config(lines)
        for line in lines:
            self.assertIn(line.split()[0], conf, line)
        spec = parse_colors((text.splitlines(),))[0]
        self.ae(len(spec), len(lines))
        bg = spec['background']
        self.assertGreaterEqual(contrast(spec['foreground'], bg), 7)
        # all 16 ANSI colors meet WCAG AA for text on the background
        for i in range(16):
            c = spec[f'color{i}']
            self.assertGreaterEqual(contrast(c, bg), 4.5, f'color{i}: #{c:06x} has contrast {contrast(c, bg):.2f}')
        self.assertGreaterEqual(contrast(spec['url_color'], bg), 4.5)
        self.assertGreaterEqual(contrast(spec['selection_foreground'], spec['selection_background']), 7)
        self.assertGreaterEqual(contrast(spec['active_tab_foreground'], spec['active_tab_background']), 7)
        self.assertGreaterEqual(contrast(spec['inactive_tab_foreground'], spec['inactive_tab_background']), 4.5)
        # the theme as applied resets everything else to the defaults
        full = ThemeColors().get_builtin_light_theme()
        self.ae(full['background'], bg)
        self.ae(full['color16'], ThemeColors().get_default_colors()['color16'])

    def test_builtin_light_theme(self):
        import tempfile
        from types import SimpleNamespace
        from unittest.mock import patch

        from kitty.colors import ThemeColors, ThemeFile
        from kitty.fast_data_types import Color, get_options

        builtin_bg, default_bg = 0xF6F4EF, 0
        boss = SimpleNamespace(args=SimpleNamespace(debug_rendering=False))
        system_scheme = ['light']
        with (
            tempfile.TemporaryDirectory() as config_dir,
            patch('kitty.colors.config_dir', config_dir),
            patch('kitty.colors.config_dir_files_ignored', return_value=False),
            patch('kitty.colors.get_boss', return_value=boss),
            patch('kitty.colors.glfw_get_system_color_theme', side_effect=lambda *a: system_scheme[0]) as query,
            patch('kitty.colors.patch_colors') as apply,
        ):

            def applied(bg: int) -> None:
                apply.assert_called_once()
                self.ae(apply.call_args.args[0]['background'], bg)
                if bg in (builtin_bg, default_bg):
                    # the built-in theme leaves transparent background colors and the background image alone
                    self.assertIsNone(apply.call_args.args[1])
                    self.assertIsNone(apply.call_args.kwargs.get('background_image_options'))
                self.assertTrue(apply.call_args.args[2], 'theme not applied as the configured colors')
                apply.reset_mock()

            def switch(scheme: str) -> bool:
                system_scheme[0] = scheme
                return tc.on_system_color_scheme_change(scheme)  # type: ignore

            # Off by default: nothing happens without theme files
            self.set_options()
            self.assertFalse(get_options().automatic_light_theme)
            tc = ThemeColors()
            tc.on_config_reload()
            apply.assert_not_called()
            self.assertFalse(switch('light'))
            self.assertFalse(switch('dark'))
            apply.assert_not_called()

            # Enabled, no theme files: switching the system scheme applies and removes the built-in theme
            system_scheme[0] = 'light'
            self.set_options({'automatic_light_theme': True})
            tc = ThemeColors()
            tc.on_config_reload()
            applied(builtin_bg)
            self.ae((tc.applied_theme, tc.applied_builtin_theme, tc.has_applied_theme), ('light', True, True))
            self.assertTrue(switch('dark'))
            applied(default_bg)
            self.ae((tc.applied_theme, tc.applied_builtin_theme), ('', False))
            self.assertFalse(switch('no_preference'))
            apply.assert_not_called()
            self.assertTrue(switch('light'))
            applied(builtin_bg)
            self.assertTrue(switch('no_preference'))
            applied(default_bg)
            self.assertFalse(switch('dark'))
            apply.assert_not_called()

            # A user light theme file always wins
            switch('light')
            applied(builtin_bg)
            path = os.path.join(config_dir, ThemeFile.light.value)
            with open(path, 'w') as f:
                f.write('background #abcdef\n')
            self.assertTrue(switch('light'))
            applied(0xABCDEF)
            self.ae((tc.applied_theme, tc.applied_builtin_theme), ('light', False))
            # Switching away from a user theme file is unchanged: nothing to remove
            self.assertFalse(switch('dark'))
            apply.assert_not_called()
            os.unlink(path)
            # A user dark theme file with no light one: built-in light, then the dark file
            with open(os.path.join(config_dir, ThemeFile.dark.value), 'w') as f:
                f.write('background #111111\n')
            self.assertTrue(switch('light'))
            applied(builtin_bg)
            self.assertTrue(switch('dark'))
            applied(0x111111)
            self.ae((tc.applied_theme, tc.applied_builtin_theme), ('dark', False))
            os.unlink(os.path.join(config_dir, ThemeFile.dark.value))

            # Config reload re-evaluates with the reloaded colors
            switch('light')
            applied(builtin_bg)
            tc.on_config_reload()
            applied(builtin_bg)

            # The option turned off restores the old behavior: nothing happens without theme files
            for opts in (
                {'automatic_light_theme': False},
                {'automatic_light_theme': True, 'background': Color(1, 2, 3)},
                {'automatic_light_theme': True, 'color1': Color(1, 2, 3)},
            ):
                self.set_options(opts)
                tc = ThemeColors()
                system_scheme[0] = 'light'
                tc.on_config_reload()
                apply.assert_not_called()
                self.assertFalse(switch('dark'))
                self.assertFalse(switch('light'))
                apply.assert_not_called()
                self.ae(tc.applied_theme, '')

            # Startup
            for scheme, enabled, expected_bg in (('light', True, builtin_bg), ('dark', True, default_bg), ('light', False, default_bg), ('light', None, default_bg)):
                opts = self.set_options(None if enabled is None else {'automatic_light_theme': enabled})
                tc = ThemeColors()
                system_scheme[0] = scheme
                query.reset_mock()
                tc.refresh()
                tc.patch_opts(opts)
                self.ae(int(opts.background), expected_bg)
                self.ae(tc.applied_builtin_theme, expected_bg == builtin_bg)
                if not enabled:
                    query.assert_not_called()  # no theme files and nothing to do, so the system is not queried
            self.ae(int(get_options().background), default_bg)

    def test_builtin_light_theme_keeps_program_colors(self):
        from types import SimpleNamespace
        from unittest.mock import patch

        from kitty.colors import ThemeColors
        from kitty.fast_data_types import COLOR_LAYER_OVERRIDE, patch_color_profiles

        s = self.create_screen()
        s.color_profile.reload_from_opts(self.set_options({'automatic_light_theme': True}))
        window = SimpleNamespace(id=1, screen=s, refresh=lambda *a, **k: None, notify_child_of_color_change=lambda: None, session_color_schemes=None)
        boss = SimpleNamespace(
            args=SimpleNamespace(debug_rendering=False), all_windows=[window], all_tab_managers=[], default_bg_changed_for=lambda *a: None
        )
        system_scheme = ['dark']
        with (
            patch('kitty.colors.config_dir_files_ignored', return_value=True),
            patch('kitty.colors.get_boss', return_value=boss),
            patch('kitty.colors.glfw_get_system_color_theme', side_effect=lambda *a: system_scheme[0]),
        ):
            tc = ThemeColors()
            tc.on_config_reload()
            cp = s.color_profile
            self.ae((int(cp.default_fg), int(cp.default_bg)), (0xDDDDDD, 0))
            # the program sets the background color and a palette color
            patch_color_profiles({'background': 0x123456}, None, (cp,), COLOR_LAYER_OVERRIDE)
            cp.set_color(4, 0x010203)
            self.ae(int(cp.default_bg), 0x123456)
            for scheme, fg, color5 in (('light', 0x2B2F36, 0x8E3A9E), ('dark', 0xDDDDDD, 0xCB1ED1), ('light', 0x2B2F36, 0x8E3A9E)):
                system_scheme[0] = scheme
                self.assertTrue(tc.on_system_color_scheme_change(scheme))  # type: ignore
                self.ae(int(cp.default_fg), fg)
                self.ae(cp.as_dict()['color5'], color5)
                self.ae(int(cp.default_bg), 0x123456, f'program color lost when switching to {scheme}')
                self.ae(cp.as_dict()['color4'], 0x010203, f'program color lost when switching to {scheme}')
            # resetting the program color reveals the theme color
            cp.reset_color(4)
            self.ae(cp.as_dict()['color4'], 0x2A5BB0)

    def test_color_scheme_option_with_theme_layers(self):
        # Silkie's color_scheme option and e-ink mode with the color layers and
        # the built-in light theme
        from types import SimpleNamespace
        from unittest.mock import patch

        from kitty.colors import SessionColorSchemes, ThemeColors, sync_eink_colors
        from kitty.fast_data_types import COLOR_LAYER_WINDOW, patch_color_profiles, set_eink_mode

        builtin_bg = 0xF6F4EF
        s = self.create_screen()
        cp = s.color_profile
        notified = []
        sessions = SessionColorSchemes()

        def apply_session_color_scheme(scheme=None):
            sessions.apply(cp, scheme or system_scheme[0])

        window = SimpleNamespace(
            id=1, screen=s, refresh=lambda *a, **k: None, notify_child_of_color_change=lambda: notified.append(1),
            session_color_schemes=sessions, apply_session_color_scheme=apply_session_color_scheme,
        )
        boss = SimpleNamespace(
            args=SimpleNamespace(debug_rendering=False), all_windows=[window], all_tab_managers=[], default_bg_changed_for=lambda *a: None
        )
        system_scheme = ['light']
        with (
            patch('kitty.colors.config_dir_files_ignored', return_value=True),
            patch('kitty.colors.get_boss', return_value=boss),
            patch('kitty.colors.get_options', side_effect=lambda: opts),
            patch('kitty.colors.glfw_get_system_color_theme', side_effect=lambda *a: system_scheme[0]),
        ):
            # color_scheme dark wins over the built-in light theme of a light system
            opts = self.set_options({'color_scheme': 'dark', 'automatic_light_theme': True})
            tc = ThemeColors()
            tc.patch_opts(opts)
            self.ae((int(opts.foreground), int(opts.background)), (0xFFFFFF, 0))
            self.ae((tc.applied_theme, tc.builtin_light_theme_allowed), ('', True))
            self.assertFalse(tc.on_system_color_scheme_change('light'))
            cp.reload_from_opts(opts)
            # launch --color and session colors are on layers above the configured colors
            patch_color_profiles({'color1': 0x010101}, None, (cp,), COLOR_LAYER_WINDOW)
            sessions.add(1, {'light': {'foreground': 0x020202}, 'dark': {'foreground': 0x030303}})
            apply_session_color_scheme()
            self.ae(int(cp.default_fg), 0x020202)
            # back to auto: the built-in light theme, under the window and session colors
            opts.color_scheme = 'auto'
            del notified[:]
            tc.apply_color_scheme()
            self.ae((tc.applied_theme, tc.applied_builtin_theme), ('light', True))
            self.ae(int(cp.default_bg), builtin_bg)
            self.ae((int(cp.default_fg), cp.as_dict()['color1']), (0x020202, 0x010101))
            self.assertTrue(notified, 'the program was not told its colors changed')
            # e-ink mode is black on white over every layer, as a layer of its own above them
            set_eink_mode(True)
            sync_eink_colors()
            self.ae((int(cp.default_fg), int(cp.default_bg), tc.applied_theme), (0, 0xFFFFFF, 'light'))
            # it forces the colors it names, the palette is left alone
            self.ae(cp.as_dict()['color1'], 0x010101)
            # the theme still follows the system, under it
            system_scheme[0] = 'dark'
            self.assertTrue(tc.on_system_color_scheme_change('dark'))
            self.ae(int(cp.default_bg), 0xFFFFFF)
            system_scheme[0] = 'light'
            self.assertTrue(tc.on_system_color_scheme_change('light'))
            self.ae(int(cp.default_bg), 0xFFFFFF)
            # and when it is turned off the theme and the session colors come back
            set_eink_mode(False)
            sync_eink_colors()
            self.ae((int(cp.default_fg), int(cp.default_bg)), (0x020202, builtin_bg))

    def test_mime_cache(self):
        import shutil
        import subprocess
        import tempfile

        from kitty.fast_data_types import clear_mime_cache_data, mime_type_aliases, mime_type_for_filename
        from kitty.guess_mime_type import guess_type, mime_types_for_matching
        from kitty.open_actions import KeyAction, actions_for_url

        if not shutil.which('update-mime-database'):
            self.skipTest('update-mime-database not available')

        def build_cache(data_dir: str, xml: str) -> None:
            pkgs = os.path.join(data_dir, 'mime', 'packages')
            os.makedirs(pkgs)
            with open(os.path.join(pkgs, 'kitty-test.xml'), 'w') as f:
                f.write(MIME_CACHE_TEST_PACKAGE.replace('MIME_TYPES', xml))
            with patch_env(PKGSYSTEM_ENABLE_FSYNC='0'):
                subprocess.run(['update-mime-database', os.path.join(data_dir, 'mime')], check=True, capture_output=True)

        with tempfile.TemporaryDirectory() as tdir:
            user, system = os.path.join(tdir, 'user'), os.path.join(tdir, 'system')
            build_cache(system, MIME_CACHE_TEST_TYPES)
            build_cache(
                user,
                (
                    '<mime-type type="text/x-kt-user"><glob pattern="*.kuser"/><glob pattern="*.kext.override"/></mime-type>'
                    '<mime-type type="text/x-kt-ext"><alias type="text/x-kt-user-alias"/></mime-type>'
                ),
            )
            build_cache(os.path.join(tdir, 'ignored'), '<mime-type type="text/x-kt-ignored"><glob pattern="*.kuser"/></mime-type>')
            try:
                with patch_env(XDG_DATA_HOME=user, XDG_DATA_DIRS=f'relative/dir:{system}/:{tdir}/missing:{tdir}/ignored'):
                    clear_mime_cache_data()
                    for name, expected in MIME_CACHE_TEST_EXPECTATIONS.items():
                        self.ae(mime_type_for_filename(name), expected, name)
                    self.ae(guess_type('/a/b.kext'), 'text/x-kt-ext')
                    self.ae(guess_type('/a/b.kjson'), 'text/json')
                    self.assertRaises(TypeError, mime_type_for_filename, b'a.kext')

                    # aliases
                    ext_names = ('text/x-kt-ext', 'text/x-kt-user-alias', 'application/x-kt-older', 'text/x-kt-old')
                    self.ae(mime_type_aliases('text/x-kt-ext'), ext_names)
                    self.ae(mime_type_aliases('text/x-kt-old'), ext_names)
                    self.ae(mime_type_aliases('unknown/x-kt'), ('unknown/x-kt',))
                    self.assertRaises(TypeError, mime_type_aliases, b'text/x-kt-ext')
                    self.ae(mime_types_for_matching('b.kjson', 'text/json'), {'text/json', 'application/json', 'application/x-kt-json'})
                    self.ae(guess_type('/a/Makefile'), 'text/x-kt-make')
                    self.ae(mime_types_for_matching('/a/Makefile', 'text/x-kt-make'), {'text/x-kt-make', 'text/makefile'})
                    self.set_options()
                    spec = """
mime text/x-kt-old
action old

mime text/MakeFile
action make

mime image/*, application/x-kt-j*
action json
"""
                    for url, action in {'/tmp/b.kext': 'old', 'file:///tmp/Makefile': 'make', 'b.kjson': 'json', 'b.kuser': ''}.items():
                        self.ae(tuple(actions_for_url(url, spec)), (KeyAction(action, ()),) if action else (), url)
                truncated = os.path.join(tdir, 'truncated')
                os.makedirs(os.path.join(truncated, 'mime'))
                with open(os.path.join(system, 'mime', 'mime.cache'), 'rb') as src, open(os.path.join(truncated, 'mime', 'mime.cache'), 'wb') as dest:
                    dest.write(src.read()[:48])
                with patch_env(XDG_DATA_HOME=truncated, XDG_DATA_DIRS=f'{tdir}/missing'):
                    clear_mime_cache_data()
                    self.assertIsNone(mime_type_for_filename('a.kext'))

                # strings in the cache that are not valid UTF-8 are ignored
                invalid = os.path.join(tdir, 'invalid')
                os.makedirs(os.path.join(invalid, 'mime'))
                with open(os.path.join(system, 'mime', 'mime.cache'), 'rb') as src, open(os.path.join(invalid, 'mime', 'mime.cache'), 'wb') as dest:
                    data = src.read()
                    self.assertIn(b'text/x-kt-ext\0', data)
                    dest.write(data.replace(b'text/x-kt-ext\0', b'text/x-kt-\xffxt\0'))
                with patch_env(XDG_DATA_HOME=invalid, XDG_DATA_DIRS=f'{tdir}/missing'):
                    clear_mime_cache_data()
                    self.assertIsNone(mime_type_for_filename('a.kext'))
                    self.ae(mime_type_for_filename('a.kjson'), 'application/json')
                    self.ae(mime_type_aliases('text/x-kt-old'), ('application/x-kt-older', 'text/x-kt-old'))
                    guess_type('/a/b.kext')

                # lone surrogates cannot be encoded and so do not match
                with patch_env(XDG_DATA_HOME=user, XDG_DATA_DIRS=system):
                    clear_mime_cache_data()
                    self.assertIsNone(mime_type_for_filename('a\ud800.kext'))
                    self.ae(mime_type_for_filename('a\udcff.kext'), 'text/x-kt-ext')
                    self.ae(mime_type_aliases('text/\ud800'), ('text/\ud800',))
                    guess_type('/a/b\ud800.kext')
            finally:
                clear_mime_cache_data()


MIME_CACHE_TEST_PACKAGE = """<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
MIME_TYPES
</mime-info>
"""

# Keep in sync with tools/utils/mime_cache_test.go
MIME_CACHE_TEST_TYPES = """
<mime-type type="text/x-kt-literal"><glob pattern="KittyLiteral"/></mime-type>
<mime-type type="text/x-kt-cs"><glob pattern="*.KCS" case-sensitive="true"/></mime-type>
<mime-type type="text/x-kt-ext"><glob pattern="*.kext"/><alias type="text/x-kt-old"/><alias type="application/x-kt-older"/></mime-type>
<mime-type type="text/x-kt-long"><glob pattern="*.long.kext"/></mime-type>
<mime-type type="text/x-kt-light"><glob pattern="*.kw" weight="40"/></mime-type>
<mime-type type="text/x-kt-heavy"><glob pattern="*.kw" weight="80"/></mime-type>
<mime-type type="text/x-kt-glob"><glob pattern="kglob[0-9].*"/></mime-type>
<mime-type type="text/x-kt-negated-glob"><glob pattern="kneg[!0-9].*"/></mime-type>
<mime-type type="text/x-kt-unicode"><glob pattern="*.kü"/></mime-type>
<mime-type type="application/json"><glob pattern="*.kjson"/><alias type="application/x-kt-json"/></mime-type>
<mime-type type="text/x-kt-make"><glob pattern="Makefile" case-sensitive="true"/></mime-type>
<mime-type type="text/x-kt-system"><glob pattern="*.kuser"/></mime-type>
"""

MIME_CACHE_TEST_EXPECTATIONS = {
    'KittyLiteral': 'text/x-kt-literal',
    'kittyliteral': 'text/x-kt-literal',
    'KITTYLITERAL': 'text/x-kt-literal',
    'xKittyLiteral': None,
    'a.KCS': 'text/x-kt-cs',
    'a.kcs': None,
    'a.kext': 'text/x-kt-ext',
    'A.KEXT': 'text/x-kt-ext',
    '/some/dir/a.kext': 'text/x-kt-ext',
    'dir.kext/noext': None,
    'x.long.kext': 'text/x-kt-long',
    'x.LONG.KEXT': 'text/x-kt-long',
    'xlong.kext': 'text/x-kt-ext',
    'a.kw': 'text/x-kt-heavy',
    'kglob1.txt': 'text/x-kt-glob',
    'KGLOB1.TXT': 'text/x-kt-glob',
    'kglobx.txt': None,
    'knegx.txt': 'text/x-kt-negated-glob',
    'kneg1.txt': None,
    'a.kü': 'text/x-kt-unicode',
    'a.KÜ': 'text/x-kt-unicode',
    'a.kuser': 'text/x-kt-user',
    'a.kext.override': 'text/x-kt-user',
    '': None,
    'a\0.kext': None,
    'notmatched': None,
}
