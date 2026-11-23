"""Local deployment ordering and immutable guest-image publication checks."""
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('publish_r2', Path(__file__).with_name('publish_r2.py'))
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.site = self.root / 'html'
        self.demo = self.site / '_static/demo'
        self.demo.mkdir(parents=True)
        self.image = b'Silkie guest bytes\x00\xff' * 300
        self.digest = hashlib.sha256(self.image).hexdigest()
        (self.demo / 'guest.ext2').write_bytes(self.image)
        self.manifest = {'image': 'guest.ext2', 'sha256': self.digest, 'bytes': len(self.image),
                         'entry': '/usr/local/bin/silkie-demo-shell'}
        self.write_manifest()
        (self.site / 'index.html').write_text('<html>demo</html>')
        (self.site / 'reference.html').write_text('<html>reference</html>')
        (self.site / 'install.sh').write_text('#!/bin/sh\necho installer\n')
        (self.demo / 'app.mjs').write_text('export const demo = true;\n')
        (self.site / '.buildinfo').write_text('local only')

    def write_manifest(self):
        (self.demo / 'guest.json').write_text(json.dumps(self.manifest, indent=4) + '\n')

    def invoke(self, extra=(), *, run=None, credentials=None):
        args = ['publish_r2.py', '--site', str(self.site), *map(str, extra)]
        credentials = credentials or (lambda *_: ('https://r2.example.invalid', {'TEST_ENV': 'safe'}))
        with patch.object(sys, 'argv', args), patch.object(publisher, 'credentials', side_effect=credentials) as auth, \
                patch.object(publisher.subprocess, 'run', side_effect=run) as aws, redirect_stdout(io.StringIO()):
            publisher.main()
        return auth, aws

    def test_staging_rewrites_only_temporary_manifest_and_retains_local_build(self):
        original = (self.demo / 'guest.json').read_bytes()
        with publisher.staged_site(self.site) as (staging, relative):
            expected = f'guest-{self.digest}.ext2'
            self.assertEqual(relative.as_posix(), '_static/demo/' + expected)
            staged_manifest = json.loads((staging / publisher.GUEST_MANIFEST).read_text())
            self.assertEqual(staged_manifest, dict(self.manifest, image=expected))
            self.assertEqual((staging / relative).read_bytes(), self.image)
            self.assertFalse((staging / '_static/demo/guest.ext2').exists())
            self.assertEqual((self.demo / 'guest.json').read_bytes(), original)
            self.assertEqual((self.demo / 'guest.ext2').read_bytes(), self.image)
        self.assertFalse(staging.exists())
        self.assertEqual((self.demo / 'guest.json').read_bytes(), original)

    def test_content_named_guest_is_staged_unchanged(self):
        name = f'guest-{self.digest}.ext2'
        (self.demo / 'guest.ext2').rename(self.demo / name)
        self.manifest['image'] = name
        self.write_manifest()
        with publisher.staged_site(self.site) as (staging, relative):
            self.assertEqual(relative.as_posix(), '_static/demo/' + name)
            self.assertEqual((staging / relative).read_bytes(), self.image)
            self.assertEqual(json.loads((staging / publisher.GUEST_MANIFEST).read_text()), self.manifest)
        self.assertEqual((self.demo / name).read_bytes(), self.image)

    def test_invalid_guest_fails_before_credentials_or_network(self):
        cases = [dict(self.manifest, sha256='invalid'), dict(self.manifest, sha256='0' * 64),
                 dict(self.manifest, bytes=1), dict(self.manifest, image='../private.ext2')]
        for invalid in cases:
            with self.subTest(invalid=invalid):
                (self.demo / 'guest.json').write_text(json.dumps(invalid))
                with patch.object(sys, 'argv', ['publish_r2.py', '--site', str(self.site)]), \
                        patch.object(publisher, 'credentials') as auth, patch.object(publisher.subprocess, 'run') as aws:
                    with self.assertRaises(SystemExit):
                        publisher.main()
                    auth.assert_not_called()
                    aws.assert_not_called()

    def test_image_precedes_manifest_assets_and_html_and_installer_has_explicit_mime(self):
        observed = []
        local_manifest = (self.demo / 'guest.json').read_bytes()

        def run(command, **kwargs):
            self.assertEqual(kwargs, {'env': {'TEST_ENV': 'safe'}, 'check': True})
            self.assertEqual(command[:4], ['aws', '--endpoint-url', 'https://r2.example.invalid', 's3'])
            self.assertNotIn('--delete', command)
            observed.append(command)
            source = Path(command[5])
            if len(observed) == 1:
                image = f'guest-{self.digest}.ext2'
                self.assertEqual((source / image).read_bytes(), self.image)
                self.assertEqual(command[6], publisher.SITE_BUCKET + '_static/demo/')
                self.assertEqual(command[7:12], ['--exclude', '*', '--include', image, '--size-only'])
                self.assertIn('public, max-age=31536000, immutable', command)
                self.assertEqual(json.loads((source / 'guest.json').read_text())['image'], image)
            self.assertEqual((self.demo / 'guest.json').read_bytes(), local_manifest)

        auth, _ = self.invoke(run=run)
        auth.assert_called_once()
        self.assertEqual([command[4] for command in observed], ['sync', 'sync', 'cp', 'sync'])
        assets, installer, html = observed[1:]
        self.assertIn('*.html', assets)
        self.assertIn('_static/demo/guest*.ext2', assets)
        self.assertIn('install.sh', assets)
        self.assertEqual(installer[6], publisher.SITE_BUCKET + 'install.sh')
        self.assertEqual(installer[installer.index('--content-type') + 1], 'text/plain; charset=utf-8')
        self.assertEqual(installer[installer.index('--cache-control') + 1], 'no-store')
        self.assertEqual(html[html.index('--exclude'):html.index('--content-type')], ['--exclude', '*', '--include', '*.html'])
        self.assertFalse(Path(html[5]).exists(), 'temporary deployment must be removed')

    def test_dry_run_and_optional_cask_tap_order_are_preserved(self):
        cask, tap = self.root / 'Silkie.zip', self.root / 'tap.git'
        cask.write_bytes(b'archive')
        (tap / 'objects').mkdir(parents=True)
        _, aws = self.invoke(['--dry-run', '--cask', cask, '--tap', tap])
        commands = [call.args[0] for call in aws.call_args_list]
        self.assertEqual(len(commands), 7)
        for command in commands:
            self.assertIn('--dryrun', command)
            self.assertNotIn('--delete', command)
        self.assertIn('guest-' + self.digest + '.ext2', commands[0])
        self.assertEqual(commands[1][6], 's3://site-brew-goblinreactor-com/casks/Silkie.zip')
        self.assertEqual(commands[2][6], 's3://site-brew-goblinreactor-com/homebrew-tap.git/objects/')
        self.assertEqual(commands[3][6], 's3://site-brew-goblinreactor-com/homebrew-tap.git/')
        self.assertIn('--include', commands[-1])

    def test_failed_image_upload_never_publishes_manifest_or_html(self):
        commands = []

        def fail(command, **_):
            commands.append(command)
            raise subprocess.CalledProcessError(1, command)

        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(run=fail)
        self.assertEqual(len(commands), 1)
        self.assertFalse(Path(commands[0][5]).exists())
        self.assertEqual(json.loads((self.demo / 'guest.json').read_text()), self.manifest)

    def test_guest_hash_uses_streamed_reads_beyond_a_single_chunk(self):
        data = b'0123456789abcdef' * 140_000
        image = self.root / 'large.ext2'
        image.write_bytes(data)
        sizes = []

        class Reader:
            def __enter__(reader):
                return reader

            def __exit__(reader, *_):
                reader.stream.close()

            def read(reader, size=-1):
                self.assertGreater(size, 0, 'guest hashing must not read the full image at once')
                sizes.append(size)
                return reader.stream.read(size)

        reader = Reader()
        reader.stream = image.open('rb')
        with patch.object(Path, 'open', return_value=reader):
            self.assertEqual(publisher.sha256_file(image), hashlib.sha256(data).hexdigest())
        self.assertGreater(len(sizes), 2)


if __name__ == '__main__':
    unittest.main()
