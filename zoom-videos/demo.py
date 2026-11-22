"""Run the same Hamingjur zoom demo in Kitty or Silkie. Press q to exit."""

import argparse
import io
from pathlib import Path

from hamingjur import Terminal, graphics
from PIL import Image


LEAF_SVG = Path(__file__).with_name('leaf.svg').read_bytes()
PHOTO, LEAF = 1, 2


def leaf_upload(pixels):
    # Pass the original drawing verbatim. Hamingjur uses these dimensions for
    # its PNG fallback and Silkie retains the SVG at the same placement.
    return graphics.svg(LEAF_SVG, image_id=LEAF, width=pixels, height=pixels,
                        display=False, compress=True, q=2)


def show(source):
    term = Terminal()
    with term.fullscreen(), term.hidden_cursor(), term.cbreak(), term.notify_on_resize():
        caps = term.silkie.capabilities(timeout=0.5)
        retains_svg = caps is not None and 201 in caps.formats
        last_grid = last_pixels = None
        while True:
            cell = term.get_cell_size(force=True)
            if cell is None:
                raise RuntimeError('The terminal did not report its cell size')
            grid = term.width, term.height
            columns = max(1, min(8, (grid[0] - 1) // 2))
            pixels = columns * cell.width
            left = max(0, (grid[0] - 2 * columns - 1) // 2)
            top = max(0, (grid[1] - 8) // 2)
            moved = grid != last_grid
            if moved:
                term.send(term.clear + term.move_xy(left, top)
                          + '\x1b[1;38;2;206;228;165mgoblin study\x1b[0m'
                          + term.move_xy(left, top + 1)
                          + '\x1b[38;2;228;233;211mFine detail\x1b[0m')
            if pixels != last_pixels:
                with io.BytesIO() as png:
                    source.resize((pixels, pixels), Image.Resampling.LANCZOS).save(png, 'PNG')
                    term.graphics.send(graphics.image(png.getvalue(), image_id=PHOTO, display=False, q=2))
                if last_pixels is None or not retains_svg:
                    term.graphics.send(leaf_upload(pixels))
            if moved or pixels != last_pixels:
                term.send(term.move_xy(left, top + 3))
                term.graphics.send(graphics.place(PHOTO, p=PHOTO, c=columns, C=1, q=2))
                if moved or not retains_svg:
                    term.send(term.move_xy(left + columns + 1, top + 3))
                    term.graphics.send(graphics.place(LEAF, p=LEAF, c=columns, C=1, q=2))
            last_grid, last_pixels = grid, pixels
            while True:
                event = term.inkey()
                if event == 'q' or event.name == 'KEY_ESCAPE':
                    return
                if event.name == 'RESIZE_EVENT':
                    break


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('goblin', type=Path, nargs='?', default=Path.home() / 'Downloads' / 'goblin.png')
    with Image.open(parser.parse_args().goblin) as original:
        show(original.convert('RGBA'))
