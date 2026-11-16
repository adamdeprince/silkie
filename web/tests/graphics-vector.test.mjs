// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {deflateSync} from 'node:zlib';
import {KittyGraphics} from '../terminal.mjs';
import {VectorScene} from '../vector.mjs';
import {InkController} from '../ink.mjs';

const encode = text => Buffer.from(text).toString('base64');
const sceneText = 'g id=10 t=2,0,0,2,10,20\np id=11 g=10 d=M0,0L20,0L20,20Z fill=#ff0000';
function fixture(options = {}) {
    const replies = [];
    const gfx = new KittyGraphics({reply: reply => replies.push(reply), ...options});
    const command = (header, data = '', x = 0, y = 0) => gfx.command(`G${header};${encode(data)}`, x, y);
    return {gfx, replies, command};
}

test('format 200 negotiation parses real vector data without creating an image or placement', async () => {
    const {gfx, replies, command} = fixture({makeCanvas() { throw new Error('query must not rasterize'); }});
    await command('a=q,i=51,I=4,p=7,f=200,s=200,v=100', sceneText);
    assert.equal(replies.pop(), '\x1b_Gi=51,I=4,p=7;OK\x1b\\');
    assert.equal(gfx.images.size, 0);
    assert.equal(gfx.placements.size, 0);
    await command('a=q,i=52,f=200,s=200,v=100', 'p id=1 d=M0,NaN');
    assert.match(replies.pop(), /i=52;EINVAL:/);
    await command('a=q,i=53,f=200,s=0,v=100', sceneText);
    assert.match(replies.pop(), /i=53;EINVAL:/);
    await command('a=q,i=54,f=200,s=200,v=100,q=1', sceneText);
    assert.equal(replies.length, 0, 'quiet success is silent');
    await command('a=q,i=54,f=200,s=200,v=100,q=1', 'p id=1 d=MNaN,0');
    assert.match(replies.pop(), /i=54;EINVAL:/, 'quiet=1 still reports an invalid scene');
});

test('compressed chunked vector upload and edit preserve the first chunk cursor and complete scene', async () => {
    const {gfx, replies} = fixture();
    const count = 5000;
    const source = Array.from({length: count}, (_, index) => `p id=${index + 1} d=M${index},0L${index},1 fill=#123456`).join('\n');
    const data = deflateSync(source).toString('base64');
    assert(data.length > 4096);
    await gfx.command(`Ga=T,i=7,p=2,f=200,s=6000,v=100,o=z,C=1,c=80,r=20,m=1;${data.slice(0, 4096)}`, 3, 4);
    assert.equal(gfx.images.size, 0);
    assert.equal(replies.length, 0);
    for (let offset = 4096; offset < data.length; offset += 4096) {
        const final = offset + 4096 >= data.length;
        await gfx.command(`Gm=${final ? 0 : 1};${data.slice(offset, offset + 4096)}`, 99, 99);
    }
    assert.match(replies.pop(), /i=7,p=2;OK/);
    const scene = gfx.images.get(7).vector;
    assert(scene instanceof VectorScene);
    assert.equal(scene.elements.size, count + 1);
    assert.deepEqual(scene.elements.get(count).path, [['M', count - 1, 0], ['L', count - 1, 1]]);
    assert.equal(gfx.placements.get('7:2').xCell, 3);
    assert.equal(gfx.placements.get('7:2').yCell, 4);
    const patch = encode('p id=6001 d=M1,2L3,4 fill=#abcdef');
    await gfx.command(`Ga=e,i=7,m=1;${patch.slice(0, 11)}`, 0, 0);
    await gfx.command(`Gm=0;${patch.slice(11)}`, 0, 0);
    assert.match(replies.pop(), /i=7;OK/);
    assert.equal(gfx.images.get(7).vector, scene);
    assert(scene.elements.has(6001));
    assert.equal(gfx.placements.size, 1);
});

test('retained edits are atomic, retain placements, and vector resources survive repeated placement deletion', async () => {
    const {gfx, replies, command} = fixture();
    await command('a=T,i=7,p=2,f=200,s=200,v=100,C=1,c=20,r=5', sceneText, 2, 3);
    const image = gfx.images.get(7), scene = image.vector, placement = gfx.placements.get('7:2');
    assert.equal(image.bitmap, null);
    const version = scene.version, elements = scene.elements;
    await command('a=e,i=7', 'x id=11\np id=12 d=MNaN,0');
    assert.match(replies.pop(), /i=7;EINVAL:/);
    assert.equal(scene.version, version);
    assert.equal(scene.elements, elements);
    await command('a=e,i=7', 'p id=11 g=10 d=M1,2L3,4 fill=#0000ff');
    assert.match(replies.pop(), /i=7;OK/);
    assert.equal(gfx.images.get(7), image);
    assert.equal(gfx.placements.get('7:2'), placement);
    assert(scene.version > version);
    for (let i = 0; i < 300; i++) {
        await command('a=d,d=i,i=7,p=2,q=2');
        assert.equal(gfx.placements.size, 0);
        assert.equal(gfx.images.get(7), image);
        await command('a=p,i=7,p=2,C=1,q=2', '', i, i + 1);
    }
    await command('a=d,d=I,i=7,q=2');
    assert.equal(gfx.images.size, 0, 'deleting a vector with bitmap=null must complete');
    assert.equal(gfx.placements.size, 0);
    await command('a=e,i=7', 'x id=11');
    assert.match(replies.pop(), /i=7;ENOENT:/);
    await command('a=t,i=8,f=200,s=200,v=100', sceneText);
    await command('a=t,i=9,f=200,s=200,v=100', sceneText);
    await command('a=d,d=A,q=2');
    assert.equal(gfx.images.size, 0, 'delete-all releases every vector resource');
});

// A drawing recorder verifies protocol coordinates and scene invalidation;
// browser pixel tests cover Canvas rasterization itself.
function recordingCanvases() {
    const canvases = [];
    const makeCanvas = () => {
        const canvas = {width: 0, height: 0};
        const operations = [];
        const ctx = {canvas, globalAlpha: 1, globalCompositeOperation: 'source-over'};
        for (const name of ['save', 'restore', 'scale', 'transform', 'beginPath', 'moveTo', 'lineTo', 'closePath', 'fill', 'arc', 'clip', 'rect', 'drawImage']) {
            ctx[name] = (...args) => operations.push([name, ...args]);
        }
        canvas.getContext = (type, options) => { canvas.options = options; return ctx; };
        canvas.operations = operations;
        canvases.push(canvas);
        return canvas;
    };
    return {canvases, makeCanvas};
}

test('vector placements draw at physical resolution and retained edits invalidate their raster cache', async () => {
    const {canvases, makeCanvas} = recordingCanvases();
    const {gfx, command} = fixture({makeCanvas, colors: {colorSpace: 'display-p3', hdr: true}});
    await command('a=T,i=7,p=2,f=200,s=200,v=100,C=1,c=10,r=5,x=50,y=25,w=100,h=50,X=4,Y=6', sceneText, 2, 3);
    const draws = [], context = {drawImage: (...args) => draws.push(args)};
    gfx.draw(context, 'above-text', 8, 16, 2, 0);
    assert.equal(canvases.length, 1);
    assert.deepEqual([canvases[0].width, canvases[0].height], [320, 320]);
    assert.deepEqual(canvases[0].options, {colorSpace: 'display-p3', colorType: 'float16'});
    assert.deepEqual(draws[0].slice(1), [80, 80, 160, 160, 18, 51, 80, 80]);
    gfx.draw(context, 'above-text', 8, 16, 2, 0);
    assert.equal(canvases.length, 1, 'unchanged draw reuses the scene raster');
    await command('a=e,i=7', 'p id=11 g=10 d=M0,0L10,0L10,10Z fill=#00ff00');
    gfx.draw(context, 'above-text', 8, 16, 2, 0);
    assert.equal(canvases.length, 2);
    assert.notEqual(draws[2][0], draws[0][0]);
    assert(canvases[1].operations.some(op => op[0] === 'fill'));
});

test('scenery cache survives placement churn and unrelated uploads but follows referenced raster lifetimes', async () => {
    const {canvases, makeCanvas} = recordingCanvases();
    const {gfx, command} = fixture({makeCanvas});
    await command('a=T,i=7,f=200,s=200,v=100,C=1', sceneText + '\ni id=12 img=42');
    const context = {drawImage() {}};
    const draw = () => gfx.draw(context, 'above-text', 8, 16, 1, 0);
    draw();
    assert.equal(canvases.length, 1);
    for (let i = 0; i < 300; i++) {
        await command('a=d,d=i,i=7,q=2');
        await command('a=p,i=7,C=1,q=2', '', i, 0);
        await command('a=t,i=8,f=200,s=200,v=100,q=2', sceneText);
        draw();
    }
    assert.equal(canvases.length, 1, 'placement deletion and unrelated image replacement reuse scenery');
    gfx.images.set(42, {bitmap: {}});
    draw();
    assert.equal(canvases.length, 2, 'a previously missing referenced raster appeared');
    gfx.images.set(42, {bitmap: {}});
    draw();
    assert.equal(canvases.length, 3, 'referenced raster replacement invalidates scenery');
    await command('a=d,d=I,i=42,q=2');
    draw();
    assert.equal(canvases.length, 4, 'referenced raster removal invalidates scenery');
    await command('a=e,i=7', 'x id=12');
    draw();
    assert.equal(canvases.length, 5);
    gfx.images.set(42, {bitmap: {}});
    draw();
    assert.equal(canvases.length, 5, 'removed references no longer invalidate scenery');
});

test('real graphics placements and transformed scene groups receive retained ink, cancellation, and application patches', async () => {
    const {gfx, command} = fixture();
    await command('a=T,i=7,p=2,f=200,s=200,v=100,C=1,c=10,r=5,x=50,y=25,w=100,h=50,X=4,Y=6', sceneText, 2, 3);
    const scene = gfx.images.get(7).vector;
    let changes = 0;
    const ink = new InkController({placementsForImage: id => gfx.placementsForImage(id, 8, 16, 2), changed: () => { changes++; }});
    assert(ink.command('777;ink;v=1:op=brush:i=7:g=10:k=100:n=10:fill=#1a1a80:w0=1:w1=5'));
    const report = {kind: 1, phase: 1, device: 1, contact: 9, tool: 1, valid: 1, p: 65535, x: 116 * 256, y: 182 * 256};
    // Physical placement is (36,102) size160x160. Its cropped scene point
    // (100,50), inverted through group scale2+translate(10,20), is (45,15).
    ink.sample(report);
    assert.equal(report.ink, 100);
    assert.deepEqual(scene.elements.get(100).points, [45, 15, 5]);
    await command('a=p,i=7,p=2,C=1,c=10,r=5,x=50,y=25,w=100,h=50,X=4,Y=6,q=2', '', 3, 3);
    ink.sample({...report, phase: 2, p: 0});
    assert.deepEqual(scene.elements.get(100).points, [45, 15, 5, 40, 15, 1], 'moving the placement updates inverse ink coordinates');
    ink.sample({...report, phase: 3, p: 65535});
    assert.equal(ink.active.size, 0);
    assert.equal(scene.elements.get(100).points.length, 9);
    ink.sample({...report, phase: 1});
    assert(scene.elements.has(101));
    ink.sample({...report, phase: 4});
    assert(!scene.elements.has(101), 'cancel removes the actual retained stroke');
    assert(scene.elements.has(100), 'completed strokes stay in the application scene');
    ink.sample({...report, phase: 1});
    assert(scene.elements.has(102));
    await command('a=e,i=7', 'x id=102');
    ink.sample({...report, phase: 2});
    assert.equal(ink.active.size, 0, 'application edits can delete an active ink stroke');
    assert(!scene.elements.has(102));
    assert.equal(changes, 6);
});
