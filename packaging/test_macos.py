"""Validate executable identities when staging old and new macOS bundles."""
import importlib.util
import plistlib
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('macos_packaging', Path(__file__).with_name('macos.py'))
macos = importlib.util.module_from_spec(spec)
spec.loader.exec_module(macos)


class MacOSBundleTests(unittest.TestCase):
    def test_executable_migration(self):
        for old_name in ('kitty', 'silkie'):
            with self.subTest(old_name=old_name), tempfile.TemporaryDirectory() as temporary:
                app = Path(temporary) / 'silkie.app'
                bundles = ((app, old_name, 'silkie'),
                           (app / 'Contents/kitty-quick-access.app', old_name + '-quick-access', 'silkie-quick-access'))
                for bundle, previous, target in bundles:
                    bin_dir = bundle / 'Contents/MacOS'
                    bin_dir.mkdir(parents=True)
                    (bin_dir / previous).write_bytes(b'executable')
                    (bin_dir / previous).chmod(0o755)
                    if previous != target:
                        (bin_dir / target).symlink_to(previous)
                    plist = bundle / 'Contents/Info.plist'
                    plist.write_bytes(plistlib.dumps({'CFBundleExecutable': previous}))
                macos.brand_executables(app)
                macos.brand_executables(app)
                for bundle, previous, target in bundles:
                    bin_dir = bundle / 'Contents/MacOS'
                    info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
                    self.assertEqual(info['CFBundleExecutable'], target)
                    self.assertFalse((bin_dir / target).is_symlink())
                    self.assertEqual((bin_dir / target).read_bytes(), b'executable')
                    self.assertEqual((bin_dir / previous).resolve(), (bin_dir / target).resolve())
                    self.assertEqual((bin_dir / target).stat().st_mode & 0o777, 0o755)

    def test_existing_compatibility_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / 'silkie.app'
            bin_dir = app / 'Contents/MacOS'
            bin_dir.mkdir(parents=True)
            (bin_dir / 'silkie').write_bytes(b'executable')
            (bin_dir / 'kitty').symlink_to('silkie')
            plist = app / 'Contents/Info.plist'
            plist.write_bytes(plistlib.dumps({'CFBundleExecutable': 'kitty'}))
            macos.brand_executables(app)
            self.assertEqual(plistlib.loads(plist.read_bytes())['CFBundleExecutable'], 'silkie')
            self.assertEqual((bin_dir / 'kitty').read_bytes(), b'executable')


if __name__ == '__main__':
    unittest.main()
