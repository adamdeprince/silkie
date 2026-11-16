#!/usr/bin/env python3
"""Serve the local website with cross-origin isolation and disk byte ranges.

CheerpX needs SharedArrayBuffer, enabled by COOP/COEP, and fetches local guest
images using HTTP Range requests. This is a development server, not a relay.
"""

import argparse
import datetime
import email.utils
import functools
import http.server
import io
import os
from pathlib import Path
import re
import urllib.parse


class UnsatisfiableRange(ValueError):
    pass


def _decimal_up_to(value: str, maximum: int) -> int:
    """Compare decimal offsets without imposing a limit on their digit count."""
    value = value.lstrip('0') or '0'
    bound = str(maximum)
    if len(value) > len(bound) or (len(value) == len(bound) and value > bound):
        return maximum + 1
    return int(value)


def byte_range(header: str, size: int) -> tuple[int, int] | None:
    """Return inclusive offsets, ignore unsupported syntax, or reject no overlap.

    Multiple ranges are deliberately ignored: sending the complete representation
    is an allowed response and avoids constructing multipart disk responses.
    """
    match = re.fullmatch(r'bytes=([0-9]*)-([0-9]*)', header.strip(), re.IGNORECASE)
    if match is None or not any(match.groups()):
        return None
    first, last = match.groups()
    if size == 0:
        raise UnsatisfiableRange
    if not first:
        suffix = _decimal_up_to(last, size)
        if suffix == 0:
            raise UnsatisfiableRange
        return max(0, size - suffix), size - 1
    start = _decimal_up_to(first, size)
    end = _decimal_up_to(last, size) if last else size - 1
    if start >= size or end < start:
        raise UnsatisfiableRange
    return start, min(end, size - 1)


def _date_timestamp(value: str) -> float | None:
    try:
        date = email.utils.parsedate_to_datetime(value)
        if date.tzinfo is None:
            date = date.replace(tzinfo=datetime.timezone.utc)
        return date.timestamp()
    except (TypeError, IndexError, OverflowError, ValueError):
        return None


class DemoHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.mjs': 'application/javascript',
        '.ext2': 'application/octet-stream',
    }

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def _within_root(self, path: str) -> bool:
        try:
            return Path(path).resolve().is_relative_to(Path(self.directory).resolve())
        except (OSError, RuntimeError):
            return False

    def send_head(self):
        self._range_size = None
        path = self.translate_path(self.path)
        if not self._within_root(path):
            self.send_error(403, 'Path is outside the website directory')
            return None
        if os.path.isdir(path):
            parts = urllib.parse.urlsplit(self.path)
            if not parts.path.endswith(('/', '%2f', '%2F')):
                destination = urllib.parse.urlunsplit(parts._replace(path=parts.path + '/'))
                self.send_response(301)
                self.send_header('Location', destination)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return None
            for index in ('index.html', 'index.htm'):
                candidate = os.path.join(path, index)
                if os.path.isfile(candidate):
                    path = candidate
                    break
            else:
                return self.list_directory(path)
        if not self._within_root(path):
            self.send_error(403, 'Path is outside the website directory')
            return None
        if path.endswith('/'):
            self.send_error(404, 'File not found')
            return None
        try:
            source = open(path, 'rb')
        except OSError:
            self.send_error(404, 'File not found')
            return None
        try:
            stat = os.fstat(source.fileno())
            modified = int(stat.st_mtime)
            since = self.headers.get('If-Modified-Since')
            if since and 'If-None-Match' not in self.headers:
                timestamp = _date_timestamp(since)
                if timestamp is not None and modified <= timestamp:
                    self.send_response(304)
                    self.send_header('Last-Modified', self.date_time_string(stat.st_mtime))
                    self.end_headers()
                    source.close()
                    return None
            offsets = None
            # RFC 9110 defines Range only for GET. HEAD describes a full GET.
            range_header = self.headers.get('Range') if self.command == 'GET' else None
            if_range = self.headers.get('If-Range')
            if range_header and if_range:
                timestamp = _date_timestamp(if_range)
                # This server does not issue entity tags; an ETag cannot match.
                if timestamp is None or modified > timestamp:
                    range_header = None
            if range_header:
                try:
                    offsets = byte_range(range_header, stat.st_size)
                except UnsatisfiableRange:
                    self.send_response(416)
                    self.send_header('Content-Range', f'bytes */{stat.st_size}')
                    self.send_header('Content-Length', '0')
                    self.send_header('Accept-Ranges', 'bytes')
                    self.end_headers()
                    source.close()
                    return None
            self.send_response(206 if offsets else 200)
            self.send_header('Content-Type', self.guess_type(path))
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Last-Modified', self.date_time_string(stat.st_mtime))
            if offsets:
                start, end = offsets
                self._range_size = end - start + 1
                source.seek(start)
                self.send_header('Content-Range', f'bytes {start}-{end}/{stat.st_size}')
                self.send_header('Content-Length', str(self._range_size))
            else:
                self.send_header('Content-Length', str(stat.st_size))
            self.end_headers()
            return source
        except BaseException:
            source.close()
            raise

    def copyfile(self, source, outputfile):
        if self._range_size is None:
            return super().copyfile(source, outputfile)
        remaining = self._range_size
        # A streaming I/O chunk, not a bound on the image or requested range.
        while remaining:
            chunk = source.read(min(io.DEFAULT_BUFFER_SIZE, remaining))
            if not chunk:
                raise OSError('File changed while streaming its byte range')
            outputfile.write(chunk)
            remaining -= len(chunk)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='127.0.0.1', help='listen address (default: loopback)')
    parser.add_argument('--port', type=int, default=8000, help='TCP port (default: 8000)')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1] / 'html', help='website directory')
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f'website directory does not exist: {root}')
    handler = functools.partial(DemoHandler, directory=str(root))
    with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
        host, port = server.server_address[:2]
        print(f'Serving {root} at http://{host}:{port}/ (cross-origin isolation enabled)', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
