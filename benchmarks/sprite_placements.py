#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince
"""Cached-sheet placement updates: kitty/launcher/kitty +launch benchmarks/sprite_placements.py.

Measures protocol parsing and placement updates, excluding frame construction,
GPU presentation, and sprite uploads. The smallest case matches Scramble's
captured maximum of 26 sprites; larger cases exercise sustained growth.
"""

import argparse
import json
import os
import statistics
import tempfile
import time
from unittest.mock import patch

from kitty_tests.base import BaseTest, parse_bytes
from kitty_tests.graphics import parse_full_response, send_command


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', type=int, default=100, help='measured update frames after warmup')
    parser.add_argument('--count', type=int, action='append', help='number of placements; repeat for several cases')
    args = parser.parse_args()
    counts = args.count or (26, 32, 96, 256, 1024, 4096)
    if args.frames < 1 or any(count < 1 for count in counts):
        parser.error('--frames and --count must be positive')
    results = {}
    with tempfile.TemporaryDirectory(prefix='silkie-placement-bench-') as cache, patch.dict(os.environ, {'KITTY_CACHE_DIRECTORY': cache}):
        for count in counts:
            test = BaseTest()
            screen = test.create_screen(cols=160, lines=50, cell_width=8, cell_height=16)
            try:
                response = parse_full_response(send_command(screen, 'a=t,f=32,i=1,s=64,v=64', bytes((180, 140, 70, 255)) * 4096))
                if response.code != 'OK':
                    raise RuntimeError(response)
                durations = []
                warmup = 20
                for frame in range(warmup + args.frames):
                    data = ''.join(
                        f'\x1b[{2+j%45};{1+(j+frame)%155}H'
                        f'\x1b_Ga=p,i=1,p={j+1},q=2,C=1,x={frame%4*16},y=0,w=16,h=16,z={-1900000000+j%45*8}\x1b\\'
                        for j in range(count)
                    ).encode()
                    start = time.perf_counter()
                    parse_bytes(screen, data)
                    duration = time.perf_counter() - start
                    if frame >= warmup:
                        durations.append(duration)
                results[count] = statistics.median(durations) * 1000
            finally:
                test.tearDown()
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
