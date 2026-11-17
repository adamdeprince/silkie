#!/usr/bin/env python3
"""Stage a Homebrew Silkie cask, checking linkage before creating the archive."""
import argparse
import hashlib
import plistlib
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(*args: str) -> str:
    return subprocess.check_output(args, text=True)


def brand_executables(app: Path) -> None:
    """Give packaged processes Silkie names, including bundles from older builds."""
    for plist in app.rglob('Info.plist'):
        if plist.parent.name != 'Contents':
            continue
        info = plistlib.loads(plist.read_bytes())
        quick = plist != app / 'Contents/Info.plist'
        executable = 'silkie-quick-access' if quick else 'silkie'
        bin_dir = plist.parent / 'MacOS'
        previous = bin_dir / info['CFBundleExecutable']
        target = bin_dir / executable
        if previous != target and not (previous.is_symlink() and previous.resolve() == target.resolve()):
            # Older source bundles may have a silkie alias pointing to kitty.
            if target.is_symlink():
                target.unlink()
            elif target.exists():
                raise SystemExit(f'Conflicting bundle executable: {target}')
            previous.rename(target)
            previous.symlink_to(executable)
        info['CFBundleExecutable'] = executable
        plist.write_bytes(plistlib.dumps(info))


def stage(source: Path, output: Path, version: str) -> None:
    app = output / 'silkie.app'
    output.mkdir(parents=True, exist_ok=True)
    if app.exists():
        raise SystemExit(f'Refusing to overwrite {app}')
    shutil.copytree(source, app, symlinks=True)
    brand_executables(app)
    python = Path(run('brew', '--prefix', 'python@3.14').strip()) / 'Frameworks/Python.framework/Versions/3.14/Python'
    if not python.is_file():
        raise SystemExit(f'Missing Homebrew Python runtime: {python}')
    dependencies = {'python@3.14'}
    binaries = []
    for path in app.rglob('*'):
        if path.is_symlink() or not path.is_file():
            continue
        if 'Mach-O' not in run('file', '-b', str(path)):
            continue
        binaries.append(path)
        for line in run('otool', '-L', str(path)).splitlines()[1:]:
            if ' (compatibility version' not in line:
                continue
            library = line.strip().split(' (compatibility version', 1)[0]
            if 'libpython3.14' in library:
                subprocess.run(['install_name_tool', '-change', library, str(python), str(path)], check=True)
                library = str(python)
            if library.startswith('/Users/') or library.startswith('/private/tmp/'):
                raise SystemExit(f'Nonportable library in {path}: {library}')
            if library.startswith('/opt/homebrew/opt/'):
                dependencies.add(library.split('/')[4])
                if not Path(library).exists():
                    raise SystemExit(f'Missing library: {library}')
    resources = app / 'Contents/Resources'
    for name in ('kitty.icns', 'Assets.car'):
        shutil.copy2(ROOT / 'logo' / name, resources / name)
    shutil.copy2(ROOT / 'logo/kitty.icns', resources / 'silkie.icns')
    # Also replace window artwork, not just the Finder/Dock resources.
    for logo_dir in resources.glob('kitty/**/logo'):
        for name in ('kitty.png', 'kitty-128.png', 'kitty.svg'):
            shutil.copy2(ROOT / 'logo' / name, logo_dir / name)
    for plist in app.rglob('Info.plist'):
        if plist.parent.name != 'Contents':
            continue
        info = plistlib.loads(plist.read_bytes())
        quick = plist != app / 'Contents/Info.plist'
        info.update(CFBundleIdentifier='com.goblinreactor.silkie' + ('-quick-access' if quick else ''),
                    CFBundleName='Silkie Quick Access' if quick else 'Silkie',
                    CFBundleDisplayName='Silkie Quick Access' if quick else 'Silkie',
                    CFBundleIconFile='silkie.icns', CFBundleVersion=version)
        plist.write_bytes(plistlib.dumps(info))
        if quick:
            target = plist.parent / 'Resources'
            target.mkdir(exist_ok=True)
            shutil.copy2(ROOT / 'logo/kitty.icns', target / 'silkie.icns')
    for path in binaries:
        subprocess.run(['codesign', '--force', '--sign', '-', str(path)], check=True)
    subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(app)], check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    exe = str(app / 'Contents/MacOS/silkie')
    subprocess.run([exe, '+runpy', 'import ssl; from kitty.fast_data_types import Screen; print("Silkie runtime OK")'], check=True)
    subprocess.run([str(app / 'Contents/MacOS/kitten'), 'show-pointer', '--help'], check=True)
    archive = output / f'silkie-{version}.zip'
    subprocess.run(['ditto', '-c', '-k', '--keepParent', str(app), str(archive)], check=True)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    recipe = '\n'.join(f'  depends_on formula: "{dep}"' for dep in sorted(dependencies))
    (output / 'silkie.rb').write_text(f'''cask "silkie" do
  version "{version}"
  sha256 "{digest}"
  url "https://brew.goblinreactor.com/casks/silkie-#{{version}}.zip"
  name "Silkie"
  desc "Silkie terminal emulator"
  homepage "https://silkie.goblinreactor.com/"
  depends_on arch: :arm64
  depends_on macos: :tahoe
{recipe}
  app "silkie.app"
  binary "#{{appdir}}/silkie.app/Contents/MacOS/silkie", target: "silkie"
  binary "#{{appdir}}/silkie.app/Contents/MacOS/kitten", target: "silkie-kitten"
  postflight do
    # Ad-hoc signed and not notarized: Gatekeeper refuses the quarantined download.
    system_command "/usr/bin/xattr", args: ["-dr", "com.apple.quarantine", "#{{appdir}}/silkie.app"]
  end
  caveats <<~EOS
    Launch with silkie; companion tools use silkie-kitten.
    Configuration: ~/.config/kitty/kitty.conf.
    This build is ad-hoc signed and is not Apple-notarized, so the cask
    clears its quarantine flag after installing it.
  EOS
end
''')
    print(f'Validated archive: {archive}\nSHA256: {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    stage(args.app.resolve(), args.output.resolve(), args.version)
