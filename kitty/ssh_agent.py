#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Adam DePrince
"""SSH agent packets over OSC 777, without blocking the terminal event loop."""

import atexit
import base64
import errno
import os
import selectors
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from uuid import uuid4

FAILURE = b'\0\0\0\1\5'
PREFIX = '777;ssh-agent;'


def token(value: str) -> bool:
    return bool(value) and all(c.isascii() and (c.isalnum() or c in '_.-') for c in value)


def parse(raw: bytes | str) -> dict[str, str]:
    if isinstance(raw, bytes):
        raw = raw.decode('ascii')
    raw = raw.removeprefix('\x1b]').removesuffix('\x1b\\').removesuffix('\x07')
    if not raw.startswith(PREFIX):
        raise ValueError('not an agent frame')
    fields: dict[str, str] = {}
    for part in raw[len(PREFIX):].split(':'):
        key, value = part.split('=', 1)
        if not key or key in fields or any(ord(c) < 32 or ord(c) > 126 for c in part):
            raise ValueError('invalid agent fields')
        fields[key] = value
    if fields.get('v') != '1' or not token(fields.get('id', '')) or not fields.get('op'):
        raise ValueError('invalid agent header')
    return fields


def frame(fields: Mapping[str, str]) -> bytes:
    return ('\x1b]' + PREFIX + ':'.join(f'{k}={v}' for k, v in fields.items()) + '\x1b\\').encode('ascii')


def packet(data: bytes) -> bool:
    return len(data) >= 5 and struct.unpack('!I', data[:4])[0] + 4 == len(data)


def permitted(data: bytes) -> bool:
    if not packet(data):
        return False
    if data[4] in (11, 13):  # SSH_AGENTC_REQUEST_IDENTITIES / SSH_AGENTC_SIGN_REQUEST
        return True
    if data[4] == 27 and len(data) >= 9:
        length = struct.unpack('!I', data[5:9])[0]
        return len(data) >= 9 + length and data[9:9 + length] in (b'session-bind@openssh.com', b'query')
    return False


class AgentSource:
    """Reuse the local agent; own and reap an agent only when we started it."""

    def __init__(self) -> None:
        self.process: subprocess.Popen | None = None
        self.directory = ''
        self.path = ''

    def address(self, env: Mapping[str, str] | None = None) -> str:
        path = (os.environ if env is None else env).get('SSH_AUTH_SOCK', '')
        if path:
            try:
                if stat.S_ISSOCK(os.stat(path).st_mode):
                    return path
            except OSError:
                pass
        if self.process is not None and self.process.poll() is None:
            return self.path
        self.close()
        exe = shutil.which('ssh-agent')
        if not exe:
            return ''
        self.directory = tempfile.mkdtemp(prefix='silkie-ssh-agent-', dir='/tmp')
        self.path = os.path.join(self.directory, 'socket')
        try:
            self.process = subprocess.Popen([exe, '-D', '-a', self.path], stdin=subprocess.DEVNULL,
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
        except OSError:
            self.close()
            return ''
        return self.path

    def close(self) -> None:
        process, self.process = self.process, None
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if self.directory:
            shutil.rmtree(self.directory, ignore_errors=True)
        self.directory = self.path = ''


source = AgentSource()
atexit.register(source.close)


@dataclass
class Connection:
    socket: socket.socket | None = None
    connecting: bool = False
    output: bytearray = field(default_factory=bytearray)
    input: bytearray = field(default_factory=bytearray)
    pending: dict[str, str] | None = None
    used_ids: set[str] = field(default_factory=set)
    deadline: float = 0


class AgentProvider:
    def __init__(self, send: Callable[[bytes], None], address: Callable[[], str] = source.address,
                 timeout: float = 120) -> None:
        self.send, self.address, self.timeout = send, address, timeout
        self.generation = uuid4().hex
        self.identity: tuple[str, int, int] | None = None
        self.path = ''
        self.connections: dict[str, Connection] = {}
        self.closed_channels: set[str] = set()
        self.selector = selectors.DefaultSelector()

    def close_connection(self, channel: str) -> None:
        self.closed_channels.add(channel)
        c = self.connections.pop(channel, None)
        if c is not None and c.socket is not None:
            try:
                self.selector.unregister(c.socket)
            except KeyError:
                pass
            c.socket.close()

    def close(self) -> None:
        for channel in tuple(self.connections):
            self.close_connection(channel)
        self.selector.close()

    def refresh(self) -> bool:
        path = self.address()
        try:
            stat = os.stat(path)
            identity = path, stat.st_dev, stat.st_ino
        except OSError:
            # An owned ssh-agent is starting asynchronously; its first request
            # waits for the socket instead of sleeping in the render thread.
            identity = path, 0, 0
        if not path:
            return False
        if self.identity is None:
            self.identity = identity
        elif identity != self.identity:
            # Creation of the socket for this same pending agent isn't a new
            # provider. Replacing an existing socket or selecting a new path is.
            if self.identity[0] != path or self.identity[1:] != (0, 0):
                for channel, c in tuple(self.connections.items()):
                    if c.pending:
                        self.reply(c.pending, FAILURE, 'changed')
                    self.close_connection(channel)
                self.generation = uuid4().hex
                self.closed_channels.clear()
            self.identity = identity
        self.path = path
        return True

    def reply(self, request: Mapping[str, str], data: bytes = FAILURE, error: str = '') -> None:
        result = {'v': '1', 'op': 'reply', 'id': request['id'], 'c': request.get('c', ''),
                  'g': request.get('g', ''), 'data': base64.b64encode(data).decode('ascii')}
        if error:
            result['error'] = error
        self.send(frame(result))

    def handle(self, raw: bytes | str, allowed: bool = True) -> None:
        try:
            f = parse(raw)
        except (UnicodeError, ValueError):
            return
        op = f['op']
        if op == 'close':
            if f.get('g') == self.generation and token(f.get('c', '')):
                self.closed_channels.add(f['c'])
                self.close_connection(f['c'])
            return
        if op not in ('query', 'request'):
            return
        available = allowed and self.refresh()
        if op == 'query':
            result = {'v': '1', 'op': 'cap', 'id': f['id'], 'caps': '1' if available else '0'}
            if available:
                result['g'] = self.generation
            self.send(frame(result))
            return
        if not available or f.get('g') != self.generation:
            self.reply(f, error='changed' if available else 'unavailable')
            return
        expiry = f.get('expires', '')
        # The protocol timestamp is an unsigned 64-bit Unix millisecond count.
        if (not expiry.isascii() or not expiry.isdecimal() or len(expiry) > 20
                or int(expiry) > 0xffffffffffffffff or int(expiry) <= time.time_ns() // 1_000_000):
            self.reply(f, error='expired')
            return
        channel = f.get('c', '')
        if not token(channel):
            self.reply(f, error='invalid')
            return
        if channel in self.closed_channels:
            self.reply(f, error='closed')
            return
        try:
            data = base64.b64decode(f.get('data', ''), validate=True)
        except ValueError:
            self.reply(f, error='invalid')
            return
        if not permitted(data):
            self.reply(f, error='denied')
            return
        c = self.connections.setdefault(channel, Connection())
        if f['id'] in c.used_ids or c.pending:
            self.reply(f, error='duplicate' if f['id'] in c.used_ids else 'busy')
            return
        c.used_ids.add(f['id'])
        c.pending, c.output, c.deadline = f, bytearray(data), time.monotonic() + self.timeout
        self.tick()

    @property
    def pending(self) -> bool:
        return any(c.pending is not None for c in self.connections.values())

    def tick(self) -> None:
        for channel, c in tuple(self.connections.items()):
            if not c.pending:
                continue
            if time.monotonic() >= c.deadline:
                self.reply(c.pending, error='timeout')
                self.close_connection(channel)
                continue
            if c.socket is None:
                sock = None
                try:
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.setblocking(False)
                    code = sock.connect_ex(self.path)
                except OSError:
                    if sock is not None:
                        sock.close()
                    self.reply(c.pending, error='unavailable')
                    self.close_connection(channel)
                    continue
                if code == errno.ENOENT and self.identity and self.identity[1:] == (0, 0):
                    sock.close()
                    continue
                if code not in (0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EAGAIN):
                    sock.close()
                    self.reply(c.pending, error='unavailable')
                    self.close_connection(channel)
                    continue
                c.socket, c.connecting = sock, code != 0
                self.selector.register(sock, selectors.EVENT_WRITE, channel)
            elif c.output:
                self.selector.modify(c.socket, selectors.EVENT_WRITE, channel)
        for key, mask in self.selector.select(0):
            channel = key.data
            c = self.connections.get(channel)
            if c is None or c.socket is None or c.pending is None:
                continue
            try:
                if c.connecting:
                    code = c.socket.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                    if code:
                        raise OSError(code, os.strerror(code))
                    c.connecting = False
                if mask & selectors.EVENT_WRITE:
                    count = c.socket.send(c.output)
                    del c.output[:count]
                    if not c.output:
                        self.selector.modify(c.socket, selectors.EVENT_READ, channel)
                if mask & selectors.EVENT_READ:
                    # Streaming scratch, independent of agent message length.
                    data = c.socket.recv(65536)
                    if not data:
                        raise OSError('agent disconnected')
                    c.input.extend(data)
                    if len(c.input) >= 4:
                        size = struct.unpack('!I', c.input[:4])[0] + 4
                        if len(c.input) >= size:
                            result = bytes(c.input)
                            if not packet(result):
                                raise OSError('invalid agent reply')
                            self.reply(c.pending, result)
                            c.pending = None
                            c.input.clear()
            except (BlockingIOError, InterruptedError):
                pass
            except OSError:
                self.reply(c.pending, error='unavailable')
                self.close_connection(channel)
