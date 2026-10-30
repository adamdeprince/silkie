#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import os
import tempfile
from unittest import mock

from kitty import profiles, settings_model
from kitty.conf.types import Option
from kitty.config import load_config
from kitty.constants import is_macos
from kitty.fast_data_types import Color, set_options
from kitty.options.definition import definition

from .base import BaseTest


class TestSettingsUI(BaseTest):

    def setUp(self):
        super().setUp()
        self.tdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tdir.cleanup)
        self.conf = os.path.join(self.tdir.name, 'kitty.conf')
        self.settings = os.path.join(self.tdir.name, 'settings.conf')
        for patcher in (
            mock.patch.object(settings_model, 'defconf', self.conf),
            mock.patch.object(settings_model, 'settings_conf', self.settings),
            mock.patch.object(profiles, 'config_dir', self.tdir.name),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)
        profiles.clear_caches()
        set_options(load_config())
        self.addCleanup(lambda: set_options(load_config()))

    def fields(self):
        ans = {}
        for page in settings_model.settings_pages():
            for section in page['sections']:
                for f in section['fields']:
                    ans[f['name']] = f
        return ans

    def test_every_option_has_a_control(self):
        fields = self.fields()
        missing = []
        for section in settings_model.sections_of_definition():
            for item in section.items:
                if settings_model.visible_option(item) and item.name not in fields:
                    missing.append(item.name)
        self.ae(missing, [])

        def walk(group):
            for item in group.items:
                if isinstance(item, Option):
                    yield item
                elif hasattr(item, 'items'):
                    yield from walk(item)

        for opt in walk(definition.root_group):
            if opt.name in ('default_profile',):
                continue
            if settings_model.applies_to_this_platform(opt.name):
                self.assertIn(opt.name, fields, f'The option {opt.name} is missing from the Settings window')
        self.assertNotIn('macos_titlebar_color' if not is_macos else 'wayland_titlebar_color', fields)
        kinds = {f['kind'] for f in fields.values()}
        self.ae(kinds, {'bool', 'choice', 'color', 'font', 'int', 'float', 'text', 'lines', 'shortcut'})

    def test_field_values_are_usable_by_native_controls(self):
        fields = self.fields()
        for f in fields.values():
            self.assertIsInstance(f['value'], str, f['name'])
            self.assertIsInstance(f['default'], str, f['name'])
            if f['kind'] == 'color':
                self.assertRegex(f['value'], r'^(#[0-9a-f]{6}|none)$', f['name'])
            if f['kind'] == 'choice':
                self.assertIn(f['value'], f['choices'], f['name'])
            if f['kind'] == 'bool':
                self.assertIn(f['value'].lower(), ('yes', 'no', 'y', 'n', 'true', 'false'), f['name'])
            self.assertNotIn(':opt:', f['help'], f['name'])
        self.ae(fields['background']['kind'], 'color')
        self.ae(fields['cursor_shape']['title'], 'Cursor shape')

    def test_setting_a_value_layers_over_kitty_conf(self):
        with open(self.conf, 'w') as f:
            f.write('# my own comment\nscrollback_lines 5000\nenable_audio_bell no\n')
        fields = self.fields()
        self.ae(fields['scrollback_lines']['value'], '5000')
        self.assertFalse(fields['scrollback_lines']['is_set'])
        self.ae(settings_model.set_global_value('scrollback_lines', '9000'), '')
        fields = self.fields()
        self.ae(fields['scrollback_lines']['value'], '9000')
        self.assertTrue(fields['scrollback_lines']['is_set'])
        opts = load_config(self.conf, self.settings)
        self.ae(opts.scrollback_lines, 9000)
        self.assertFalse(opts.enable_audio_bell)
        # kitty.conf is never touched
        with open(self.conf) as f:
            self.assertIn('# my own comment', f.read())
        settings_model.reset_global_value('scrollback_lines')
        self.ae(self.fields()['scrollback_lines']['value'], '5000')

    def test_invalid_values_are_rejected_and_not_stored(self):
        self.assertTrue(settings_model.set_global_value('background', 'not a color'))
        self.assertTrue(settings_model.set_global_value('no_such_option', '1'))
        self.assertFalse(os.path.exists(self.settings) and 'not a color' in open(self.settings).read())

    def test_shortcuts(self):
        self.ae(settings_model.set_global_value('map:new_tab', 'ctrl+alt+n, ctrl+alt+m'), '')
        layer = settings_model.read_layer(self.settings)
        self.assertIn('ctrl+alt+n new_tab', layer['map'])
        self.assertIn('ctrl+alt+m new_tab', layer['map'])
        self.assertTrue(any(line.endswith(' no_op') for line in layer['map']), 'default keys must be unbound')
        opts = load_config(self.settings)
        self.assertTrue(opts.keyboard_modes)
        # User lines without a row of their own are kept apart from generated ones
        self.ae(settings_model.set_global_value('map', 'ctrl+alt+x launch htop'), '')
        layer = settings_model.read_layer(self.settings)
        self.assertIn('ctrl+alt+x launch htop', layer['map'])
        self.assertIn('ctrl+alt+n new_tab', layer['map'])
        settings_model.reset_global_value('map:new_tab')
        layer = settings_model.read_layer(self.settings)
        self.ae(layer['map'], ['ctrl+alt+x launch htop'])
        self.assertTrue(settings_model.set_global_value('map:new_tab', 'not a key+++'))

    def test_font_values(self):
        self.ae(settings_model.font_family_from_spec('family="Fira Code" features="+zero"'), 'Fira Code')
        self.ae(settings_model.font_family_from_spec('monospace'), 'monospace')
        self.ae(settings_model.font_spec_from_family('Fira Code'), 'family="Fira Code"')
        self.ae(settings_model.font_spec_from_family('auto'), 'auto')
        self.ae(settings_model.set_global_value('font_family', 'Fira Code'), '')
        self.ae(self.fields()['font_family']['value'], 'Fira Code')

    def test_profile_files(self):
        p = profiles.create_profile('My Work!')
        self.ae(p.id, 'my-work')
        self.ae(profiles.create_profile('My Work').id, 'my-work-2')
        self.ae(profiles.set_profile_value(p, 'command', 'fish -l'), '')
        self.ae(profiles.set_profile_value(p, 'directory', '~/work'), '')
        self.ae(profiles.set_profile_value(p, 'env', 'A=1\nB=x=y\n'), '')
        self.ae(profiles.set_profile_value(p, 'background', '#102030'), '')
        self.ae(profiles.set_profile_value(p, 'font_size', '16'), '')
        self.ae(profiles.set_profile_value(p, 'font_family', 'Fira Code'), '')
        self.assertTrue(profiles.set_profile_value(p, 'background', 'nonsense'))
        self.assertTrue(profiles.set_profile_value(p, 'env', 'no equals'))
        self.assertTrue(profiles.set_profile_value(p, 'clear_all_shortcuts', 'yes'), 'not a per-profile option')
        self.assertTrue(profiles.set_profile_value(p, 'name', ' '))

        q = profiles.load_profiles()['my-work']
        self.ae((q.name, q.command, q.directory), ('My Work!', 'fish -l', '~/work'))
        self.ae(profiles.environment_of(q), {'A': '1', 'B': 'x=y'})
        self.ae(profiles.command_of(q), ['fish', '-l'])
        self.ae(profiles.directory_of(q), os.path.expanduser('~/work'))
        parsed = profiles.parsed_options(q)
        self.ae(parsed['background'], Color(0x10, 0x20, 0x30))
        self.ae(parsed['font_size'], 16)
        profiles.reset_profile_value(q, 'background')
        profiles.clear_caches()
        self.assertNotIn('background', profiles.parsed_options(profiles.load_profiles()['my-work']))
        copy = profiles.create_profile('Copy', copy_of=q)
        self.ae(profiles.environment_of(copy), {'A': '1', 'B': 'x=y'})
        self.assertFalse(profiles.delete_profile(profiles.DEFAULT_PROFILE_ID))
        self.assertTrue(profiles.delete_profile('my-work'))
        self.assertNotIn('my-work', profiles.load_profiles())
        self.assertIn(profiles.DEFAULT_PROFILE_ID, profiles.load_profiles())

    def test_profile_model(self):
        p = profiles.create_profile('Ops')
        profiles.set_profile_value(p, 'foreground', '#ff0000')
        m = profiles.profile_model()
        self.ae([x['id'] for x in m['profiles']], ['default', 'ops'])
        self.assertFalse(m['profiles'][0]['can_delete'])
        self.assertTrue(m['profiles'][0]['is_default'])
        ops = m['profiles'][1]
        pages = {pg['title']: {f['name']: f for f in pg['fields']} for pg in ops['pages']}
        self.ae(set(pages), {'General', 'Colors', 'Text', 'Window', 'Terminal'})
        fg = pages['Colors']['foreground']
        self.ae((fg['value'], fg['is_set'], fg['scope']), ('#ff0000', True, 'profile:ops'))
        self.assertFalse(pages['Colors']['background']['is_set'])
        # Every option named per profile exists, renaming one must not silently drop it
        for _, names in profiles.PROFILE_PAGES:
            for name in names:
                self.assertIsNotNone(settings_model.option_by_name(name), name)
