#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Adam DePrince
"""Client helpers for Silkie's f=200 scenes and font/measurement service.

Write the yielded bytes through your TUI's output queue. Feed complete APC
bodies from its input parser to parse_service_response(). No terminal reads
or writes occur implicitly.
"""
import base64
import json
import zlib
from collections.abc import Iterable, Iterator, Mapping, Sequence
from typing import Any


def _json(obj: Mapping[str, Any]) -> bytes:
    return json.dumps(obj, ensure_ascii=False, allow_nan=False, separators=(',', ':')).encode('utf-8')


def _id(value: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 < value <= 0xffffffff:
        raise ValueError('IDs must be nonzero unsigned 32-bit integers')
    return value


def transmission(control: Mapping[str, int | str], payload: bytes, compress: bool = False) -> Iterator[bytes]:
    """Generate normal kitty graphics chunks without repeatedly copying the tail."""
    fields = dict(control)
    if compress:
        payload = zlib.compress(payload)
        fields['o'] = 'z'
    view = memoryview(payload)
    # The wire protocol allows 4096 base64 bytes per graphics chunk.
    for offset in range(0, max(1, len(view)), 3072):
        chunk = view[offset:offset+3072]
        more = int(offset+len(chunk) < len(view))
        header = ','.join(f'{k}={v}' for k, v in fields.items())
        yield b'\x1b_G' + (header + (',' if header else '') + f'm={more};').encode('ascii') + base64.b64encode(chunk) + b'\x1b\\'
        fields.clear()


def service_request(request_id: int, obj: Mapping[str, Any], compress: bool = False) -> Iterator[bytes]:
    return transmission({'a': 'v', 'f': 200, 'i': _id(request_id)}, _json(obj), compress)


def font_upload(request_id: int, font_id: str, data: bytes, index: int = 0) -> Iterator[bytes]:
    return service_request(request_id, {'op': 'upload', 'id': font_id, 'data': base64.b64encode(data).decode('ascii'), 'index': index}, True)


def measure_text(request_id: int, layout: Mapping[str, Any]) -> Iterator[bytes]:
    return service_request(request_id, {**layout, 'op': 'measure'})


def parse_service_response(apc: bytes) -> tuple[int, dict[str, Any]]:
    """Accept a complete framed APC or a parser's bare G-prefixed APC body.

    Returns the request ID and decoded JSON; raises ValueError for errors.
    """
    if apc.startswith(b'\x1b_'):
        if not apc.endswith(b'\x1b\\'):
            raise ValueError('Incomplete vector service response')
        apc = apc[2:-2]
    header, sep, body = apc.partition(b';')
    if not sep or not header.startswith(b'G'):
        raise ValueError('Invalid vector service response')
    fields = dict(field.split(b'=', 1) for field in header[1:].split(b','))
    request_id = _id(int(fields[b'i']))
    if body.startswith(b'OK;'):
        result = json.loads(base64.b64decode(body[3:], validate=True))
        if not isinstance(result, dict):
            raise ValueError('Invalid vector service result')
        return request_id, result
    if body.startswith(b'EINVAL:'):
        try:
            error = json.loads(base64.b64decode(body[7:], validate=True))['error']
        except (ValueError, KeyError, TypeError):
            error = body.decode('utf-8', 'replace')
        raise ValueError(error)
    raise ValueError(body.decode('utf-8', 'replace'))


def text_record(element_id: int, layout: Mapping[str, Any], group: int = 0, transform: Sequence[float] | None = None) -> str:
    record = f't id={_id(element_id)} g={_id(group) if group else 0} text=' + base64.b64encode(_json(layout)).decode('ascii')
    if transform is not None:
        if len(transform) != 6:
            raise ValueError('Affine transforms have six components')
        record += ' t=' + ','.join(map(str, transform))
    return record


def clip_group(element_id: int, box: Sequence[float], parent: int = 0, transform: Sequence[float] | None = None) -> str:
    if len(box) != 4 or box[2] < 0 or box[3] < 0:
        raise ValueError('A clip box is x,y,width,height with nonnegative size')
    if element_id:
        _id(element_id)
    record = f'g id={element_id} p={_id(parent) if parent else 0} box=' + ','.join(map(str, box))
    if transform is not None:
        if len(transform) != 6:
            raise ValueError('Affine transforms have six components')
        record += ' t=' + ','.join(map(str, transform))
    return record


def scene(image_id: int, width: int, height: int, records: Iterable[str], action: str = 'T') -> Iterator[bytes]:
    if action not in ('t', 'T', 'e'):
        raise ValueError('Scene actions are t, T or e')
    return transmission({'a': action, 'f': 200, 'i': _id(image_id), 's': width, 'v': height}, '\n'.join(records).encode(), True)
