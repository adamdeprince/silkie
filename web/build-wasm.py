#!/usr/bin/env python3
"""Compile the portable terminal core to browser WASM; no npm dependencies."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=root / 'docs/silkie/_static/demo/silkie-terminal.wasm')
    args = parser.parse_args()
    cargo = shutil.which('cargo')
    if not cargo:
        parser.error('Rust is required. Install rustup and run rustup target add wasm32-unknown-unknown')
    env = os.environ.copy()
    # Prefer rustup's compiler with its installed target over a separate Homebrew
    # rustc. Explicit RUSTC/CARGO overrides remain authoritative.
    rustup_cargo = Path.home() / '.cargo/bin/cargo'
    if not env.get('RUSTC') and rustup_cargo.exists():
        cargo = str(rustup_cargo)
        env['RUSTC'] = str(rustup_cargo.with_name('rustc'))
    cargo = env.get('CARGO', cargo)
    target = Path(env.get('CARGO_TARGET_DIR', root / 'web/wasm/target')).resolve()
    subprocess.run([cargo, 'build', '--release', '--target', 'wasm32-unknown-unknown', '--manifest-path', str(root / 'web/wasm/Cargo.toml'), '--target-dir', str(target)], env=env, check=True)
    artifact = target / 'wasm32-unknown-unknown/release/silkie_browser_terminal.wasm'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(artifact, args.output)
    print(f'Built {args.output} ({args.output.stat().st_size:,} bytes)')


if __name__ == '__main__':
    main()
