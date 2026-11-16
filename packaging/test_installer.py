"""Installer guard and Homebrew dispatch checks; APT is tested on real hosts."""
import hashlib
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

INSTALLER = Path(__file__).resolve().parents[1] / 'docs/silkie/install.sh'


class InstallerTests(unittest.TestCase):
    def run_installer(self, *, tapped=True, installed=False, font_installed=False, font_failure=False, license_failure=False, arch='arm64', version='26.5'):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = {
                'uname': 'if [ "$1" = -s ]; then echo Darwin; else echo "$TEST_ARCH"; fi',
                'sw_vers': 'echo "$TEST_VERSION"',
                'brew': '''printf '%s\n' "$*" >>"$TEST_LOG"
case "$1" in
tap) if [ "$TEST_TAPPED" = yes ]; then echo goblinreactor/tap; fi ;;
list)
    if [ "$2" = --cask ] && [ "$3" = --versions ]; then
        echo 'font-iosevka 34.9.1'
    elif [ "$3" = font-iosevka ]; then
        [ "$TEST_FONT_INSTALLED" = yes ]
    else
        [ "$TEST_INSTALLED" = yes ]
    fi ;;
install|upgrade)
    if [ "$3" = font-iosevka ] && [ "$TEST_FONT_FAILURE" = yes ]; then exit 1; fi ;;

esac
''',
                'curl': '''printf '%s\n' "$*" >>"$TEST_LOG"
[ "$TEST_LICENSE_FAILURE" != yes ] || exit 22
printf 'Copyright Renzhi Li. SIL Open Font License 1.1.\n' >"$4"
''',
            }
            for name, body in scripts.items():
                script = root / name
                script.write_text('#!/bin/bash\n' + body + '\n')
                script.chmod(0o755)
            env = dict(os.environ, PATH=f'{root}:/usr/bin:/bin', TEST_ARCH=arch,
                       TEST_VERSION=version, TEST_TAPPED='yes' if tapped else 'no',
                       TEST_INSTALLED='yes' if installed else 'no',
                       TEST_FONT_INSTALLED='yes' if font_installed else 'no',
                       TEST_FONT_FAILURE='yes' if font_failure else 'no',
                       TEST_LICENSE_FAILURE='yes' if license_failure else 'no', TEST_LOG=str(root / 'log'))
            # Redirect the real font destination into the fixture; do not change HOME.
            script = root / 'install.sh'
            script.write_text(INSTALLER.read_text().replace('$HOME/Library/Fonts', str(root / 'fonts')))
            result = subprocess.run(['/bin/bash', str(script)], env=env, stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, start_new_session=True)
            log = (root / 'log').read_text() if (root / 'log').exists() else ''
            if result.returncode == 0:
                self.assertIn('SIL Open Font License', (root / 'fonts/Iosevka-LICENSE.md').read_text())
            return result, log

    def test_existing_source_installs_without_prompt(self):
        result, log = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('install --cask goblinreactor/tap/silkie', log)
        self.assertNotIn('Continue?', result.stderr)
        self.assertIn('install --cask font-iosevka', log)
        self.assertIn('/v34.9.1/LICENSE.md', log)

    def test_existing_installation_upgrades(self):
        result, log = self.run_installer(installed=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('upgrade --cask goblinreactor/tap/silkie', log)

    def test_font_upgrade_is_independent_of_silkie_installation(self):
        result, log = self.run_installer(font_installed=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('install --cask goblinreactor/tap/silkie', log)
        self.assertIn('upgrade --cask font-iosevka', log)
        result, log = self.run_installer(installed=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('install --cask font-iosevka', log)

    def test_font_or_license_failure_does_not_report_success(self):
        for kwargs in ({'font_failure': True}, {'license_failure': True}):
            with self.subTest(kwargs=kwargs):
                result, log = self.run_installer(**kwargs)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('Silkie and Iosevka installed', result.stdout)

    def test_new_source_requires_terminal_approval(self):
        result, log = self.run_installer(tapped=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires approval', result.stderr)
        self.assertNotIn('update', log)
        self.assertNotIn('install', log)

    def test_unsupported_macs_do_not_run_homebrew(self):
        for args in ({'arch': 'x86_64'}, {'version': '25.0'}):
            with self.subTest(args=args):
                result, log = self.run_installer(**args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(log, '')


class LinuxFontTests(unittest.TestCase):
    def test_upstream_fonts_license_checksum_and_repeated_install(self):
        for corrupt in (False, True):
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive = root / 'upstream.zip'
                with zipfile.ZipFile(archive, 'w') as z:
                    for name in ('Iosevka-Regular.ttf', 'Iosevka-Extended.ttf', 'Iosevka-ExtendedItalic.ttf'):
                        z.writestr('ttf/' + name, b'unmodified upstream font fixture ' + name.encode())
                digest = hashlib.sha256(archive.read_bytes()).hexdigest()
                tools = root / 'bin'
                tools.mkdir()
                scripts = {
                    'id': 'echo 501',
                    'curl': 'case "$2" in */LICENSE.md) printf "Copyright Renzhi Li. SIL OFL 1.1.\\n" >"$4" ;; *) cp "$TEST_ARCHIVE" "$4" ;; esac',
                    'fc-cache': 'printf "%s\\n" "$*" >>"$TEST_LOG"',
                }
                for name, body in scripts.items():
                    path = tools / name
                    path.write_text('#!/bin/bash\n' + body + '\n')
                    path.chmod(0o755)
                # Only the release digest is replaced, so checksum verification
                # still runs against the bytes downloaded by the curl fixture.
                script = INSTALLER.read_text().removesuffix('main "$@"\n')
                if not corrupt:
                    script = script.replace('7a77a3dfa0e2d713a8920a30624863e23df2867a1f55a5e3d6d23dad07c14c65', digest)
                destination = root / 'data with spaces'
                env = dict(os.environ, PATH=f'{tools}:/usr/bin:/bin:/sbin',
                           TEST_ARCHIVE=str(archive), TEST_LOG=str(root / 'cache-log'), XDG_DATA_HOME=str(destination))
                script += '\ninstall_iosevka_linux "$1"\n'
                for run in range(2 if not corrupt else 1):
                    stage = root / f'stage-{run}'
                    stage.mkdir()
                    result = subprocess.run(['/bin/bash', '-c', script, 'installer-test', str(stage)],
                                            env=env, capture_output=True, text=True)
                    fonts = destination / 'fonts/iosevka'
                    if corrupt:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertFalse(fonts.exists())
                        self.assertFalse((root / 'cache-log').exists())
                    else:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(len(list(fonts.glob('*.ttf'))), 3)
                        self.assertIn('SIL OFL', (fonts / 'Iosevka-LICENSE.md').read_text())
                        with zipfile.ZipFile(archive) as z:
                            for name in z.namelist():
                                self.assertEqual((fonts / Path(name).name).read_bytes(), z.read(name))
                        self.assertIn(str(fonts), (root / 'cache-log').read_text())


if __name__ == '__main__':
    unittest.main()
