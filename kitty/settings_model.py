#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

"""
The data behind the native Settings window.

Everything here is derived from kitty/options/definition.py, so that a new option
appears in the window without any further work and nothing has to be repeated
in the Cocoa and GTK front ends. Those are renderers of the plain dicts and lists
produced by :func:`settings_pages`, they know nothing about individual options.
"""

import math
import os
import re
import tempfile
from collections.abc import Iterable, Iterator
from decimal import Decimal, localcontext
from typing import Any, NamedTuple

from .conf.types import Group, MouseMapping, MultiOption, Option, ShortcutMapping
from .conf.utils import BadLine, parse_config_base
from .constants import defconf, is_macos, settings_conf


def settings_conf_path() -> str:
    # Written by the Settings window and read after silkie.conf, so that silkie.conf stays
    # exactly as the user left it.
    return settings_conf


Layer = dict[str, list[str]]


def read_layer(path: str, bad_lines: list[BadLine] | None = None) -> Layer:
    ' The raw values of a configuration file, followed through includes, by key '
    from .options.parse import create_result_dict, parse_conf_item

    ans: Layer = {}

    def on_line(key: str, line: str) -> None:
        val = line.partition(' ')[2].strip() if ' ' in line else line.partition('\t')[2].strip()
        ans.setdefault(key, []).append(val)

    try:
        f = open(path, encoding='utf-8', errors='replace')
    except OSError:
        return ans
    with f:
        result = create_result_dict()
        parse_config_base(f, parse_conf_item, result, accumulate_bad_lines=bad_lines, effective_config_lines=on_line)
    return ans


def atomic_write_text(path: str, text: str) -> None:
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=os.path.basename(path), suffix='.tmp')
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


SETTINGS_HEADER = '''\
# Written by the Settings window. It is read after silkie.conf, so what is set here wins.
# Editing it by hand is fine, comments are not preserved.
'''


def write_layer(path: str, layer: Layer, header: str = SETTINGS_HEADER, metadata: Iterable[str] = ()) -> None:
    lines = [header.rstrip()]
    lines.extend(metadata)
    for key in layer:
        lines.extend(f'{key} {val}' for val in layer[key])
    atomic_write_text(path, '\n'.join(lines) + '\n')


def validate(key: str, value: str) -> str:
    ' Returns an error message, empty when the value is acceptable '
    from .options.parse import create_result_dict, parse_conf_item

    opt = option_by_name(key)
    if isinstance(opt, Option) and field_kind(opt) in ('int', 'float'):
        try:
            number = int(value) if field_kind(opt) == 'int' else float(value)
        except (ValueError, OverflowError):
            return f'{humanize(key)} requires a number'
        if isinstance(number, float) and not math.isfinite(number):
            return f'{humanize(key)} requires a finite number'
        domain = numeric_domain(opt)
        low, high = domain['minimum'], domain['maximum']
        if low is not None and (number < low or (number == low and domain['minimum_exclusive'])):
            return f'{humanize(key)} must be {"greater than" if domain["minimum_exclusive"] else "at least"} {low:g}'
        if high is not None and (number > high or (number == high and domain['maximum_exclusive'])):
            return f'{humanize(key)} must be {"less than" if domain["maximum_exclusive"] else "at most"} {high:g}'

    bad: list[BadLine] = []
    lines = value.splitlines() or ['']
    for line in lines:
        ans = create_result_dict()
        parse_config_base([f'{key} {line}'], parse_conf_item, ans, accumulate_bad_lines=bad)
        def finite(item: Any) -> bool:
            if isinstance(item, float):
                return math.isfinite(item)
            if isinstance(item, dict):
                return all(finite(v) for v in item.values())
            if isinstance(item, tuple | list):
                return all(map(finite, item))
            return True
        if not finite(ans):
            return f'{humanize(key)} requires finite numbers'
    if bad:
        return str(bad[0].exception) or f'Not a valid value for {key}'
    return ''


def validate_key(text: str) -> str:
    ' Returns an error message, empty when text is a key or a sequence of keys that a shortcut can use '
    from .options.utils import InvalidMods, parse_shortcut, sequence_sep

    for part in text.split(sequence_sep):
        try:
            _, _, key = parse_shortcut(part.strip())
        except InvalidMods:
            return f'Unknown modifier in: {part}'
        if key == 0 or ' ' in part.strip():
            return f'Unknown key: {part}'
    return ''


def key_text(sk: Any) -> str:
    ' The real keys of a shortcut, which is more use to someone reading a menu than kitty_mod '
    from .fast_data_types import glfw_get_key_name
    from .types import mod_to_names

    names = list(mod_to_names(sk.mods))
    kname = (glfw_get_key_name(0, sk.key) if sk.is_native else glfw_get_key_name(sk.key, 0)) or f'{sk.key}'
    names.append({' ': 'space'}.get(kname, kname))
    return '+'.join(names)


# Fields, shared by the global pages and the profile pages {{{

ACRONYMS = {
    'hdr': 'HDR', 'url': 'URL', 'urls': 'URLs', 'os': 'OS', 'id': 'ID', 'macos': 'macOS', 'ns': 'NS', 'tty': 'TTY',
    'ssh': 'SSH', 'gpu': 'GPU', 'ltr': 'LTR', 'x11': 'X11', 'wm': 'WM', 'dpi': 'DPI', 'cmd': 'command', 'cwd': 'working directory',
}
BOOL_PARSERS = {'to_bool'}
COLOR_PARSERS = {'to_color', 'to_color_or_none', 'macos_titlebar_color', 'titlebar_color', 'scrollbar_color', 'cursor_text_color'}
INT_PARSERS = {'int', 'positive_int', 'scrollback_lines', 'tab_bar_min_tabs'}
FLOAT_PARSERS = {'float', 'positive_float', 'unit_float', 'signed_unit_float', 'to_font_size',
                 'spatial_angle', 'spatial_divergence', 'spatial_scale', 'spatial_phase_limit', 'spatial_exponent', 'hdr_multiplier'}
FONT_PARSERS = {'parse_font_spec'}
# Options whose docs say a reload does not change them
NEEDS_RESTART_PAT = re.compile(r'(reloading\s+the\s+config\s+is\s+not\s+supported|restart\s+\w+\s+after\s+changing|has\s+no\s+effect\s+(on|when)\s+(reload|reloading))', re.I)
MARKUP_PAT = re.compile(r':(?:\w+):`(?:([^`<]*?)\s*<[^`]*>|([^`]*))`')


def plain_text(rst: str) -> str:
    ' The documentation of an option as text, without the markup '
    text = MARKUP_PAT.sub(lambda m: (m.group(1) or m.group(2) or '').lstrip('~!'), rst)
    text = text.replace('``', '').replace('::\n', ':\n')
    # Preserve file names, paths, and identifiers such as kitty_mod.
    text = re.sub(r'(?<![\w./-])kitty(?![\w./-])', 'Silkie', text)
    paras = [' '.join(p.split()) for p in re.split(r'\n\s*\n', text.strip()) if p.strip()]
    return '\n\n'.join(paras)


def humanize(name: str) -> str:
    if name == 'spatial_divergence_exponent':
        return 'Divergence multiplier (2^X)'
    words = name.split('_')
    if len(words) > 1 and words[0] in ('macos', 'linux', 'wayland', 'x11'):
        words = words[1:]
    words = [ACRONYMS.get(w, w) for w in words]
    ans = ' '.join(words)
    return ans[0].upper() + ans[1:] if ans else name


def applies_to_this_platform(name: str) -> bool:
    if is_macos:
        return not name.startswith(('linux_', 'wayland_', 'x11_'))
    return not name.startswith('macos_')


def parser_name(item: Option | MultiOption) -> str:
    return str(getattr(item.parser_func, '__name__', item.parser_func))


def field_kind(opt: Option) -> str:
    if opt.choices:
        return 'choice'
    p = parser_name(opt)
    if p in BOOL_PARSERS:
        return 'bool'
    if p in COLOR_PARSERS:
        return 'color'
    if p in INT_PARSERS:
        return 'int'
    if p in FLOAT_PARSERS:
        return 'float'
    if p in FONT_PARSERS:
        return 'font'
    return 'text'


def numeric_domain(opt: Option) -> dict[str, Any]:
    """Semantic bounds, independent of the size of a native widget's counter.

    Signed scroll multipliers, drag tolerance and documented negative disable
    values stay signed. Composite duration/mouse options use text controls.
    None means no semantic bound, rather than an invented large number.
    """
    parser = parser_name(opt)
    low: float | int | None = None
    high: float | int | None = None
    step = 1. if field_kind(opt) == 'int' else 0.1
    if parser in ('positive_float', 'positive_int', 'spatial_divergence', 'spatial_phase_limit'):
        low = 0
    elif parser in ('hdr_multiplier', 'spatial_scale', 'tab_bar_min_tabs'):
        low = 1
    elif parser == 'to_font_size':
        from .options.utils import MINIMUM_FONT_SIZE
        low = MINIMUM_FONT_SIZE
    elif parser == 'unit_float':
        low, high, step = 0, 1, .05
    elif parser == 'signed_unit_float':
        low, high, step = -1, 1, .05
    elif parser == 'scrollback_lines':
        low = -1 # canonical unlimited sentinel
    if parser in ('spatial_angle', 'spatial_divergence'):
        low, high = 0, 180
    if parser == 'spatial_exponent':
        low, high = -6, 6
    if opt.name.endswith('_opacity'):
        low, high, step = 0, 1, .05
    if opt.name in ('background_blur', 'update_check_interval'):
        low = 0
    if opt.name in ('window_resize_step_cells', 'window_resize_step_lines', 'tab_title_max_lines'):
        low = 1
    if opt.name == 'clipboard_max_size':
        step = 1.
    return {'minimum': low, 'maximum': high, 'step': step,
            'minimum_exclusive': parser == 'spatial_angle',
            'maximum_exclusive': parser in ('spatial_angle', 'spatial_divergence')}


def stepped_value(name: str, text: str, direction: int) -> str:
    """Step an input-sized decimal exactly, without native counter limits."""
    opt = option_by_name(name)
    if not isinstance(opt, Option) or field_kind(opt) not in ('int', 'float') or direction not in (-1, 1) or validate(name, text):
        return text
    domain = numeric_domain(opt)
    value = Decimal(text)
    step = Decimal(str(domain['step']))
    with localcontext() as context:
        context.prec = max(context.prec, len(value.as_tuple().digits) + abs(value.as_tuple().exponent) +
                           len(step.as_tuple().digits) + abs(step.as_tuple().exponent) + 2)
        result = value + direction * step
    if domain['minimum'] is not None and result < Decimal(str(domain['minimum'])):
        result = Decimal(str(domain['minimum']))
    if domain['maximum'] is not None and result > Decimal(str(domain['maximum'])):
        result = Decimal(str(domain['maximum']))
    rendered = format(result, 'f')
    if '.' in rendered:
        rendered = rendered.rstrip('0').rstrip('.')
    return text if validate(name, rendered) else rendered


def default_value(opt: Option) -> str:
    if is_macos and isinstance(opt.macos_defval, str):
        return opt.macos_defval
    return str(opt.defval_as_string)


def font_family_from_spec(value: str) -> str:
    ' The family from a font specification such as: family="Fira Code" features="+zero" '
    m = re.match(r'''family\s*=\s*(?:"([^"]*)"|'([^']*)'|(\S+))''', value.strip())
    if m:
        return m.group(1) or m.group(2) or m.group(3) or ''
    return value.strip()


def font_spec_from_family(family: str) -> str:
    family = family.strip()
    if family in ('', 'auto', 'monospace') or '=' in family:
        return family or 'auto'
    return f'family="{family}"'


def normalized_color(value: str, allow_none: bool) -> str:
    from .conf.utils import to_color

    if value == 'none':
        return value if allow_none else ''
    try:
        return to_color(value).as_sharp
    except ValueError:
        return ''


def field_for_option(opt: Option, value_layers: tuple[Layer, ...], scope: str) -> dict[str, Any]:
    ' value_layers is ordered from least to most specific, the last is what the field edits '
    default = default_value(opt)
    value = default
    for layer in value_layers:
        if opt.name in layer:
            value = layer[opt.name][-1]
    kind = field_kind(opt)
    if kind == 'font':
        default, value = font_family_from_spec(default), font_family_from_spec(value)
    elif kind == 'color':
        # The native color controls understand only #rrggbb and none. Values that are not colors, such as
        # those that refer to another color, are edited as text.
        normalized = normalized_color(value, parser_name(opt) == 'to_color_or_none')
        if normalized:
            value = normalized
        else:
            kind = 'text'
    return {
        'name': opt.name,
        'title': humanize(opt.name),
        **(numeric_domain(opt) if kind in ('int', 'float') else {}),
        'kind': kind,
        'value': value,
        'default': default,
        'is_set': opt.name in value_layers[-1],
        'choices': list(opt.choices) if opt.choices else [],
        'help': plain_text(opt.long_text),
        'restart': bool(NEEDS_RESTART_PAT.search(opt.long_text)),
        'allow_none': parser_name(opt) == 'to_color_or_none',
        'scope': scope,
    }


def field_for_multi_option(opt: MultiOption, value_layers: tuple[Layer, ...], scope: str) -> dict[str, Any]:
    lines = [v.defval_as_str for v in opt.items if v.add_to_default and (v.only == '' or v.only == ('macos' if is_macos else 'linux'))]
    default = '\n'.join(lines)
    value = default
    for layer in value_layers:
        if opt.name in layer:
            value = '\n'.join(layer[opt.name])
    return {
        'name': opt.name, 'title': humanize(opt.name), 'kind': 'lines', 'value': value, 'default': default,
        'is_set': opt.name in value_layers[-1], 'choices': [], 'help': plain_text(opt.long_text),
        'restart': bool(NEEDS_RESTART_PAT.search(opt.long_text)), 'allow_none': False, 'scope': scope,
    }
# }}}


# Pages, in the order of the toolbar. Top level groups that are not listed go to Advanced, so that nothing
# added to the definition can get lost.
PAGE_OF_GROUP = {
    'experimental': 'experimental',
    'spatial': 'spatial',
    'advanced': 'general', 'bell': 'general', 'scrollback': 'general',
    'fonts': 'appearance', 'cursor': 'appearance', 'window': 'appearance', 'tabbar': 'appearance', 'colors': 'appearance',
    'mouse': 'pointer',
    'shortcuts': 'keys',
}
PAGE_TITLES = (('general', 'General'), ('appearance', 'Appearance'), ('spatial', 'Spatial Display'), ('profiles', 'Profiles'), ('keys', 'Keys'), ('pointer', 'Pointer'), ('advanced', 'Advanced'), ('experimental', 'Experimental and untested'))
# The 240 cube and gray colors would drown the sixteen people actually change
EXTENDED_COLOR_PAT = re.compile(r'color(1[6-9]|[2-9]\d|1\d\d|2[0-5]\d)$')
SHORTCUT_PREFIX = 'map:'
SECTION_TITLES = {'advanced': 'Shell and terminal', 'os': 'Operating system', 'color_table': 'Colors', 'shortcuts': 'Modifier keys'}


class Section(NamedTuple):
    page: str
    title: str
    items: list[Any]


def sections_of_definition() -> Iterator[Section]:
    from .options.definition import definition

    def walk(g: Group, page: str, title: str) -> Iterator[Section]:
        own: list[Any] = []
        for item in g.items:
            if isinstance(item, Group):
                sub = page or PAGE_OF_GROUP.get(item.name.partition('.')[0], 'advanced')
                yield from walk(item, sub, item.title)
            else:
                own.append(item)
        if own:
            yield Section(page or 'advanced', SECTION_TITLES.get(g.name, title) or 'Other', own)

    yield from walk(definition.root_group, '', '')


def effective_shortcuts() -> dict[str, list[str]]:
    ' The active key bindings by action, as the text of the configuration file '
    from .fast_data_types import get_options

    ans: dict[str, list[str]] = {}
    try:
        opts = get_options()
    except RuntimeError:
        return ans
    for sk, defs in opts.keyboard_modes[''].keymap.items():
        d = defs[-1]
        if d.definition and d.definition != 'no_op' and d.is_suitable_for_global_shortcut:
            ans.setdefault(' '.join(d.definition.split()), []).append(key_text(sk))
    return ans


def shortcut_fields(items: Iterable[ShortcutMapping], layer: Layer, active: dict[str, list[str]]) -> list[dict[str, Any]]:
    want = 'macos' if is_macos else 'linux'
    by_action: dict[str, dict[str, Any]] = {}
    for m in items:
        if m.only and m.only != want:
            continue
        action = ' '.join(m.action_def.split())
        f = by_action.get(action)
        if f is None:
            f = by_action[action] = {
                'name': SHORTCUT_PREFIX + action, 'title': m.short_text, 'kind': 'shortcut', 'value': '', 'default': [],
                'is_set': False, 'choices': [], 'help': plain_text(m.long_text) or action, 'restart': False, 'allow_none': False,
                'scope': 'global',
            }
        f['default'].append(m.key)
    overridden = {' '.join(line.partition(' ')[2].split()) for line in layer.get('map', ())}
    for action, f in by_action.items():
        f['default'] = ', '.join(f['default'])
        keys = active.get(action)
        f['value'] = ', '.join(sorted(set(keys))) if keys is not None else (f['default'] if not active else '')
        f['is_set'] = action in overridden
    return list(by_action.values())


def custom_lines_field(key: str, layer: Layer, title: str, help_text: str, known_actions: frozenset[str]) -> dict[str, Any]:
    ' Lines of a key such as map that no other row of the page owns '
    lines = [line for line in layer.get(key, ()) if ' '.join(line.partition(' ')[2].split()) not in known_actions]
    return {
        'name': key, 'title': title, 'kind': 'lines', 'value': '\n'.join(lines), 'default': '', 'is_set': bool(lines),
        'choices': [], 'help': help_text, 'restart': False, 'allow_none': False, 'scope': 'global',
    }


def visible_option(item: Any) -> bool:
    if isinstance(item, Option | MultiOption):
        if isinstance(item, Option) and item.name == 'default_profile':
            return False  # set from the Profiles page
        return applies_to_this_platform(item.name)
    return False


def global_layers() -> tuple[Layer, Layer]:
    return read_layer(defconf), read_layer(settings_conf_path())


def settings_pages() -> list[dict[str, Any]]:
    ' Everything except the Profiles page, which comes from kitty.profiles '
    base, ours = global_layers()
    layers = (base, ours)
    pages: dict[str, dict[str, Any]] = {pid: {'id': pid, 'title': title, 'sections': []} for pid, title in PAGE_TITLES if pid != 'profiles'}
    active = effective_shortcuts()
    known_actions: set[str] = set()
    for section in sections_of_definition():
        fields = [
            field_for_option(i, layers, 'global') if isinstance(i, Option) else field_for_multi_option(i, layers, 'global')
            for i in section.items if visible_option(i)
        ]
        shortcuts = shortcut_fields((i for i in section.items if isinstance(i, ShortcutMapping)), ours, active)
        known_actions.update(f['name'][len(SHORTCUT_PREFIX):] for f in shortcuts)
        fields.extend(shortcuts)
        extended = [f for f in fields if EXTENDED_COLOR_PAT.match(f['name'])]
        if extended:
            fields = [f for f in fields if not EXTENDED_COLOR_PAT.match(f['name'])]
            pages[section.page]['sections'].append({'title': section.title, 'fields': fields})
            pages[section.page]['sections'].append({'title': 'Extended colors', 'fields': extended})
            continue
        if fields:
            pages[section.page]['sections'].append({'title': section.title, 'fields': fields})
    frozen = frozenset(known_actions)
    pages['keys']['sections'].append({'title': 'Your own shortcuts', 'fields': [custom_lines_field(
        'map', ours, 'Additional shortcuts', 'One per line: a key and the action it runs, for example: ctrl+alt+x launch htop', frozen)]})
    pages['pointer']['sections'].append({'title': 'Your own mouse actions', 'fields': [custom_lines_field(
        'mouse_map', ours, 'Additional mouse actions', 'One per line: a button, an event, a grab mode and an action, for example: left press ungrabbed mouse_selection normal',
        frozenset())]})
    return [pages[pid] for pid, _ in PAGE_TITLES if pid in pages]


def set_shortcut(layer: Layer, action: str, keys: str, default_keys: str) -> None:
    ' Rebind action to the comma separated keys, unbinding the keys it has by default '
    action = ' '.join(action.split())
    lines = [line for line in layer.get('map', ()) if ' '.join(line.partition(' ')[2].split()) != action]
    wanted = [k.strip() for k in keys.split(',') if k.strip()]
    for k in default_keys.split(','):
        k = k.strip()
        if k and k not in wanted:
            lines.append(f'{k} no_op')
    lines.extend(f'{k} {action}' for k in wanted)
    if lines:
        layer['map'] = lines
    else:
        layer.pop('map', None)


def set_custom_lines(layer: Layer, key: str, value: str, known_actions: frozenset[str]) -> None:
    keep = [line for line in layer.get(key, ()) if ' '.join(line.partition(' ')[2].split()) in known_actions]
    keep.extend(line.strip() for line in value.splitlines() if line.strip())
    if keep:
        layer[key] = keep
    else:
        layer.pop(key, None)


def all_actions_with_rows() -> frozenset[str]:
    return frozenset(
        ' '.join(i.action_def.split()) for s in sections_of_definition() for i in s.items if isinstance(i, ShortcutMapping)
    )


def option_by_name(name: str) -> Option | MultiOption | None:
    for s in sections_of_definition():
        for i in s.items:
            if isinstance(i, Option | MultiOption) and i.name == name:
                return i
    return None


def set_global_value(name: str, value: str) -> str:
    ' Store a change from the Settings window. Returns an error message, empty on success. '
    path = settings_conf_path()
    layer = read_layer(path)
    if name.startswith(SHORTCUT_PREFIX):
        action = name[len(SHORTCUT_PREFIX):]
        for k in value.split(','):
            if k.strip() and (err := validate_key(k.strip())):
                return err
        defaults = [
            i.key for s in sections_of_definition() for i in s.items
            if isinstance(i, ShortcutMapping) and ' '.join(i.action_def.split()) == ' '.join(action.split())
            and (not i.only or i.only == ('macos' if is_macos else 'linux'))
        ]
        set_shortcut(layer, action, value, ', '.join(defaults))
    elif name in ('map', 'mouse_map'):
        for line in value.splitlines():
            if line.strip() and (err := validate(name, line.strip())):
                return err
        set_custom_lines(layer, name, value, all_actions_with_rows() if name == 'map' else frozenset())
    else:
        opt = option_by_name(name)
        if opt is None:
            return f'Unknown setting: {name}'
        if isinstance(opt, Option) and field_kind(opt) == 'font':
            value = font_spec_from_family(value)
        if isinstance(opt, MultiOption):
            lines = [line.strip() for line in value.splitlines() if line.strip()]
            for line in lines:
                if err := validate(name, line):
                    return err
            layer[name] = lines
        else:
            if err := validate(name, value):
                return err
            layer[name] = [value]
    write_layer(path, layer)
    return ''


def reset_global_value(name: str) -> None:
    path = settings_conf_path()
    layer = read_layer(path)
    if name.startswith(SHORTCUT_PREFIX):
        action = ' '.join(name[len(SHORTCUT_PREFIX):].split())
        lines = [line for line in layer.get('map', ()) if ' '.join(line.partition(' ')[2].split()) != action]
        if lines:
            layer['map'] = lines
        else:
            layer.pop('map', None)
    else:
        layer.pop(name, None)
    write_layer(path, layer)


def monospace_families() -> list[str]:
    ' The names offered by the font fields '
    if is_macos:
        from .fonts.core_text import all_fonts_map
    else:
        from .fonts.fontconfig import all_fonts_map
    families = {f['family'] for fonts in all_fonts_map(True)['family_map'].values() for f in fonts if f.get('family')}
    return ['auto', 'monospace'] + sorted(families, key=str.lower)
