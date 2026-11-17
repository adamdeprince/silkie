#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

"""
Profiles, in the sense of iTerm2: named sets of settings for a kind of work, for
example a profile with its own colors, font and shell for a production machine.

A profile is a file in the profiles folder of the configuration directory, in the
syntax of silkie.conf plus a few profile_* keys. It sets only what differs from the
global settings. Most of kitty's options are process wide, and native tabs make every
tab its own OS window, so a profile holds the options that can really differ between
OS windows: PROFILE_PAGES lists them, and applying them is below.
"""

import os
import re
from collections.abc import Iterable, Sequence
from contextlib import suppress
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from .conf.types import Option
from .constants import config_dir, config_dir_files_ignored
from .fast_data_types import COLOR_LAYER_WINDOW, get_options
from .settings_model import (
    Layer,
    atomic_write_text,
    field_for_option,
    font_spec_from_family,
    field_kind,
    global_layers,
    option_by_name,
    read_layer,
    validate,
)

if TYPE_CHECKING:
    from .tabs import TabManager
    from .window import Window

DEFAULT_PROFILE_ID = 'default'
ANSI_COLORS = tuple(f'color{i}' for i in range(16))
PROFILE_PAGES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ('Colors', ('foreground', 'background', 'selection_foreground', 'selection_background', 'cursor', 'cursor_text_color', 'url_color') + ANSI_COLORS),
    ('Text', ('font_family', 'bold_font', 'italic_font', 'bold_italic_font', 'font_size', 'cursor_shape')),
    ('Window', ('background_opacity', 'initial_window_width', 'initial_window_height', 'window_padding_width', 'enabled_layouts')),
    ('Terminal', ('scrollback_lines',)),
)
OS_WINDOW_KEYS = frozenset(('font_family', 'bold_font', 'italic_font', 'bold_italic_font', 'font_size', 'background_opacity'))
FONT_KEYS = ('font_family', 'bold_font', 'italic_font', 'bold_italic_font')
PROFILE_OPTION_NAMES = frozenset(name for _, names in PROFILE_PAGES for name in names)
META_KEYS = {'profile_name': 'name', 'profile_command': 'command', 'profile_directory': 'directory', 'profile_title': 'title'}
HEADER = '''\
# A profile, edited by Settings > Profiles. Editing it by hand is fine, comments are not preserved.
# Only what differs from the global settings is set here.'''


def profiles_dir() -> str:
    return os.path.join(config_dir, 'profiles')


@dataclass
class Profile:
    id: str
    name: str
    command: str = ''
    directory: str = ''
    title: str = ''
    env: list[str] = field(default_factory=list)
    # The raw values, by key
    options: Layer = field(default_factory=dict)

    @property
    def path(self) -> str:
        return os.path.join(profiles_dir(), f'{self.id}.conf')

    def save(self) -> None:
        lines = [HEADER]
        for key, attr in META_KEYS.items():
            if val := getattr(self, attr):
                lines.append(f'{key} {val}')
        lines.extend(f'profile_env {e}' for e in self.env)
        for key, vals in self.options.items():
            lines.extend(f'{key} {v}' for v in vals)
        atomic_write_text(self.path, '\n'.join(lines) + '\n')


def parse_profile(profile_id: str, lines: Iterable[str]) -> tuple[Profile, list[str]]:
    p = Profile(profile_id, profile_id)
    rest = []
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, _, val = line.partition(' ')
        val = val.strip()
        if key in META_KEYS:
            setattr(p, META_KEYS[key], val)
        elif key == 'profile_env':
            p.env.append(val)
        else:
            rest.append(line)
    return p, rest


def load_profile(path: str) -> Profile:
    profile_id = os.path.splitext(os.path.basename(path))[0]
    with open(path, encoding='utf-8', errors='replace') as f:
        p, rest = parse_profile(profile_id, f)
    for line in rest:
        key, _, val = line.partition(' ')
        p.options.setdefault(key, []).append(val.strip())
    return p


def load_profiles() -> dict[str, Profile]:
    ' All profiles by id, the default profile first and always present '
    ans = {DEFAULT_PROFILE_ID: Profile(DEFAULT_PROFILE_ID, 'Default')}
    if config_dir_files_ignored():  # --config=NONE
        return ans
    with suppress(OSError):
        for name in sorted(os.listdir(profiles_dir()), key=str.lower):
            if name.endswith('.conf'):
                with suppress(OSError):
                    p = load_profile(os.path.join(profiles_dir(), name))
                    ans[p.id] = p
    return ans


def slug_for(name: str, existing: Iterable[str]) -> str:
    base = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-') or 'profile'
    taken = set(existing)
    ans, n = base, 1
    while ans in taken:
        n += 1
        ans = f'{base}-{n}'
    return ans


def create_profile(name: str, copy_of: Profile | None = None) -> Profile:
    existing = load_profiles()
    p = Profile(slug_for(name, existing), name)
    if copy_of is not None:
        p.command, p.directory, p.title, p.env = copy_of.command, copy_of.directory, copy_of.title, list(copy_of.env)
        p.options = {k: list(v) for k, v in copy_of.options.items()}
    p.save()
    return p


def delete_profile(profile_id: str) -> bool:
    if profile_id == DEFAULT_PROFILE_ID:
        return False
    with suppress(FileNotFoundError):
        os.remove(os.path.join(profiles_dir(), f'{profile_id}.conf'))
    return True


def set_profile_value(profile: Profile, name: str, value: str) -> str:
    ' Store a change from the Settings window. Returns an error message, empty on success. '
    if name in META_KEYS.values():
        if name == 'name' and not value.strip():
            return 'A profile needs a name'
        setattr(profile, name, value.strip())
    elif name == 'env':
        lines = [line.strip() for line in value.splitlines() if line.strip()]
        for line in lines:
            if '=' not in line:
                return f'Not in the form NAME=value: {line}'
        profile.env = lines
    elif name in PROFILE_OPTION_NAMES:
        opt = option_by_name(name)
        if isinstance(opt, Option) and field_kind(opt) == 'font':
            value = font_spec_from_family(value)
        if err := validate(name, value):
            return err
        profile.options[name] = [value]
    else:
        return f'{name} cannot be set per profile'
    profile.save()
    return ''


def reset_profile_value(profile: Profile, name: str) -> None:
    if name in META_KEYS.values() and name != 'name':
        setattr(profile, name, '')
    elif name == 'env':
        profile.env = []
    else:
        profile.options.pop(name, None)
    profile.save()


def meta_fields(p: Profile) -> list[dict[str, Any]]:
    def f(name: str, title: str, val: str, help_text: str, kind: str = 'text') -> dict[str, Any]:
        return {
            'name': name, 'title': title, 'kind': kind, 'value': val, 'default': '', 'is_set': bool(val), 'choices': [], 'help': help_text,
            'restart': False, 'allow_none': False, 'scope': f'profile:{p.id}',
        }

    return [
        {**f('name', 'Name', p.name, 'The name shown in the Profiles menu'), 'is_set': False},  # has no default to go back to
        f('command', 'Command', p.command, 'The program to run. Leave empty to run your shell.'),
        f('directory', 'Working directory', p.directory, 'The directory the program starts in. Leave empty for the default.'),
        f('title', 'Tab title', p.title, 'A fixed title for tabs of this profile. Leave empty to follow the running program.'),
        f('env', 'Environment', '\n'.join(p.env), 'One NAME=value per line, set for programs started in this profile.', 'lines'),
    ]


def profile_model() -> dict[str, Any]:
    ' The Profiles page of the Settings window '
    base, ours = global_layers()
    default = get_options().default_profile
    ans = []
    for p in load_profiles().values():
        pages = [{'title': 'General', 'fields': meta_fields(p)}]
        for title, names in PROFILE_PAGES:
            fields = []
            for name in names:
                opt = option_by_name(name)
                if isinstance(opt, Option):
                    fields.append(field_for_option(opt, (base, ours, p.options), f'profile:{p.id}'))
            pages.append({'title': title, 'fields': fields})
        ans.append({'id': p.id, 'name': p.name, 'is_default': p.id == default, 'can_delete': p.id != DEFAULT_PROFILE_ID, 'pages': pages})
    return {'id': 'profiles', 'title': 'Profiles', 'profiles': ans}


# Applying a profile {{{

parsed_cache: dict[str, dict[str, Any]] = {}


def clear_caches() -> None:
    parsed_cache.clear()


def parsed_options(profile: Profile) -> dict[str, Any]:
    ' The profile options, with values as parsed by the configuration system '
    ans = parsed_cache.get(profile.id)
    if ans is None:
        from .config import parse_config

        lines = [f'{k} {v}' for k, vals in profile.options.items() if k in PROFILE_OPTION_NAMES for v in vals]
        ans = parsed_cache[profile.id] = parse_config(lines, allow_geninclude=False) if lines else {}
    return ans


def profile_for_new_os_window(profile_id: str = '') -> Profile:
    profiles = load_profiles()
    return profiles.get(profile_id or get_options().default_profile) or profiles[DEFAULT_PROFILE_ID]


def options_with_profile(profile: Profile, keys: Sequence[str]) -> Any:
    ' A copy of the global options with values of these keys taken from the profile '
    opts = get_options()
    parsed = parsed_options(profile)
    return opts._replace(**{k: parsed[k] for k in keys if k in parsed})


def sizing_options(profile: Profile) -> Any:
    ' The options that decide the size of a new OS window of this profile '
    parsed = parsed_options(profile)
    keys = ('initial_window_width', 'initial_window_height')
    if not any(k in parsed for k in keys):
        return get_options()
    # A size chosen for the profile wins over the size of the last window
    return options_with_profile(profile, keys)._replace(remember_window_size=False)


def wants_transparency(profile: Profile) -> bool:
    return parsed_options(profile).get('background_opacity', 1.) < 1.


def apply_to_os_window(tm: 'TabManager') -> None:
    ' Apply what lives on the OS window. Must happen before its windows are created, as it changes the cell size. '
    from .fast_data_types import change_background_opacity, os_window_font_size, set_os_window_font_set
    from .fonts.render import font_set_for_options

    profile = tm.profile
    parsed = parsed_options(profile)
    opts = get_options()
    font_set = 0
    if any(k in parsed for k in FONT_KEYS):
        font_set = font_set_for_options(options_with_profile(profile, FONT_KEYS))
    size = parsed.get('font_size', opts.font_size)
    set_os_window_font_set(tm.os_window_id, font_set)
    os_window_font_size(tm.os_window_id, size, True)
    change_background_opacity(tm.os_window_id, parsed.get('background_opacity', opts.background_opacity))


def apply_to_window(w: 'Window', profile: Profile) -> None:
    ' Apply what lives on a single terminal, called for new windows and after the configuration is reloaded. '
    from .colors import patch_color_profiles, parse_colors

    parsed = parsed_options(profile)
    color_lines = [f'{k}={profile.options[k][-1]}' for k in profile.options if k in ANSI_COLORS or k in COLOR_KEYS]
    if color_lines:
        spec, tbc = parse_colors(color_lines, allow_reading_conf_files=False)
        # The window color layer, above the configured colors and below the
        # colors set by sessions and programs. None for tbc leaves the
        # transparent background colors as they are.
        patch_color_profiles(spec, tbc, (w.screen.color_profile,), COLOR_LAYER_WINDOW)
    if 'cursor_shape' in parsed:
        w.screen.cursor.shape = parsed['cursor_shape']
    if 'window_padding_width' in parsed:
        e = parsed['window_padding_width']
        w.padding.left, w.padding.top, w.padding.right, w.padding.bottom = e.left, e.top, e.right, e.bottom
        w.update_effective_padding()


COLOR_KEYS = frozenset(('foreground', 'background', 'selection_foreground', 'selection_background', 'cursor', 'cursor_text_color', 'url_color'))


def environment_of(profile: Profile) -> dict[str, str]:
    ans = {}
    for line in profile.env:
        k, sep, v = line.partition('=')
        if sep:
            ans[k.strip()] = v
    return ans


def command_of(profile: Profile) -> list[str] | None:
    if profile.command:
        from .utils import shlex_split

        return list(shlex_split(profile.command))
    return None


def directory_of(profile: Profile) -> str | None:
    return os.path.expanduser(profile.directory) if profile.directory else None
# }}}
