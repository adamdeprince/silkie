// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {VectorScene, parseVectorPath, parseVectorColor, multiplyMatrices} from '../vector.mjs';

const dimensions = {width: 200, height: 100};
const square = 'p id=1 d=M0,0L200,0L200,100L0,100Z fill=#ffffff\np id=2 d=M20,20L80,20L80,80L20,80Z fill=#ff0000\n';

function canvasFactory(log = []) {
    return () => {
        const canvas = {width: 200, height: 100};
        let matrix = [1, 0, 0, 1, 0, 0];
        const stack = [];
        const ctx = {canvas, globalAlpha: 1, globalCompositeOperation: 'source-over',
            save() { stack.push({matrix: [...matrix], alpha: this.globalAlpha, blend: this.globalCompositeOperation}); log.push(['save']); },
            restore() { const saved = stack.pop(); assert(saved, 'unbalanced canvas restore'); matrix = saved.matrix; this.globalAlpha = saved.alpha; this.globalCompositeOperation = saved.blend; log.push(['restore']); },
            transform(...value) { matrix = multiplyMatrices(matrix, value); log.push(['transform', ...value]); },
            setTransform(...value) { const m = value[0]; matrix = value.length === 1 ? [m.a, m.b, m.c, m.d, m.e, m.f] : value; },
            scale(x, y) { matrix = multiplyMatrices(matrix, [x, 0, 0, y, 0, 0]); },
            translate(x, y) { matrix = multiplyMatrices(matrix, [1, 0, 0, 1, x, y]); },
            rotate(angle) { const c = Math.cos(angle), s = Math.sin(angle); matrix = multiplyMatrices(matrix, [c, s, -s, c, 0, 0]); },
            getTransform() { const [a, b, c, d, e, f] = matrix; return {a, b, c, d, e, f}; },
            getContextAttributes() { return {colorSpace: 'display-p3', colorType: 'float16'}; },
            beginPath() { log.push(['beginPath']); },
            moveTo(...v) { log.push(['moveTo', ...v]); }, lineTo(...v) { log.push(['lineTo', ...v]); },
            quadraticCurveTo(...v) { log.push(['quadraticCurveTo', ...v]); }, bezierCurveTo(...v) { log.push(['bezierCurveTo', ...v]); },
            closePath() { log.push(['closePath']); }, arc(...v) { log.push(['arc', ...v]); },
            rect(...v) { log.push(['rect', ...v]); }, clip(rule) { log.push(['clip', rule]); },
            fill(rule) { log.push(['fill', this.fillStyle, rule]); }, stroke() { log.push(['stroke', this.strokeStyle, this.lineWidth]); },
            setLineDash(v) { log.push(['dash', ...v]); },
            fillRect(...v) { log.push(['fillRect', ...v]); },
            drawImage(...v) { log.push(['drawImage', v[0], this.globalAlpha, ...v.slice(1)]); },
            createLinearGradient(...v) { log.push(['linear', ...v]); return {addColorStop(offset, paint) { log.push(['stop', offset, paint]); }}; },
            createRadialGradient(...v) { log.push(['radial', ...v]); return {addColorStop(offset, paint) { log.push(['stop', offset, paint]); }}; },
            measureText(text) { return {width: [...text].length * 8, actualBoundingBoxAscent: 10, actualBoundingBoxDescent: 3}; },
            fillText(...v) { log.push(['fillText', ...v]); }, strokeText(...v) { log.push(['strokeText', ...v]); },
        };
        canvas.getContext = (kind, options) => { log.push(['context', kind, options]); return ctx; };
        canvas.context = ctx;
        canvas.stack = stack;
        return canvas;
    };
}

test('native game record examples retain scene order and atomic patches', () => {
    const scene = VectorScene.parse(square, dimensions);
    assert.deepEqual([...scene.elements.keys()], [0, 1, 2]);
    scene.patch('p id=1 d=M0,0L1,0L1,1Z fill=#000000');
    assert.deepEqual([...scene.elements.keys()], [0, 1, 2]);
    const old = scene.elements, version = scene.version;
    assert.throws(() => scene.patch('x id=1\np id=3 d=MNaN,0'), /Line 2/);
    assert.equal(scene.elements, old);
    assert.equal(scene.version, version);
    assert(scene.elements.has(1));
    scene.patch('g id=0 t=2,0,0,2,-100,-50');
    assert.deepEqual(scene.groupTransform(0), [2, 0, 0, 2, -100, -50]);
});

test('SVG path subset supports implicit, relative, and reflected curves and rejects arcs', () => {
    const path = parseVectorPath('M10,10h5v5l-5,0zM0,0c1,2,3,4,5,6s2,3,4,5q1,2,3,4t5,6');
    assert.deepEqual(path.slice(0, 5), [['M', 10, 10], ['L', 15, 10], ['L', 15, 15], ['L', 10, 15], ['Z']]);
    assert.deepEqual(path[7], ['C', 7, 8, 7, 9, 9, 11]);
    assert.deepEqual(path.at(-1), ['Q', 14, 17, 17, 21]);
    assert.deepEqual(parseVectorPath('M0,0,1,2,3,4'), [['M', 0, 0], ['L', 1, 2], ['L', 3, 4]]);
    for (const text of ['M0,0A2,2,0,0,0,4,4', 'M1,2L3', 'M1e999,0', 'M0,0z1,2']) assert.throws(() => parseVectorPath(text));
});

test('native game symbols, gradients, groups, and pen records render with retained updates', () => {
    const source = [
        'l id=9 x1=0 y1=0 x2=100 y2=0 stops=0,#000000,1,#ffffff',
        'g id=10 p=0 t=1,0,0,1,10,20 box=0,0,80,80',
        's id=20 d=M0,0C0,10,10,10,10,0Z',
        'u id=21 g=10 s=20 t=2,0,0,2,0,0 fill=@9',
        'k id=22 g=10 pts=0,0,1,10,10,3 fill=#ff880080',
    ].join('\n');
    const scene = VectorScene.parse(source, dimensions);
    scene.patch('a id=22 pts=20,10,4');
    assert.deepEqual(scene.elements.get(22).points, [0, 0, 1, 10, 10, 3, 20, 10, 4]);
    assert.deepEqual(scene.groupTransform(10), [1, 0, 0, 1, 10, 20]);
    const log = [], makeCanvas = canvasFactory(log);
    const canvas = makeCanvas();
    scene.draw(canvas.context, {makeCanvas});
    assert(log.some(item => item[0] === 'bezierCurveTo'));
    assert(log.some(item => item[0] === 'arc'));
    assert(log.some(item => item[0] === 'clip'));
    assert(log.filter(item => item[0] === 'stop').length > 2, 'gradient should interpolate in linear light at output resolution');
    assert.equal(canvas.stack.length, 0);
    scene.patch('x g=10');
    for (const id of [10, 21, 22]) assert(!scene.elements.has(id));
    assert(scene.elements.has(20), 'resource symbols are independent from group children');
});

test('opacity is applied to isolated groups and layer color precision is retained', () => {
    const scene = VectorScene.parse('g id=1 p=0 o=50\np id=2 g=1 d=M0,0L20,0L20,20Z fill=#ff0000\np id=3 g=1 d=M0,0L10,0L10,10Z fill=#0000ff', dimensions);
    const log = [], makeCanvas = canvasFactory(log), canvas = makeCanvas();
    scene.draw(canvas.context, {makeCanvas});
    const composites = log.filter(item => item[0] === 'drawImage');
    assert.equal(composites.length, 1);
    assert.equal(composites[0][2], .5);
    assert(log.some(item => item[0] === 'context' && item[2]?.colorType === 'float16'));
    assert.equal(canvas.stack.length, 0);
});

test('group cycles and numeric overflows cannot partially mutate a retained scene', () => {
    const scene = VectorScene.parse(square, dimensions), old = scene.elements;
    for (const patch of [
        'g id=8 p=9\ng id=9 p=8',
        'g id=5 t=1e308,0,0,1e308,0,0\ng id=6 p=5 t=2,0,0,2,0,0',
        'p id=5 d=M1e308,0 t=2,0,0,2,0,0',
        'x all id=4294967296',
        'l id=10 stops=0,#000000,1,p3:2/0/0',
    ]) {
        assert.throws(() => scene.patch(patch));
        assert.equal(scene.elements, old);
    }
    // A patch may temporarily introduce a cycle and then remove that subtree;
    // the deletion walk must terminate before validating the final scene.
    scene.patch('g id=8 p=9\ng id=9 p=8\nx g=8');
    assert(!scene.elements.has(8) && !scene.elements.has(9));
});

test('retained geometry and nesting grow beyond previous fixed buffer sizes', () => {
    const count = 10000;
    const source = Array.from({length: count}, (_, i) => `g id=${i + 1} p=${i} t=1,0,0,1,1,0`).join('\n') + `\np id=${count + 1} g=${count} d=M0,0L1,1 fill=#000000`;
    const scene = VectorScene.parse(source, dimensions);
    assert.equal(scene.groupTransform(count)[4], count);
    const canvas = canvasFactory()();
    scene.draw(canvas.context);
    assert.equal(canvas.stack.length, 0);
    const path = 'L1,2'.repeat(20000);
    scene.patch(`p id=20000 d=M0,0${path}`);
    assert.equal(scene.elements.get(20000).path.length, 20001);
    scene.patch('x g=1');
    assert(!scene.elements.has(count + 1));
    assert(scene.elements.has(20000));
});

test('UTF-8 text, raster references, clips, and unsupported extension records coexist', () => {
    const layout = {font: 'serif', size: 20, x: 10, y: 30, angle: -12, runs: [
        {text: 'Window ', fill: '#dddddd'},
        {text: 'café 中文 🐔', weight: 700, italic: true, underline: 'double', fill: 'p3:1/0.2/0*250'},
    ]};
    const encoded = Buffer.from(JSON.stringify(layout)).toString('base64');
    const scene = VectorScene.parse(`z future=ignored\ns id=10 d=M0,0L100,0L100,100Z\ng id=20 clip=10\nt id=30 g=20 text=${encoded}\ni id=40 g=20 img=99 o=80`, dimensions);
    const log = [], canvas = canvasFactory(log)();
    scene.draw(canvas.context, {rasterImages: new Map([[99, {bitmap: {name: 'sprite'}}]])});
    assert(log.some(item => item[0] === 'fillText' && item[1] === 'café 中文 🐔'));
    assert(log.some(item => item[0] === 'drawImage' && item[1].name === 'sprite'));
    assert.throws(() => scene.patch('t id=31 text=AA=='), /UTF-8 JSON/);
});

test('wide-gamut and HDR vector color values are retained without premature clipping', () => {
    assert(parseVectorColor('#ff0000*250').linear[0] > 2);
    assert(parseVectorColor('p3:1/0/0').linear[0] > 1);
    assert(parseVectorColor('p3:1/0/0').linear[1] < 0);
    assert.equal(parseVectorColor('linear:-1/2/3/0.5').alpha, .5);
    for (const color of ['#123', '#zzzzzz', 'srgb:2/0/0', 'linear:1/2/3/2', '#ffffff*1e999', 'linear:1e308/0/0*1e308']) assert.throws(() => parseVectorColor(color));
});
