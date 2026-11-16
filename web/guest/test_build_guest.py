"""Behavior checks for archives, executable compatibility, and image sizing."""

import gzip
import importlib.util
import io
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('build_guest', Path(__file__).parents[1] / 'build-guest.py')
guest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guest)


def archive_member(name, payload):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w') as archive:
        member = tarfile.TarInfo(name)
        member.size = len(payload)
        archive.addfile(member, io.BytesIO(payload))
    return gzip.compress(output.getvalue())


class GuestBuilderTests(unittest.TestCase):
    def test_reads_all_concatenated_apk_streams_without_copying_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'package.apk'
            archive.write_bytes(archive_member('.SIGN.test', b'signature') +
                                archive_member('.PKGINFO', b'control') +
                                archive_member('usr/bin/test', b'executable\x00bytes'))
            output = root / 'output'
            output.mkdir()
            guest.unpack(archive, output)
            self.assertEqual((output / 'usr/bin/test').read_bytes(), b'executable\x00bytes')
            self.assertFalse((output / '.PKGINFO').exists())

    def test_rejects_parent_traversal_and_host_symlink_traversal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / 'output'
            output.mkdir()
            archive = root / 'package.apk'
            archive.write_bytes(archive_member('../outside', b'escape'))
            with self.assertRaises(ValueError):
                guest.unpack(archive, output)
            (output / 'link').symlink_to(root)
            archive.write_bytes(archive_member('link/outside', b'escape'))
            with self.assertRaises(ValueError):
                guest.unpack(archive, output)
            self.assertFalse((root / 'outside').exists())

    def test_rejects_wrong_architecture_and_dynamic_elf(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'game'
            header = bytearray(52)
            header[:6] = b'\x7fELF\x01\x01'
            struct.pack_into('<H', header, 18, 3)
            struct.pack_into('<I', header, 28, 52)
            struct.pack_into('<HH', header, 42, 32, 1)
            path.write_bytes(header + bytes(32))
            guest.elf_i386_static(path)
            path.write_bytes(header + struct.pack('<I', 3) + bytes(28))
            with self.assertRaises(ValueError):
                guest.elf_i386_static(path)
            header[4] = 2
            path.write_bytes(header + bytes(32))
            with self.assertRaises(ValueError):
                guest.elf_i386_static(path)

    def test_image_capacity_grows_with_content(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            before, _ = guest.image_dimensions(root, 1)
            with (root / 'large').open('wb') as stream:
                stream.truncate(128 * 1024 * 1024)
            after, _ = guest.image_dimensions(root, 1)
            self.assertGreater(after - before, 256 * 1024 * 1024)
            self.assertEqual(after % guest.BLOCK, 0)

    def test_guest_math_defines_every_wrapped_function(self):
        shim = (Path(guest.__file__).parent / 'guest/math/src/lib.rs').read_text()
        for game, functions in guest.GUEST_MATH.items():
            self.assertIn(game, guest.GAMES)
            for function in functions:
                self.assertRegex(shim, rf'\b__wrap_{function}\b', f'{game} wraps {function}')

    def test_game_wrapper_keeps_launcher_alive_through_input_handoff(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            root, sources = base / 'root', base / 'sources'
            (root / 'etc').mkdir(parents=True)
            for name in ('passwd', 'group'):
                (root / 'etc' / name).write_text('')
            for name in guest.GAMES:
                game = sources / name
                (game / 'src').mkdir(parents=True)
                (game / 'src/main.rs').write_text('fn main() {}')
            workspace = sources / 'presidential'
            (workspace / 'crates/terminal-ui/src').mkdir(parents=True)
            (workspace / 'crates/terminal-ui/src/main.rs').write_text('fn main() {}')
            (workspace / 'crates/terminal-ui/target').mkdir()
            (workspace / 'crates/terminal-ui/target/artifact').touch()
            (workspace / 'data').mkdir()
            (workspace / 'data/baseline.toml').write_text('seed = 7')
            (sources / 'silkie-loophole/NOTICE').write_text('Asset credits')
            header = bytearray(52)
            header[:6] = b'\x7fELF\x01\x01'
            struct.pack_into('<H', header, 18, 3)
            struct.pack_into('<I', header, 28, 52)
            struct.pack_into('<HH', header, 42, 32, 0)
            binary = base / 'static-i386'
            binary.write_bytes(header)
            sprites = base / 'sprites'
            (sprites / '24').mkdir(parents=True)
            (sprites / '24/atlas.png').write_bytes(b'prepared sprite art')
            guest.populate(root, dict.fromkeys(guest.GAMES, binary), sources, binary,
                           {'silkie-sprint': sprites})
            self.assertEqual((root / 'usr/share/silkie/sprites/silkie-sprint/24/atlas.png').read_bytes(),
                             b'prepared sprite art')
            wrapper = (root / 'usr/local/bin/silkie-sprint').read_text()
            self.assertEqual(wrapper, '#!/bin/sh\nexec /usr/local/libexec/silkie-console-session /usr/local/libexec/silkie-sprint --no-tempfile "$@"\n')
            presidential = (root / 'usr/local/bin/presidential').read_text()
            self.assertEqual(presidential, '#!/bin/sh\nexec /usr/local/libexec/silkie-console-session /usr/local/libexec/presidential "$@"\n')
            self.assertIn('silkie-loophole --no-tempfile "$@"', (root / 'usr/local/bin/silkie-loophole').read_text())
            self.assertIn('ground-stop --no-tempfile "$@"', (root / 'usr/local/bin/ground-stop').read_text())
            bundled = root / 'usr/share/silkie/sources'
            self.assertTrue((bundled / 'presidential/crates/terminal-ui/src/main.rs').is_file())
            self.assertFalse((bundled / 'presidential/crates/terminal-ui/target').exists())
            self.assertEqual((bundled / 'presidential/data/baseline.toml').read_text(), 'seed = 7')
            self.assertEqual((bundled / 'silkie-loophole/NOTICE').read_text(), 'Asset credits')
            self.assertEqual((root / 'usr/local/libexec/silkie-console-session').read_bytes(), binary.read_bytes())
            self.assertTrue((root / 'usr/local/libexec/silkie-console-session').stat().st_mode & 0o111)
            self.assertTrue((root / 'usr/share/silkie/sources/console-session/src/main.rs').is_file())


if __name__ == '__main__':
    unittest.main()
