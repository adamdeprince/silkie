#!/usr/bin/env python3
"""Publish Silkie's website and optional macOS release to Cloudflare R2."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE_BUCKET = 's3://site-silkie-goblinreactor-com/'
GUEST_MANIFEST = Path('_static/demo/guest.json')


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        # A streaming read chunk, never a limit on the guest image size.
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def link_or_copy(source: str, destination: str) -> None:
    if not Path(source).is_symlink():
        try:
            os.link(source, destination)
            return
        except OSError:
            pass  # Temporary storage can be on another filesystem.
    shutil.copy2(source, destination)


@contextmanager
def staged_site(site: Path):
    """Validate locally, then rewrite only a temporary deployment manifest."""
    manifest_path = site / GUEST_MANIFEST
    manifest = json.loads(manifest_path.read_text())
    digest = manifest.get('sha256', '')
    if not isinstance(digest, str) or not re.fullmatch(r'[a-f0-9]{64}', digest):
        raise SystemExit('Guest manifest has no valid SHA-256 identity; rebuild the site')
    image_name = manifest.get('image', '')
    if (not isinstance(image_name, str) or not image_name or
            Path(image_name).name != image_name or image_name in ('.', '..') or
            '/' in image_name or '\\' in image_name):
        raise SystemExit('Guest manifest image must be a local filename')
    image = manifest_path.parent / image_name
    if sha256_file(image) != digest:
        raise SystemExit('Guest image SHA-256 does not match guest.json; refusing deployment')
    if 'bytes' in manifest and manifest['bytes'] != image.stat().st_size:
        raise SystemExit('Guest image size does not match guest.json; refusing deployment')
    immutable_name = f'guest-{digest}.ext2'
    relative_image = GUEST_MANIFEST.parent / immutable_name
    with tempfile.TemporaryDirectory(prefix='silkie-r2-') as temporary:
        staging = Path(temporary) / 'site'
        shutil.copytree(site, staging, copy_function=link_or_copy)
        staged_image = staging / GUEST_MANIFEST.parent / image_name
        if staged_image.name != immutable_name:
            staged_image.replace(staging / relative_image)
        manifest['image'] = immutable_name
        staged_manifest = staging / GUEST_MANIFEST
        # copytree may have hardlinked this file to the original HTML tree.
        # Unlink before writing, keeping the local build byte-for-byte intact.
        staged_manifest.unlink()
        staged_manifest.write_text(json.dumps(manifest, indent=2) + '\n')
        yield staging, relative_image


def credentials(path: Path, account: str | None) -> tuple[str, dict[str, str]]:
    token = path.expanduser().read_text().strip()

    def api(route: str):
        request = urllib.request.Request('https://api.cloudflare.com/client/v4' + route,
                                         headers={'Authorization': 'Bearer ' + token})
        with urllib.request.urlopen(request) as response:
            result = json.load(response)
        if not result['success']:
            raise SystemExit('Cloudflare credential verification failed')
        return result['result']

    if account is None:
        accounts = api('/accounts')
        if len(accounts) != 1:
            raise SystemExit('Specify --account when the token can access multiple accounts')
        account = accounts[0]['id']
    verified = api(f'/accounts/{account}/tokens/verify')
    if verified['status'] != 'active':
        raise SystemExit('Cloudflare token is not active')
    # R2 derives S3 credentials from the token; keep them in process environment,
    # never command arguments, generated files, or deployment logs.
    env = dict(os.environ, AWS_ACCESS_KEY_ID=verified['id'],
               AWS_SECRET_ACCESS_KEY=hashlib.sha256(token.encode()).hexdigest(),
               AWS_DEFAULT_REGION='auto', AWS_EC2_METADATA_DISABLED='true',
               AWS_REQUEST_CHECKSUM_CALCULATION='when_required',
               AWS_RESPONSE_CHECKSUM_VALIDATION='when_required', AWS_PAGER='')
    env.pop('AWS_SESSION_TOKEN', None)
    return f'https://{account}.r2.cloudflarestorage.com', env


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--credentials', type=Path, default=Path('~/r2-god.txt'))
    parser.add_argument('--account')
    parser.add_argument('--site', type=Path, default=ROOT / 'html')
    parser.add_argument('--cask', type=Path, help='Validated macOS ZIP archive')
    parser.add_argument('--tap', type=Path, help='Bare Homebrew Git repository with server-info updated')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if bool(args.cask) != bool(args.tap):
        parser.error('--cask and --tap must be supplied together')
    if not (args.site / 'install.sh').is_file():
        parser.error('Build the site with make html first')
    # All guest validation and staging precedes credential reads and network.
    with staged_site(args.site) as (deployment, guest_image):
        endpoint, env = credentials(args.credentials, args.account)

        def aws(*command: str) -> None:
            flags = ['--dryrun'] if args.dry_run else []
            subprocess.run(['aws', '--endpoint-url', endpoint, 's3', *command, *flags,
                            '--only-show-errors'], env=env, check=True)

        # Install immutable bytes before publishing any manifest or page that
        # refers to them. Existing images remain available to running guests.
        aws('cp', str(deployment / guest_image), SITE_BUCKET + guest_image.as_posix(),
            '--content-type', 'application/octet-stream',
            '--cache-control', 'public, max-age=31536000, immutable')
        if args.cask:
            aws('cp', str(args.cask), f's3://site-brew-goblinreactor-com/casks/{args.cask.name}',
                '--content-type', 'application/zip', '--cache-control', 'public, max-age=31536000, immutable')
            # Upload Git objects before advancing the advertised branch references.
            aws('sync', str(args.tap / 'objects'), 's3://site-brew-goblinreactor-com/homebrew-tap.git/objects/',
                '--cache-control', 'no-cache')
            aws('sync', str(args.tap), 's3://site-brew-goblinreactor-com/homebrew-tap.git/',
                '--exclude', 'objects/*', '--exclude', 'config', '--exclude', 'hooks/*',
                '--exclude', 'FETCH_HEAD', '--cache-control', 'no-cache')
        aws('sync', str(deployment), SITE_BUCKET,
            '--exclude', '.buildinfo*', '--exclude', '*.html', '--exclude', 'install.sh',
            '--exclude', '_static/demo/guest*.ext2', '--cache-control', 'no-cache')
        aws('cp', str(deployment / 'install.sh'), SITE_BUCKET + 'install.sh',
            '--content-type', 'text/plain; charset=utf-8', '--cache-control', 'no-store')
        aws('sync', str(deployment), SITE_BUCKET,
            '--exclude', '*', '--include', '*.html', '--content-type', 'text/html; charset=utf-8',
            '--cache-control', 'no-cache')
    print('R2 deployment complete' if not args.dry_run else 'R2 deployment dry run complete')


if __name__ == '__main__':
    main()
