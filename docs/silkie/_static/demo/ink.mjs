// SPDX-License-Identifier: GPL-3.0-only
// Terminal-side ink edits the application's retained scene, never a separate
// overlay. Placement coordinates are physical pixels, as in OSC 777 reports.
import {parseOSC, osc, integer} from './input.mjs';

const U32_MAX = 0xffffffff; // Element/image ids are uint32 in the protocol.
const real = value => {
    if (typeof value !== 'string' || !/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/.test(value)) return null;
    const result = Number(value);
    return Number.isFinite(result) ? result : null;
};

function inversePoint(matrix, x, y) {
    if (!matrix || matrix.length !== 6 || !matrix.every(Number.isFinite)) return null;
    const [a, b, c, d, e, f] = matrix;
    const determinant = a * d - b * c;
    if (!Number.isFinite(determinant) || determinant === 0) return null;
    const px = x - e, py = y - f;
    const result = [(d * px - c * py) / determinant, (a * py - b * px) / determinant];
    return result.every(Number.isFinite) ? result : null;
}

export class InkController {
    constructor({placementsForImage, changed = () => {}, send = () => {}, onError = error => console.error(error)} = {}) {
        this.placementsForImage = placementsForImage || (() => []);
        this.changed = changed;
        this.send = send;
        this.onError = onError;
        this.brush = null;
        this.mode = 0;
        this.active = new Map();
    }

    get capabilities() { return {ink: true}; }
    get listing() { return ['ink.1']; }

    reset() { this.mode = 0; this.brush = null; this.active.clear(); }

    command(payload) {
        const frame = typeof payload === 'string' ? parseOSC(payload) : payload;
        if (!frame || frame.selector !== 'ink') return false;
        const f = frame.fields;
        if (f.op === 'query') {
            const id = integer(f.id, 0, 0x7fffffff);
            if (id === null) return true;
            const reply = {v: 1, op: 'cap', id, caps: 1, mode: this.mode};
            if (this.brush) Object.assign(reply, this.brush, {k: this.brush.n ? this.brush.k : 0});
            this.send(osc('ink', reply));
        } else if (f.op === 'mode') {
            if (f.mode === undefined) this.send(osc('ink', {v: 1, op: 'mode', mode: this.mode}));
            else {
                const mode = integer(f.mode, 0, 1);
                if (mode === null) return true;
                this.mode = mode && this.brush ? 1 : 0;
                if (!this.mode) this.active.clear();
            }
        } else if (f.op === 'brush') {
            const i = integer(f.i, 1, U32_MAX), g = integer(f.g ?? '0', 0, U32_MAX);
            const k = integer(f.k, 1, U32_MAX), n = integer(f.n ?? '1', 1, U32_MAX);
            const w1 = real(f.w1), w0 = real(f.w0 ?? f.w1);
            const e = real(f.e ?? '1'), tools = integer(f.tools ?? '58', 1, 511);
            const fill = f.fill ?? '#000000';
            if ([i, g, k, n, w0, w1, e, tools].some(value => value === null) ||
                w0 < 0 || w1 < 0 || !(e > 0) ||
                k + n - 1 > U32_MAX || !/^(?:#[\da-fA-F]{6}(?:[\da-fA-F]{2})?|@\d+)$/.test(fill) ||
                (fill.startsWith('@') && integer(fill.slice(1), 1, U32_MAX) === null)) return true;
            this.brush = {i, g, k, n, fill, w0, w1, e, tools};
            this.mode = 1;
        }
        return true;
    }

    point(placement, brush, report) {
        const {x, y, width, height, scene} = placement;
        if (!(width > 0 && height > 0) || !scene) return null;
        const sx = placement.sx ?? 0, sy = placement.sy ?? 0;
        const sw = placement.sw ?? scene.width, sh = placement.sh ?? scene.height;
        const px = sx + (report.x / 256 - x) * sw / width;
        const py = sy + (report.y / 256 - y) * sh / height;
        const point = inversePoint(scene.groupTransform(brush.g), px, py);
        if (!point) return null;
        const pressure = report.valid & 1 ? report.p / 65535 : 1;
        const strokeWidth = brush.w0 + (brush.w1 - brush.w0) * pressure ** brush.e;
        return [...point, strokeWidth];
    }

    sample(report) {
        if (report.kind !== 1) return report;
        const key = `${report.device}:${report.contact}`;
        let stroke = this.active.get(key);
        if (report.phase === 1 && this.mode && this.brush && this.brush.n && (this.brush.tools & (1 << report.tool))) {
            const brush = {...this.brush};
            const placements = this.placementsForImage(brush.i)
                .filter(p => p.scene && report.x / 256 >= p.x && report.y / 256 >= p.y &&
                    report.x / 256 < p.x + p.width && report.y / 256 < p.y + p.height)
                .sort((a, b) => (b.z || 0) - (a.z || 0) || (b.order || 0) - (a.order || 0));
            const placement = placements[0];
            if (!placement) return report;
            const point = this.point(placement, brush, report);
            if (!point) return report;
            try {
                placement.scene.patch(`k id=${brush.k} g=${brush.g} pts=${point.join(',')} fill=${brush.fill}`);
            } catch { return report; }
            stroke = {brush, placementId: placement.id, scene: placement.scene, id: brush.k};
            this.active.set(key, stroke);
            this.brush.k++; this.brush.n--;
            report.ink = stroke.id;
            this.changed();
        } else if (stroke) {
            const placement = this.placementsForImage(stroke.brush.i)
                .find(p => p.id === stroke.placementId && p.scene === stroke.scene);
            if (!placement || !this.mode) { this.active.delete(key); return report; }
            if (stroke.scene.elements && !stroke.scene.elements.has(stroke.id)) { this.active.delete(key); return report; }
            if (report.phase === 4) {
                try { stroke.scene.patch(`x id=${stroke.id}`); this.changed(); }
                catch (error) { this.onError(error); }
                this.active.delete(key);
            } else if ([2, 3].includes(report.phase)) {
                const point = this.point(placement, stroke.brush, report);
                if (point) {
                    try { stroke.scene.patch(`a id=${stroke.id} pts=${point.join(',')}`); this.changed(); }
                    catch (error) {
                        this.active.delete(key);
                        this.onError(error);
                    }
                }
                if (report.phase === 3) this.active.delete(key);
            } else if (report.phase === 5) this.active.delete(key);
        }
        return report;
    }
}
