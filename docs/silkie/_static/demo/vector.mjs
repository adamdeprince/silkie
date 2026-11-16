// SPDX-License-Identifier: GPL-3.0-only
// Retained f=200 scenes. Geometry is kept in user units and rasterized at each
// placement's resolution. Parsing and edits are independent of the DOM.
import {parseVectorColor as parseLinearColor} from './color.mjs';
const utf8 = new TextDecoder('utf-8', {fatal: true});
const numberPattern = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/;
const numberPrefix = /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?/;
const identity = Object.freeze([1, 0, 0, 1, 0, 0]);
const drawable = new Set(['g', 'p', 'u', 'k', 'i', 't']);
const types = new Set(['g', 'p', 's', 'u', 'k', 'i', 't', 'l', 'r', 'a', 'x']);
const finite = (value, label = 'number') => {
    if (typeof value !== 'string' || !numberPattern.test(value)) throw new Error(`Bad ${label}`);
    const result = Number(value);
    if (!Number.isFinite(result)) throw new Error(`Non-finite ${label}`);
    return result;
};
const uint = (value, label = 'id') => {
    const result = finite(value, label);
    // IDs are uint32 in the wire format; this is not a storage/count limit.
    if (!Number.isInteger(result) || result < 0 || result > 0xffffffff) throw new Error(`Bad ${label}`);
    return result;
};
const numbers = (text, count, label = 'number list') => {
    const result = text === '' ? [] : text.split(',').map(item => finite(item, label));
    if (count !== undefined && result.length !== count) throw new Error(`Bad ${label}`);
    return result;
};
const choice = (value, choices, label) => {
    if (!choices.includes(value)) throw new Error(`Bad ${label}`);
    return value;
};
const positiveDimensions = (width, height) => {
    if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0) throw new Error('Bad vector image dimensions');
};

export function multiplyMatrices(a, b) {
    const result = [
        a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],
        a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
        a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5],
    ];
    if (!result.every(Number.isFinite)) throw new Error('Transform arithmetic overflow');
    return result;
}

// Canonical M/L/Q/C/Z commands, with absolute coordinates. The SVG commands
// supported by Silkie are parsed explicitly so browser-only arcs cannot sneak
// into a scene that would fail in the desktop terminal.
export function parseVectorPath(text) {
    const commands = [];
    let at = 0, command = null, previous = null;
    let x = 0, y = 0, startX = 0, startY = 0, controlX = 0, controlY = 0;
    const skip = () => { while (text[at] === ',') at++; };
    const take = count => {
        const values = [];
        while (values.length < count) {
            skip();
            const match = numberPrefix.exec(text.slice(at));
            if (!match) throw new Error(`Bad ${command} path coordinates`);
            values.push(finite(match[0], 'path coordinate'));
            at += match[0].length;
        }
        return values;
    };
    while (at < text.length) {
        skip();
        if (at === text.length) break;
        if (/[a-z]/i.test(text[at])) command = text[at++];
        else if (!command) throw new Error('Path coordinates without a command');
        const upper = command.toUpperCase(), relative = command !== upper;
        if (!'MLHVCSQTZ'.includes(upper)) throw new Error(`Unsupported path command ${command}`);
        if (upper === 'Z') {
            commands.push(['Z']); x = startX; y = startY; previous = 'Z'; command = null;
            continue;
        }
        const counts = {M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, Q: 4, T: 2};
        const values = take(counts[upper]);
        if (relative) {
            if (upper === 'H') values[0] += x;
            else if (upper === 'V') values[0] += y;
            else for (let i = 0; i < values.length; i += 2) { values[i] += x; values[i + 1] += y; }
        }
        if (!values.every(Number.isFinite)) throw new Error('Path coordinate arithmetic overflow');
        if (upper === 'M') {
            [x, y] = values; startX = x; startY = y;
            commands.push(['M', x, y]); command = relative ? 'l' : 'L';
        } else if (upper === 'L') { [x, y] = values; commands.push(['L', x, y]); }
        else if (upper === 'H') { x = values[0]; commands.push(['L', x, y]); }
        else if (upper === 'V') { y = values[0]; commands.push(['L', x, y]); }
        else if (upper === 'C') {
            commands.push(['C', ...values]); [controlX, controlY, x, y] = values.slice(2);
        } else if (upper === 'S') {
            const cx = previous === 'C' || previous === 'S' ? 2 * x - controlX : x;
            const cy = previous === 'C' || previous === 'S' ? 2 * y - controlY : y;
            commands.push(['C', cx, cy, ...values]); [controlX, controlY, x, y] = values;
        } else if (upper === 'Q') {
            commands.push(['Q', ...values]); [controlX, controlY, x, y] = values;
        } else if (upper === 'T') {
            const cx = previous === 'Q' || previous === 'T' ? 2 * x - controlX : x;
            const cy = previous === 'Q' || previous === 'T' ? 2 * y - controlY : y;
            [x, y] = values; controlX = cx; controlY = cy;
            commands.push(['Q', cx, cy, x, y]);
        }
        if (!commands.at(-1).slice(1).every(Number.isFinite)) throw new Error('Path coordinate arithmetic overflow');
        previous = upper;
    }
    return commands;
}

export function parseVectorColor(text) {
    if (text === 'none') return null;
    if (text.startsWith('@')) return {gradient: uint(text.slice(1), 'gradient reference')};
    const rgba = parseLinearColor(text);
    return {linear: rgba.slice(0, 3), alpha: rgba[3], source: text};
}

const cssLinear = color => `color(srgb-linear ${color.linear.join(' ')} / ${color.alpha})`;
export function vectorColorCSS(text) {
    const color = parseVectorColor(text);
    if (color?.gradient !== undefined) throw new Error('Gradient needs a scene');
    return color ? cssLinear(color) : null;
}

function layoutFromBase64(encoded) {
    let binary;
    try { binary = Uint8Array.from(atob(encoded), char => char.charCodeAt(0)); }
    catch { throw new Error('Bad base64 text layout'); }
    let layout;
    try { layout = JSON.parse(utf8.decode(binary)); }
    catch { throw new Error('Bad UTF-8 JSON text layout'); }
    if (!layout || typeof layout !== 'object' || Array.isArray(layout)) throw new Error('Text layout must be an object');
    const runs = layout.runs ?? [{text: layout.text ?? ''}];
    if (!Array.isArray(runs)) throw new Error('Text runs must be an array');
    for (const run of runs) {
        if (!run || typeof run !== 'object' || typeof run.text !== 'string') throw new Error('Bad text run');
        const style = {...layout, ...run};
        for (const key of ['size', 'weight', 'slant', 'stroke_width', 'x', 'y', 'angle']) {
            if (style[key] !== undefined && (typeof style[key] !== 'number' || !Number.isFinite(style[key]))) throw new Error(`Bad text ${key}`);
        }
        if (style.size !== undefined && style.size <= 0) throw new Error('Text size must be positive');
        for (const key of ['fill', 'stroke', 'background', 'underline_color']) if (style[key] !== undefined) parseVectorColor(style[key]);
        if (style.direction !== undefined && !['ltr', 'rtl', 'ttb', 'btt'].includes(style.direction)) throw new Error('Bad text direction');
    }
    return {...layout, runs};
}

function record(type, fields) {
    if (!fields.has('id')) throw new Error('Record without id');
    const id = uint(fields.get('id'));
    if (id === 0 && type !== 'g') throw new Error('Only the root group has id 0');
    const element = {type, id, parent: fields.has('g') ? uint(fields.get('g'), 'parent group') : 0, transform: identity};
    if (fields.has('t')) element.transform = numbers(fields.get('t'), 6, 'transform');
    if (type === 'g') {
        if (fields.has('p')) element.parent = uint(fields.get('p'), 'parent group');
        if (id === 0 && fields.has('p')) throw new Error('Root group cannot have a parent');
        element.opacity = fields.has('o') ? Math.min(100, Math.max(0, finite(fields.get('o'), 'opacity'))) / 100 : 1;
        element.visible = !fields.has('v') || finite(fields.get('v'), 'visibility') !== 0;
        element.blend = fields.has('blend') ? choice(fields.get('blend'), ['normal', 'multiply', 'screen'], 'blend') : 'normal';
        if (fields.has('clip')) element.clip = uint(fields.get('clip'), 'clip path');
        if (fields.has('box')) {
            element.box = numbers(fields.get('box'), 4, 'clipping box');
            if (element.box[2] < 0 || element.box[3] < 0 || !Number.isFinite(element.box[0] + element.box[2]) || !Number.isFinite(element.box[1] + element.box[3])) throw new Error('Bad clipping box');
        }
    } else if (type === 'p' || type === 's' || type === 'a') {
        if (fields.has('d')) element.path = parseVectorPath(fields.get('d'));
        if (type !== 'a' && !fields.has('d')) throw new Error('Path without d');
    }
    if (['p', 's', 'u', 'k'].includes(type)) {
        element.fill = fields.get('fill') ?? (fields.has('stroke') ? 'none' : '#000000');
        element.stroke = fields.get('stroke') ?? 'none';
        parseVectorColor(element.fill); parseVectorColor(element.stroke);
        element.width = fields.has('w') ? finite(fields.get('w'), 'stroke width') : 1;
        element.rule = fields.has('rule') ? choice(fields.get('rule'), ['nz', 'eo'], 'fill rule') : 'nz';
        element.cap = fields.has('cap') ? choice(fields.get('cap'), type === 'k' ? ['round', 'butt'] : ['butt', 'round', 'square'], 'line cap') : type === 'k' ? 'round' : 'butt';
        element.join = fields.has('join') ? choice(fields.get('join'), ['miter', 'round', 'bevel'], 'line join') : 'miter';
        element.miter = fields.has('ml') ? finite(fields.get('ml'), 'miter limit') : 4;
        element.dash = fields.has('dash') ? numbers(fields.get('dash'), undefined, 'dash pattern') : [];
        if (element.dash.some(n => n < 0)) throw new Error('Negative dash length');
        element.dashPhase = fields.has('dp') ? finite(fields.get('dp'), 'dash phase') : 0;
    }
    if (type === 'k' || (type === 'a' && fields.has('pts'))) {
        if (!fields.has('pts')) throw new Error('Pen without pts');
        element.points = numbers(fields.get('pts'), undefined, 'pen points');
        if (element.points.length % 3) throw new Error('Pen points must be x,y,width triples');
        if (element.points.some((n, i) => i % 3 === 2 && n < 0)) throw new Error('Negative pen width');
    }
    if (type === 'a' && !element.path && !element.points) throw new Error('Append without d or pts');
    if (type === 'u' || type === 'i') {
        const ref = fields.get(type === 'u' ? 's' : 'img');
        element.ref = uint(ref, type === 'u' ? 'symbol' : 'image');
        if (!element.ref) throw new Error('Reference cannot be zero');
        if (type === 'i') element.opacity = fields.has('o') ? Math.min(100, Math.max(0, finite(fields.get('o'), 'opacity'))) / 100 : 1;
    }
    if (type === 't') {
        if (!fields.has('text')) throw new Error('Text record without text');
        element.layout = layoutFromBase64(fields.get('text'));
    }
    if (type === 'l' || type === 'r') {
        const keys = type === 'l' ? ['x1', 'y1', 'x2', 'y2'] : ['cx', 'cy', 'r'];
        element.geometry = keys.map(key => fields.has(key) ? finite(fields.get(key), 'gradient coordinate') : 0);
        if (type === 'r') {
            if (element.geometry[2] < 0) throw new Error('Negative gradient radius');
            element.geometry.push(fields.has('fx') ? finite(fields.get('fx')) : element.geometry[0], fields.has('fy') ? finite(fields.get('fy')) : element.geometry[1]);
        }
        if (!fields.has('stops')) throw new Error('Gradient without stops');
        const parts = fields.get('stops').split(',');
        if (parts.length % 2 || !parts.length) throw new Error('Bad gradient stops');
        element.stops = [];
        for (let i = 0; i < parts.length; i += 2) {
            const offset = finite(parts[i], 'gradient offset');
            const color = parseVectorColor(parts[i + 1]);
            if (offset < 0 || offset > 1 || !color || color.gradient !== undefined) throw new Error('Bad gradient stop');
            element.stops.push({offset, paint: parts[i + 1], color});
        }
        element.stops.sort((a, b) => a.offset - b.offset);
    }
    return element;
}

function validateGroups(elements) {
    const complete = new Set();
    const world = new Map([[0, elements.get(0).transform]]);
    for (const element of elements.values()) {
        if (element.type !== 'g' || complete.has(element.id)) continue;
        const chain = new Set();
        let current = element;
        while (current?.type === 'g' && current.id !== 0 && !complete.has(current.id)) {
            if (chain.has(current.id)) throw new Error('Cycle in group parents');
            chain.add(current.id);
            current = elements.get(current.parent);
        }
        for (const id of chain) complete.add(id);
    }
    // Resolve transforms with an explicit growing stack. Overflow is rejected
    // while a patch is still provisional, not silently ignored by Canvas.
    for (const element of elements.values()) {
        if (element.type !== 'g' || world.has(element.id)) continue;
        const chain = [];
        let group = element;
        while (group?.type === 'g' && !world.has(group.id)) { chain.push(group); group = elements.get(group.parent); }
        if (!group || !world.has(group.id)) continue;
        let matrix = world.get(group.id);
        for (let i = chain.length - 1; i >= 0; i--) {
            matrix = multiplyMatrices(matrix, chain[i].transform);
            world.set(chain[i].id, matrix);
        }
    }
    for (const element of elements.values()) {
        if (element.type === 'g' || !world.has(element.parent)) continue;
        const matrix = multiplyMatrices(world.get(element.parent), element.transform);
        const geometry = element.type === 'u' ? elements.get(element.ref) : element;
        const pairs = [];
        for (const [, ...values] of geometry?.path ?? []) for (let i = 0; i < values.length; i += 2) pairs.push([values[i], values[i + 1]]);
        for (let i = 0; i < (geometry?.points?.length ?? 0); i += 3) pairs.push([geometry.points[i], geometry.points[i + 1]]);
        for (const [x, y] of pairs) {
            if (!Number.isFinite(matrix[0] * x + matrix[2] * y + matrix[4]) || !Number.isFinite(matrix[1] * x + matrix[3] * y + matrix[5])) throw new Error('Transformed geometry arithmetic overflow');
        }
    }
}

function deleteGroup(elements, id) {
    if (id === 0) { elements.clear(); return; }
    const children = new Map();
    for (const element of elements.values()) {
        if (element.id === 0 || !drawable.has(element.type)) continue;
        if (!children.has(element.parent)) children.set(element.parent, []);
        children.get(element.parent).push(element.id);
    }
    const stack = [id];
    const removed = new Set();
    while (stack.length) {
        const current = stack.pop();
        if (removed.has(current)) continue;
        removed.add(current);
        elements.delete(current);
        for (const child of children.get(current) ?? []) stack.push(child);
    }
}

function replayPath(ctx, commands) {
    for (const [op, ...values] of commands) {
        if (op === 'M') ctx.moveTo(...values);
        else if (op === 'L') ctx.lineTo(...values);
        else if (op === 'Q') ctx.quadraticCurveTo(...values);
        else if (op === 'C') ctx.bezierCurveTo(...values);
        else ctx.closePath();
    }
}

function defaultCanvas() {
    if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(1, 1);
    if (typeof document !== 'undefined') return document.createElement('canvas');
    throw new Error('A Canvas implementation is required to render vectors');
}

export class VectorScene {
    constructor({width, height, colorResolver = vectorColorCSS, makeCanvas = defaultCanvas} = {}) {
        positiveDimensions(width, height);
        Object.assign(this, {width, height, colorResolver, makeCanvas});
        this.elements = new Map([[0, record('g', new Map([['id', '0']]))]]);
        this.version = 0;
    }

    static parse(data, options) { return new VectorScene(options).patch(data); }

    patch(data) {
        const source = typeof data === 'string' ? data : utf8.decode(data);
        const elements = new Map(this.elements);
        const lines = source.split('\n');
        for (let index = 0; index < lines.length; index++) {
            const line = lines[index].trim();
            if (!line) continue;
            try {
                if (line.length > 1 && line[1] !== ' ') throw new Error('Record type must be a single character');
                const type = line[0];
                if (!types.has(type)) continue;
                const fields = new Map();
                let all = false;
                for (const token of line.slice(1).trim().split(/ +/).filter(Boolean)) {
                    const equals = token.indexOf('=');
                    if (equals < 0) {
                        if (type === 'x' && token === 'all') all = true;
                        else throw new Error('Field without =');
                    } else fields.set(token.slice(0, equals), token.slice(equals + 1));
                }
                if (type === 'x') {
                    if (fields.has('id')) uint(fields.get('id'));
                    if (fields.has('g')) uint(fields.get('g'));
                    if (all) elements.clear();
                    else if (fields.has('g')) deleteGroup(elements, uint(fields.get('g')));
                    else if (fields.has('id')) elements.delete(uint(fields.get('id')));
                    else throw new Error('Delete record without id, g or all');
                } else {
                    const element = record(type, fields);
                    if (type === 'a') {
                        const target = elements.get(element.id);
                        if (!target) throw new Error('Append target does not exist');
                        if (element.path && !['p', 's'].includes(target.type)) throw new Error('Append path target is not a path or symbol');
                        if (element.points && target.type !== 'k') throw new Error('Append pen target is not a pen');
                        elements.set(element.id, {...target,
                            ...(element.path ? {path: [...target.path, ...element.path]} : {}),
                            ...(element.points ? {points: [...target.points, ...element.points]} : {}),
                        });
                    } else elements.set(element.id, element);
                }
            } catch (error) { throw new Error(`Line ${index + 1}: ${error.message}`, {cause: error}); }
        }
        if (!elements.has(0)) elements.set(0, record('g', new Map([['id', '0']])));
        validateGroups(elements);
        this.elements = elements;
        this.version++;
        return this;
    }

    groupTransform(id) {
        const chain = [];
        const seen = new Set();
        let group = this.elements.get(id);
        while (group?.type === 'g') {
            if (seen.has(group.id)) return null;
            seen.add(group.id); chain.push(group.transform);
            if (group.id === 0) break;
            group = this.elements.get(group.parent);
        }
        if (group?.id !== 0) return null;
        let matrix = [...identity];
        for (let i = chain.length - 1; i >= 0; i--) matrix = multiplyMatrices(matrix, chain[i]);
        if (!Number.isFinite(matrix[0] * matrix[3] - matrix[1] * matrix[2]) || matrix[0] * matrix[3] === matrix[1] * matrix[2]) return null;
        return matrix;
    }

    paint(ctx, source, colorResolver = this.colorResolver) {
        if (source === 'none') return null;
        if (!source.startsWith('@')) return colorResolver(source);
        const resource = this.elements.get(uint(source.slice(1), 'gradient reference'));
        if (!resource || !['l', 'r'].includes(resource.type)) return null;
        const g = resource.geometry;
        const gradient = resource.type === 'l' ? ctx.createLinearGradient(...g) : ctx.createRadialGradient(g[3], g[4], 0, g[0], g[1], g[2]);
        // Canvas gradients normally interpolate encoded RGB. Sample linear-light
        // colors at output-pixel spacing; the number grows with real resolution,
        // and is bounded only by the visible canvas diagonal, not a fixed limit.
        const transform = ctx.getTransform();
        const scale = Math.max(Math.hypot(transform.a, transform.b), Math.hypot(transform.c, transform.d));
        const length = resource.type === 'l' ? Math.hypot(g[2] - g[0], g[3] - g[1]) : g[2];
        const pixels = Math.max(1, Math.ceil(Math.min(length * scale, Math.hypot(ctx.canvas.width, ctx.canvas.height))));
        const stops = resource.stops;
        gradient.addColorStop(stops[0].offset, colorResolver(stops[0].paint));
        for (let index = 1; index < stops.length; index++) {
            const a = stops[index - 1], b = stops[index];
            const steps = Math.max(1, Math.ceil((b.offset - a.offset) * pixels));
            for (let step = 1; step < steps; step++) {
                const t = step / steps;
                const linear = a.color.linear.map((value, i) => value + (b.color.linear[i] - value) * t);
                const alpha = a.color.alpha + (b.color.alpha - a.color.alpha) * t;
                gradient.addColorStop(a.offset + (b.offset - a.offset) * t, colorResolver(`linear:${linear.join('/')}/${alpha}`));
            }
            gradient.addColorStop(b.offset, colorResolver(b.paint));
        }
        return gradient;
    }

    drawPath(ctx, element, path, colorResolver) {
        ctx.beginPath(); replayPath(ctx, path);
        const fill = this.paint(ctx, element.fill, colorResolver);
        if (fill) { ctx.fillStyle = fill; ctx.fill(element.rule === 'eo' ? 'evenodd' : 'nonzero'); }
        const stroke = this.paint(ctx, element.stroke, colorResolver);
        if (stroke && element.width > 0) {
            ctx.strokeStyle = stroke; ctx.lineWidth = element.width;
            ctx.lineCap = element.cap; ctx.lineJoin = element.join;
            ctx.miterLimit = element.miter > 0 ? element.miter : 4;
            ctx.setLineDash(element.dash); ctx.lineDashOffset = element.dashPhase;
            ctx.stroke();
        }
    }

    drawPen(ctx, element, colorResolver) {
        const fill = this.paint(ctx, element.fill, colorResolver);
        if (!fill) return;
        const pts = element.points;
        ctx.beginPath();
        for (let i = 0; i + 5 < pts.length; i += 3) {
            const dx = pts[i + 3] - pts[i], dy = pts[i + 4] - pts[i + 1], length = Math.hypot(dx, dy);
            if (!length) continue;
            const nx = -dy / length, ny = dx / length, r0 = pts[i + 2] / 2, r1 = pts[i + 5] / 2;
            // Match the winding of the round joins below so their overlap is
            // a union under nonzero fill, rather than punched-out holes.
            ctx.moveTo(pts[i] - nx * r0, pts[i + 1] - ny * r0);
            ctx.lineTo(pts[i + 3] - nx * r1, pts[i + 4] - ny * r1);
            ctx.lineTo(pts[i + 3] + nx * r1, pts[i + 4] + ny * r1);
            ctx.lineTo(pts[i] + nx * r0, pts[i + 1] + ny * r0); ctx.closePath();
        }
        for (let i = 0; i < pts.length; i += 3) {
            if (element.cap === 'butt' && (i === 0 || i + 3 === pts.length)) continue;
            const radius = pts[i + 2] / 2;
            if (!radius) continue;
            ctx.moveTo(pts[i] + radius, pts[i + 1]); ctx.arc(pts[i], pts[i + 1], radius, 0, Math.PI * 2);
        }
        ctx.fillStyle = fill; ctx.fill('nonzero');
    }

    drawText(ctx, element, colorResolver) {
        const layout = element.layout;
        ctx.translate(layout.x ?? 0, layout.y ?? 0);
        if (layout.angle) ctx.rotate(layout.angle * Math.PI / 180);
        let x = 0, y = 0;
        for (const run of layout.runs) {
            const style = {...layout, ...run};
            const size = style.size ?? 16;
            const family = style.font?.startsWith('@') ? 'monospace' : style.font ?? ({sans: 'sans-serif', serif: 'serif', mono: 'monospace'}[style.role] ?? 'monospace');
            const selector = ['monospace', 'serif', 'sans-serif', 'system-ui'].includes(family) ? family : JSON.stringify(family);
            ctx.font = `${style.italic ? 'italic ' : ''}${style.weight ?? 400} ${size}px ${selector}`;
            ctx.textBaseline = 'alphabetic';
            ctx.direction = style.direction === 'rtl' ? 'rtl' : 'ltr';
            const pieces = run.text.split('\n');
            for (let index = 0; index < pieces.length; index++) {
                if (index) { x = 0; y += size * 1.2; }
                const text = pieces[index], metrics = ctx.measureText(text), width = metrics.width;
                const ascent = metrics.actualBoundingBoxAscent ?? size * 0.8, descent = metrics.actualBoundingBoxDescent ?? size * 0.2;
                const background = style.background === undefined ? null : this.paint(ctx, style.background, colorResolver);
                if (background) { ctx.fillStyle = background; ctx.fillRect(x, y - ascent, width, ascent + descent); }
                ctx.save(); ctx.translate(x, y);
                if (style.slant) ctx.transform(1, 0, -Math.tan(style.slant * Math.PI / 180), 1, 0, 0);
                const fill = this.paint(ctx, style.fill ?? '#000000', colorResolver);
                const stroke = this.paint(ctx, style.stroke ?? 'none', colorResolver);
                if (fill) { ctx.fillStyle = fill; ctx.fillText(text, 0, 0); }
                if (stroke && (style.stroke_width ?? 1) > 0) {
                    ctx.strokeStyle = stroke; ctx.lineWidth = style.stroke_width ?? 1; ctx.lineJoin = 'round'; ctx.strokeText(text, 0, 0);
                }
                const decoration = this.paint(ctx, style.underline_color ?? style.fill ?? '#000000', colorResolver);
                if (decoration) {
                    ctx.strokeStyle = decoration; ctx.lineWidth = Math.max(size / 16, Number.EPSILON);
                    const line = baseline => { ctx.beginPath(); ctx.moveTo(0, baseline); ctx.lineTo(width, baseline); ctx.stroke(); };
                    const underline = style.underline ?? 'none';
                    if (underline !== 'none') {
                        ctx.setLineDash(underline === 'dotted' ? [ctx.lineWidth, ctx.lineWidth] : underline === 'dashed' ? [size / 4, size / 8] : []);
                        if (underline === 'curly') {
                            ctx.beginPath(); ctx.moveTo(0, descent + size / 10);
                            const period = size / 3;
                            if (!period || !Number.isFinite(width / period)) throw new Error('Text decoration arithmetic overflow');
                            for (let at = 0; at < width;) {
                                const next = at + period;
                                if (next === at) throw new Error('Text decoration cannot advance at this numeric precision');
                                ctx.quadraticCurveTo(at + period / 4, descent, Math.min(width, at + period / 2), descent + size / 10);
                                at = next;
                            }
                            ctx.stroke();
                        } else { line(descent + size / 10); if (underline === 'double') line(descent + size / 5); }
                    }
                    ctx.setLineDash([]);
                    if (style.strike) line(-ascent / 3);
                    if (style.overline) line(-ascent);
                }
                ctx.restore();
                if (style.direction === 'ttb' || style.direction === 'btt') y += (style.direction === 'btt' ? -1 : 1) * size;
                else x += width;
            }
        }
    }

    draw(ctx, {colorResolver = this.colorResolver, rasterImages = new Map(), makeCanvas = this.makeCanvas, contextOptions = ctx.getContextAttributes?.()} = {}) {
        const children = new Map();
        for (const element of this.elements.values()) {
            if (element.id === 0 || !drawable.has(element.type)) continue;
            if (!children.has(element.parent)) children.set(element.parent, []);
            children.get(element.parent).push(element);
        }
        const stack = [{group: this.elements.get(0), parent: ctx, entered: false}];
        try {
            while (stack.length) {
                const frame = stack.at(-1), group = frame.group;
                if (!frame.entered) {
                    frame.entered = true;
                    if (!group.visible || group.opacity === 0) { stack.pop(); continue; }
                    frame.parent.save(); frame.saved = true;
                    frame.parent.transform(...group.transform);
                    const clipping = this.elements.get(group.clip);
                    if (clipping && ['p', 's'].includes(clipping.type)) {
                        frame.parent.beginPath(); frame.parent.save(); frame.parent.transform(...clipping.transform);
                        replayPath(frame.parent, clipping.path); frame.parent.restore();
                        frame.parent.clip(clipping.rule === 'eo' ? 'evenodd' : 'nonzero');
                    }
                    if (group.box) { frame.parent.beginPath(); frame.parent.rect(...group.box); frame.parent.clip(); }
                    frame.ctx = frame.parent;
                    if (group.opacity < 1) {
                        frame.layer = makeCanvas();
                        frame.layer.width = ctx.canvas.width; frame.layer.height = ctx.canvas.height;
                        frame.ctx = frame.layer.getContext('2d', contextOptions);
                        if (!frame.ctx) throw new Error('Cannot allocate vector group canvas');
                        frame.ctx.setTransform(frame.parent.getTransform());
                        frame.ctx.globalCompositeOperation = frame.parent.globalCompositeOperation;
                    }
                    if (group.blend !== 'normal') frame.ctx.globalCompositeOperation = group.blend;
                    frame.children = children.get(group.id) ?? []; frame.index = 0;
                }
                if (frame.index === frame.children.length) {
                    if (frame.layer) {
                        frame.parent.save(); frame.parent.setTransform(1, 0, 0, 1, 0, 0);
                        frame.parent.globalAlpha *= group.opacity; frame.parent.globalCompositeOperation = 'source-over';
                        frame.parent.drawImage(frame.layer, 0, 0); frame.parent.restore();
                    }
                    frame.parent.restore(); frame.saved = false; stack.pop();
                    continue;
                }
                const element = frame.children[frame.index++];
                if (element.type === 'g') { stack.push({group: element, parent: frame.ctx, entered: false}); continue; }
                const target = frame.ctx;
                target.save();
                try {
                    target.transform(...element.transform);
                    if (element.type === 'p') this.drawPath(target, element, element.path, colorResolver);
                    else if (element.type === 'u') {
                        const symbol = this.elements.get(element.ref);
                        if (symbol?.type === 's') this.drawPath(target, element, symbol.path, colorResolver);
                    } else if (element.type === 'k') this.drawPen(target, element, colorResolver);
                    else if (element.type === 't') this.drawText(target, element, colorResolver);
                    else if (element.type === 'i') {
                        const image = rasterImages.get(element.ref);
                        if (image && !image.vector) { target.globalAlpha *= element.opacity; target.imageSmoothingEnabled = true; target.drawImage(image.bitmap ?? image, 0, 0); }
                    }
                } finally { target.restore(); }
            }
        } finally {
            for (let index = stack.length - 1; index >= 0; index--) if (stack[index].saved) stack[index].parent.restore();
        }
    }

    rasterize(width, height, options = {}) {
        if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width <= 0 || height <= 0) throw new Error('Bad vector raster dimensions');
        const canvas = (options.makeCanvas ?? this.makeCanvas)();
        canvas.width = width; canvas.height = height;
        if (canvas.width !== width || canvas.height !== height) throw new Error('Browser cannot allocate vector raster dimensions');
        const ctx = canvas.getContext('2d', options.contextOptions);
        if (!ctx) throw new Error('Cannot allocate vector canvas');
        ctx.scale(width / this.width, height / this.height);
        this.draw(ctx, options);
        if (ctx.isContextLost?.()) throw new Error('ENOMEM: vector drawing context was lost');
        return canvas;
    }
}
