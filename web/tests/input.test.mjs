// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import test from 'node:test';
import {BrowserInput, BrowserHaptics, parseOSC, integer} from '../input.mjs';
import {InkController} from '../ink.mjs';

const decode = text => parseOSC(text).fields;
const request = (selector, fields) => `777;${selector};v=1:${fields}`;
function pointer(overrides = {}) {
    return {type: 'pointerdown', pointerType: 'pen', pointerId: 7, clientX: 60, clientY: 120,
        timeStamp: 408211.25, pressure: .2, tiltX: -12, tiltY: 6, twist: 90,
        tangentialPressure: 0, button: 0, buttons: 1, preventDefault() {}, ...overrides};
}
function fixture() {
    const reports = [], events = [];
    const doc = {visibilityState: 'visible', hasFocus: () => true, addEventListener() {}, removeEventListener() {}};
    const canvas = {width: 400, height: 300, ownerDocument: doc,
        getBoundingClientRect: () => ({left: 10, top: 20, width: 200, height: 150}),
        setPointerCapture: id => events.push(id)};
    const environment = {PointerEvent: class {}, document: doc, navigator: {}, addEventListener() {}, removeEventListener() {}};
    const geometry = () => ({width: 400, height: 300, cellWidth: 10, cellHeight: 20});
    const input = new BrowserInput(canvas, {send: frame => reports.push(frame), environment, geometry, now: () => 500000});
    return {input, canvas, environment, reports, events};
}
function fakeScene(transform = [1, 0, 0, 1, 0, 0]) {
    const records = [];
    return {width: 400, height: 300, records, groupTransform: () => transform,
        patch: record => records.push(record)};
}

test('OSC parsing rejects duplicate fields/version/overflow and has no arbitrary field limit', () => {
    assert.equal(parseOSC('777;ink;v=1:op=mode:mode=0:mode=1'), null);
    assert.equal(parseOSC('777;ink;v=2:op=query:id=1'), null);
    assert.equal(integer('1'.repeat(100000), 0, 0xffffffff), null);
    assert.equal(integer('0'.repeat(100000) + '7', 0, 10), 7);
    assert.equal(parseOSC(`777;ink;v=1:op=query:id=1:future=${'x'.repeat(100000)}`).fields.future.length, 100000);
});

test('tablet discovery grants only browser-supported modes and does not fabricate hardware identifiers', () => {
    const {input, reports} = fixture();
    input.command(request('tablet-pointer', 'op=query:id=42'));
    assert.equal(decode(reports.pop()).caps, '3');
    input.command(request('tablet-pointer', 'op=mode:mode=127'));
    input.command(request('tablet-pointer', 'op=mode'));
    assert.equal(decode(reports.pop()).mode, '3');
    assert.deepEqual(input.listing, ['tablet-pointer.1']);
    assert.equal(input.capabilities.tabletHardwareIds, false);
    input.command(request('tablet-pointer', 'op=mode:mode=4294967296'));
    assert.equal(input.mode, 3);
});

test('a browser without Pointer Events cannot advertise pen input or terminal-side ink', () => {
    const {canvas, environment, reports} = fixture(); delete environment.PointerEvent;
    const input = new BrowserInput(canvas, {environment, send: frame => reports.push(frame), ink: new InkController()});
    input.command(request('tablet-pointer', 'op=query:id=1'));
    input.command(request('ink', 'op=query:id=2'));
    assert.equal(decode(reports[0]).caps, '0'); assert.equal(decode(reports[1]).caps, '0');
    assert.deepEqual(input.listing, []);
});

test('real pointer measurements become exact native pen fields in physical pixels', () => {
    const {input, reports, events} = fixture();
    input.command(request('tablet-pointer', 'op=mode:mode=1'));
    assert.equal(input.handle(pointer()), true);
    const report = decode(reports[0]);
    assert.equal(report.x, '25600'); assert.equal(report.y, '51200');
    assert.equal(report.p, '13107'); assert.equal(report.tx, '-1200'); assert.equal(report.ty, '600');
    assert.equal(report.twist, '9000'); assert.equal(report.valid, '7');
    assert.equal(report.t, '408211'); assert.equal(report.tu, '250');
    assert.equal(report.device, '0'); assert.equal(report.contact, '7');
    assert.equal(report.serial, undefined); assert.equal(report.hwid, undefined);
    assert.deepEqual(events, [7]);
    input.handle(pointer({type: 'pointerup', pressure: 0, buttons: 0, button: 0}));
    const up = decode(reports.at(-1));
    assert.equal(up.phase, '3'); assert.equal(up.p, '0'); assert.equal(up.buttons, '0');
});

test('default browser pressure/tilt values stay unknown until measured, then zero remains a measurement', () => {
    const {input, reports} = fixture();
    input.command(request('tablet-pointer', 'op=mode:mode=1'));
    input.handle(pointer({pressure: .5, tiltX: 0, tiltY: 0, twist: 0}));
    assert.equal(decode(reports.at(-1)).valid, '0');
    assert.equal(decode(reports.at(-1)).p, undefined);
    input.handle(pointer({type: 'pointermove', pressure: .4, tangentialPressure: -.2}));
    assert.equal(decode(reports.at(-1)).valid, '15');
    input.handle(pointer({type: 'pointermove', pressure: 0, tiltX: 0, tiltY: 0, twist: 0, tangentialPressure: 0}));
    const zero = decode(reports.at(-1));
    assert.equal(zero.valid, '15'); assert.equal(zero.p, '0'); assert.equal(zero.tx, '0'); assert.equal(zero.tangent, '0');
});

test('eraser maps to the eraser tool without leaving its tip pressed after up', () => {
    const {input, reports} = fixture(); input.command(request('tablet-pointer', 'op=mode:mode=1'));
    input.handle(pointer({button: 5, buttons: 32}));
    assert.equal(decode(reports.at(-1)).tool, '2'); assert.equal(decode(reports.at(-1)).buttons, '1');
    input.handle(pointer({type: 'pointerup', button: 5, buttons: 0}));
    assert.equal(decode(reports.at(-1)).tool, '2'); assert.equal(decode(reports.at(-1)).buttons, '0');
});

test('coalesced samples retain order and all contacts are cancelled on mode changes', () => {
    const {input, reports} = fixture(); input.command(request('tablet-pointer', 'op=mode:mode=3'));
    for (let id = 1; id <= 300; id++) input.handle(pointer({pointerId: id, pointerType: 'touch'}));
    assert.equal(input.contacts.size, 300);
    assert.equal(reports.length, 600); // Each touch delivery ends with its frame.
    const begin = reports.length;
    input.handle(pointer({pointerId: 301}));
    input.handle(pointer({type: 'pointermove', pointerId: 301,
        getCoalescedEvents: () => [pointer({pointerId: 301, clientX: 61, timeStamp: 408212}), pointer({pointerId: 301, clientX: 62, timeStamp: 408213})]}));
    assert.deepEqual(reports.slice(begin).map(text => decode(text).t), ['408211', '408212', '408213']);
    input.command(request('tablet-pointer', 'op=mode:mode=0'));
    assert.equal(input.contacts.size, 0);
    assert.equal(reports.map(decode).filter(f => f.phase === '4').length, 301);
    assert.equal(input.handle(pointer()), false);
});

test('native haptic targets remain unsupported despite an optional device vibration backend', () => {
    const replies = [], vibrations = []; let now = 0;
    const haptic = new BrowserHaptics({send: frame => replies.push(frame), now: () => now,
        environment: {navigator: {userActivation: {hasBeenActive: true}, vibrate: pattern => { vibrations.push(pattern); return true; }}}});
    haptic.command(request('haptic', 'op=query:id=3'));
    assert.equal(decode(replies[0]).caps, '0');
    haptic.command(request('haptic', 'op=play:pattern=generic'));
    haptic.command(request('haptic', 'op=start:target=pen:pattern=pencil'));
    assert.equal(vibrations.length, 0);
    assert.equal(haptic.playVibration('alignment'), true);
    assert.equal(haptic.playVibration('alignment'), false);
    now = 50; assert.equal(haptic.playVibration('complete'), true);
    haptic.stop(); assert.equal(vibrations.at(-1), 0);
});

test('ink draws the exact retained k/a records before forwarding pen reports', () => {
    const {input, reports} = fixture(), scene = fakeScene(); let changes = 0;
    const placement = {id: '7:1', x: 30, y: 40, width: 400, height: 300, z: -1, order: 1, scene};
    const ink = new InkController({placementsForImage: () => [placement], changed: () => { changes++; }});
    input.ink = ink;
    ink.command(request('ink', 'op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5'));
    input.command(request('tablet-pointer', 'op=mode:mode=1'));
    input.handle(pointer());
    assert.equal(decode(reports[0]).ink, '1000');
    assert.equal(scene.records[0], 'k id=1000 g=0 pts=70,160,1.8 fill=#1a1a80');
    input.handle(pointer({type: 'pointermove', clientX: 62.5, clientY: 121, pressure: .6}));
    assert.equal(scene.records[1], 'a id=1000 pts=75,162,3.4');
    input.handle(pointer({type: 'pointerup', clientX: 63, clientY: 121, pressure: 0, buttons: 0}));
    assert.equal(scene.records[2], 'a id=1000 pts=76,162,1');
    assert.equal(ink.active.size, 0); assert.equal(changes, 3);
});

test('ink tracks moved placements and transforms, cancellation deletes, exhausted ids fall back', () => {
    const scene = fakeScene([2, 0, 0, 2, 10, 20]);
    const placement = {id: 'p', x: 30, y: 40, width: 400, height: 300, scene};
    const ink = new InkController({placementsForImage: () => [placement]});
    ink.command(request('ink', 'op=brush:i=7:g=8:k=4294967295:n=1:w1=5'));
    const report = {kind: 1, phase: 1, device: 0, contact: 0, tool: 1, valid: 0, x: 25600, y: 51200};
    ink.sample(report);
    assert.equal(report.ink, 4294967295);
    assert.equal(scene.records[0], 'k id=4294967295 g=8 pts=30,70,5 fill=#000000');
    placement.x = 40;
    ink.sample({...report, phase: 2});
    assert.equal(scene.records[1], 'a id=4294967295 pts=25,70,5');
    ink.sample({...report, phase: 4});
    assert.equal(scene.records[2], 'x id=4294967295');
    const exhausted = {...report, ink: undefined}; ink.sample(exhausted);
    assert.equal(exhausted.ink, undefined);
    const before = ink.brush;
    ink.command(request('ink', 'op=brush:i=7:k=4294967295:n=2:w1=5'));
    assert.equal(ink.brush, before);
});

test('simultaneous ink tools grow dynamically; disabling leaves completed paths intact', () => {
    const scene = fakeScene(), placement = {id: 'p', x: 0, y: 0, width: 400, height: 300, scene};
    const ink = new InkController({placementsForImage: () => [placement]});
    ink.command(request('ink', 'op=brush:i=7:k=100:n=1000:w1=5'));
    for (let id = 0; id < 300; id++) ink.sample({kind: 1, phase: 1, device: 0, contact: id, tool: 1, valid: 0, x: 2560, y: 2560});
    assert.equal(ink.active.size, 300); assert.equal(scene.records.length, 300);
    ink.command(request('ink', 'op=mode:mode=0'));
    assert.equal(ink.active.size, 0);
    assert.equal(scene.records.length, 300);
    ink.reset(); assert.equal(ink.brush, null);
});

test('ink accepts finite widths and pressure exponents beyond former arbitrary caps atomically', () => {
    const scene = fakeScene(), placement = {id: 'p', x: 0, y: 0, width: 400, height: 300, scene};
    const ink = new InkController({placementsForImage: () => [placement]});
    ink.command(request('ink', 'op=brush:i=7:k=100:n=2:w0=1e299:w1=1e300:e=1e200'));
    assert.equal(ink.mode, 1);
    assert.equal(ink.brush.w0, 1e299); assert.equal(ink.brush.w1, 1e300); assert.equal(ink.brush.e, 1e200);
    const zero = ink.point(placement, ink.brush, {x: 2560, y: 2560, valid: 1, p: 0});
    const full = ink.point(placement, ink.brush, {x: 2560, y: 2560, valid: 1, p: 65535});
    assert.equal(zero[2], 1e299); assert.equal(full[2], 1e300);
    const accepted = ink.brush;
    ink.command(request('ink', 'op=mode:mode=0'));
    for (const invalid of ['w0=1e309:w1=2', 'w0=1:w1=1e309', 'w0=1:w1=2:e=1e309',
        'w0=1:w1=NaN', 'w0=1:w1=Infinity', 'w0=-1:w1=2', 'w0=1:w1=2:e=0', 'w0=1:w1=2:e=-1']) {
        ink.command(request('ink', `op=brush:i=8:k=200:${invalid}`));
        assert.equal(ink.brush, accepted, invalid);
        assert.equal(ink.mode, 0, invalid);
    }
});
