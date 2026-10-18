#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Adam DePrince
"""Font resources and rich text layout for graphics protocol f=200.

The state dictionary belongs to one PTY (shared by its two screen buffers).
Both measurement and retained text use layout(), including fallback and decoration.
"""
import base64
import json
import math
import unicodedata
from collections.abc import Iterator
from typing import Any

from kitty.fast_data_types import vector_font, vector_font_covers, vector_font_info, vector_shape, vector_validate_color
from kitty.fonts import Descriptor, FontSpec, family_name_to_key

from . import styles
from .common import all_fonts_map, get_fine_grained_font, get_font_files, get_variable_data_for_descriptor


def number(value: Any) -> float:
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value):
        raise ValueError('Expected a finite number')
    return float(value)


def decode(data: bytes) -> dict:
    obj = json.loads(data)
    if not isinstance(obj, dict):
        raise ValueError('Expected a JSON object')
    return obj


def system_fonts() -> list[dict]:
    ans = []
    for candidates in all_fonts_map(False)['family_map'].values():
        for d in candidates:
            weight, slant = styles.native_metrics(d)
            ans.append({'family': d['family'], 'style': d['style'], 'postscript_name': d['postscript_name'],
                        'weight': weight, 'slant': slant, 'monospace': bool(d.get('monospace', d.get('spacing', 0) == 100))})
    return sorted(ans, key=lambda f: (f['family'], f['style'], f['postscript_name']))


def resolve(state: dict, style: dict) -> tuple[Any, dict, float]:
    name = style.get('font', '')
    if not isinstance(name, str):
        raise ValueError('font must be a family name or @session-font-id')
    weight = number(style.get('weight', 400))
    if not 100 <= weight <= 900:
        raise ValueError('weight must be between 100 and 900')
    italic = style.get('italic', False)
    if not isinstance(italic, bool):
        raise ValueError('italic must be a boolean')
    axes = style.get('axes', {})
    if not isinstance(axes, dict) or any(not isinstance(k, str) or len(k) != 4 for k in axes):
        raise ValueError('axes must map four-character OpenType tags to numbers')
    axes = {k: number(v) for k, v in axes.items()}
    fonts = state.setdefault('fonts', {})
    if name.startswith('@'):
        entry = fonts[name[1:]]
        font, info, native_slant = entry['font'], entry['info'], entry['slant']
    else:
        role = style.get('role', 'default')
        if name:
            spec = FontSpec(family=name, postscript_name=name)
        else:
            spec = styles.options.font_role.get(role, styles.options.font_family)
            if role == 'default':
                spec = None
        if spec is None:
            descriptor = get_font_files(styles.options)['bi' if italic and weight >= 600 else 'italic' if italic else 'bold' if weight >= 600 else 'medium']
        else:
            if spec.system:
                spec = FontSpec(family=spec.system)
            descriptor = get_fine_grained_font(spec, weight >= 600, italic, monospaced=False)
        vd = get_variable_data_for_descriptor(descriptor)
        exact_face = bool(name and name.casefold() == descriptor['postscript_name'].casefold())
        if not vd['axes'] and not exact_face:
            candidates = all_fonts_map(False)['family_map'].get(family_name_to_key(descriptor['family']), ())
            def score(d: Descriptor) -> tuple:
                it = any(s in d['style'].lower() for s in ('italic', 'oblique'))
                return it != italic, abs(styles.native_metrics(d)[0] - weight), d['postscript_name']
            if candidates:
                descriptor = min(candidates, key=score)
        key = styles.freeze(descriptor)
        cache = state.setdefault('system', {})
        if key not in cache:
            font = vector_font(descriptor)
            cache[key] = font, vector_font_info(font), styles.native_metrics(descriptor)[1]
        font, info, native_slant = cache[key]
    available = {a['tag']: a for a in info['axes']}
    defaults = {'wght': weight, 'ital': float(italic)}
    for tag, value in defaults.items():
        if tag in available and tag not in axes:
            axes[tag] = max(available[tag]['minimum'], min(available[tag]['maximum'], value))
    for tag, value in axes.items():
        if tag not in available or not available[tag]['minimum'] <= value <= available[tag]['maximum']:
            raise ValueError(f'Unsupported value for font axis {tag}')
    native_slant = axes.get('slnt', native_slant)
    slant = style.get('slant')
    shear = 0.0
    if slant is not None:
        slant = number(slant)
        if not -15 <= slant <= 15:
            raise ValueError('slant must be between -15 and 15 degrees')
        shear = math.tan(math.radians(native_slant)) - math.tan(math.radians(slant))
    return font, axes, shear


def clusters(text: str) -> Iterator[str]:
    # Keep combining marks, variation selectors and joined sequences with their base.
    start = 0
    for i in range(1, len(text)):
        cp = ord(text[i])
        if unicodedata.combining(text[i]) or text[i - 1] == '\u200d' or cp in (0x200c, 0x200d) or 0xfe00 <= cp <= 0xfe0f or 0xe0100 <= cp <= 0xe01ef:
            continue
        yield text[start:i]
        start = i
    if text:
        yield text[start:]


def segments(state: dict, text: str, style: dict) -> Iterator[tuple[str, tuple[Any, dict, float]]]:
    primary = resolve(state, style)
    if not text:
        yield '', primary
        return
    candidates = style.get('fallback', [])
    if not isinstance(candidates, list) or any(not isinstance(x, str) for x in candidates):
        raise ValueError('fallback must be an array of font names')
    pending, selected = [], primary
    for cluster in clusters(text):
        chosen = primary
        if not vector_font_covers(primary[0], cluster):
            cp = ord(cluster[0])
            kind = 3 if 0x1100 <= cp <= 0x11ff or 0x3130 <= cp <= 0x318f or 0xac00 <= cp <= 0xd7ff else 2 if 0x3040 <= cp <= 0x30ff else 1
            cjk = styles.cjk_spec(kind)
            fallbacks = candidates + ([cjk.system or cjk.family or 'sans-serif'] if cjk else [])
            for candidate in fallbacks:
                fallback_style = {k: v for k, v in style.items() if k != 'axes'}
                fallback_style['font'] = candidate
                f = resolve(state, fallback_style)
                if vector_font_covers(f[0], cluster):
                    chosen = f
                    break
        if chosen[0] is not selected[0] and pending:
            yield ''.join(pending), selected
            pending = []
        pending.append(cluster)
        selected = chosen
    if pending:
        yield ''.join(pending), selected


def rect(x: float, y: float, w: float, h: float) -> list:
    return [(0, x, y), (1, x + w, y), (1, x + w, y + h), (1, x, y + h), (4,)]


def bounds(commands: list) -> list[float] | None:
    # Exact extrema of quadratic/cubic outlines, rather than their control boxes.
    xs, ys = [], []
    x = y = sx = sy = 0.0
    for cmd in commands:
        op, *p = cmd
        if op == 0:
            x, y = sx, sy = p
            continue
        if op == 4:
            p, op = [sx, sy], 1
        if op == 1:
            xs.extend((x, p[0]))
            ys.extend((y, p[1]))
        else:
            for start, controls, dest in ((x, p[0::2], xs), (y, p[1::2], ys)):
                v = [start, *controls]
                dest.extend((v[0], v[-1]))
                if op == 2:
                    den = v[0] - 2 * v[1] + v[2]
                    ts = [(v[0] - v[1]) / den] if den else []
                else:
                    a = -v[0] + 3*v[1] - 3*v[2] + v[3]
                    b = 2*(v[0] - 2*v[1] + v[2])
                    c = v[1] - v[0]
                    if abs(a) < 1e-15:
                        ts = [-c / b] if b else []
                    else:
                        disc = b*b - 4*a*c
                        ts = [(-b + math.sqrt(disc))/(2*a), (-b - math.sqrt(disc))/(2*a)] if disc >= 0 else []
                for t in ts:
                    if 0 < t < 1:
                        q = v[:]
                        while len(q) > 1:
                            q = [a*(1-t) + b*t for a, b in zip(q, q[1:])]
                        dest.append(q[0])
        x, y = p[-2:]
    return [min(xs), min(ys), max(xs), max(ys)] if xs else None


def layout(state: dict, obj: dict) -> tuple[list, dict]:
    runs = obj.get('runs', [{'text': obj.get('text', '')}])
    if not isinstance(runs, list) or any(not isinstance(r, dict) or not isinstance(r.get('text'), str) for r in runs):
        raise ValueError('runs must contain objects with a text string')
    base = {k: v for k, v in obj.items() if k not in ('runs', 'text')}
    output, missing, logical_boxes = [], [], []
    x = y = width = ascent = descent = line_height = 0.0
    index = 0
    for run in runs:
        style = base | run
        if any(not isinstance(style.get(k, False), bool) for k in ('strike', 'overline')):
            raise ValueError('strike and overline must be booleans')
        size = number(style.get('size', 16))
        if size <= 0:
            raise ValueError('size must be positive')
        features = style.get('features', [])
        if not isinstance(features, list) or any(not isinstance(f, str) for f in features):
            raise ValueError('features must be an array of OpenType feature strings')
        direction = style.get('direction', '')
        if direction not in ('', 'ltr', 'rtl', 'ttb', 'btt'):
            raise ValueError('Invalid text direction')
        color = style.get('fill', '#000000')
        for part_index, part in enumerate(run['text'].split('\n')):
            if part_index:
                width = max(width, x)
                x = 0.0
                y += line_height or size * 1.2
                line_height = 0.0
                index += 1
            for text, (font, axes, shear) in segments(state, part, style):
                shaped = vector_shape(font, text, size, axes, features, direction, style.get('language', ''), style.get('script', ''), shear)
                path = [(op, *(v + (x if i % 2 == 0 else y) for i, v in enumerate(p))) for op, *p in shaped['path']]
                advance, dy = shaped['advance']
                a, d, gap, underline_y, thickness, strike_y, _ = shaped['metrics']
                ascent, descent = max(ascent, a), max(descent, d)
                line_height = max(line_height, a + d + gap)
                logical_boxes.append([min(x, x+advance), min(y, y+dy)-a, max(x, x+advance), max(y, y+dy)+d])
                if style.get('background'):
                    output.append((rect(x, y-a, advance, a+d), style['background'], 'none', 0))
                output.append((path, color, style.get('stroke', 'none'), number(style.get('stroke_width', 1))))
                thickness = max(thickness, size / 32)
                decoration = []
                u = style.get('underline', 'none')
                if u not in ('none', 'single', 'double', 'curly', 'dotted', 'dashed'):
                    raise ValueError('Invalid underline style')
                if u != 'none':
                    uy = y + max(underline_y, thickness)
                    if u in ('single', 'double'):
                        decoration += rect(x, uy, advance, thickness)
                        if u == 'double':
                            decoration += rect(x, uy + 2*thickness, advance, thickness)
                    else:
                        step = thickness * (4 if u in ('curly', 'dashed') else 2)
                        at = 0.0
                        while at < advance:
                            length = min(step/2, advance-at)
                            if u == 'curly':
                                decoration.extend([(0, x+at, uy), (3, x+at+step/4, uy-2*thickness, x+at+step/4, uy+2*thickness, x+at+length, uy)])
                            else:
                                decoration += rect(x+at, uy, length, thickness)
                            at += step if u != 'curly' else step/2
                if u == 'curly' and decoration:
                    output.append((decoration, 'none', style.get('underline_color', color), thickness))
                    decoration = []
                if style.get('strike', False):
                    decoration += rect(x, y+strike_y, advance, thickness)
                if style.get('overline', False):
                    decoration += rect(x, y-a, advance, thickness)
                if decoration:
                    dc = style.get('underline_color', color)
                    output.append((decoration, dc, 'none', 0))
                missing.extend(index + c for c in shaped['missing'])
                index += len(text)
                x += advance
                y += dy
    width = max(width, x)
    ox, oy, angle = number(obj.get('x', 0)), number(obj.get('y', 0)), math.radians(number(obj.get('angle', 0)))
    c, s = math.cos(angle), math.sin(angle)
    boxes = []
    for i, (path, fill, stroke, w) in enumerate(output):
        if not isinstance(fill, str) or not isinstance(stroke, str) or w < 0:
            raise ValueError('Invalid text paint or stroke width')
        vector_validate_color(fill)
        vector_validate_color(stroke)
        transformed = []
        for op, *p in path:
            q = []
            for px, py in zip(p[0::2], p[1::2]):
                q.extend((ox + c*px - s*py, oy + s*px + c*py))
            transformed.append((op, *q))
        output[i] = transformed, fill, stroke, w
        if fill != 'none' or stroke != 'none':
            b = bounds(transformed)
            if b:
                grow = w/2 if stroke != 'none' else 0
                boxes.append([b[0]-grow, b[1]-grow, b[2]+grow, b[3]+grow])
    ink = [min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes)] if boxes else None
    if logical_boxes:
        width = max(b[2] for b in logical_boxes) - min(b[0] for b in logical_boxes)
    height = max(b[3] for b in logical_boxes) - min(b[1] for b in logical_boxes) if logical_boxes else 0
    metrics = {'width': width, 'height': height, 'advance': [x, y], 'ascent': ascent, 'descent': descent,
               'ink': ink, 'missing': missing}
    return output, metrics


def compile_text(state: dict, data: bytes) -> list:
    return layout(state, decode(base64.b64decode(data, validate=True)))[0]


def service(state: dict, data: bytes, request_id: int) -> bytes:
    try:
        obj = decode(data)
        op = obj.get('op')
        if op == 'fonts':
            ans = {'version': 1, 'features': ['text', 'session-fonts', 'measure', 'clip-box', 'hdr', 'p3', 'rec2020'],
                   'fonts': system_fonts(), 'roles': ['default', *styles.options.font_role],
                   'session': [{'id': k, **v['info']} for k, v in state.setdefault('fonts', {}).items()]}
        elif op == 'inspect':
            font, _, _ = resolve(state, obj)
            ans = vector_font_info(font)
        elif op == 'upload':
            name = obj['id']
            if not isinstance(name, str) or not name:
                raise ValueError('Font id must be a nonempty string')
            raw = base64.b64decode(obj['data'], validate=True)
            font = vector_font(raw, obj.get('index', 0))
            info = vector_font_info(font)
            # Optional face metadata supplies the native angle for synthetic slant.
            slant = info['slant']
            state.setdefault('fonts', {})[name] = {'font': font, 'info': info, 'slant': slant}
            ans = {'id': name, **info}
        elif op == 'release':
            del state.setdefault('fonts', {})[obj['id']]
            ans = {'id': obj['id']}
        elif op == 'measure':
            _, ans = layout(state, obj)
        else:
            raise ValueError('Unknown vector service operation')
        result = 'OK;' + base64.b64encode(json.dumps(ans, separators=(',', ':'), allow_nan=False).encode()).decode('ascii')
    except (ValueError, TypeError, KeyError, OverflowError) as e:
        result = 'EINVAL:' + base64.b64encode(json.dumps({'error': str(e)}).encode()).decode('ascii')
    return f'Gi={request_id};{result}'.encode('utf-8')
