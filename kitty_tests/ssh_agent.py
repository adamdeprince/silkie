#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Adam DePrince

import base64
import socket
import socketserver
import struct
import tempfile
import threading
import time
from contextlib import contextmanager
from pathlib import Path

from kitty.ssh_agent import FAILURE, AgentProvider, AgentSource, frame, parse

from .base import BaseTest, parse_bytes


def packet(body):
    return struct.pack('!I', len(body)) + body


@contextmanager
def fake_agent(path):
    class Server(socketserver.ThreadingUnixStreamServer):
        daemon_threads = True
        requests = []
        opened = 0

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            self.server.opened += 1
            while header := self.rfile.read(4):
                if len(header) != 4:
                    break
                data = self.rfile.read(struct.unpack('!I', header)[0])
                self.server.requests.append(data)
                if data[0] == 11:
                    response = packet(b'\x0c\0\0\0\0')
                elif data[0] == 27:
                    response = packet(b'\x06')
                else:
                    response = packet(b'\x0e' + data[1:])
                self.wfile.write(response)
                self.wfile.flush()

    with Server(str(path), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server
        finally:
            server.shutdown()
            thread.join()


class TestSSHAgent(BaseTest):
    def test_ssh_agent_owned_lifecycle(self):
        import shutil

        if not shutil.which('ssh-agent'):
            self.skipTest('ssh-agent is unavailable')
        source = AgentSource()
        try:
            path = source.address({})
            process = source.process
            self.assertTrue(path)
            deadline = time.monotonic() + 5
            while not Path(path).exists():
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.001)
            self.ae(source.address({}), path)
            self.ae(source.address({'SSH_AUTH_SOCK': __file__}), path)
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(path)
                connection.sendall(packet(b'\x0b'))
                self.ae(connection.recv(9), packet(b'\x0c\0\0\0\0'))
        finally:
            source.close()
        self.assertFalse(Path(path).exists())
        self.assertIsNotNone(process.poll())

    def test_ssh_agent_parser_dispatch(self):
        screen = self.create_screen()
        received = []
        screen.callbacks.ssh_agent_protocol = lambda data: received.append(bytes(data))
        raw = frame({'v': '1', 'op': 'query', 'id': 'large', 'ignored': 'x' * (1024 * 1024)})
        parse_bytes(screen, raw)
        self.ae(received, [raw[6:-2]])  # callback gets ssh-agent;... after OSC 777;

    def test_ssh_agent_packets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'agent'
            with fake_agent(path) as server:
                received = []
                provider = AgentProvider(received.append, lambda: str(path))

                def request(body, ident, **overrides):
                    f = {'v': '1', 'op': 'request', 'id': ident, 'g': provider.generation, 'c': 'connection',
                         'expires': str(time.time_ns() // 1_000_000 + 120000),
                         'data': base64.b64encode(packet(body)).decode()}
                    f.update(overrides)
                    before = len(received)
                    provider.handle(frame(f))
                    deadline = time.monotonic() + 5
                    while len(received) == before:
                        provider.tick()
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.0001)
                    return parse(received[-1])

                try:
                    provider.handle(frame({'v': '1', 'op': 'query', 'id': 'probe'}), False)
                    self.ae(parse(received[-1])['caps'], '0')
                    self.ae(server.opened, 0)
                    self.ae(base64.b64decode(request(b'\x0b', 'list')['data']), packet(b'\x0c\0\0\0\0'))
                    binding = b'session-bind@openssh.com'
                    self.ae(base64.b64decode(request(b'\x1b' + struct.pack('!I', len(binding)) + binding, 'bind')['data']), packet(b'\x06'))
                    large = b'x' * (1024 * 1024)
                    self.ae(base64.b64decode(request(b'\x0d' + large, 'large')['data']), packet(b'\x0e' + large))
                    for i in range(1024):
                        self.ae(base64.b64decode(request(b'\x0b', f'list{i}')['data']), packet(b'\x0c\0\0\0\0'))
                    self.ae(server.opened, 1)  # binding and requests used the same agent connection
                    self.ae(len(server.requests), 1027)
                    self.ae(request(b'\x0b', 'list')['error'], 'duplicate')
                    self.ae(request(b'\x13', 'remove')['error'], 'denied')
                    self.ae(request(b'\x0b', 'old', g='old-generation')['error'], 'changed')
                    self.ae(request(b'\x0b', 'late', expires='1')['error'], 'expired')
                    self.ae(len(server.requests), 1027)
                    provider.handle(frame({'v': '1', 'op': 'close', 'id': 'close', 'c': 'connection', 'g': provider.generation}))
                    self.ae(provider.connections, {})
                    self.ae(request(b'\x0b', 'after-close')['error'], 'closed')
                finally:
                    provider.close()

    def test_ssh_agent_provider_change(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory) / 'a', Path(directory) / 'b'
            with fake_agent(a), fake_agent(b):
                active = [str(a)]
                received = []
                provider = AgentProvider(received.append, lambda: active[0])
                try:
                    provider.handle(frame({'v': '1', 'op': 'query', 'id': 'first'}))
                    first = parse(received[-1])['g']
                    active[0] = str(b)
                    provider.handle(frame({'v': '1', 'op': 'query', 'id': 'second'}))
                    self.assertNotEqual(first, parse(received[-1])['g'])
                    provider.handle(frame({'v': '1', 'op': 'request', 'id': 'stale', 'c': 'c', 'g': first,
                                          'expires': str(time.time_ns() // 1_000_000 + 120000), 'data': 'AAAAAQs='}))
                    self.ae(parse(received[-1])['error'], 'changed')
                    self.ae(base64.b64decode(parse(received[-1])['data']), FAILURE)
                finally:
                    provider.close()

    def test_ssh_agent_frame_validation(self):
        for raw in ('777;ssh-agent;v=1:op=query:id=x:id=y', '777;ssh-agent;v=2:op=query:id=x',
                    '777;ssh-agent;v=1:op=query:id=bad id', '777;ssh-agent;v=1:op=query:id=x:\x01=z'):
            with self.assertRaises(ValueError):
                parse(raw)

    def test_ssh_agent_socket_error(self):
        received = []
        provider = AgentProvider(received.append, lambda: '/tmp/' + 'x' * 300)
        try:
            provider.handle(frame({'v': '1', 'op': 'query', 'id': 'probe'}))
            provider.handle(frame({'v': '1', 'op': 'request', 'id': 'request', 'c': 'connection',
                                  'g': provider.generation, 'expires': str(time.time_ns() // 1_000_000 + 120000),
                                  'data': 'AAAAAQs='}))
            self.ae(parse(received[-1])['error'], 'unavailable')
            self.assertFalse(provider.pending)
        finally:
            provider.close()
