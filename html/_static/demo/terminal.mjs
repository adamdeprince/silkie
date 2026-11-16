import {createTranslator} from './i18n.mjs';
// SPDX-License-Identifier: GPL-3.0-only
// Browser platform backend for the portable Silkie terminal core.
import {TextSelection} from './selection.mjs';
import {VectorScene} from './vector.mjs';
import {createColorContext, cellPaint, decodeGraphicsPixels, parseVectorPaint, respondColorOSC} from './color.mjs';
import {BrowserInput, parseOSC, integer} from './input.mjs';
import {InkController} from './ink.mjs';

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const ESC = '\x1b';
const color = n => `#${n.toString(16).padStart(6, '0')}`;

export const capabilities = Object.freeze({
    backend: 'silkie-browser-wasm',
    text: true, trueColor: true, scrollback: true, alternateScreen: true,
    bitmapGraphics: true, graphicsCursorMovement: false, graphicsFormats: [24, 32, 64, 100, 200], graphicsMedia: ['d'],
    kittyKeyboard: true, keyboardReleaseEvents: true, mouse: true, bracketedPaste: true, focusReports: true,
    vectorGraphics: true, hdr: false, wideGamut: true, cellAlpha: true,
    tablet: false, touch: false, gestures: false, haptics: false,
    ink: false, gpuSurfaces: false, nativeWindowActions: false,
    presentationFeedback: false, latencyControl: false, fontProtocol: false,
    pointerImages: false, displayProtocol: false,
});

// Separate from the DOM adapter so protocol and WASM tests can use the exact
// production byte path in Node. No input, scrollback, or escape-length cap.
export async function createCore(source) {
    let bytes = source;
    if (typeof source === 'string' || source instanceof URL) {
        const response = await fetch(source);
        if (!response.ok) throw new Error(`Terminal WASM download failed: HTTP ${response.status}`);
        bytes = await response.arrayBuffer();
    }
    const { instance } = await WebAssembly.instantiate(bytes, {});
    const api = instance.exports;
    let handle = 0;
    const json = ptr => JSON.parse(decoder.decode(new Uint8Array(api.memory.buffer, ptr, api.terminal_output_len(handle))));
    return {
        init(cols, rows, { convertEol = false } = {}) {
            if (handle) throw new Error('Terminal is already initialized');
            handle = api.terminal_new(cols, rows);
            if (!handle) throw new Error('Unable to allocate terminal screen');
            api.terminal_newline_default(handle, Number(convertEol));
        },
        write(data) {
            if (!handle) throw new Error('Terminal is disposed');
            const bytes = typeof data === 'string' ? encoder.encode(data) : data;
            const ptr = api.terminal_input(handle, bytes.length);
            if (!ptr && bytes.length) throw new Error('Unable to allocate terminal input');
            new Uint8Array(api.memory.buffer, ptr, bytes.length).set(bytes);
            if (api.terminal_feed(handle) !== 0) throw new Error('Terminal could not process input');
            return json(api.terminal_events(handle));
        },
        snapshot(offset = 0) { return json(api.terminal_snapshot(handle, offset)); },
        resize(cols, rows) {
            if (api.terminal_resize(handle, cols, rows) !== 0) throw new Error('Unable to resize terminal');
        },
        cellSize(w, h) { api.terminal_cell_size(handle, w, h); },
        modes() { return api.terminal_modes(handle); },
        kittyFlags() { return api.terminal_kitty_flags(handle); },
        resetKeyboard() { api.terminal_keyboard_reset(handle); },
        dispose() { if (handle) api.terminal_free(handle); handle = 0; },
    };
}

export class KittyGraphics {
    constructor({ reply, changed = () => {}, makeCanvas = () => document.createElement('canvas'), colors = {} }) {
        this.colors = colors;
        this.lifetime = {};
        this.reply = reply;
        this.changed = changed;
        this.makeCanvas = makeCanvas;
        this.images = new Map();
        this.placements = new Map();
        this.pending = null;
        this.nextId = 1;
    }
    clear() { this.placements.clear(); this.changed(); }
    dispose() { this.lifetime = {}; for (const image of this.images.values()) image.bitmap?.close?.(); this.images.clear(); this.clear(); this.pending = null; }
    answer(fields, result, reply = this.reply) {
        const quiet = Number(fields.q || 0);
        if (quiet === 2 || (quiet === 1 && result === 'OK')) return;
        const identity = ['i', 'I', 'p'].filter(k => fields[k] !== undefined).map(k => `${k}=${fields[k]}`).join(',');
        reply(`${ESC}_G${identity};${result}${ESC}\\`);
    }
    async command(sequence, cursorX, cursorY, reply = this.reply) {
        const lifetime = this.lifetime;
        if (!sequence.startsWith('G')) return;
        let fields = {};
        const split = sequence.indexOf(';');
        const header = sequence.slice(1, split < 0 ? undefined : split);
        for (const pair of header.split(',')) {
            const pos = pair.indexOf('=');
            if (pos > 0) fields[pair.slice(0, pos)] = pair.slice(pos + 1);
        }
        const encoded = split < 0 ? '' : sequence.slice(split + 1);
        if (this.pending) {
            Object.assign(this.pending.fields, fields);
            this.pending.parts.push(encoded);
            if (fields.m === '1') return;
            ({ fields, cursorX, cursorY } = this.pending);
        } else if (fields.m === '1') {
            this.pending = { fields, parts: [encoded], cursorX, cursorY };
            return;
        }
        const parts = this.pending ? this.pending.parts : [encoded];
        this.pending = null;
        try {
            const action = fields.a || 't';
            if (action === 'd') { this.delete(fields); return; }
            if (action === 'p') {
                if (fields.C !== '1') throw new Error('ENOTSUP: browser placements require C=1');
                const id = Number(fields.i);
                if (!this.images.has(id)) throw new Error('ENOENT: unknown image');
                this.place(fields, id, cursorX, cursorY);
                this.answer(fields, 'OK', reply);
                return;
            }
            if (action === 'T' && fields.C !== '1') throw new Error('ENOTSUP: browser placements require C=1');
            if (!['t', 'T', 'q', 'e'].includes(action)) throw new Error('ENOTSUP: unsupported graphics action');
            if ((fields.t || 'd') !== 'd') throw new Error('ENOTSUP: browser graphics require inline data');
            const format = Number(fields.f || 32);
            if (action !== 'e' && ![24, 32, 64, 100, 200].includes(format)) throw new Error('ENOTSUP: unsupported image format');
            const encodedData = parts.join('');
            let data;
            if (Uint8Array.fromBase64) data = Uint8Array.fromBase64(encodedData);
            else {
                const binary = atob(encodedData);
                data = new Uint8Array(binary.length);
                for (let i = 0; i < binary.length; i++) data[i] = binary.charCodeAt(i);
            }
            if (fields.o) {
                if (fields.o !== 'z') throw new Error('ENOTSUP: unsupported compression');
                data = new Uint8Array(await new Response(new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate'))).arrayBuffer());
                if (lifetime !== this.lifetime) return;
            }
            let bitmap = null, vector = null;
            if (action === 'e') {
                const image = this.images.get(Number(fields.i));
                if (!image?.vector) throw new Error('ENOENT: vector image not found');
                image.vector.patch(decoder.decode(data));
            } else {
                if (format === 200) {
                    vector = VectorScene.parse(data, {width: Number(fields.s), height: Number(fields.v),
                        makeCanvas: this.makeCanvas, colorResolver: paint => parseVectorPaint(paint, this.colors)});
                } else if (format === 100) {
                    bitmap = await createImageBitmap(new Blob([data], {type: 'image/png'}));
                    if (lifetime !== this.lifetime) { bitmap.close(); return; }
                } else bitmap = decodeGraphicsPixels(fields, data, {makeCanvas: this.makeCanvas, capabilities: this.colors});
                if (action === 'q') bitmap?.close?.();
                else {
                    const id = Number(fields.i || this.nextId++);
                    this.images.get(id)?.bitmap?.close?.();
                    this.images.set(id, {bitmap, vector, width: vector?.width || bitmap.width, height: vector?.height || bitmap.height});
                    if (action === 'T') this.place(fields, id, cursorX, cursorY);
                }
            }
            this.answer(fields, 'OK', reply);
            this.changed();
        } catch (error) {
            this.answer(fields, error.message.startsWith('E') ? error.message : `EINVAL: ${error.message}`, reply);
        }
    }
    place(fields, imageId, x, y) {
        const placementId = Number(fields.p || 0);
        this.placements.set(`${imageId}:${placementId}`, { ...fields, imageId, placementId, xCell: x, yCell: y });
        this.changed();
    }
    delete(fields) {
        const kind = fields.d || 'a';
        const id = Number(fields.i), placementId = Number(fields.p || 0);
        if (kind.toLowerCase() === 'a') this.placements.clear();
        else if (kind.toLowerCase() === 'i') {
            for (const [key, placement] of this.placements) {
                if (placement.imageId === id && (!fields.p || placement.placementId === placementId)) this.placements.delete(key);
            }
        } else if (kind.toLowerCase() === 'p') {
            for (const [key, placement] of this.placements) if (placement.placementId === placementId) this.placements.delete(key);
        }
        if (kind === kind.toUpperCase()) {
            const ids = kind === 'A' ? [...this.images.keys()] : [id];
            for (const id of ids) { this.images.get(id)?.bitmap?.close?.(); this.images.delete(id); }
        }
        this.changed();
    }
    scroll(rows) { for (const p of this.placements.values()) p.yCell -= rows; this.changed(); }
    placement(p, image, cw, ch, dpr, scrollOffset = 0) {
        const sx = Number(p.x || 0), sy = Number(p.y || 0);
        const sw = Number(p.w || image.width - sx), sh = Number(p.h || image.height - sy);
        let width = p.c ? Number(p.c) * cw : sw / dpr;
        let height = p.r ? Number(p.r) * ch : sh / dpr;
        if (p.c && !p.r) height = width * sh / sw;
        if (p.r && !p.c) width = height * sw / sh;
        return {x: p.xCell * cw + Number(p.X || 0) / dpr,
            y: (p.yCell + scrollOffset) * ch + Number(p.Y || 0) / dpr, width, height, sx, sy, sw, sh};
    }
    placementsForImage(id, cw, ch, dpr) {
        const result = []; let order = 0;
        for (const [key, p] of this.placements) {
            const image = this.images.get(p.imageId);
            if (p.imageId === id && image?.vector) {
                const box = this.placement(p, image, cw, ch, dpr);
                result.push({...box, id: key, x: box.x * dpr, y: box.y * dpr,
                    width: box.width * dpr, height: box.height * dpr, z: Number(p.z || 0), order, scene: image.vector});
            }
            order++;
        }
        return result;
    }
    draw(ctx, layer, cw, ch, dpr, scrollOffset) {
        const placements = [...this.placements.values()].sort((a, b) => Number(a.z || 0) - Number(b.z || 0));
        for (const p of placements) {
            const z = Number(p.z || 0);
            const placementLayer = z < -1073741824 ? 'behind-background' : z < 0 ? 'below-text' : 'above-text';
            if (placementLayer !== layer) continue;
            const image = this.images.get(p.imageId); if (!image) continue;
            const box = this.placement(p, image, cw, ch, dpr, scrollOffset);
            let bitmap = image.bitmap;
            if (image.vector) {
                const width = Math.ceil(image.width * box.width * dpr / box.sw);
                const height = Math.ceil(image.height * box.height * dpr / box.sh);
                const key = `${width}:${height}:${image.vector.version}`;
                // Only raster resources referenced by this scene can change
                // its pixels. Placement churn and unrelated uploads must not
                // repaint the entire retained scenery.
                const dependenciesChanged = image.cache && [...image.cache.dependencies].some(([id, resource]) => this.images.get(id) !== resource);
                if (image.cache?.key !== key || dependenciesChanged) {
                    bitmap = image.vector.rasterize(width, height, {
                        makeCanvas: this.makeCanvas, rasterImages: this.images,
                        colorResolver: paint => parseVectorPaint(paint, this.colors),
                        contextOptions: {colorSpace: this.colors.colorSpace || 'srgb', colorType: this.colors.hdr ? 'float16' : 'unorm8'},
                    });
                    const dependencies = new Map();
                    for (const element of image.vector.elements.values()) {
                        if (element.type === 'i') dependencies.set(element.ref, this.images.get(element.ref));
                    }
                    image.cache = {key, dependencies, bitmap};
                } else bitmap = image.cache.bitmap;
                ctx.drawImage(bitmap, box.sx * width / image.width, box.sy * height / image.height,
                    box.sw * width / image.width, box.sh * height / image.height, box.x, box.y, box.width, box.height);
            } else ctx.drawImage(bitmap, box.sx, box.sy, box.sw, box.sh, box.x, box.y, box.width, box.height);
        }
    }

}

export function encodeKey(event, modes = 0, kittyFlags = 0, release = false) {
    if (event.isComposing || event.key === 'Dead' || ['Shift', 'Control', 'Alt', 'Meta', 'CapsLock'].includes(event.key)) return null;
    const mod = 1 + (event.shiftKey ? 1 : 0) + (event.altKey ? 2 : 0) + (event.ctrlKey ? 4 : 0) + (event.metaKey ? 8 : 0);
    const kittyKeys = { Escape: 27, Enter: 13, Tab: 9, Backspace: 127 };
    const kittyLetters = { ArrowUp: 'A', ArrowDown: 'B', ArrowRight: 'C', ArrowLeft: 'D', Home: 'H', End: 'F', F1: 'P', F2: 'Q', F3: 'R', F4: 'S' };
    const kittyTildes = { Insert: 2, Delete: 3, PageUp: 5, PageDown: 6, F5: 15, F6: 17, F7: 18, F8: 19, F9: 20, F10: 21, F11: 23, F12: 24 };
    if (kittyFlags) {
        if (release && !(kittyFlags & 2)) return null;
        const suffix = `${mod}${kittyFlags & 2 ? `:${release ? 3 : event.repeat ? 2 : 1}` : ''}`;
        if (kittyLetters[event.key]) return `${ESC}[1;${suffix}${kittyLetters[event.key]}`;
        if (kittyTildes[event.key]) return `${ESC}[${kittyTildes[event.key]};${suffix}~`;
        const code = kittyKeys[event.key] ?? ([...event.key].length === 1 ? event.key.codePointAt(0) : null);
        if (code === null) return null;
        return `${ESC}[${code};${mod}${kittyFlags & 2 ? `:${release ? 3 : event.repeat ? 2 : 1}` : ''}u`;
    }
    if (release) return null;
    const arrows = { ArrowUp: 'A', ArrowDown: 'B', ArrowRight: 'C', ArrowLeft: 'D', Home: 'H', End: 'F' };
    if (arrows[event.key]) return mod > 1 ? `${ESC}[1;${mod}${arrows[event.key]}` : `${ESC}${modes & 1 ? 'O' : '['}${arrows[event.key]}`;
    const special = { Insert: 2, Delete: 3, PageUp: 5, PageDown: 6, F5: 15, F6: 17, F7: 18, F8: 19, F9: 20, F10: 21, F11: 23, F12: 24 };
    if (special[event.key]) return `${ESC}[${special[event.key]}${mod > 1 ? `;${mod}` : ''}~`;
    if (/^F[1-4]$/.test(event.key)) return `${ESC}O${String.fromCharCode(79 + Number(event.key.slice(1)))}`;
    let text = { Enter: '\r', Tab: event.shiftKey ? `${ESC}[Z` : '\t', Backspace: '\x7f', Escape: ESC }[event.key];
    if (text === undefined && [...event.key].length === 1) {
        text = event.key;
        if (event.ctrlKey) { const n = event.key.toUpperCase().charCodeAt(0); if (n >= 64 && n <= 95) text = String.fromCharCode(n - 64); else if (event.key === ' ') text = '\0'; }
    }
    if (text === undefined) return null;
    return event.altKey ? ESC + text : text;
}

// A physical key belongs to the keyboard scope that received its press. An
// old release/repeat must never be delivered to a new application, even when
// both applications negotiate identical flags. Keep retired presses until
// keyup so autorepeat cannot turn a held key into a new application's press.
export class KeyboardInput {
    constructor() { this.scope = {}; this.pressed = new Map(); }
    changeScope() { this.scope = {}; }
    encode(event, modes, flags, release = false) {
        const id = event.code || event.key;
        const previous = this.pressed.get(id);
        if (release) {
            this.pressed.delete(id);
            return previous?.scope === this.scope ? encodeKey({...event, key: previous.event.key}, modes, flags, true) : null;
        }
        if (event.repeat && previous?.scope !== this.scope) return null;
        const encoded = encodeKey(event, modes, flags);
        if (encoded !== null) this.pressed.set(id, {scope: this.scope, event: {...event, key: event.key, code: event.code,
            shiftKey: event.shiftKey, altKey: event.altKey, ctrlKey: event.ctrlKey, metaKey: event.metaKey}});
        return encoded;
    }
    releaseAll(modes, flags, send) {
        for (const {scope, event} of this.pressed.values()) {
            if (scope !== this.scope) continue;
            const encoded = encodeKey({...event, repeat: false}, modes, flags, true);
            if (encoded !== null) send(encoded);
        }
        this.pressed.clear();
    }
}

// Private guest-launcher handshake, deliberately independent of the graphics
// queue. Sending its ACK synchronously makes every later user input follow it
// in the console FIFO. The launcher consumes exactly through this marker,
// then restores the shell tty without flushing any subsequent shell input.
export function acknowledgeInputHandoff(payload, {core, keyboard, send}) {
    const frame = parseOSC(payload);
    if (frame?.selector !== 'browser-input' || frame.fields.op !== 'handoff') return false;
    const id = integer(frame.fields.id, 1, 0x7fffffff);
    if (id === null || Object.keys(frame.fields).length !== 3) return false;
    core.resetKeyboard();
    keyboard.changeScope();
    send(`${ESC}]777;browser-input;v=1:op=ack:id=${id}${ESC}\\`);
    return true;
}

export async function createTerminal(element, { wasmUrl = new URL('silkie-terminal.wasm', import.meta.url), onData = () => {}, onResize = () => {}, onError = error => console.error(error), onLink = () => {}, onCopyError = () => {}, fontSize = 14, convertEol = false, language = 'en' } = {}) {
    const t = createTranslator(language);
    const core = await createCore(wasmUrl);
    const canvas = document.createElement('canvas');
    canvas.setAttribute('role', 'img'); canvas.setAttribute('aria-label', t('Terminal output'));
    canvas.style.cssText = 'display:block;touch-action:none;max-width:100%';
    const input = document.createElement('textarea');
    input.setAttribute('aria-label', t('Terminal input')); input.autocapitalize = 'off'; input.autocomplete = 'off'; input.spellcheck = false;
    input.style.cssText = 'position:absolute;left:0;bottom:0;width:1px;height:1px;opacity:0;resize:none';
    element.style.position ||= 'relative'; element.append(canvas, input);
    const colors = await createColorContext(canvas);
    const ctx = colors.ctx;
    let cw, ch, dpr, cols = 80, rows = 24, initialized = false, disposed = false, frame = 0, scrollOffset = 0, failed = false;
    let eventChain = Promise.resolve();
    let screenScope = {};
    let guestScope = {};
    const keyboard = new KeyboardInput();
    const send = text => { if (!disposed && !failed) onData(typeof text === 'string' ? encoder.encode(text) : text); };
    const fail = error => { if (!failed) { failed = true; onError(error); } };
    const readFrame = () => core.snapshot(scrollOffset);
    const selection = new TextSelection(offset => core.snapshot(offset));
    let selectionScreen;
    const render = () => {
        frame = 0; if (disposed || failed || !initialized || (core.modes() & 512)) return;
        try {
            const snapshot = readFrame();
            scrollOffset = Math.min(scrollOffset, snapshot[7]);
            if (selectionScreen !== snapshot[5]) { selection.clear(); selectionScreen = snapshot[5]; }
            canvas.textContent = snapshot[6].map(line => line.map(cell => cell[0]).join('')).join('\n');
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.fillStyle = '#11130f'; ctx.fillRect(0, 0, cols * cw, rows * ch);
            graphics.draw(ctx, 'behind-background', cw, ch, dpr, scrollOffset);
            for (let y = 0; y < rows; y++) for (let x = 0; x < cols; x++) {
                const cell = snapshot[6][y][x];
                if (!(cell[3] & (8 | 32)) || cell[8] < 100) continue;
                ctx.fillStyle = cellPaint(cell, false, colors.capabilities); ctx.fillRect(x * cw, y * ch, cw, ch);
            }
            graphics.draw(ctx, 'below-text', cw, ch, dpr, scrollOffset);
            for (let y = 0; y < rows; y++) for (let x = 0; x < cols; x++) {
                const cell = snapshot[6][y][x];
                const selected = selection.contains(snapshot[7] - scrollOffset + y, x);
                const cursor = snapshot[4] && !scrollOffset && document.activeElement === input && x === snapshot[2] && y === snapshot[3];
                if (cell[8] < 100 || selected || cursor || (cell[3] & 8)) {
                    ctx.fillStyle = selected ? '#34431e' : cellPaint(cell, false, colors.capabilities, {opaque: selected || cursor});
                    ctx.fillRect(x * cw, y * ch, cw, ch);
                }
            }
            ctx.textBaseline = 'alphabetic';
            for (let y = 0; y < rows; y++) for (let x = 0; x < cols; x++) {
                const [text, fg, bg, attr] = snapshot[6][y][x]; if (!text || text === ' ') continue;
                ctx.font = `${attr & 2 ? 'italic ' : ''}${attr & 1 ? 'bold ' : ''}${fontSize}px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`;
                ctx.fillStyle = cellPaint(snapshot[6][y][x], true, colors.capabilities); ctx.fillText(text, x * cw, y * ch + fontSize);
                if ((attr & 4) || snapshot[6][y][x][9]) ctx.fillRect(x * cw, (y + 1) * ch - 2, cw, 1);
            }
            graphics.draw(ctx, 'above-text', cw, ch, dpr, scrollOffset);
            if (snapshot[4] && !scrollOffset && document.activeElement === input) {
                ctx.strokeStyle = '#a6e22e'; ctx.lineWidth = 1; ctx.strokeRect(snapshot[2] * cw + .5, snapshot[3] * ch + .5, cw - 1, ch - 1);
            }
            colors.present();
        } catch (error) { fail(error); }
    };
    const schedule = () => { if (!frame && !disposed) frame = requestAnimationFrame(render); };
    const graphics = new KittyGraphics({reply: send, changed: schedule, colors: colors.capabilities});
    const ink = new InkController({placementsForImage: id => graphics.placementsForImage(id, cw, ch, dpr), send, changed: schedule, onError: fail});
    const pointers = new BrowserInput(canvas, {send, changed: schedule, ink,
        geometry: () => ({width: cols * cw * dpr, height: rows * ch * dpr, cellWidth: cw * dpr, cellHeight: ch * dpr})});
    const actualCapabilities = {...capabilities, ...colors.capabilities, ...pointers.capabilities, displayProtocol: true};
    function withPointerReplies(reply, action) {
        const previous = [pointers.send, ink.send, pointers.haptics.send];
        try { pointers.send = ink.send = pointers.haptics.send = reply; return action(); }
        finally { [pointers.send, ink.send, pointers.haptics.send] = previous; }
    }
    async function processEvent([kind, payload, x, y], scope) {
        const reply = text => { if (scope === screenScope) send(text); };
        if (disposed) return;
        if (kind === 'reply') reply(payload);
        else if (kind === 'apc') await graphics.command(payload, x, y, reply);
        else if (kind === 'clear-graphics') graphics.clear();
        else if (kind === 'reset-graphics') { graphics.dispose(); ink.active.clear(); }
        else if (kind === 'reset-input') {
            pointers.contacts.clear();
            withPointerReplies(reply, () => pointers.reset());
        }
        else if (kind === 'scroll') graphics.scroll(Number(payload));
        else if (kind === 'osc') {
            // Apply mode cleanup even if the app has already left its screen,
            // but never send a delayed reply into the following shell.
            const handled = withPointerReplies(reply, () => pointers.command(payload));
            if (handled) return;
            if (respondColorOSC(payload, colors.capabilities, {send: reply})) return;
            const [selector, name, ...tail] = payload.split(';');
            if (selector === '777' && name === 'caps') {
                const fields = Object.fromEntries(tail.join(';').split(':').map(p => p.split('=')));
                if (fields.v === '1' && fields.op === 'reset') { withPointerReplies(reply, () => pointers.reset()); return; }
                if (fields.v === '1' && fields.op === 'query') reply(`${ESC}]777;caps;v=1:op=cap:id=${fields.id || 0}:caps=3:ext=${['caps.1', 'display.1', ...pointers.listing].join(',')}:media=d:format=24,32,64,100,200${ESC}\\`);
            } else if (selector === '10' && name === '?') reply(`${ESC}]10;rgb:f7f7/f6f6/f1f1${ESC}\\`);
            else if (selector === '11' && name === '?') reply(`${ESC}]11;rgb:1111/1313/0f0f${ESC}\\`);
        }
    }
    function resize(width, height) {
        if (disposed || failed) return;
        const previousPixels = [Math.round(cw * dpr), Math.round(ch * dpr)];
        dpr = window.devicePixelRatio || 1; ch = Math.ceil(fontSize * 1.35);
        ctx.font = `${fontSize}px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`; cw = Math.round(ctx.measureText('M').width * dpr) / dpr;
        const nextCols = Math.max(1, Math.floor(width / cw)); const nextRows = Math.max(1, Math.floor(height / ch));
        const changed = nextCols !== cols || nextRows !== rows;
        cols = nextCols; rows = nextRows;
        canvas.width = Math.ceil(cols * cw * dpr); canvas.height = Math.ceil(rows * ch * dpr);
        colors.resize(canvas.width, canvas.height);
        canvas.style.width = `${cols * cw}px`; canvas.style.height = `${rows * ch}px`;
        if (!initialized) { core.init(cols, rows, { convertEol }); initialized = true; } else if (changed) core.resize(cols, rows);
        const cellWidth = Math.round(cw * dpr), cellHeight = Math.round(ch * dpr);
        core.cellSize(cellWidth, cellHeight);
        // Font size and display density can change without changing the grid.
        // Applications still need SIGWINCH so they can query pixel dimensions.
        if (changed || cellWidth !== previousPixels[0] || cellHeight !== previousPixels[1]) onResize(cols, rows);
        schedule();
    }
    const bounds = element.getBoundingClientRect(); resize(bounds.width || 800, bounds.height || 520);
    const observer = new ResizeObserver(entries => { const { width, height } = entries[0].contentRect; try { resize(width, height); } catch (error) { fail(error); } }); observer.observe(element);
    const listeners = [];
    const listen = (target, event, callback, options) => { target.addEventListener(event, callback, options); listeners.push(() => target.removeEventListener(event, callback, options)); };
    // Zoom or moving between displays can change physical cell dimensions
    // while ResizeObserver sees the same CSS box.
    listen(window, 'resize', () => {
        const box = element.getBoundingClientRect();
        try { resize(box.width, box.height); } catch (error) { fail(error); }
    });
    for (const type of ['keydown', 'keyup']) listen(input, type, event => {
        if (type === 'keyup') {
            const encoded = keyboard.encode(event, core.modes(), core.kittyFlags(), true);
            if (encoded !== null) { event.preventDefault(); send(encoded); }
            return;
        }
        if (type === 'keydown' && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'c' && selection.bounds()) {
            // Native copy events also cover the browser's Edit > Copy action.
            event.preventDefault();
            if (!document.execCommand('copy')) {
                if (navigator.clipboard?.writeText) navigator.clipboard.writeText(selection.text()).catch(onCopyError);
                else onCopyError(new Error('Clipboard access is unavailable'));
            }
            return;
        }
        if (type === 'keydown' && (event.metaKey || (event.ctrlKey && event.shiftKey)) && event.key.toLowerCase() === 'a') {
            event.preventDefault(); selection.selectAll(); input.value = selection.text(); input.select(); schedule(); return;
        }
        // Leave browser shortcuts and clipboard shortcuts to their native handlers.
        if (event.metaKey || (event.ctrlKey && event.shiftKey && ['C', 'V'].includes(event.key.toUpperCase()))) return;
        if (type === 'keydown' && !['Shift', 'Control', 'Alt', 'Meta'].includes(event.key)) { selection.clear(); input.value = ''; schedule(); }
        const encoded = keyboard.encode(event, core.modes(), core.kittyFlags());
        if (encoded !== null) { event.preventDefault(); scrollOffset = 0; send(encoded); schedule(); }
    });
    listen(input, 'copy', event => {
        if (!selection.bounds()) return;
        try { event.clipboardData.setData('text/plain', selection.text()); event.preventDefault(); }
        catch (error) { onCopyError(error); }
    });
    listen(input, 'compositionstart', () => { selection.clear(); input.value = ''; });
    listen(input, 'input', () => { if (input.value) send(input.value); input.value = ''; });
    listen(input, 'paste', event => { const text = event.clipboardData?.getData('text'); if (text === undefined) return; event.preventDefault(); send(core.modes() & 2 ? `${ESC}[200~${text}${ESC}[201~` : text); });
    listen(input, 'compositionend', () => { if (input.value) send(input.value); input.value = ''; });
    listen(input, 'focus', () => { if (core.modes() & 1024) send(`${ESC}[I`); schedule(); });
    listen(input, 'blur', () => {
        keyboard.releaseAll(core.modes(), core.kittyFlags(), send);
        if (core.modes() & 1024) send(`${ESC}[O`);
        schedule();
    });
    // A window/tab can lose focus while its textarea remains activeElement.
    listen(window, 'blur', () => keyboard.releaseAll(core.modes(), core.kittyFlags(), send));
    listen(document, 'visibilitychange', () => {
        if (document.visibilityState === 'hidden') keyboard.releaseAll(core.modes(), core.kittyFlags(), send);
    });
    let pressed = 0;
    const mouse = (event, code, release = false) => {
        const modes = core.modes(); if (!(modes & (4 | 8 | 16))) return false;
        const box = canvas.getBoundingClientRect();
        const px = Math.max(0, Math.min(cols * cw - 1, event.clientX - box.left)); const py = Math.max(0, Math.min(rows * ch - 1, event.clientY - box.top));
        const x = modes & 32 ? Math.floor(px * dpr) + 1 : Math.floor(px / cw) + 1;
        const y = modes & 32 ? Math.floor(py * dpr) + 1 : Math.floor(py / ch) + 1;
        code += (event.shiftKey ? 4 : 0) + (event.altKey ? 8 : 0) + (event.ctrlKey ? 16 : 0);
        if (modes & (32 | 64)) send(`${ESC}[<${code};${x};${y}${release ? 'm' : 'M'}`);
        else if (x < 224 && y < 224) send(Uint8Array.of(27, 91, 77, 32 + (release ? 3 : code), 32 + x, 32 + y));
        event.preventDefault(); return true;
    };
    let selecting = false;
    const cellAt = event => {
        const box = canvas.getBoundingClientRect(), view = readFrame();
        const column = Math.max(0, Math.min(cols - 1, Math.floor((event.clientX - box.left) / cw)));
        const row = Math.max(0, Math.min(rows - 1, Math.floor((event.clientY - box.top) / ch)));
        return {line: view[7] - scrollOffset + row, column, link: view[6][row]?.[column]?.[9]};
    };
    listen(canvas, 'pointerdown', event => {
        input.focus({preventScroll: true});
        if (pointers.handle(event, 'pointerdown')) return;
        pressed = event.button === 1 ? 1 : event.button === 2 ? 2 : 0;
        canvas.setPointerCapture(event.pointerId);
        if (event.button === 0 && (event.shiftKey || !(core.modes() & (4 | 8 | 16)))) {
            const cell = cellAt(event); selection.start(cell.line, cell.column); selecting = true;
            input.value = ''; schedule(); event.preventDefault();
        } else mouse(event, pressed);
    });
    listen(canvas, 'pointerup', event => {
        if (pointers.handle(event, 'pointerup')) return;
        if (selecting) {
            selecting = false;
            if (selection.bounds()) { input.value = selection.text(); input.select(); }
            else { const {link} = cellAt(event); if (link) onLink(link); }
            schedule();
        } else mouse(event, pressed, true);
        pressed = 0;
    });
    listen(canvas, 'pointermove', event => {
        if (pointers.handle(event, 'pointermove')) return;
        if (selecting) { const cell = cellAt(event); selection.extend(cell.line, cell.column); schedule(); }
        else {
            const mode = core.modes();
            canvas.style.cursor = cellAt(event).link ? 'pointer' : 'text';
            if ((mode & 16) || ((mode & 8) && event.buttons)) mouse(event, (event.buttons ? pressed : 3) + 32);
        }
    });
    listen(canvas, 'pointercancel', event => { pointers.handle(event, 'pointercancel'); selecting = false; pressed = 0; });
    listen(canvas, 'pointerleave', event => { pointers.handle(event, 'pointerleave'); });
    listen(canvas, 'contextmenu', event => { if ((core.modes() & (4 | 8 | 16)) && !event.shiftKey) event.preventDefault(); });
    listen(canvas, 'wheel', event => {
        if (core.modes() & (4 | 8 | 16)) mouse(event, event.deltaY < 0 ? 64 : 65);
        else { const frame = readFrame(); scrollOffset = Math.max(0, Math.min(frame[7], scrollOffset + Math.sign(event.deltaY) * 3)); event.preventDefault(); schedule(); }
    }, { passive: false });
    return {
        capabilities: actualCapabilities,
        get fontSize() { return fontSize; },
        get selectedText() { return selection.text(); },
        get graphicsStatistics() {
            let vectorImages = 0;
            for (const image of graphics.images.values()) if (image.vector) vectorImages++;
            return {images: graphics.images.size, vectorImages, placements: graphics.placements.size};
        },
        links() {
            const snapshot = readFrame(), links = [];
            for (let y = 0; y < rows; y++) for (let x = 0; x < cols;) {
                const uri = snapshot[6][y][x][9];
                if (!uri) { x++; continue; }
                const start = x;
                while (x < cols && snapshot[6][y][x][9] === uri) x++;
                links.push({uri, x: start * cw, y: y * ch, width: (x - start) * cw, height: ch});
            }
            return links;
        },
        get cols() { return cols; }, get rows() { return rows; },
        get keyboardFlags() { return core.kittyFlags(); },
        get modes() { return core.modes(); },
        write(bytes) {
            if (disposed || failed) return;
            try {
                const events = core.write(bytes);
                for (const event of events) {
                    if (event[0] === 'keyboard-scope') { keyboard.changeScope(); continue; }
                    if (event[0] === 'osc' && acknowledgeInputHandoff(event[1], {core, keyboard, send})) {
                        guestScope = {}; screenScope = {};
                        withPointerReplies(() => {}, () => pointers.reset());
                        graphics.dispose(); ink.active.clear();
                        continue;
                    }
                    if (event[0] === 'screen-scope') { screenScope = {}; continue; }
                    const scope = screenScope;
                    const owner = guestScope;
                    eventChain = eventChain.then(() => {
                        if (owner !== guestScope) return;
                        if (scope !== screenScope && !['osc', 'reset-input'].includes(event[0])) return;
                        return processEvent(event, scope);
                    });
                }
                eventChain = eventChain.then(schedule).catch(fail);
                schedule();
            } catch (error) { fail(error); }
        },
        resize(columns, lines) { resize(columns * cw, lines * ch); },
        setFontSize(size) {
            if (!Number.isFinite(size) || size <= 0) throw new Error('Invalid terminal font size');
            fontSize = size; const box = element.getBoundingClientRect(); resize(box.width, box.height);
        },
        focus() { input.focus({ preventScroll: true }); },
        async flush() { await eventChain; if (frame) { cancelAnimationFrame(frame); render(); } },
        dispose() { if (disposed) return; disposed = true; observer.disconnect(); listeners.forEach(remove => remove()); if (frame) cancelAnimationFrame(frame); pointers.dispose(); ink.active.clear(); graphics.dispose(); colors.dispose?.(); core.dispose(); canvas.remove(); input.remove(); },
    };
}
