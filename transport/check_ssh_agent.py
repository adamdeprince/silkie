#!/usr/bin/env python3
"""Exercise the transport with real ssh-agent, ssh-add and verified signatures.

Compile test_agent_proxy.cc, then run this script with --proxy PATH. Add
--odin PATH and/or --skiff-client PATH --skiff-server PATH for real PTY and
network relay checks. All keys, agents, servers and sockets are temporary.
"""
import argparse
import base64
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from kitty.ssh_agent import AgentProvider, FAILURE, frame, parse

ST = b'\x1b\\'


def packet(body):
    return struct.pack('!I', len(body)) + body


class Test:
    def __init__(self, root):
        self.root, self.terminals, self.agents = root, [], []
        self.env = dict(os.environ, TERM='xterm-kitty', SHELL='/bin/sh',
                        XDG_CONFIG_HOME=str(root / 'config'), ODIN_SEAT_KITTY_MEDIUM='direct')
        for key in ('ODIN_SEAT_SOCK', 'ODIN_SEAT_SESSION', 'TMUX', 'SSH_AUTH_SOCK'):
            self.env.pop(key, None)
        self.fixture = root / 'child.py'
        self.fixture.write_text('import os,sys,time\nfrom pathlib import Path\n'
                                'Path(sys.argv[1]).write_text(os.environ["SSH_AUTH_SOCK"])\n'
                                'time.sleep(300)\n')

    def pump(self):
        for terminal in self.terminals:
            terminal.pump()
        time.sleep(.001)

    def wait(self, predicate, seconds=15):
        deadline = time.monotonic() + seconds
        while not predicate():
            assert time.monotonic() < deadline, 'agent test timed out'
            self.pump()

    def command(self, argv, env=None, success=True):
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            proc = subprocess.Popen(argv, env=env or self.env, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
            try:
                self.wait(lambda: proc.poll() is not None, 30)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
            out.seek(0); err.seek(0)
            stdout, stderr = out.read(), err.read()
            assert (proc.returncode == 0) == success, (argv, proc.returncode, stderr)
            return stdout, stderr

    def agent(self, name):
        key, sock = self.root / name, self.root / (name + '.sock')
        self.command(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', name, '-f', str(key)])
        proc = subprocess.Popen(['ssh-agent', '-D', '-a', str(sock)], stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.agents.append(proc)
        self.wait(sock.exists)
        self.command(['ssh-add', str(key)], dict(self.env, SSH_AUTH_SOCK=str(sock)))
        return key, sock

    def listing(self, path, key):
        stdout, _ = self.command(['ssh-add', '-L'], dict(self.env, SSH_AUTH_SOCK=str(path)))
        assert stdout.strip() == Path(str(key) + '.pub').read_bytes().strip(), stdout

    def sign(self, path, key, terminal, expect_terminal=True):
        data = self.root / ('message-' + key.name)
        data.write_bytes(b'a real signature made through the terminal transport\n')
        signature = Path(str(data) + '.sig')
        signature.unlink(missing_ok=True)
        before = terminal.signs
        self.command(['ssh-keygen', '-Y', 'sign', '-f', str(key) + '.pub', '-n', 'silkie-agent', str(data)],
                     dict(self.env, SSH_AUTH_SOCK=str(path)))
        assert (terminal.signs > before) == expect_terminal, 'unexpected signing provider'
        allowed = self.root / 'allowed-signers'
        allowed.write_bytes(b'test ' + Path(str(key) + '.pub').read_bytes())
        with data.open('rb') as source:
            verified = subprocess.run(['ssh-keygen', '-Y', 'verify', '-f', str(allowed), '-I', 'test',
                                       '-n', 'silkie-agent', '-s', str(signature)], stdin=source,
                                      capture_output=True, timeout=5)
        assert verified.returncode == 0, verified.stderr

    def exchange(self, sock, data):
        sock.sendall(data)
        got = bytearray()
        sock.setblocking(False)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            self.pump()
            try:
                part = sock.recv(65536)
            except BlockingIOError:
                continue
            if not part:
                return bytes(got)
            got.extend(part)
            if len(got) >= 4 and len(got) >= struct.unpack('!I', got[:4])[0] + 4:
                return bytes(got)
        raise AssertionError('agent socket reply timed out')

    def close(self):
        for terminal in self.terminals:
            terminal.close()
        for proc in self.agents:
            proc.terminate()
            proc.wait(timeout=5)


class Endpoint:
    def __init__(self, test, address):
        self.test, self.address = test, address
        self.provider = AgentProvider(self.write, lambda: str(self.address))
        self.signs = 0
        self.allowed = True

    def handle(self, raw):
        f = parse(raw)
        if f['op'] == 'request' and base64.b64decode(f['data'])[4] == 13:
            self.signs += 1
        self.provider.handle(raw, self.allowed)


class ProxyTerminal(Endpoint):
    def __init__(self, test, binary, address):
        self.proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=test.env)
        self.path = Path(self.proc.stdout.readline().decode().strip())
        os.set_blocking(self.proc.stdout.fileno(), False)
        self.buffer = bytearray()
        super().__init__(test, address)
        test.terminals.append(self)

    def write(self, data):
        self.proc.stdin.write(data + b'\n')
        self.proc.stdin.flush()

    def pump(self):
        if self.proc.poll() is not None:
            return
        while select.select([self.proc.stdout], [], [], 0)[0]:
            part = os.read(self.proc.stdout.fileno(), 65536)
            if not part:
                break
            self.buffer.extend(part)
        while b'\n' in self.buffer:
            raw, _, rest = self.buffer.partition(b'\n')
            self.buffer[:] = rest
            self.handle(bytes(raw))
        self.provider.tick()

    def close(self):
        self.provider.close()
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)


class Terminal(Endpoint):
    def __init__(self, test, argv, address, env=None):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 1000, 600))
        self.proc = subprocess.Popen(argv, env=env or test.env, stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        self.buffer, self.seen, self.answered = bytearray(), bytearray(), set()
        super().__init__(test, address)
        test.terminals.append(self)

    def write(self, data):
        os.write(self.master, data)

    def pump(self):
        if self.proc.poll() is not None:
            return
        if select.select([self.master], [], [], 0)[0]:
            try:
                part = os.read(self.master, 1 << 20)
            except OSError:
                return
            self.buffer.extend(part); self.seen.extend(part)
        for query, response in ((b'\x1b[16t', b'\x1b[6;20;10t'), (b'\x1b[18t', b'\x1b[8;30;100t'),
                                (b'\x1b[c', b'\x1b[?62;22c')):
            if query in self.seen and query not in self.answered:
                self.write(response); self.answered.add(query)
        # Consume whole OSCs, leaving partial frames intact across reads.
        while True:
            match = re.search(rb'\x1b\]([^\x1b\x07]*)(?:\x1b\\|\x07)', self.buffer)
            if not match:
                break
            raw = bytes(match[0]); del self.buffer[:match.end()]
            if b']777;ssh-agent;' in raw:
                self.handle(raw)
            elif b']777;caps;' in raw and b'op=query' in raw:
                ident = re.search(rb'(?:;|:)id=([^:\x1b\x07]*)', raw)[1]
                self.write(b'\x1b]777;caps;v=1:op=cap:id=' + ident +
                           b':caps=3:ext=caps.1,ssh-agent.1:media=d:format=24,32' + ST)
        self.provider.tick()

    def close(self):
        self.provider.close()
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.wait()
        os.close(self.master)


def check_proxy(test, binary, a, b):
    terminal = ProxyTerminal(test, binary, a[1])
    test.listing(terminal.path, a[0])
    test.sign(terminal.path, a[0], terminal)
    with socket.socket(socket.AF_UNIX) as conn:
        conn.connect(str(terminal.path))
        assert test.exchange(conn, packet(b'\x0b'))[4] == 12
        terminal.address = b[1]
        before = terminal.signs
        key_blob = base64.b64decode(Path(str(a[0]) + '.pub').read_bytes().split()[1])
        sign = b'\x0d' + struct.pack('!I', len(key_blob)) + key_blob + struct.pack('!I', 4) + b'test' + b'\0\0\0\0'
        assert test.exchange(conn, packet(sign)) in (b'', FAILURE)
        assert terminal.signs == before, 'a request crossed provider generations'
    test.listing(terminal.path, b[0])
    test.sign(terminal.path, b[0], terminal)
    assert terminal.path.stat().st_mode & 0o777 == 0o600
    assert terminal.path.parent.stat().st_mode & 0o777 == 0o700
    terminal.close(); test.terminals.remove(terminal)
    print('proxy: real key listing, verified signatures, provider change and socket permissions passed')


def check_odin(test, binary, a, b):
    address = test.root / 'odin.socket'
    inner = test.root / 'inner.socket'
    def cmd(*args):
        return test.command([binary, '-S', str(address), *args])
    pathfile = test.root / 'odin-agent-path'
    cmd('new-session', '-d', '--', sys.executable, str(test.fixture), str(pathfile))
    test.wait(pathfile.exists)
    path = Path(pathfile.read_text())
    try:
        one = Terminal(test, [binary, '-S', str(address), 'attach'], a[1])
        test.wait(lambda: b'\x1b[c' in one.seen)
        test.listing(path, a[0]); test.sign(path, a[0], one)
        with socket.socket(socket.AF_UNIX) as conn:
            conn.connect(str(path)); assert test.exchange(conn, packet(b'\x0b'))[4] == 12
            two = Terminal(test, [binary, '-S', str(address), 'attach'], b[1])
            test.wait(lambda: b'\x1b[c' in two.seen)
            # A terminal bootstrap finishes asynchronously after the last reply.
            test.listing(path, b[0])
            try:
                stale = test.exchange(conn, packet(b'\x0b'))
            except BrokenPipeError:
                stale = b''
            assert stale in (b'', FAILURE)
        test.sign(path, b[0], two)
        cmd('upgrade')
        test.listing(path, b[0])  # the socket pathname and listening fd survived exec
        inner_pathfile = test.root / 'inner-agent-path'
        test.command([binary, '-S', str(inner), 'new-session', '-d', '--', sys.executable,
                      str(test.fixture), str(inner_pathfile)])
        test.wait(inner_pathfile.exists)
        cmd('new-window', '--', 'env', '-u', 'ODIN_SEAT_SOCK', '-u', 'ODIN_SEAT_SESSION',
            binary, '-S', str(inner), 'attach')
        try:
            test.wait(lambda: bool(test.command([binary, '-S', str(inner), 'list-clients'])[0].strip()))
        except AssertionError:
            print('nested Odin output:', cmd('capture-window', '-w', '2'), file=sys.stderr)
            raise
        inner_path = Path(inner_pathfile.read_text())
        test.listing(inner_path, b[0]); test.sign(inner_path, b[0], two)
        two.close(); test.terminals.remove(two)
        test.listing(path, a[0]); test.listing(inner_path, a[0])
        test.command([binary, '-S', str(inner), 'kill-server'])
        print('odin: clients A/B, nested sessions, verified signatures and stable socket after upgrade passed')
    finally:
        if inner.exists():
            test.command([binary, '-S', str(inner), 'kill-server'])
        cmd('kill-server')


def check_skiff(test, client, server, a, b, fallback=False):
    pathfile = test.root / 'skiff-agent-path'
    env = dict(test.env, MOSH_CLIENT_CAPS='keepalive-v1,sixel-state-v1', MOSH_SERVER_NETWORK_TMOUT='120')
    out, err = test.command([server, 'new', '-i', '127.0.0.1', '-c', '256', '--', sys.executable,
                             str(test.fixture), str(pathfile)], env)
    port, key = re.search(rb'MOSH CONNECT (\d+) (\S+)', out).groups()
    pid = int(re.search(rb'detached, pid = (\d+)', err)[1])
    env.update(MOSH_KEY=key.decode(), GOBLIN_SKIFF_SIXEL_STATE='1', GOBLIN_SKIFF_COMPACT_KEEPALIVE='1',
               GOBLIN_SKIFF_MASCOT='none', MOSH_PREDICTION_DISPLAY='never', MOSH_NO_TERM_INIT='1')
    link = test.root / 'fallback-agent'
    if fallback:
        link.symlink_to(a[1])
        env['SSH_AUTH_SOCK'] = str(link)
    terminal = Terminal(test, [client, *(['-A'] if fallback else []), '127.0.0.1', port.decode()], a[1], env)
    terminal.allowed = not fallback
    try:
        test.wait(pathfile.exists)
        path = Path(pathfile.read_text())
        test.listing(path, a[0]); test.sign(path, a[0], terminal, not fallback)
        if fallback:
            link.unlink(); link.symlink_to(b[1])
        else:
            terminal.address = b[1]
        test.listing(path, b[0]); test.sign(path, b[0], terminal, not fallback)
        print('skiff: real client/server, changed keys and verified signatures passed' + (' with -A local fallback' if fallback else ' through the terminal channel'))
    finally:
        terminal.close(); test.terminals.remove(terminal)
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass


def check_odin_fallback(test, binary, a, b):
    address, pathfile = test.root / 'fallback.socket', test.root / 'fallback-agent-path'
    def command(*args):
        return test.command([binary, '-S', str(address), *args])
    command('new-session', '-d', '--', sys.executable, str(test.fixture), str(pathfile))
    try:
        test.wait(pathfile.exists)
        path = Path(pathfile.read_text())
        unavailable = Terminal(test, [binary, '-S', str(address), 'attach'], a[1])
        unavailable.allowed = False
        test.wait(lambda: b'\x1b[c' in unavailable.seen)
        test.command(['ssh-add', '-L'], dict(test.env, SSH_AUTH_SOCK=str(path)), success=False)
        link = test.root / 'odin-fallback-agent'
        link.symlink_to(a[1])
        env = dict(test.env, SSH_AUTH_SOCK=str(link))
        fallback = Terminal(test, [binary, '-A', '-S', str(address), 'attach'], a[1], env)
        fallback.allowed = False
        test.wait(lambda: b'\x1b[c' in fallback.seen)
        test.listing(path, a[0]); test.sign(path, a[0], fallback, expect_terminal=False)
        link.unlink(); link.symlink_to(b[1])
        test.listing(path, b[0]); test.sign(path, b[0], fallback, expect_terminal=False)
        print('odin: unsupported terminal fails cleanly; explicit -A fallback follows changed local agents')
    finally:
        command('kill-server')


def check_chain(test, odin, client, server, a, b):
    """Two Odin layers separated by the real Skiff client/server transport."""
    remote = test.root / 'chain-remote.socket'
    local = test.root / 'chain-local.socket'
    pathfile = test.root / 'chain-agent-path'
    env = dict(test.env, MOSH_CLIENT_CAPS='keepalive-v1,sixel-state-v1', MOSH_SERVER_NETWORK_TMOUT='120')
    out, err = test.command([server, 'new', '-i', '127.0.0.1', '-c', '256', '--', odin, '-S', str(remote),
                             'new-session', '--', sys.executable, str(test.fixture), str(pathfile)], env)
    port, key = re.search(rb'MOSH CONNECT (\d+) (\S+)', out).groups()
    pid = int(re.search(rb'detached, pid = (\d+)', err)[1])
    env.update(MOSH_KEY=key.decode(), GOBLIN_SKIFF_SIXEL_STATE='1', GOBLIN_SKIFF_COMPACT_KEEPALIVE='1',
               GOBLIN_SKIFF_MASCOT='none', MOSH_PREDICTION_DISPLAY='never', MOSH_NO_TERM_INIT='1')
    config, wrapper = test.root / 'chain-client.json', test.root / 'chain-client.py'
    config.write_text(json.dumps({'env': env, 'argv': [client, '127.0.0.1', port.decode()]}))
    wrapper.write_text('import json,os,sys\nc=json.load(open(sys.argv[1]));os.execvpe(c["argv"][0],c["argv"],c["env"])\n')
    def command(*args):
        return test.command([odin, '-S', str(local), *args])
    try:
        command('new-session', '-d', '--', sys.executable, str(wrapper), str(config))
        first = Terminal(test, [odin, '-S', str(local), 'attach'], a[1])
        test.wait(pathfile.exists, 30)
        path = Path(pathfile.read_text())
        test.listing(path, a[0]); test.sign(path, a[0], first)
        second = Terminal(test, [odin, '-S', str(local), 'attach'], b[1])
        test.wait(lambda: b'\x1b[c' in second.seen)
        test.listing(path, b[0]); test.sign(path, b[0], second)
        command('upgrade')
        test.listing(path, b[0])
        print('chain: Odin -> Skiff -> Odin -> native provider, client A/B and upgrade passed with verified signatures')
    finally:
        if remote.exists():
            test.command([odin, '-S', str(remote), 'kill-server'])
        if local.exists():
            command('kill-server')
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--proxy')
    parser.add_argument('--odin')
    parser.add_argument('--skiff-client')
    parser.add_argument('--skiff-server')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='silkie-agent-test-', dir='/tmp') as directory:
        test = Test(Path(directory))
        try:
            a, b = test.agent('client-A'), test.agent('client-B')
            if args.proxy:
                check_proxy(test, args.proxy, a, b)
            if args.odin:
                check_odin(test, args.odin, a, b)
                check_odin_fallback(test, args.odin, a, b)
            if args.skiff_client:
                check_skiff(test, args.skiff_client, args.skiff_server, a, b)
                (test.root / 'skiff-agent-path').unlink(missing_ok=True)
                check_skiff(test, args.skiff_client, args.skiff_server, a, b, fallback=True)
                if args.odin:
                    check_chain(test, args.odin, args.skiff_client, args.skiff_server, a, b)
        finally:
            test.close()


if __name__ == '__main__':
    main()
