#!/usr/bin/env python
# License: GPL v3 Copyright: 2017, Kovid Goyal <kovid at kovidgoyal.net>

"""Export Silkie's Goblin artwork for application and terminal window icons."""

import argparse
import base64
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

BASE = Path(__file__).resolve().parent
SOURCE = BASE / 'silkie.png'


def export_icons(image: Image.Image) -> None:
    # Keep the inherited resource names used by the launchers and packagers.
    for size, name in ((256, 'kitty.png'), (128, 'kitty-128.png')):
        image.resize((size, size), Image.Resampling.LANCZOS).save(BASE / name)
    image.resize((1024, 1024), Image.Resampling.LANCZOS).save(BASE / 'kitty.icns')
    embedded = base64.b64encode(SOURCE.read_bytes()).decode('ascii')
    width, height = image.size
    (BASE / 'kitty.svg').write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{width}" height="{height}" viewBox="0 0 {width} {height}">'
        '<title>Silkie: Goblin behind a terminal</title>'
        '<desc>Black ink on white woodcut of a fluffy black-and-white Silkie chicken behind a terminal.</desc>'
        f'<image width="{width}" height="{height}" xlink:href="data:image/png;base64,{embedded}"/></svg>\n'
    )


def export_macos_assets(image: Image.Image) -> None:
    with tempfile.TemporaryDirectory(prefix='silkie-icon-') as temporary:
        work = Path(temporary)
        catalog = work / 'Silkie.xcassets'
        app_icon = catalog / 'kitty.appiconset'
        artwork = catalog / 'SilkieArtwork.imageset'
        app_icon.mkdir(parents=True)
        artwork.mkdir()
        info = {'author': 'Silkie', 'version': 1}
        (catalog / 'Contents.json').write_text(json.dumps({'info': info}))
        images = []
        # These sizes and scales are the macOS app icon catalog format.
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                pixels = size * scale
                filename = f'icon_{size}x{size}@{scale}x.png'
                image.resize((pixels, pixels), Image.Resampling.LANCZOS).save(app_icon / filename)
                images.append({'filename': filename, 'idiom': 'mac', 'size': f'{size}x{size}', 'scale': f'{scale}x'})
        (app_icon / 'Contents.json').write_text(json.dumps({'images': images, 'info': info}))
        image.save(artwork / 'silkie.png')
        (artwork / 'Contents.json').write_text(
            json.dumps(
                {
                    'images': [{'filename': 'silkie.png', 'idiom': 'universal'}],
                    'info': info,
                }
            )
        )
        subprocess.run(
            [
                'xcrun',
                'actool',
                '--compile',
                str(work),
                '--platform',
                'macosx',
                '--minimum-deployment-target',
                '12.0',
                '--app-icon',
                'kitty',
                '--output-partial-info-plist',
                str(work / 'icon-info.plist'),
                str(catalog),
            ],
            check=True,
        )
        shutil.copy2(work / 'Assets.car', BASE / 'Assets.car')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-macos-assets', action='store_true', help='Export PNG, SVG and ICNS without invoking Xcode')
    args = parser.parse_args()
    with Image.open(SOURCE) as source:
        image = source.convert('RGB')
    if image.width != image.height:
        raise SystemExit(f'The source icon must be square, got {image.width}x{image.height}')
    export_icons(image)
    if sys.platform == 'darwin' and not args.skip_macos_assets:
        export_macos_assets(image)
    print(f'Exported Silkie icons from {SOURCE}')


if __name__ == '__main__':
    main()
