// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';
import { createCore, KittyGraphics, encodeKey, KeyboardInput, acknowledgeInputHandoff } from '../terminal.mjs';
const wasm = await readFile(new URL('../../docs/silkie/_static/demo/silkie-terminal.wasm', import.meta.url));
const enc = new TextEncoder();
const row = (frame, y) => frame[6][y].map(cell => cell[0]).join('');

test('real WASM handles fragmented UTF8, shell controls, alternate screens, and resize', async () => {
    const core = await createCore(wasm); core.init(20, 4);
    try {
        for (const byte of enc.encode('é界e\u0301')) core.write(Uint8Array.of(byte));
        assert.equal(row(core.snapshot(), 0).trim(), 'é界e\u0301');
        core.write('\r\n\x1b[32mgreen\x1b[0m');
        assert.equal(core.snapshot()[6][1][0][1], 0xa6e22e);
        core.write('\x1b[?1049hgame'); assert.match(row(core.snapshot(), 0), /^game/);
        core.resize(25, 6); core.write('\x1b[?1049l');
        assert.equal(core.snapshot()[0], 25); assert.match(row(core.snapshot(), 0), /^é界/);
        core.cellSize(10, 20);
        const replies = core.write('\x1b[16t\x1b[14t\x1b[?1016$p\x1b[c');
        assert.equal(replies[0][1], '\x1b[6;20;10t');
        assert.equal(replies[1][1], '\x1b[4;120;250t');
        assert.equal(replies[2][1], '\x1b[?1016;2$y');
        assert.equal(replies.at(-1)[1], '\x1b[?62;4;22c');
    } finally { core.dispose(); }
});

test('escape payloads and scrollback grow beyond old-sized buffers', async () => {
    const core = await createCore(wasm); core.init(20, 4);
    try {
        const payload = 'A'.repeat(2 * 1024 * 1024 + 1);
        assert.equal(core.write('\x1b_G' + payload).length, 0);
        const events = core.write('\x1b\\');
        assert.equal(events[0][1].length, payload.length + 1);
        const text = 'line\r\n'.repeat(20000);
        core.write(text);
        assert.equal(core.snapshot()[7], 19997);
        assert.match(row(core.snapshot(19997), 0), /^line/);
        assert.match(row(core.snapshot(), 0), /^line/);
    } finally { core.dispose(); }
});

test('screen scopes precede graphics resets and delimit queued protocol work', async () => {
    const core = await createCore(wasm); core.init(20, 4);
    const events = bytes => core.write(bytes).map(event => event.slice(0, 2));
    try {
        for (const mode of [1047, 1049]) {
            assert.deepEqual(events(`\x1b_Gbefore\x1b\\\x1b[?${mode}h\x1b]777;display;query\x07`), [
                ['apc', 'Gbefore'], ['screen-scope', 'alternate'], ['reset-graphics', ''], ['osc', '777;display;query'],
            ]);
            assert.deepEqual(events(`\x1b[?${mode}h`), [], 'setting the active screen again preserves its scope');
            assert.deepEqual(events(`\x1b_Gold\x1b\\\x1b[?${mode}l\x1b]777;display;query\x07`), [
                ['apc', 'Gold'], ['screen-scope', 'primary'], ['reset-graphics', ''], ['osc', '777;display;query'],
            ]);
            assert.deepEqual(events(`\x1b[?${mode}l`), [], 'resetting an inactive alternate mode preserves the primary scope');
        }
        assert.deepEqual(events('\x1b_Gold\x1b\\\x1bc'), [
            ['apc', 'Gold'], ['keyboard-scope', ''], ['screen-scope', 'primary'], ['reset-graphics', ''], ['reset-input', ''],
        ], 'RIS starts a new scope even on the primary screen');
        events('\x1b[?1049h');
        assert.deepEqual(events('\x1bc'), [['keyboard-scope', ''], ['screen-scope', 'primary'], ['reset-graphics', ''], ['reset-input', '']], 'RIS leaves the alternate screen');
    } finally { core.dispose(); }
});

test('keyboard encodes control keys, non-BMP input, mouse-era keys, and kitty release events', () => {
    const event = key => ({ key, shiftKey: false, altKey: false, ctrlKey: false, metaKey: false });
    assert.equal(encodeKey({ ...event('c'), ctrlKey: true }), '\x03');
    assert.equal(encodeKey(event('🐔')), '🐔');
    assert.equal(encodeKey(event('ArrowUp'), 1), '\x1bOA');
    assert.equal(encodeKey(event('ArrowUp'), 0, 11), '\x1b[1;1:1A');
    assert.equal(encodeKey(event('ArrowUp'), 0, 11, true), '\x1b[1;1:3A');
    assert.equal(encodeKey(event(' '), 0, 11), '\x1b[32;1:1u');
    assert.equal(encodeKey(event('Delete'), 0, 11), '\x1b[3;1:1~');
});

test('bitmap negotiation rejects unsupported formats and media instead of hanging', async () => {
    const replies = [];
    const gfx = new KittyGraphics({ reply: reply => replies.push(reply) });
    await gfx.command('Ga=q,i=8,f=999,s=1,v=1;cCBpZD0x', 0, 0);
    assert.match(replies.pop(), /i=8;ENOTSUP/);
    await gfx.command('Ga=q,i=9,t=t,f=24,s=1,v=1;AAAA', 0, 0);
    assert.match(replies.pop(), /i=9;ENOTSUP/);
    await gfx.command('Ga=q,i=10,G=3,f=24,s=1,v=1;AAAA', 0, 0);
    assert.match(replies.pop(), /i=10;EINVAL/);
});

test('inline RGB chunks, placements, updates, and deletion preserve protocol state', async () => {
    const original = globalThis.ImageData;
    globalThis.ImageData = class { constructor(data, width, height) { Object.assign(this, { data, width, height }); } };
    const replies = []; const received = [];
    const gfx = new KittyGraphics({ reply: r => replies.push(r), makeCanvas: () => ({ getContext: () => ({ putImageData: data => received.push(data) }) }) });
    try {
        // Chunk boundaries are protocol boundaries; the aggregate can grow freely.
        const rgb = Buffer.alloc(3 * 3000, 0x77).toString('base64');
        await gfx.command(`Ga=t,i=12,f=24,s=3000,v=1,m=1;${rgb.slice(0, 4096)}`, 2, 3);
        assert.equal(gfx.images.size, 0);
        await gfx.command(`Gm=0;${rgb.slice(4096)}`, 0, 0);
        assert.equal(gfx.images.get(12).width, 3000);
        assert.equal(received[0].data.length, 12000);
        assert.equal(received[0].data[3], 255);
        await gfx.command('Ga=p,i=12,p=7,c=8,r=2,z=-1,C=1,q=2;', 2, 3);
        assert.equal(gfx.placements.get('12:7').xCell, 2);
        gfx.scroll(1); assert.equal(gfx.placements.get('12:7').yCell, 2);
        await gfx.command('Ga=d,d=i,i=12,p=7,q=2;', 0, 0);
        assert.equal(gfx.placements.size, 0); assert.equal(gfx.images.size, 1);
        await gfx.command('Ga=d,d=I,i=12,q=2;', 0, 0);
        assert.equal(gfx.images.size, 0);
    } finally { globalThis.ImageData = original; }
});

test('kitty keyboard stack restores the shell and only grants implemented flags', async () => {
    const core = await createCore(wasm); core.init(20, 4);
    try {
        core.write('\x1b[>11u'); assert.equal(core.kittyFlags(), 11);
        core.write('\x1b[>31u'); assert.equal(core.kittyFlags(), 11);
        core.write('\x1b[<u'); assert.equal(core.kittyFlags(), 11);
        const key = { key: 'q', shiftKey: false, altKey: false, ctrlKey: false, metaKey: false };
        assert.equal(encodeKey(key, core.modes(), core.kittyFlags(), true), '\x1b[113;1:3u');
        core.write('\x1b[<u'); assert.equal(core.kittyFlags(), 0);
        core.write('\x1b[=1u\x1b[=2;2u'); assert.equal(core.kittyFlags(), 3);
        core.write('\x1b[=1;3u'); assert.equal(core.kittyFlags(), 2);
        core.resetKeyboard(); assert.equal(core.kittyFlags(), 0);
        core.write('\x1b[<u'); assert.equal(core.kittyFlags(), 0);
    } finally { core.dispose(); }
});

test('zlib payloads decode using browser DecompressionStream and reject size mismatch', async () => {
    const { deflateSync } = await import('node:zlib');
    const original = globalThis.ImageData;
    globalThis.ImageData = class { constructor(data, width, height) { Object.assign(this, { data, width, height }); } };
    const replies = [], pixels = [];
    const gfx = new KittyGraphics({ reply: r => replies.push(r), makeCanvas: () => ({ getContext: () => ({ putImageData: data => pixels.push(data) }) }) });
    try {
        const compressed = deflateSync(Uint8Array.of(255, 0, 0, 128)).toString('base64');
        await gfx.command(`Ga=t,i=17,f=32,s=1,v=1,o=z;${compressed}`, 0, 0);
        assert.match(replies.pop(), /i=17;OK/);
        assert.deepEqual([...pixels[0].data], [255, 0, 0, 128]);
        await gfx.command('Ga=q,i=18,f=32,s=2,v=2;AAAA', 0, 0);
        assert.match(replies.pop(), /i=18;EINVAL/);
    } finally { globalThis.ImageData = original; }
});

test('kitty z threshold separates explicit cell backgrounds from images and text', () => {
    const gfx = new KittyGraphics({ reply: () => {} });
    for (const [id, z] of [[1, -2000000000], [2, -1073741824], [3, -1], [4, 0]]) {
        gfx.images.set(id, { bitmap: { id }, width: 1, height: 1 });
        gfx.place({ p: '1', z: String(z), C: '1' }, id, 0, 0);
    }
    const draws = [];
    const context = { drawImage: bitmap => draws.push(bitmap.id) };
    gfx.draw(context, 'behind-background', 10, 20, 1, 0); assert.deepEqual(draws, [1]);
    gfx.draw(context, 'below-text', 10, 20, 1, 0); assert.deepEqual(draws, [1, 2, 3]);
    gfx.draw(context, 'above-text', 10, 20, 1, 0); assert.deepEqual(draws, [1, 2, 3, 4]);
});

test('explicit background remains distinct from default even when colors match', async () => {
    const core = await createCore(wasm); core.init(20, 4);
    try {
        core.write('a\x1b[48;2;17;19;15mb\x1b[49mc\x1b[40md\x1b[0me');
        const cells = core.snapshot()[6][0];
        assert.equal(cells[0][3] & 32, 0);
        assert.equal(cells[1][3] & 32, 32);
        assert.equal(cells[0][2], cells[1][2]);
        assert.equal(cells[2][3] & 32, 0);
        assert.equal(cells[3][3] & 32, 32);
        assert.equal(cells[4][3] & 32, 0);
    } finally { core.dispose(); }
});

test('WASM32 rejects overflowing CSI and SGR fields without treating them as defaults', async () => {
    const core = await createCore(wasm); core.init(20, 4);
    try {
        core.write('\x1b[2;4H\x1b[32;48;2;10;20;30mX');
        const before = core.snapshot();
        core.write('\x1b[4294967296H');
        assert.deepEqual(core.snapshot().slice(2, 4), before.slice(2, 4));
        core.write('\x1b[1;4294967296H');
        assert.deepEqual(core.snapshot().slice(2, 4), before.slice(2, 4));
        core.write(`\x1b[${'9'.repeat(100000)}J`);
        assert.deepEqual(core.snapshot(), before);
        core.write('\x1b[0;4294967296mY');
        const a = core.snapshot()[6][1][3], b = core.snapshot()[6][1][4];
        assert.deepEqual(a.slice(1), b.slice(1), 'overflow must not apply the preceding SGR reset');
        core.write('\x1b[38:2::4294967296:0:0mZ');
        assert.deepEqual(core.snapshot()[6][1][5].slice(1), a.slice(1));
        // Legitimate default fields, colon RGB, and zero-padded numbers remain valid.
        core.write('\x1b[;H\x1b[38:2::1:2:3mC');
        assert.equal(core.snapshot()[6][0][0][1], 0x010203);
        core.write(`\x1b[${'0'.repeat(100000)}2;2HD`);
        assert.equal(core.snapshot()[6][1][1][0], 'D');
        core.cellSize(0xffffffff, 0xffffffff);
        assert.deepEqual(core.write('\x1b[14t'), [], 'pixel dimension overflow must not wrap');
    } finally { core.dispose(); }
});

test('DEC newline mode changes ground-state LF only and resets to normal VT semantics', async () => {
    const core = await createCore(wasm); core.init(20, 8);
    try {
        core.write('abc\nX');
        assert.equal(core.snapshot()[6][1][3][0], 'X', 'LF normally preserves the column');
        core.write('\x1b[20hY\nZ');
        assert.equal(core.snapshot()[6][2][0][0], 'Z', 'LNM makes LF start the next line');
        core.write('\x1b[20l\nQ');
        assert.equal(core.snapshot()[6][3][1][0], 'Q', 'resetting LNM restores column preservation');
        core.write('\x1b[20h');
        const position = core.snapshot().slice(2, 4);
        const events = core.write('\x1b_Ga=t;A\nB\x1b\\\x1b]52;c;A\nB\x1b\\');
        assert.equal(events[0][1], 'Ga=t;A\nB');
        assert.equal(events[1][1], '52;c;A\nB');
        assert.deepEqual(core.snapshot().slice(2, 4), position, 'string payload LF never affects the cursor');
        core.write('\x1bcab\nX');
        assert.equal(core.snapshot()[6][1][2][0], 'X', 'RIS resets LNM to its VT default');
    } finally { core.dispose(); }
});

test('CheerpX newline default is opt-in, survives RIS, and leaves string payload bytes intact', async () => {
    const core = await createCore(wasm); core.init(20, 4, { convertEol: true });
    try {
        core.write('abc\nX');
        assert.equal(core.snapshot()[6][1][0][0], 'X');
        core.write('\x1b[20lY\nZ');
        assert.equal(core.snapshot()[6][2][2][0], 'Z', 'explicit LNM reset still works');
        core.write('\x1bcabc\nX');
        assert.equal(core.snapshot()[6][1][0][0], 'X', 'RIS restores the configured platform default');
        const position = core.snapshot().slice(2, 4);
        const events = core.write('\x1b_Ga=t;A\nB\x1b\\\x1b]52;c;A\nB\x1b\\');
        assert.equal(events[0][1], 'Ga=t;A\nB');
        assert.equal(events[1][1], '52;c;A\nB');
        assert.deepEqual(core.snapshot().slice(2, 4), position);
    } finally { core.dispose(); }
});

test('held keys deliver press/repeat/release only to their owning keyboard scope', () => {
    const keyboard = new KeyboardInput();
    const key = {key: 'w', code: 'KeyW', shiftKey: false, altKey: false, ctrlKey: false, metaKey: false};
    assert.equal(keyboard.encode(key, 0, 11), '\x1b[119;1:1u');
    assert.equal(keyboard.encode({...key, repeat: true}, 0, 11), '\x1b[119;1:2u');
    assert.equal(keyboard.encode(key, 0, 11, true), '\x1b[119;1:3u');
    assert.equal(keyboard.encode(key, 0, 11, true), null, 'an unmatched keyup is never sent');
    keyboard.encode(key, 0, 11);
    keyboard.changeScope();
    assert.equal(keyboard.encode({...key, repeat: true}, 0, 11), null, 'old held-key repeat cannot become a new press');
    assert.equal(keyboard.encode(key, 0, 11, true), null, 'identical flags still belong to different applications');
    assert.equal(keyboard.encode(key, 0, 0), 'w');
    keyboard.changeScope();
    assert.equal(keyboard.encode(key, 0, 11, true), null, 'a legacy press cannot release into a newly started game');
    for (let n = 0; n < 20000; n++) {
        assert.equal(keyboard.encode(key, 0, 11), '\x1b[119;1:1u');
        assert.equal(keyboard.encode(key, 0, 11, true), '\x1b[119;1:3u');
    }
    assert.equal(keyboard.pressed.size, 0);
});

test('focus loss releases current held keys once and retires keys from old applications', () => {
    const keyboard = new KeyboardInput(), sent = [];
    const key = (name, code) => ({key: name, code, shiftKey: false, altKey: false, ctrlKey: false, metaKey: false});
    keyboard.encode(key('q', 'KeyQ'), 0, 11);
    keyboard.changeScope();
    keyboard.encode(key('ArrowUp', 'ArrowUp'), 0, 11);
    keyboard.encode(key(' ', 'Space'), 0, 11);
    keyboard.releaseAll(0, 11, value => sent.push(value));
    assert.deepEqual(sent, ['\x1b[1;1:3A', '\x1b[32;1:3u']);
    keyboard.releaseAll(0, 11, value => sent.push(value));
    assert.equal(sent.length, 2);
    assert.equal(keyboard.encode(key('ArrowUp', 'ArrowUp'), 0, 11, true), null);
});

test('guest handoff is a synchronous nonce boundary and resets leaked keyboard state', async () => {
    const core = await createCore(wasm); core.init(20, 4);
    const keyboard = new KeyboardInput(), sent = [];
    const options = {core, keyboard, send: value => {
        assert.equal(core.kittyFlags(), 0, 'the shell mode must apply before its ACK enters the input FIFO');
        sent.push(value);
    }};
    try {
        core.write('\x1b[>11u\x1b[>11u');
        const old = keyboard.scope;
        for (const invalid of ['777;browser-input;v=1:op=handoff:id=0', '777;browser-input;v=1:op=handoff:id=2147483648',
            '777;browser-input;v=2:op=handoff:id=1', '777;browser-input;v=1:op=ack:id=1',
            '777;browser-input;v=1:op=handoff:id=7:extra=1']) {
            assert.equal(acknowledgeInputHandoff(invalid, options), false);
            assert.equal(core.kittyFlags(), 11);
            assert.equal(keyboard.scope, old);
        }
        const payload = '777;browser-input;v=1:op=handoff:id=2147483647';
        let events = [];
        for (const byte of enc.encode('\x1b]' + payload + '\x1b\\')) events.push(...core.write(Uint8Array.of(byte)));
        assert.equal(events.length, 1);
        assert.equal(acknowledgeInputHandoff(events[0][1], options), true);
        assert.deepEqual(sent, ['\x1b]777;browser-input;v=1:op=ack:id=2147483647\x1b\\']);
        assert.notEqual(keyboard.scope, old);
        core.write('\x1b[<u'); assert.equal(core.kittyFlags(), 0, 'no abandoned keyboard stack survives the handoff');
    } finally { core.dispose(); }
});

test('retiring a guest cannot resurrect images or replies from an unfinished decode', async () => {
    const previous = globalThis.createImageBitmap;
    let finish, closed = 0;
    globalThis.createImageBitmap = () => new Promise(resolve => { finish = resolve; });
    const replies = [], gfx = new KittyGraphics({reply: reply => replies.push(reply)});
    try {
        const command = gfx.command('Ga=T,i=1,f=100,C=1;cG5n', 0, 0);
        gfx.dispose();
        finish({width: 10, height: 10, close: () => closed++});
        await command;
        assert.equal(closed, 1);
        assert.equal(gfx.images.size, 0);
        assert.equal(gfx.placements.size, 0);
        assert.deepEqual(replies, []);
    } finally { globalThis.createImageBitmap = previous; gfx.dispose(); }
});
