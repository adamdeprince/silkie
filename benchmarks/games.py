#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince
"""Replay captured game vector workloads: kitty/launcher/kitty +launch benchmarks/games.py.

Fixtures contain Scramble's seed-5 level-1 ground/canopy and its four actual
fence edits during 1800 simulation ticks. Its 200x56-cell, 18x36-pixel-cell
window displays the 2048x1152 scene at 3456x1944. Ground Stop uses seed 7 at
120 seconds, a selected aircraft and prediction, a 1060x900 field, and 51
actual radar patches over 61 display frames. Its map-pan camera transforms
are synthetic examples of the game's root-transform records. Sprint contains
the real Country Lane ground/canopy at 3600x1800 and generated nest decal
transitions using the game's decal_elements(), rather than simulation events.

Full renders measure linear output; damage updates use the existing RGBA8
test surface, including its output conversion. Geometry, anti-aliasing and
damage selection are the same as the GPU path. Hashes permit output comparison.
"""

import argparse
import base64
import gzip
import hashlib
import json
import os
import statistics
import tempfile
import time
import zlib
from pathlib import Path
from unittest.mock import patch

from kitty_tests.base import BaseTest, parse_bytes


def command(records: str, width: int, height: int, edit: bool = False) -> bytes:
    data = records.encode()
    control = 'a=e,i=1' if edit else f'a=t,f=200,i=1,s={width},v={height}'
    if len(data) > 2048:
        data = zlib.compress(data, 1)
        control += ',o=z'
    encoded = base64.b64encode(data)
    chunks = []
    for offset in range(0, len(encoded), 4096):
        more = int(offset + 4096 < len(encoded))
        header = control + ',' if not offset else ''
        chunks.append(b'\x1b_G' + f'{header}m={more};'.encode() + encoded[offset:offset+4096] + b'\x1b\\')
    return b''.join(chunks)


def run(case: dict, repeat: int) -> dict:
    test = BaseTest()
    screen = test.create_screen()
    manager = screen.grman
    width, height = case['width'], case['height']
    rw, rh = case.get('render_width', width), case.get('render_height', height)
    initial = command(case['initial'], width, height)
    updates = [command(records, width, height, True) for records in case['updates']]

    def send(data: bytes) -> None:
        screen.callbacks.clear()
        parse_bytes(screen, data)
        if b';OK' not in screen.callbacks.wtcbuf:
            raise RuntimeError(screen.callbacks.wtcbuf)

    try:
        start = time.perf_counter()
        send(initial)
        parse_ms = (time.perf_counter() - start) * 1000
        full_times = []
        for _ in range(repeat):
            start = time.perf_counter()
            linear = manager.render_vector_linear(1, rw, rh)
            full_times.append((time.perf_counter() - start) * 1000)
        full_hash = hashlib.sha256(linear).hexdigest()
        edit_times, damage_times, areas, hashes = [], [], [], []
        for _ in range(repeat):
            send(initial)
            pixels = bytearray()
            manager.update_vector_pixels(1, rw, rh, pixels)
            for update in updates:
                start = time.perf_counter()
                send(update)
                edit_times.append((time.perf_counter() - start) * 1000)
                start = time.perf_counter()
                rect = manager.update_vector_pixels(1, rw, rh, pixels)
                damage_times.append((time.perf_counter() - start) * 1000)
                areas.append(rect[2] * rect[3] if rect else 0)
            hashes.append(hashlib.sha256(pixels).hexdigest())
        if len(set(hashes)) != 1:
            raise RuntimeError(f'Unstable repeated renders in {case["name"]}')
        return {'name': case['name'], 'size': [rw, rh], 'records': len(case['initial'].splitlines()),
                'updates': len(updates), 'initial_parse_ms': parse_ms, 'linear_full_ms': statistics.median(full_times),
                'edit_median_ms': statistics.median(edit_times) if edit_times else 0,
                'damage_median_ms': statistics.median(damage_times) if damage_times else 0,
                'damage_pixels_median': statistics.median(areas) if areas else 0,
                'linear_sha256': full_hash, 'updated_rgba_sha256': hashes[0]}
    finally:
        test.tearDown()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--case', action='append', help='case name; repeat to select several')
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('--repeat must be positive')
    fixtures = json.loads(gzip.decompress((Path(__file__).parent / 'data/games.json.gz').read_bytes()))
    results = []
    with tempfile.TemporaryDirectory(prefix='silkie-games-bench-') as cache, patch.dict(os.environ, {'KITTY_CACHE_DIRECTORY': cache}):
        for case in fixtures:
            if not args.case or case['name'] in args.case:
                result = run(case, args.repeat)
                results.append(result)
                print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
