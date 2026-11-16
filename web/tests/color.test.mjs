import assert from 'node:assert/strict';
import {test} from 'node:test';
import {readFile} from 'node:fs/promises';
import {createCore} from '../terminal.mjs';
import {cellPaint, createColorContext, decodeGraphicsPixels, halfToFloat, parseVectorColor, parseVectorPaint, respondColorOSC, toLinearSRGB} from '../color.mjs';

const wasm = await readFile(new URL('../../docs/silkie/_static/demo/silkie-terminal.wasm', import.meta.url));

test('real WASM retains native alpha, gamut and brightness in cells and scrollback', async () => {
    const core = await createCore(wasm); core.init(20, 3);
    try {
        core.write('\x1b[38:2:1:255:0:0m\x1b[48:2:2:0:255:0m\x1b[56:238m\x1b[57:99999m\x1b[70:79mA');
        const cell = core.snapshot()[6][0][0];
        assert.deepEqual(cell.slice(4, 9), [250, 1675, 1, 2, 78]);
        core.write('\x1b[31mB\x1b[49mC\x1b[0mD');
        assert.equal(core.snapshot()[6][0][1][6], 0, 'palette selects sRGB');
        assert.equal(core.snapshot()[6][0][2][7], 0, 'default background selects sRGB');
        assert.equal(core.snapshot()[6][0][2][8], 78, 'background alpha survives color changes');
        assert.deepEqual(core.snapshot()[6][0][3].slice(4, 9), [100, 100, 0, 0, 100]);
        core.write('\r\n\r\n\r\n');
        assert.deepEqual(core.snapshot(1)[6][0][0].slice(4, 9), [250, 1675, 1, 2, 78]);
    } finally {core.dispose();}
});

test('colon RGB color-space fields do not become unrelated SGR parameters', async () => {
    const core = await createCore(wasm); core.init(12, 2);
    try {
        core.write('\x1b[38:2::12:34:56mA\x1b[38:2:77:80:90:100mB\x1b[70;31mC');
        const cells = core.snapshot()[6][0];
        assert.equal(cells[0][1], 0x0c2238);
        assert.equal(cells[1][1], 0x505a64);
        assert.equal(cells[1][6], 0, 'unknown color space is sRGB');
        assert.equal(cells[2][8], 100, 'semicolon alpha resets rather than consumes 31');
        assert.equal(cells[2][1], 0xe06c75);
    } finally {core.dispose();}
});

test('OSC 8 hyperlink metadata survives scrollback and is cleared by erase', async () => {
    const core = await createCore(wasm); core.init(12, 2);
    try {
        core.write('\x1b]8;id=login;silkie:tailscale\x1b\\Login\x1b]8;;\x1b\\ plain');
        assert.equal(core.snapshot()[6][0][0][9], 'silkie:tailscale');
        assert.equal(core.snapshot()[6][0][6][9], null);
        core.write('\r\n\r\n');
        assert.equal(core.snapshot(1)[6][0][0][9], 'silkie:tailscale');
        core.write('\x1b[1;1H\x1b[2K');
        assert.equal(core.snapshot()[6][0][0][9], null);
        core.write('\x1b]8;;https://example.test/é\x1b\\X');
        assert.equal(core.snapshot()[6][0][0][9], 'https://example.test/é');
    } finally {core.dispose();}
});

test('cell paints keep foreground opaque, apply alpha to background, and ignore it under reverse', () => {
    const cell = ['A', 0xff0000, 0x0000ff, 0, 200, 100, 0, 0, 30];
    assert.match(cellPaint(cell, false), /\/ 0.3\)/);
    assert.match(cellPaint(cell, true), /\/ 1\)/);
    assert.equal(cellPaint(cell, true), cellPaint({...cell, 4: 100}, true), 'SDR brightness clamps to headroom 1');
    assert.match(cellPaint(cell, true, {hdr: true}), /srgb-linear 2 0 0/);
    assert.match(cellPaint({...cell, 3: 8}, false), /\/ 1\)/);
    assert.match(cellPaint(cell, false, {}, {opaque: true}), /\/ 1\)/);
});

test('vector colors preserve extended channels and linear-light brightness', () => {
    assert.deepEqual(parseVectorColor('linear:2/-0.2/0.5/0.4'), [2, -.2, .5, .4]);
    assert.deepEqual(parseVectorColor('#ff000080*250'), [2.5, 0, 0, 128 / 255]);
    const p3 = toLinearSRGB([1, 0, 0], 1);
    assert(p3[0] > 1 && p3[1] < 0, 'P3 red must retain out-of-sRGB values');
    const wideRed = parseVectorPaint('p3:1/0/0', {colorSpace: 'display-p3', hdr: false});
    const components = wideRed.match(/display-p3 ([^/]+)/)[1].trim().split(' ').map(Number);
    assert(components[0] > .999 && components[1] < .0001 && components[2] < .0001, 'P3 output must retain its primary rather than clip in sRGB');
    assert.match(parseVectorPaint('linear:2/0/0', {hdr: true}), /srgb-linear 2 0 0/);
    assert.match(parseVectorPaint('linear:2/0/0', {hdr: false}), /srgb-linear 1 0 0/);
    assert.throws(() => parseVectorColor('p3:2/0/0'), /range/);
    assert.throws(() => parseVectorColor('linear:Infinity/0/0'), /components/);
    assert.throws(() => parseVectorColor('linear:/0/0'), /components/);
    assert.throws(() => parseVectorColor('linear:0x10/0/0'), /components/);
    assert.throws(() => parseVectorColor('#ffffff*'), /brightness/);
    assert.throws(() => parseVectorColor('linear:1e308/0/0*1e308'), /overflow/);
});

test('half float decoder handles finite values, subnormals and nonfinite encodings', () => {
    assert.equal(halfToFloat(0x3c00), 1);
    assert.equal(halfToFloat(0x4000), 2);
    assert.equal(halfToFloat(1), 2 ** -24);
    assert.equal(halfToFloat(0xbc00), -1);
    assert.equal(halfToFloat(0x7c00), Infinity);
    assert(Number.isNaN(halfToFloat(0x7e00)));
});

test('graphics decode converts tagged gamut and clamps SDR HDR pixels without losing alpha', () => {
    const saved = globalThis.ImageData;
    let captured;
    globalThis.ImageData = class {constructor(data, width, height, settings) {Object.assign(this, {data, width, height, settings});}};
    const makeCanvas = () => ({getContext: () => ({putImageData: image => {captured = image;}})});
    try {
        const hdr = Uint8Array.of(0, 0x40, 0, 0x3c, 0, 0, 0, 0x38); // linear [2,1,0,.5]
        decodeGraphicsPixels({f: 64, s: 1, v: 1}, hdr, {makeCanvas});
        assert.deepEqual([...captured.data], [255, 255, 0, 128]);
        const malformed = hdr.slice(); malformed[1] = 0x7c;
        assert.throws(() => decodeGraphicsPixels({f: 64, s: 1, v: 1}, malformed, {makeCanvas}), /non-finite/);
        assert.throws(() => decodeGraphicsPixels({f: 32, s: 1.5, v: 2}, new Uint8Array(12), {makeCanvas}), /dimensions/);
        decodeGraphicsPixels({f: 24, G: 1, s: 1, v: 1}, Uint8Array.of(255, 0, 0), {makeCanvas});
        assert.deepEqual([...captured.data], [255, 0, 0, 255]);
    } finally {globalThis.ImageData = saved;}
});

test('requested P3/float16 attributes do not falsely advertise browser support', async () => {
    const makeCanvas = () => ({getContext: () => ({getContextAttributes: () => ({colorSpace: 'srgb', colorType: 'unorm8'})})});
    const visible = makeCanvas();
    const result = await createColorContext(visible, {makeCanvas, environment: {matchMedia: () => ({matches: true}), CSS: {supports: () => true}}});
    assert.equal(result.capabilities.hdr, false);
    assert.equal(result.capabilities.float16, false);
    assert.equal(result.capabilities.gamut, 'srgb');
    assert.equal(result.capabilities.headroom, null);
    assert.equal(result.capabilities.wideGamut, true, 'tagged-color conversion is still supported in SDR');
});

test('byte atlases preserve pixels and carry their source gamut into Canvas conversion', () => {
    const saved = globalThis.ImageData, captured = [];
    globalThis.ImageData = class {constructor(data, width, height, settings) {captured.push({data, width, height, settings});}};
    const contexts = [];
    const makeCanvas = () => ({getContext(type, settings) {contexts.push(settings); return {putImageData() {}};}});
    try {
        const bytes = Uint8Array.of(19, 255, 64, 0, 128, 7).subarray(1, 5);
        for (const gamut of [0, 1]) {
            decodeGraphicsPixels({f:32, G:gamut, s:1, v:1}, bytes, {makeCanvas, capabilities:{colorSpace:'display-p3'}});
            assert.deepEqual([...captured.at(-1).data], [255, 64, 0, 128]);
            assert.equal(captured.at(-1).settings.colorSpace, gamut ? 'display-p3' : 'srgb');
            assert.equal(contexts.at(-1).colorSpace, 'display-p3');
        }
        decodeGraphicsPixels({f:24, s:2, v:1}, Uint8Array.of(1,2,3,4,5,6), {makeCanvas});
        assert.deepEqual([...captured.at(-1).data], [1,2,3,255,4,5,6,255]);
    } finally {globalThis.ImageData = saved;}
});

test('display query reports actual gamut/scale and unknown physical fields without guesses', () => {
    const replies = [];
    const environment = {devicePixelRatio: 2, matchMedia: query => ({matches: query === '(color-gamut: p3)'})};
    const options = {environment, send: value => replies.push(value)};
    assert(respondColorOSC('777;display;v=1:op=query:id=42', {colorSpace: 'display-p3', headroom: null}, options));
    assert.match(replies[0], /id=42:mode=0:kind=unknown:color=rgb:bits=0:gamut=p3:headroom=0:scale=2:period=0:ppi=0/);
    respondColorOSC('777;display;v=1:op=mode:mode=1', {}, options);
    assert.match(replies.at(-1), /op=mode:mode=0/);
    const count = replies.length;
    for (const frame of ['v=1:op=query:id=1:id=2', 'v=2:op=query:id=1', 'v=1:op=query:id=-1', 'v=1:op=query:id=2147483648', 'v=1:op=mode:mode=2']) {
        respondColorOSC(`777;display;${frame}`, {}, options);
    }
    assert.equal(replies.length, count, 'invalid requests must be rejected as a whole');
    assert.equal(respondColorOSC('777;haptic;v=1:op=query:id=1', {}, options), false);
});
