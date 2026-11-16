#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince
"""CPU vector rasterization: kitty/launcher/kitty +launch benchmarks/vector.py.

Reports median render time and a hash of the linear pixels for before/after
comparisons. Scene construction and transmission are outside the timed region.
"""

import argparse
import hashlib
import json
import math
import os
import statistics
import tempfile
import time
from unittest.mock import patch

from kitty_tests.base import BaseTest
from kitty_tests.graphics import parse_full_response, send_command


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeat', type=int, default=7, help='timed renders per scene')
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('--repeat must be positive')
    with tempfile.TemporaryDirectory(prefix='silkie-vector-bench-') as cache, patch.dict(os.environ, {'KITTY_CACHE_DIRECTORY': cache}):
        test = BaseTest()
        screen = test.create_screen()

        def upload(command: str, data: str | bytes) -> None:
            if isinstance(data, str):
                data = data.encode()
            # Match the normal graphics protocol transport chunks.
            for offset in range(0, len(data), 3072):
                more = int(offset + 3072 < len(data))
                response = send_command(screen, (command + ',' if not offset else '') + f'm={more}', data[offset:offset+3072])
            result = parse_full_response(response)
            if result.code != 'OK':
                raise RuntimeError(result)

        square = 'p id=1 d=M0,0L512,0L512,512L0,512Z fill='
        stops = ','.join(f'{i/511},#{i%256:02x}40{255-i%256:02x}' for i in range(512))
        scenes = {
            'solid': square + '#436ab7',
            'gradient-2': 'l id=2 x1=0 y1=0 x2=512 y2=0 stops=0,#ff0000,1,#0000ff\n' + square + '@2',
            'gradient-512': 'l id=2 x1=0 y1=0 x2=512 y2=0 stops=' + stops + '\n' + square + '@2',
            'radial-512': 'r id=2 cx=256 cy=256 r=300 stops=' + stops + '\n' + square + '@2',
            'strokes': '\n'.join(f'p id={i+1} d=M{(i%24)*21},{(i//24)*20}L{(i%24)*21+12},{(i//24)*20+16}'
                                 ' stroke=#436ab7 w=2 cap=round' for i in range(600)),
            'long-stroke': 'p id=1 d=M1,256'
                           + ''.join(f'L{i*510/2499+1:.8f},{256+math.sin(i*.02)*180:.8f}' for i in range(2500))
                           + ' stroke=#436ab7 w=2',
        }
        for fmt, bpp in ((24, 3), (32, 4)):
            upload(f'a=t,f={fmt},i={fmt},s=64,v=64', bytes((i*37) % 256 for i in range(64*64*bpp)))
            scenes[f'raster-{fmt}'] = f'i id=1 img={fmt} t=8,0,0,8,0,0'
        results = {}
        try:
            for name, scene in scenes.items():
                upload('a=t,f=200,i=1,s=512,v=512', scene)
                render = screen.grman.render_vector_linear
                for _ in range(2):
                    render(1, 512, 512)
                durations = []
                for _ in range(args.repeat):
                    start = time.perf_counter()
                    pixels = render(1, 512, 512)
                    durations.append(time.perf_counter() - start)
                results[name] = {'ms': statistics.median(durations) * 1000, 'sha256': hashlib.sha256(pixels).hexdigest()}
        finally:
            test.tearDown()
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
