#!/usr/bin/env python3
"""Stage browser integration assets for Sphinx's normal html/ build."""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'docs/silkie/_static/demo')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name in ('app.mjs', 'i18n.mjs', 'config.mjs', 'runtime.mjs', 'terminal.mjs', 'demo.css', 'selection.mjs', 'vector.mjs', 'color.mjs', 'input.mjs', 'ink.mjs'):
        source = ROOT / 'web' / name
        if not source.is_file():
            parser.error(f'Missing browser asset: {source}')
        shutil.copyfile(source, args.output / name)
    print(f'Browser assets staged in {args.output}')


if __name__ == '__main__':
    main()
