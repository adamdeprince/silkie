import assert from 'node:assert/strict';
import test from 'node:test';
import {DEMO_GAMES, DemoSession, createGameLauncher, demoGameFromURL, demoStorageName, featureReport, readGuestManifest} from './runtime.mjs';

const identity = 'a'.repeat(64);
function fixture(game = null) {
    const calls = {input: [], run: [], console: [], state: [], network: [], login: [], writes: [], created: [], storage: []};
    let finish;
    const guest = {
        run: (...args) => {
            calls.run.push(args);
            if (args[0] === '/usr/local/libexec/silkie-console-session') return Promise.resolve({status: 0});
            return new Promise(resolve => { finish = resolve; });
        },
        setCustomConsole: (...args) => { calls.console.push(args); return byte => calls.input.push(byte); },
        networkLogin: async () => {},
    };
    const CheerpX = {
        HttpBytesDevice: {create: async image => ({image})},
        IDBDevice: {create: async name => { calls.storage.push(name); return {name}; }},
        OverlayDevice: {create: async (...args) => ({args})},
        DataDevice: {create: async () => ({writeFile: async (...args) => calls.writes.push(args)})},
        Linux: {create: async options => { calls.created.push(options); return guest; }},
    };
    const output = [];
    const terminal = {cols: 100, rows: 30, capabilities: {text: true}, write: bytes => output.push(bytes), focus() {}};
    const session = new DemoSession({CheerpX, manifest: {sha256: identity, image: 'http://localhost/guest.ext2'}, terminal, game,
        onState: (...args) => calls.state.push(args), onNetwork: (...args) => calls.network.push(args), onLogin: url => calls.login.push(url)});
    return {session, guest, calls, output, done: () => finish({status: 0})};
}

test('manifest requires image identity and same-origin disk', async () => {
    const url = new URL('http://localhost/_static/demo/guest.json');
    const load = data => readGuestManifest(url, async () => ({ok: true, json: async () => data}));
    assert.equal((await load({sha256: identity})).image, 'http://localhost/_static/demo/guest.ext2');
    await assert.rejects(load({sha256: 'x'}), /SHA-256/);
    await assert.rejects(load({sha256: identity, image: 'https://other.example/guest.ext2'}), /website/);
    await assert.rejects(readGuestManifest(url, async () => ({ok: false})), /make browser-demo/);
});

test('game URLs accept only one exact allowlisted name', () => {
    for (const game of DEMO_GAMES) {
        assert.equal(demoGameFromURL(`https://silkie.example/browser-demo.html?game=${game}`), game);
    }
    for (const query of ['', '?game=', '?game=/bin/sh', '?game=silkie-sprint%3Bls', '?game=silkie-sprint&game=silkie-scramble', '?game=SILKIE-SPRINT']) {
        assert.equal(demoGameFromURL(`https://silkie.example/browser-demo.html${query}`), null);
    }
    assert.throws(() => fixture('/bin/sh'), /Choose/);
});

test('game launcher opens independent named windows and reuses each live game', () => {
    const calls = [], popups = [];
    const launch = createGameLauncher({url: 'https://silkie.example/browser-demo.html?ignored=yes#old', openWindow: (...args) => {
        calls.push(args);
        const popup = {closed: false, focuses: 0, focus() { this.focuses++; }};
        popups.push(popup);
        return popup;
    }});
    const sprint = launch('silkie-sprint');
    assert.equal(launch('silkie-sprint'), sprint);
    assert.equal(calls.length, 1);
    assert.equal(sprint.focuses, 2);
    assert.deepEqual(calls[0], ['https://silkie.example/browser-demo.html?game=silkie-sprint', 'silkie-silkie-sprint', 'popup,width=960,height=720']);
    assert.notEqual(launch('silkie-scramble'), sprint);
    assert.notEqual(calls[0][1], calls[1][1]);
    sprint.closed = true;
    assert.notEqual(launch('silkie-sprint'), sprint);
    assert.equal(calls[2][1], calls[0][1]);
    assert.throws(() => launch('echo dangerous'), /Choose/);
    assert.throws(() => launch(null), /Choose/);
    assert.equal(calls.length, 3);
    const blocked = createGameLauncher({url: 'https://silkie.example/browser-demo.html', openWindow: () => null});
    assert.equal(blocked('silkie-sprint'), null);
});

test('homepage and all game filesystems have matching distinct lock/storage identities', async () => {
    const names = [];
    for (const game of [null, ...DEMO_GAMES]) {
        const {session, calls, done} = fixture(game);
        const first = session.start();
        await session.initialize();
        const expected = demoStorageName(identity, game);
        assert.deepEqual(calls.storage, [expected]);
        names.push(expected);
        const gameEnv = calls.run[0][2].env.filter(entry => entry.startsWith('SILKIE_DEMO_GAME='));
        assert.deepEqual(gameEnv, game ? [`SILKIE_DEMO_GAME=${game}`] : []);
        assert.equal(calls.run[0][0], '/usr/local/bin/silkie-demo-shell');
        done(); await first;
        const second = session.start();
        await new Promise(resolve => setImmediate(resolve));
        done(); await second;
        assert.deepEqual(calls.storage, [expected]);
        assert.deepEqual(calls.run[1][2].env, calls.run[0][2].env);
        assert.equal(calls.created.length, 1);
    }
    assert.equal(new Set(names).size, DEMO_GAMES.length + 1);
    assert.equal(names[0], `silkie-demo-${identity}`);
    assert.throws(() => demoStorageName(identity, 'unlisted-game'), /Choose/);
});

test('report lists browser restrictions, not implementation gaps, and embeds SSH links', () => {
    const report = featureReport({text: true, vectors: false});
    assert.match(report, /Browser restrictions/);
    assert.match(report, /No raw TCP/);
    assert.match(report, /\x1b\]8;;silkie:tailscale/);
    assert.match(report, /silkie-features/);
    assert.doesNotMatch(report, /not ported|not enabled|not included|vectors|key-release/);
});

test('initialization is shared and does not initiate a network login', async () => {
    const {session, guest, calls} = fixture();
    let logins = 0;
    guest.networkLogin = async () => { logins++; };
    await Promise.all([session.initialize(), session.initialize()]);
    assert.equal(calls.created.length, 1);
    assert.equal(logins, 0);
    const mounts = calls.created[0].mounts;
    assert.deepEqual(mounts.map(m => m.path), ['/', '/run/silkie', '/dev', '/proc']);
    assert.equal(calls.writes[0][0], '/browser-features.txt');
});

test('split UTF-8 and binary graphics output are copied without decoding', async () => {
    const {session, calls, output} = fixture();
    await session.initialize();
    const write = calls.console[0][0];
    const first = Uint8Array.of(0xf0, 0x9f);
    write(first); first.fill(0);
    write(Uint8Array.of(0x90, 0x94, 0x1b, 0x5f, 0x47, 0xff));
    assert.deepEqual([...output[0], ...output[1]], [0xf0, 0x9f, 0x90, 0x94, 0x1b, 0x5f, 0x47, 0xff]);
});

test('one shell at a time, repeated shells reuse initialized guest', async () => {
    const {session, calls, done} = fixture();
    const first = session.start();
    await session.initialize();
    await session.start();
    assert.equal(calls.run.length, 1);
    done(); await first;
    const second = session.start();
    await new Promise(resolve => setImmediate(resolve));
    done(); await second;
    assert.equal(calls.created.length, 1);
    assert.equal(calls.run.length, 2);
    assert.equal(calls.run[0][2].uid, 1000);
    assert.ok(calls.run[0][2].env.includes('TERM=xterm-256color'));
});

test('input larger than former parser buffers and resize preserve bytes', async () => {
    const {session, calls, done} = fixture();
    const run = session.start();
    await session.initialize();
    const input = Uint8Array.from({length: 1024 * 1024 + 37}, (_, i) => i % 256);
    session.send(input);
    assert.deepEqual(Uint8Array.from(calls.input), input);
    session.resize(132, 50);
    assert.deepEqual(calls.console.at(-1).slice(1), [132, 50]);
    done(); await run;
    calls.input.length = 0;
    session.send(Uint8Array.of(65));
    assert.equal(calls.input.length, 0);
});

test('Tailscale login is explicit, failures clear stale login and allow retry', async () => {
    const {session, guest, calls} = fixture();
    await assert.rejects(session.connect(), /Start the shell/);
    await session.initialize();
    const callbacks = calls.created[0].networkInterface;
    callbacks.loginUrlCb('https://login.tailscale.com/ignored');
    assert.equal(calls.login.length, 0);
    guest.networkLogin = async () => {
        callbacks.loginUrlCb('https://login.tailscale.com/a/test');
        throw new Error('cancelled');
    };
    await assert.rejects(session.connect(), /cancelled/);
    assert.equal(calls.login.at(-1), null);
    assert.equal(session.connectRequested, false);
    callbacks.loginUrlCb('https://login.tailscale.com/stale');
    assert.equal(calls.login.at(-1), null);
    guest.networkLogin = async () => {
        callbacks.netmapUpdateCb({self: {addresses: ['100.1.2.3/32']}});
        callbacks.stateUpdateCb(6);
    };
    await session.connect();
    assert.deepEqual(calls.network.at(-1), ['connected', '100.1.2.3/32']);
});

test('concurrent connect calls share one login attempt', async () => {
    const {session, guest} = fixture();
    await session.initialize();
    let finish, attempts = 0;
    guest.networkLogin = () => { attempts++; return new Promise(resolve => { finish = resolve; }); };
    const first = session.connect(), second = session.connect();
    await Promise.resolve();
    assert.equal(attempts, 1);
    finish(); await Promise.all([first, second]);
});

test('failed guest initialization requests a reload instead of creating duplicate workers', async () => {
    const {session, calls} = fixture();
    session.CheerpX.Linux.create = async () => { throw new Error('disk read failed'); };
    await assert.rejects(session.start(), /Reload the page/);
    assert.equal(session.initializationFailed, true);
    assert.equal(session.running, false);
    assert.equal(calls.state.at(-1)[0], 'fatal');
});

test('malformed or insecure login URL rejects the attempt and clears the link', async () => {
    for (const url of ['not a URL', 'http://login.tailscale.com/']) {
        const {session, guest, calls} = fixture();
        await session.initialize();
        guest.networkLogin = () => {
            calls.created[0].networkInterface.loginUrlCb(url);
            return new Promise(() => {});
        };
        await assert.rejects(session.connect());
        assert.equal(session.connectRequested, false);
        assert.equal(calls.login.at(-1), null);
        assert.equal(calls.network.at(-1)[0], 'error');
    }
});

test('resize notifies only the foreground application and coalesces overlapping notifications', async () => {
    const {session, guest, calls, done} = fixture();
    const run = session.start();
    await session.initialize();
    const original = guest.run;
    let completeSignal;
    guest.run = (...args) => {
        if (args[0] !== '/usr/local/libexec/silkie-console-session') return original(...args);
        calls.run.push(args);
        return new Promise(resolve => { completeSignal = resolve; });
    };
    session.resize(90, 25);
    session.resize(91, 26);
    session.resize(92, 27);
    assert.equal(calls.run.filter(args => args[0] === '/usr/local/libexec/silkie-console-session').length, 1);
    completeSignal({status: 0});
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(calls.run.filter(args => args[0] === '/usr/local/libexec/silkie-console-session').length, 2);
    completeSignal({status: 0});
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(session.signalingResize, false);
    assert.equal(calls.run.at(-1)[0], '/usr/local/libexec/silkie-console-session');
    assert.deepEqual(calls.run.at(-1)[1], ['--notify-resize']);
    assert.equal(calls.run.at(-1)[2].uid, 1000);
    done(); await run;
});
