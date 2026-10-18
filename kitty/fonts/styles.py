#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

"""Persistent text styles for OSC 777; IDs remain valid in scrollback after reloads."""

import locale
import math
import re
from functools import cache
from typing import NamedTuple

from kitty.fast_data_types import font_style_metrics
from kitty.options.types import Options, defaults

from . import Descriptor, FontSpec, family_name_to_key
from .common import all_fonts_map, get_fine_grained_font, get_font_files, get_font_from_spec, get_variable_data_for_descriptor, set_axis_values


class TextStyle(NamedTuple):
    role: str = 'default'
    weight: int = 0
    slant: int | None = None  # OpenType convention, hundredths of a degree


styles = [TextStyle()]
style_ids = {styles[0]: 0}
options = defaults
descriptor_indices: dict[object, int] = {}
role_pattern = re.compile(r'[A-Za-z][A-Za-z0-9_.-]*\Z')
integer_pattern = re.compile(r'-?[0-9]+\Z')


def configure(opts: Options) -> None:
    global options
    options = opts
    descriptor_indices.clear()
    descriptor_index.cache_clear()
    metrics_cache.clear()


def intern(style: TextStyle) -> int:
    ans = style_ids.get(style)
    if ans is None:
        ans = len(styles)
        # Cell storage uses a uint32_t handle, rather than limiting configured roles.
        if ans > 0xffffffff:
            raise OverflowError('Text style handles exhausted')
        styles.append(style)
        style_ids[style] = ans
    return ans


def parse(current: int, data: bytes, current_italic: int = 0) -> tuple[int, int, str]:
    """Validate the entire request before changing any cursor attributes."""
    unchanged = current, -1, ''
    try:
        fields: dict[str, str] = {}
        for field in data.decode('ascii').split(':'):
            key, sep, value = field.partition('=')
            if not sep or not key or key in fields or '\0' in field:
                return unchanged
            fields[key] = value
        version = fields.get('v', '')
        if not version.isdecimal() or int(version) != 1:
            return unchanged
        op = fields.get('op')
        style = styles[current]
        role = fields.get('role', style.role)
        if not role_pattern.fullmatch(role):
            return unchanged
        weight = style.weight
        if 'weight' in fields:
            if not integer_pattern.fullmatch(fields['weight']):
                return unchanged
            weight = int(fields['weight'])
            if weight != 0 and not 100 <= weight <= 900:
                return unchanged
        slant = style.slant
        if 'slant' in fields:
            if fields['slant'] == 'auto':
                slant = None
            else:
                if not integer_pattern.fullmatch(fields['slant']):
                    return unchanged
                slant = int(fields['slant'])
                if not -1500 <= slant <= 1500:
                    return unchanged
        italic = -1
        if 'italic' in fields:
            if fields['italic'] not in ('0', '1'):
                return unchanged
            italic = int(fields['italic'])
        query_id = fields.get('id', '0')
        if not query_id.isascii() or not query_id.isdecimal() or not 0 <= int(query_id) <= 0x7fffffff:
            return unchanged
        query_id = str(int(query_id))
        if op == 'select':
            if role != 'default' and role not in options.font_role:
                role = 'default'
            return intern(TextStyle(role, weight, slant)), italic, ''
        if op == 'reset':
            return 0, -1, ''
        if op == 'query':
            roles = ','.join(('default', *options.font_role))
            return current, -1, (f'777;font;v=1:op=cap:id={query_id}:caps=1:roles={roles}'
                                 ':weight_min=100:weight_max=900:slant_min=-1500:slant_max=1500:synthetic_slant=1')
        if op == 'state':
            return current, -1, f'777;font;v=1:op=state:id={query_id}:{style_fields(style)}:italic={current_italic}'
    except (UnicodeError, ValueError, IndexError):
        pass
    return unchanged


def style_fields(style: TextStyle) -> str:
    slant = 'auto' if style.slant is None else str(style.slant)
    return f'role={style.role}:weight={style.weight}:slant={slant}'


def as_ansi(style_id: int) -> str:
    return f'\x1b]777;font;v=1:op=select:{style_fields(styles[style_id])}\x1b\\'


def select_slot(current: int, slot: int) -> int:
    roles = ('default', *options.font_role)
    role = roles[slot] if slot < len(roles) else 'default'
    return intern(styles[current]._replace(role=role))


def cjk_region() -> str:
    region = options.cjk_font_locale
    if region != 'auto':
        return region
    lang = (locale.getlocale()[0] or '').lower().replace('-', '_')
    if lang.startswith('ja'):
        return 'jp'
    if lang.startswith('ko'):
        return 'kr'
    if lang.startswith('zh'):
        if any(x in lang for x in ('hk', 'mo', 'hant_hk')):
            return 'hk'
        if any(x in lang for x in ('tw', 'hant')):
            return 'tc'
    return 'sc'


def cjk_spec(kind: int = 1) -> FontSpec | None:
    if not options.cjk_font.is_auto:
        return options.cjk_font
    region = 'jp' if kind == 2 else 'kr' if kind == 3 else cjk_region()
    sarasa = {'jp': 'J', 'kr': 'K', 'sc': 'SC', 'tc': 'TC', 'hk': 'HC'}[region]
    system_family = {'jp': 'Hiragino Sans', 'kr': 'Apple SD Gothic Neo', 'sc': 'PingFang SC', 'tc': 'PingFang TC', 'hk': 'PingFang HK'}[region]
    families = all_fonts_map(False)['family_map']
    for name in (f'Sarasa Term {sarasa}', f'Noto Sans CJK {region.upper()}',
                 f'Noto Sans Mono CJK {region.upper()}', system_family):
        if family_name_to_key(name) in families:
            return FontSpec(family=name)
    return None


metrics_cache: dict[tuple, tuple[int, float]] = {}


def native_metrics(descriptor: Descriptor) -> tuple[int, float]:
    # Ask HarfBuzz for the selected collection member, rather than guessing a
    # TTC index from its path (CoreText descriptors need not expose an index).
    key = descriptor['path'], descriptor.get('index', 0), descriptor['postscript_name']
    ans = metrics_cache.get(key)
    if ans is None:
        ans = metrics_cache[key] = font_style_metrics(descriptor)
    return ans


def freeze(value: object) -> object:
    if isinstance(value, dict):
        return tuple(sorted((k, freeze(v)) for k, v in value.items()))
    if isinstance(value, (list, tuple)):
        return tuple(map(freeze, value))
    return value


@cache
def descriptor_index(style_id: int, bold: bool, italic: bool, cjk: int) -> int:
    from .render import current_faces

    style = styles[style_id]
    spec = cjk_spec(cjk) if cjk else options.font_role.get(style.role, options.font_family)
    if spec is None:
        return -1  # Let the platform supply glyphs outside the preferred CJK families.
    if cjk:
        if spec.system:
            spec = FontSpec(family=spec.system)
        descriptor = get_fine_grained_font(spec, bold, italic, monospaced=False).copy()
    elif style.role == 'default' or style.role not in options.font_role:
        key = 'bi' if bold and italic else 'bold' if bold else 'italic' if italic else 'medium'
        descriptor = get_font_files(options)[key].copy()
    else:
        if spec.is_auto:
            spec = FontSpec(system='monospace')
        descriptor = get_font_from_spec(spec, bold, italic).copy()
    vd = get_variable_data_for_descriptor(descriptor)
    if style.weight:
        weight_axis = next((a for a in vd['axes'] if a['tag'] == 'wght'), None)
        if weight_axis is not None:
            set_axis_values({'wght': max(weight_axis['minimum'], min(weight_axis['maximum'], style.weight))}, descriptor, vd)
        else:
            candidates = all_fonts_map(not cjk)['family_map'].get(family_name_to_key(descriptor['family']), ())
            base_width = descriptor.get('width', 0)
            def score(candidate: Descriptor) -> tuple:
                is_italic = 'italic' in candidate['style'].lower() or 'oblique' in candidate['style'].lower()
                return (is_italic != italic, abs(candidate.get('width', 0) - base_width),
                        abs(native_metrics(candidate)[0] - style.weight), candidate['postscript_name'])
            if candidates:
                features = descriptor.get('features')
                descriptor = min(candidates, key=score).copy()
                if features is not None:
                    descriptor['features'] = features
    if style.slant is not None:
        # Preserve the selected roman/italic shapes and set their absolute angle.
        # In particular this makes upright italic and inverse slant possible.
        native = native_metrics(descriptor)[1]
        if any(a['tag'] == 'slnt' for a in vd['axes']):
            from .common import get_axis_values
            native = get_axis_values(descriptor, vd).get('slnt', native)
        shear = math.tan(math.radians(native)) - math.tan(math.radians(style.slant / 100))
        descriptor['matrix'] = (1.0, shear, 0.0, 1.0)
    key = freeze(descriptor)
    index = descriptor_indices.get(key)
    if index is None:
        index = len(current_faces)
        if index > 0xffffffff:
            raise OverflowError('Font descriptor indices exhausted')
        descriptor_indices[key] = index
        current_faces.append((descriptor, bold, italic))
    return index
