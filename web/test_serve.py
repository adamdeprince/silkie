"""Check the actual HTTP contract needed by CheerpX's image loader."""

import concurrent.futures
import functools
import http.client
import http.server
from pathlib import Path
import tempfile
import threading
import unittest

from serve import DemoHandler, UnsatisfiableRange, byte_range


class RangeParsingTest(unittest.TestCase):
    def test_offsets(self):
        self.assertEqual(byte_range('bytes=0-0', 10), (0, 0))
        self.assertEqual(byte_range('bytes=5-', 10), (5, 9))
        self.assertEqual(byte_range('bytes=-3', 10), (7, 9))
        self.assertEqual(byte_range('bytes=5-999', 10), (5, 9))
        self.assertEqual(byte_range('bytes=-999', 10), (0, 9))
        self.assertEqual(byte_range('bytes=00005-00009', 10), (5, 9))

    def test_decimal_offsets_have_no_digit_limit(self):
        large = '9' * 10000
        self.assertEqual(byte_range('bytes=-' + large, 10), (0, 9))
        self.assertEqual(byte_range('bytes=0-' + large, 10), (0, 9))
        self.assertEqual(byte_range('bytes=' + '0' * 10000 + '5-', 10), (5, 9))
        with self.assertRaises(UnsatisfiableRange):
            byte_range('bytes=' + large + '-', 10)

    def test_unsatisfiable(self):
        for header, size in [('bytes=10-', 10), ('bytes=-0', 10), ('bytes=8-2', 10), ('bytes=0-', 0)]:
            with self.subTest(header=header, size=size), self.assertRaises(UnsatisfiableRange):
                byte_range(header, size)

    def test_unsupported_ranges_are_ignored(self):
        for header in ('bytes=0-1,4-5', 'things=0-1', 'bytes=-', 'bytes=x-y', 'bytes=0-1junk'):
            with self.subTest(header=header):
                self.assertIsNone(byte_range(header, 10))


class QuietHandler(DemoHandler):
    def log_message(self, *args):
        pass


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.payload = bytes(range(256)) * 4097 + b'tail'
        (cls.root / 'guest.ext2').write_bytes(cls.payload)
        (cls.root / 'empty.ext2').write_bytes(b'')
        (cls.root / 'terminal.wasm').write_bytes(b'\0asm')
        (cls.root / 'terminal.mjs').write_text('export const ready = true;')
        (cls.root / 'index.html').write_text('<title>Silkie</title>')
        (cls.root / 'sub').mkdir()
        (cls.root / 'sub/index.html').write_text('<title>Subdirectory</title>')
        handler = functools.partial(QuietHandler, directory=str(cls.root))
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def request(self, path='/guest.ext2', method='GET', headers=None):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=10)
        try:
            connection.request(method, path, headers=headers or {})
            response = connection.getresponse()
            result = response.status, dict(response.getheaders()), response.read()
            return result
        finally:
            connection.close()

    def assert_isolated(self, headers):
        self.assertEqual(headers['Cross-Origin-Opener-Policy'], 'same-origin')
        self.assertEqual(headers['Cross-Origin-Embedder-Policy'], 'require-corp')
        self.assertEqual(headers['Cross-Origin-Resource-Policy'], 'same-origin')

    def test_full_image_and_headers(self):
        status, headers, body = self.request()
        self.assertEqual(status, 200)
        self.assertEqual(body, self.payload)
        self.assertEqual(int(headers['Content-Length']), len(self.payload))
        self.assertEqual(headers['Accept-Ranges'], 'bytes')
        self.assertIn('Last-Modified', headers)
        self.assert_isolated(headers)

    def test_range_variants(self):
        cases = [('bytes=0-0', 0, 0), ('bytes=4096-9000', 4096, 9000),
                 ('bytes=8192-', 8192, len(self.payload) - 1),
                 ('bytes=-17', len(self.payload) - 17, len(self.payload) - 1),
                 ('bytes=1-999999999', 1, len(self.payload) - 1)]
        for value, first, last in cases:
            with self.subTest(value=value):
                status, headers, body = self.request(headers={'Range': value})
                self.assertEqual(status, 206)
                self.assertEqual(body, self.payload[first:last + 1])
                self.assertEqual(int(headers['Content-Length']), last - first + 1)
                self.assertEqual(headers['Content-Range'], f'bytes {first}-{last}/{len(self.payload)}')
                self.assert_isolated(headers)

    def test_head_ignores_range_as_required_by_http(self):
        status, headers, body = self.request(method='HEAD', headers={'Range': 'bytes=4-8'})
        self.assertEqual(status, 200)
        self.assertEqual(body, b'')
        self.assertEqual(int(headers['Content-Length']), len(self.payload))
        self.assertNotIn('Content-Range', headers)
        self.assert_isolated(headers)

    def test_unsatisfiable_ranges(self):
        for path, value, size in [('/guest.ext2', f'bytes={len(self.payload)}-', len(self.payload)),
                                  ('/empty.ext2', 'bytes=0-0', 0)]:
            status, headers, body = self.request(path, headers={'Range': value})
            self.assertEqual(status, 416)
            self.assertEqual(headers['Content-Range'], f'bytes */{size}')
            self.assertEqual(body, b'')
            self.assert_isolated(headers)

    def test_multiple_ranges_fall_back_to_full_response(self):
        status, headers, body = self.request(headers={'Range': 'bytes=0-3,8-11'})
        self.assertEqual(status, 200)
        self.assertEqual(body, self.payload)
        self.assertNotIn('Content-Range', headers)

    def test_if_range_date_and_entity_tag(self):
        _, headers, _ = self.request(method='HEAD')
        status, _, body = self.request(headers={'Range': 'bytes=0-3', 'If-Range': headers['Last-Modified']})
        self.assertEqual((status, body), (206, self.payload[:4]))
        for value in ('Thu, 01 Jan 1970 00:00:00 GMT', '"unknown-tag"'):
            status, _, body = self.request(headers={'Range': 'bytes=0-3', 'If-Range': value})
            self.assertEqual((status, body), (200, self.payload))

    def test_conditional_get(self):
        _, headers, _ = self.request(method='HEAD')
        status, headers, body = self.request(headers={'If-Modified-Since': headers['Last-Modified']})
        self.assertEqual((status, body), (304, b''))
        self.assert_isolated(headers)

    def test_mime_types(self):
        for path, expected in [('/terminal.wasm', 'application/wasm'),
                               ('/terminal.mjs', 'application/javascript')]:
            status, headers, _ = self.request(path)
            self.assertEqual(status, 200)
            self.assertEqual(headers['Content-Type'], expected)

    def test_directory_redirect_and_index(self):
        status, headers, body = self.request('/sub?demo=1')
        self.assertEqual(status, 301)
        self.assertEqual(headers['Location'], '/sub/?demo=1')
        self.assertEqual(body, b'')
        self.assert_isolated(headers)
        status, _, body = self.request('/sub/')
        self.assertEqual((status, body), (200, b'<title>Subdirectory</title>'))

    def test_symlinks_cannot_escape_root(self):
        with tempfile.TemporaryDirectory() as outside:
            secret = Path(outside) / 'private.txt'
            secret.write_text('not website content')
            link = self.root / 'outside.txt'
            link.symlink_to(secret)
            try:
                status, headers, body = self.request('/outside.txt')
                self.assertEqual(status, 403)
                self.assertNotIn(b'not website content', body)
                self.assert_isolated(headers)
            finally:
                link.unlink()

    def test_repeated_concurrent_large_ranges(self):
        def read_range(index):
            first = index * 17
            status, _, body = self.request(headers={'Range': f'bytes={first}-'})
            self.assertEqual(status, 206)
            self.assertEqual(body, self.payload[first:])
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(read_range, range(24)))


if __name__ == '__main__':
    unittest.main()
