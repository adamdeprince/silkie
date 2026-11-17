#!/usr/bin/env python3
"""Build a CheerpX i386 guest without Docker, root, or modifying game sources.

Requires Rust/rustup with i686-unknown-linux-musl and e2fsprogs (mke2fs,
debugfs). Network access is needed only for missing, SHA256-pinned downloads
and Cargo dependencies. The games are linked statically with Rust's bundled
LLD, using the source checkout's Cargo.lock.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen
import uuid

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
TARGET = 'i686-unknown-linux-musl'
BLOCK = 4096  # ext2 filesystem block size, not an input or stream limit.
GAMES = {
    'silkie-sprint': ('--no-tempfile',),
    'silkie-scramble': ('--no-tempfile',),
    'presidential': (),  # Charts and maps already use inline graphics.
    'silkie-loophole': ('--no-tempfile',),
    'ground-stop': ('--no-tempfile',),
}
# Math functions each game links from guest/math rather than i386 musl, whose
# versions use x87 instructions that are slow under CheerpX. Only functions a
# game's measured hot paths use are listed; results keep full precision.
GUEST_MATH = {
    'presidential': ('exp', 'log', 'pow', 'expf', 'logf', 'powf'),
    'ground-stop': ('exp', 'exp2', 'log', 'log2', 'log10', 'pow', 'powf',
                    'asin', 'acos', 'atan', 'atan2', 'acosf', 'atan2f',
                    'floor', 'ceil', 'trunc', 'floorf', 'ceilf', 'truncf',
                    'fmod', 'fmodf', 'hypot', 'hypotf', 'ldexp', 'scalbn'),
}


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(*args: str | Path, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def download(item: dict, cache: Path) -> Path:
    name = item['url'].rsplit('/', 1)[1]
    target = cache / name
    if target.exists() and digest(target) == item['sha256']:
        return target
    print(f'Downloading {name}', flush=True)
    with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            with urlopen(item['url'], timeout=60) as source:
                shutil.copyfileobj(source, stream)
            stream.close()
            if digest(temporary) != item['sha256']:
                raise ValueError(f'SHA256 mismatch for {name}')
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
    return target


def unpack(archive: Path, root: Path) -> None:
    # APK v2 concatenates signature, metadata, and data gzip/tar members.
    # ignore_zeros lets tarfile consume all three rather than just the signature.
    with tarfile.open(archive, 'r:gz', ignore_zeros=True) as source:
        for member in source:
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts:
                raise ValueError(f'Unsafe archive path: {member.name}')
            if not name.parts or name.parts[0].startswith('.'):
                continue  # APK signatures/control records are not guest files.
            destination = root.joinpath(*name.parts)
            if not destination.parent.resolve().is_relative_to(root.resolve()):
                raise ValueError(f'Archive traverses a symlink: {member.name}')
            destination.parent.mkdir(parents=True, exist_ok=True)
            if member.isdir():
                destination.mkdir(exist_ok=True)
                destination.chmod(member.mode & 0o7777)
            elif member.isfile():
                destination.unlink(missing_ok=True)
                with source.extractfile(member) as data, destination.open('wb') as output:
                    shutil.copyfileobj(data, output)
                destination.chmod(member.mode & 0o7777)
            elif member.issym():
                destination.unlink(missing_ok=True)
                destination.symlink_to(member.linkname)
            elif member.islnk():
                link = PurePosixPath(member.linkname)
                if link.is_absolute() or '..' in link.parts:
                    raise ValueError(f'Unsafe hard link: {member.linkname}')
                target = root.joinpath(*link.parts)
                if not target.resolve().is_relative_to(root.resolve()):
                    raise ValueError(f'Hard link traverses a symlink: {member.linkname}')
                destination.unlink(missing_ok=True)
                os.link(target, destination)
            # Device files are supplied by CheerpX's /dev mount.


def elf_i386_static(path: Path) -> None:
    with path.open('rb') as stream:
        header = stream.read(52)  # ELF32 header, fixed by the ELF specification.
        if len(header) != 52 or header[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', header, 18)[0] != 3:
            raise ValueError(f'{path.name} must be a little-endian i386 ELF executable')
        offset = struct.unpack_from('<I', header, 28)[0]
        entry_size, count = struct.unpack_from('<HH', header, 42)
        if entry_size < 32 or offset + count * entry_size > path.stat().st_size:
            raise ValueError(f'Invalid ELF program headers in {path.name}')
        for number in range(count):
            stream.seek(offset + number * entry_size)
            if struct.unpack('<I', stream.read(4))[0] == 3:  # PT_INTERP
                raise ValueError(f'{path.name} must be static (no ELF interpreter)')


def rust_tools() -> tuple[Path, Path, Path]:
    # Explicit paths avoid mixing Homebrew Cargo/rustc with rustup's stdlib.
    def which(name: str) -> Path:
        return Path(run('rustup', 'which', '--toolchain', 'stable', name, capture_output=True, text=True).stdout.strip())
    cargo, rustc = which('cargo'), which('rustc')
    sysroot = Path(run(rustc, '--print', 'sysroot', capture_output=True, text=True).stdout.strip())
    host = next(line.split(': ', 1)[1] for line in run(rustc, '-vV', capture_output=True, text=True).stdout.splitlines() if line.startswith('host: '))
    linker = sysroot / 'lib/rustlib' / host / 'bin/rust-lld'
    if not linker.is_file():
        raise ValueError(f'Rust bundled LLD is missing: {linker}')
    if not (sysroot / 'lib/rustlib' / TARGET).is_dir():
        raise ValueError(f'Install the guest target with: rustup target add {TARGET} --toolchain stable')
    return cargo, rustc, linker


def build_guest_math(cache: Path) -> Path:
    """Compile SSE2 math instead of emulating musl's x87 log/power routines."""
    cargo, rustc, linker = rust_tools()
    env = os.environ.copy()
    env.update({
        'RUSTC': str(rustc),
        'CARGO_TARGET_DIR': str(cache / 'math-target'),
        'CARGO_TARGET_I686_UNKNOWN_LINUX_MUSL_LINKER': str(linker),
        'CARGO_TARGET_I686_UNKNOWN_LINUX_MUSL_RUSTFLAGS': '-C linker-flavor=ld.lld',
    })
    run(cargo, 'build', '--locked', '--release', '--target', TARGET,
        '--manifest-path', HERE / 'guest/math/Cargo.toml', env=env)
    library = cache / 'math-target' / TARGET / 'release/libsilkie_guest_math.a'
    # Cargo does not track the contents of a raw linker argument. An immutable
    # pathname makes changes to the shim invalidate the executable's build.
    libraries = cache / 'math-libraries'
    libraries.mkdir(parents=True, exist_ok=True)
    destination = libraries / f'{digest(library)}.a'
    if not destination.is_file():
        with tempfile.TemporaryDirectory(dir=libraries) as staging:
            temporary = Path(staging) / 'math.a'
            shutil.copyfile(library, temporary)
            os.replace(temporary, destination)
    return destination


def build_game(name: str, source: Path, cache: Path) -> Path:
    cargo, rustc, linker = rust_tools()
    env = os.environ.copy()
    env.update({
        'RUSTC': str(rustc),
        'CARGO_TARGET_DIR': str(cache / 'cargo-target'),
        'CARGO_TARGET_I686_UNKNOWN_LINUX_MUSL_LINKER': str(linker),
        'CARGO_TARGET_I686_UNKNOWN_LINUX_MUSL_RUSTFLAGS':
            f'-C linker-flavor=ld.lld -C strip=symbols --remap-path-prefix={source}=/src/{name}',
    })
    command = [cargo, 'build', '--locked', '--release', '--target', TARGET,
               '--manifest-path', source / 'Cargo.toml', '--bin', name]
    if name in GUEST_MATH:
        # Link only this guest executable against the math shim. Passing the
        # flags to the final crate avoids recompiling unrelated dependencies.
        library = build_guest_math(cache)
        command[1] = 'rustc'
        command.extend(['-p', name, '--', '-C', f'link-arg={library}'])
        for function in GUEST_MATH[name]:
            command.extend(['-C', f'link-arg=--wrap={function}'])
    run(*command, env=env)
    return cache / 'cargo-target' / TARGET / 'release' / name


def build_sprite_cache(name: str, source: Path, cache: Path) -> Path:
    """Paint every supported sprite resolution natively, outside CheerpX."""
    identity = hashlib.sha256()
    inputs = [source / 'Cargo.toml', source / 'Cargo.lock',
              *sorted((source / 'src').rglob('*')), *sorted((source / 'assets').rglob('*'))]
    for path in inputs:
        if path.is_file():
            identity.update(str(path.relative_to(source)).encode() + b'\0')
            identity.update(bytes.fromhex(digest(path)))
    destination = cache / 'sprites' / name / identity.hexdigest()
    if destination.is_dir():
        return destination
    cargo, rustc, _ = rust_tools()
    host = next(line.split(': ', 1)[1] for line in run(rustc, '-vV', capture_output=True, text=True).stdout.splitlines() if line.startswith('host: '))
    native_target = cache / 'native-target'
    env = os.environ.copy()
    env.update({'RUSTC': str(rustc), 'CARGO_TARGET_DIR': str(native_target)})
    env.pop('SILKIE_SPRITE_CACHE', None)
    run(cargo, 'build', '--locked', '--release', '--target', host, '--lib',
        '--manifest-path', source / 'Cargo.toml', env=env)
    library = name.replace('-', '_')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sprite-build-', dir=destination.parent) as directory:
        staging = Path(directory)
        helper = staging / 'paint.rs'
        helper.write_text(f'fn main() {{ {library}::render::sprite_cache::write_all(&std::path::PathBuf::from(std::env::args_os().nth(1).unwrap())).unwrap(); }}\n')
        release = native_target / host / 'release'
        run(rustc, '--edition=2024', '--target', host, '-O', helper,
            '--extern', f'{library}={release / ("lib" + library + ".rlib")}',
            '-L', f'dependency={release / "deps"}',
            '-L', f'dependency={native_target / "release/deps"}', '-o', staging / 'paint')
        print(f'Preparing {name} sprite art for the browser guest', flush=True)
        run(staging / 'paint', staging / 'art', env=env)
        os.replace(staging / 'art', destination)
    return destination


def ext2_tool(name: str) -> str:
    # Homebrew keeps e2fsprogs keg-only; Android's unrelated mke2fs may be on PATH.
    for base in (Path('/opt/homebrew/opt/e2fsprogs/sbin'), Path('/usr/local/opt/e2fsprogs/sbin')):
        if (base / name).is_file():
            return str(base / name)
    result = shutil.which(name)
    if not result:
        raise ValueError(f'{name} is required; install e2fsprogs')
    return result


def image_dimensions(root: Path, free_ratio: float) -> tuple[int, int]:
    # Account for allocated data, directories, inode tables, indirect-block
    # metadata, and content-sized writable headroom. No fixed image-size cap.
    entries = list(root.rglob('*'))
    data = sum(math.ceil(max(path.lstat().st_size, 1) / BLOCK) * BLOCK for path in entries)
    inodes = math.ceil((len(entries) + 11) * (1 + free_ratio))
    total = math.ceil((data + inodes * 128) * (1 + free_ratio) * 1.1 / BLOCK) * BLOCK
    return total, inodes


def make_image(root: Path, output: Path, free_ratio: float, epoch: int) -> None:
    size, inodes = image_dimensions(root, free_ratio)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('wb') as stream:
        stream.truncate(size)
    env = os.environ.copy()
    env['MKE2FS_CONFIG'] = str(HERE / 'guest/mke2fs.conf')
    env['E2FSPROGS_FAKE_TIME'] = str(epoch)
    identity = hashlib.sha256()
    for path in sorted(root.rglob('*')):
        identity.update(str(path.relative_to(root)).encode())
        identity.update(str(stat.S_IMODE(path.lstat().st_mode)).encode())
        if path.is_symlink():
            identity.update(os.readlink(path).encode())
        elif path.is_file():
            identity.update(bytes.fromhex(digest(path)))
    filesystem_uuid = str(uuid.UUID(bytes=identity.digest()[:16], version=5))
    run(ext2_tool('mke2fs'), '-q', '-F', '-t', 'ext2', '-I', '128',
        '-b', str(BLOCK), '-O', 'none', '-N', str(inodes), '-m', '0',
        '-U', filesystem_uuid, '-E', f'root_owner=0:0,hash_seed={filesystem_uuid}',
        '-d', root, output, env=env)
    with output.open('rb') as stream:
        stream.seek(1024 + 92)
        if struct.unpack('<III', stream.read(12)) != (0, 0, 0):
            raise ValueError('Formatter enabled features incompatible with ext2 revision 0')
    # Staging is unprivileged. Set guest ownership in the image, rather than
    # chowning files on the developer's host or requiring a mounted filesystem.
    with tempfile.NamedTemporaryFile('w', dir=output.parent, delete=False) as commands:
        command_file = Path(commands.name)
        try:
            commands.write('set_super_value rev_level 0\n')
            for path in [root, *root.rglob('*')]:
                relative = '/' + str(path.relative_to(root)) if path != root else '/'
                if any(char in relative for char in ('"', '\n', '\r', '\\')):
                    raise ValueError(f'Unsupported debugfs path: {relative!r}')
                owner = 1000 if relative == '/home/demo' or relative.startswith('/home/demo/') else 0
                commands.write(f'set_inode_field "{relative}" uid {owner}\n')
                commands.write(f'set_inode_field "{relative}" gid {owner}\n')
                for field in ('atime', 'mtime', 'ctime'):
                    commands.write(f'set_inode_field "{relative}" {field} @{epoch}\n')
            commands.close()
            run(ext2_tool('debugfs'), '-w', '-f', command_file, output,
                stdout=subprocess.DEVNULL, env=env)
        finally:
            command_file.unlink(missing_ok=True)


def populate(root: Path, binaries: dict[str, Path], sources: Path, console_session: Path,
             sprite_caches: dict[str, Path] | None = None) -> None:
    for directory in ('dev', 'proc', 'tmp', 'run/silkie', 'home/demo', 'usr/local/bin', 'usr/local/libexec', 'usr/share/silkie/sources'):
        (root / directory).mkdir(parents=True, exist_ok=True)
    (root / 'tmp').chmod(0o1777)
    (root / 'home/demo').chmod(0o700)
    for name in ('silkie-demo-shell', 'silkie-features'):
        shutil.copyfile(HERE / 'guest' / name, root / 'usr/local/bin' / name)
        (root / 'usr/local/bin' / name).chmod(0o755)
    shutil.copyfile(HERE / 'guest/silkie-demo.bashrc', root / 'etc/silkie-demo.bashrc')
    elf_i386_static(console_session)
    shutil.copyfile(console_session, root / 'usr/local/libexec/silkie-console-session')
    (root / 'usr/local/libexec/silkie-console-session').chmod(0o755)
    shutil.copytree(HERE / 'guest/console-session', root / 'usr/share/silkie/sources/console-session',
                    ignore=shutil.ignore_patterns('target'))
    shutil.copytree(HERE / 'guest/math', root / 'usr/share/silkie/sources/guest-math',
                    ignore=shutil.ignore_patterns('target'))
    for name, binary in binaries.items():
        if sprite_caches and name in sprite_caches:
            shutil.copytree(sprite_caches[name], root / 'usr/share/silkie/sprites' / name)
        elf_i386_static(binary)
        shutil.copyfile(binary, root / 'usr/local/libexec' / name)
        (root / 'usr/local/libexec' / name).chmod(0o755)
        wrapper = root / 'usr/local/bin' / name
        options = ''.join(f' {option}' for option in GAMES[name])
        wrapper.write_text(f'#!/bin/sh\nexec /usr/local/libexec/silkie-console-session /usr/local/libexec/{name}{options} "$@"\n')
        wrapper.chmod(0o755)
        # Include the game source and asset/license notices corresponding to
        # these binaries. Never copy target/, credentials, or checkout metadata.
        source = sources / name
        destination = root / 'usr/share/silkie/sources' / name
        destination.mkdir()
        for part in ('src', 'assets', 'crates', 'data', 'Cargo.toml', 'Cargo.lock', 'README.md', 'LICENSE', 'LICENSE.md', 'NOTICE'):
            item = source / part
            if item.is_dir():
                shutil.copytree(item, destination / part, ignore=shutil.ignore_patterns('target', '.git'))
            elif item.is_file():
                shutil.copyfile(item, destination / part)
    with (root / 'etc/passwd').open('a') as stream:
        stream.write('demo:x:1000:1000:Silkie demo:/home/demo:/bin/bash\n')
    with (root / 'etc/group').open('a') as stream:
        stream.write('demo:x:1000:\n')
    (root / 'home/demo/README').write_text(
        'Silkie browser demo\n\n'
        'Games: ' + ', '.join(binaries) + '.\n'
        'Run silkie-features for browser capabilities and limitations.\n'
        'Use the terminal Tailscale link to connect your own network.\n'
        'The guest localhost is separate from your host computer.\n'
        'Bundled game sources: /usr/share/silkie/sources\n')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=PROJECT / 'docs/silkie/_static/demo',
                        help='Directory for guest.json and the image, which is named guest-<sha256>.ext2')
    parser.add_argument('--games-dir', type=Path, default=PROJECT.parent)
    parser.add_argument('--game-binaries', type=Path, help='Reuse previously compiled static i386 game binaries from this directory')
    parser.add_argument('--cache', type=Path, default=Path(tempfile.gettempdir()) / 'silkie-guest-build')
    parser.add_argument('--free-space-ratio', type=float, default=1.0,
                        help='Writable free capacity relative to bundled content (default: 1.0); not an image size cap')
    args = parser.parse_args()
    if not math.isfinite(args.free_space_ratio) or args.free_space_ratio < 0:
        parser.error('--free-space-ratio must be a finite nonnegative number')
    args.cache.mkdir(parents=True, exist_ok=True)
    lock = json.loads((HERE / 'guest/alpine.lock.json').read_text())
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '1792608778'))  # Locked rootfs release date.
    archives = [download(item, args.cache) for item in [lock['rootfs'], *lock['packages']]]
    binaries = {
        name: args.game_binaries / name if args.game_binaries else build_game(name, args.games_dir / name, args.cache)
        for name in GAMES
    }
    console_session = build_game('silkie-console-session', HERE / 'guest/console-session', args.cache)
    sprite_caches = {name: build_sprite_cache(name, args.games_dir / name, args.cache)
                     for name in ('silkie-sprint', 'silkie-scramble')}
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='rootfs-', dir=args.cache) as staging, \
            tempfile.TemporaryDirectory(prefix='.guest-build-', dir=args.output_dir) as artifact_dir:
        root = Path(staging)
        for archive in archives:
            unpack(archive, root)
        populate(root, binaries, args.games_dir, console_session, sprite_caches)
        for path in [root, *root.rglob('*')]:
            os.utime(path, (epoch, epoch), follow_symlinks=False)
        image = Path(artifact_dir) / 'guest.ext2'
        make_image(root, image, args.free_space_ratio, epoch)
        image_digest = digest(image)
        # Named by content, as the website serves it: pages already open keep
        # reading their image, and guest.json is the same here as on the site.
        output = args.output_dir / f'guest-{image_digest}.ext2'
        metadata = {
            'image': output.name, 'sha256': image_digest,
            'bytes': image.stat().st_size, 'architecture': 'i386',
            'distribution': f"Alpine Linux {lock['version']}",
            'entry': '/usr/local/bin/silkie-demo-shell',
            'cwd': '/home/demo', 'uid': 1000, 'gid': 1000,
            'games': {name: {'sha256': digest(binary)} for name, binary in binaries.items()},
            'console_session': {'sha256': digest(console_session)},
        }
        manifest = Path(artifact_dir) / 'guest.json'
        manifest.write_text(json.dumps(metadata, indent=2) + '\n')
        manifest_path = args.output_dir / 'guest.json'
        previous = previous_image(manifest_path)
        # A failed download/compile/format must leave the previous demo usable.
        os.replace(image, output)
        os.replace(manifest, manifest_path)
    if previous is not None and previous != output:
        previous.unlink(missing_ok=True)
    print(f'Built {output} ({metadata["bytes"]:,} bytes)')


def previous_image(manifest: Path) -> Path | None:
    """The image the current guest.json names, to remove once replaced."""
    try:
        name = json.loads(manifest.read_text()).get('image')
    except (OSError, ValueError, AttributeError):
        return None
    if not isinstance(name, str) or not name or Path(name).name != name or name in ('.', '..'):
        return None
    return manifest.parent / name


if __name__ == '__main__':
    main()
