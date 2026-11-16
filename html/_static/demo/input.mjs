// SPDX-License-Identifier: GPL-3.0-only
// Pointer Events supply the browser's real measurements. Missing hardware
// identity, tablet surface coordinates, and pad controls are never fabricated.
const ESC = '\x1b';
const encoder = new TextEncoder();
const I31_MAX = 0x7fffffff;
const POSITION_MAX = 1073741823; // OSC 777's signed physical-pixel position field.

export function integer(value, minimum = 0, maximum = Number.MAX_SAFE_INTEGER) {
    if (typeof value !== 'string' || !/^-?\d+$/.test(value)) return null;
    const number = Number(value);
    return Number.isSafeInteger(number) && number >= minimum && number <= maximum ? number : null;
}

export function parseOSC(payload) {
    if (typeof payload !== 'string') return null;
    if (payload.startsWith(`${ESC}]`)) payload = payload.slice(2).replace(/(?:\x1b\\|\x07)$/, '');
    const match = /^777;([a-zA-Z][a-zA-Z0-9-]*);(.*)$/s.exec(payload);
    if (!match) return null;
    const fields = Object.create(null);
    for (const field of match[2].split(':')) {
        const split = field.indexOf('=');
        if (split < 1) return null;
        const key = field.slice(0, split), value = field.slice(split + 1);
        if (!/^[a-zA-Z][a-zA-Z0-9_-]*$/.test(key) || Object.hasOwn(fields, key) || /[^\x20-\x7e]/.test(value)) return null;
        fields[key] = value;
    }
    if (fields.v !== '1' || !fields.op) return null;
    return {selector: match[1], fields};
}

export function osc(selector, fields) {
    return `${ESC}]777;${selector};${Object.entries(fields).map(([key, value]) => `${key}=${value}`).join(':')}${ESC}\\`;
}

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
function timeFields(timestamp, now) {
    const milliseconds = Number.isFinite(timestamp) && timestamp >= 0 ? timestamp : now();
    const t = Math.floor(milliseconds), tu = Math.floor((milliseconds - t) * 1000);
    return tu ? {t, tu} : {t};
}

export class BrowserHaptics {
    constructor({send = () => {}, environment = globalThis, now = () => performance.now(), focused = () => true} = {}) {
        Object.assign(this, {send, environment, now, focused});
        this.lastTap = -Infinity;
    }
    // Vibration API targets the phone/device, not a trackpad, pen, or dial.
    // Native haptic caps stay zero until a browser provides that target API.
    get caps() { return 0; }
    get vibrationSupported() { return typeof this.environment.navigator?.vibrate === 'function'; }
    command(payload) {
        const frame = typeof payload === 'string' ? parseOSC(payload) : payload;
        if (!frame || frame.selector !== 'haptic') return false;
        if (frame.fields.op === 'query') {
            const id = integer(frame.fields.id, 0, I31_MAX);
            if (id !== null) this.send(osc('haptic', {v: 1, op: 'cap', id, caps: 0}));
        }
        return true;
    }
    // An explicitly opted-in device-vibration UI can call this fallback. OSC
    // trackpad/pen/dial requests cannot: their target semantics would be false.
    playVibration(pattern) {
        const taps = {generic: [12], alignment: [8], level: [8, 20, 8], click: [6], complete: [12, 30, 12]};
        if (!this.vibrationSupported || !this.focused() || !Object.hasOwn(taps, pattern)) return false;
        if (this.environment.navigator.userActivation && !this.environment.navigator.userActivation.hasBeenActive) return false;
        const now = this.now();
        if (now - this.lastTap < 50) return false; // Native protocol's tap cadence.
        const played = this.environment.navigator.vibrate(taps[pattern]);
        if (played) this.lastTap = now;
        return Boolean(played);
    }
    stop() { if (this.vibrationSupported) this.environment.navigator.vibrate(0); }
}

export class BrowserInput {
    constructor(canvas, {send = () => {}, geometry = () => ({}), ink = null,
        changed = () => {}, environment = globalThis, now = () => performance.now(), autoAttach = false} = {}) {
        Object.assign(this, {canvas, send, geometry, ink, changed, environment, now});
        this.mode = 0;
        this.contacts = new Map();
        this.listeners = [];
        this.haptics = new BrowserHaptics({send, environment, now, focused: () => this.focused()});
        this.attachLifecycle();
        if (autoAttach) this.attach();
    }

    get supported() { return typeof this.environment.PointerEvent === 'function'; }
    get caps() { return this.supported ? 1 | 2 : 0; }
    get listing() { return this.supported ? ['tablet-pointer.1', ...(this.ink ? ['ink.1'] : [])] : []; }
    get capabilities() {
        return {tablet: this.supported, touch: this.supported, ink: this.supported && Boolean(this.ink),
            haptics: false, browserVibration: this.haptics.vibrationSupported,
            tabletHardwareIds: false, tabletPads: false};
    }

    focused() {
        const doc = this.canvas.ownerDocument || this.environment.document;
        return (!doc?.hasFocus || doc.hasFocus()) && (!doc || doc.visibilityState !== 'hidden');
    }

    command(payload) {
        const frame = typeof payload === 'string' ? parseOSC(payload) : payload;
        if (!frame) return false;
        if (frame.selector === 'haptic') return this.haptics.command(frame);
        if (frame.selector === 'ink') {
            if (this.supported && this.ink) return this.ink.command(frame);
            const id = integer(frame.fields.id, 0, I31_MAX);
            if (frame.fields.op === 'query' && id !== null) this.send(osc('ink', {v: 1, op: 'cap', id, caps: 0, mode: 0}));
            else if (frame.fields.op === 'mode' && frame.fields.mode === undefined) this.send(osc('ink', {v: 1, op: 'mode', mode: 0}));
            return true;
        }
        if (frame.selector !== 'tablet-pointer') return false;
        const f = frame.fields;
        if (f.op === 'query') {
            const id = integer(f.id, 0, I31_MAX);
            if (id === null) return true;
            const g = this.geometry();
            // These upper bounds are the protocol's field ranges, not browser
            // allocation limits. Event coordinates retain their finer range.
            this.send(osc('tablet-pointer', {v: 1, op: 'cap', id, caps: this.caps, mode: this.mode,
                w: clamp(Math.round(g.width || this.canvas.width || 1), 1, 65535),
                h: clamp(Math.round(g.height || this.canvas.height || 1), 1, 65535),
                cw: clamp(Math.round(g.cellWidth || 1), 1, 4096), ch: clamp(Math.round(g.cellHeight || 1), 1, 4096)}));
        } else if (f.op === 'mode') {
            if (f.mode === undefined) this.send(osc('tablet-pointer', {v: 1, op: 'mode', mode: this.mode}));
            else {
                const requested = integer(f.mode, 0, 0xffffffff);
                if (requested === null) return true;
                const mode = this.supported ? requested & 3 : 0;
                if (mode !== this.mode) { this.cancelAll(); this.mode = mode; }
            }
        }
        return true;
    }

    report(event, phase, state) {
        const box = this.canvas.getBoundingClientRect();
        const g = this.geometry();
        const scaleX = (g.width || this.canvas.width || box.width) / box.width;
        const scaleY = (g.height || this.canvas.height || box.height) / box.height;
        if (![scaleX, scaleY, event.clientX, event.clientY].every(Number.isFinite) || !box.width || !box.height) return null;
        const finger = event.pointerType === 'touch';
        const eraser = event.button === 5 || Boolean(event.buttons & 32);
        const tool = finger ? 0 : eraser ? 2 : state.down ? state.tool : 1;
        const report = {v: 1, op: 'event', kind: finger ? 2 : 1, phase, device: 0,
            ...timeFields(event.timeStamp, this.now), contact: state.contact,
            x: clamp(Math.round((event.clientX - box.left) * scaleX * 256), -POSITION_MAX, POSITION_MAX),
            y: clamp(Math.round((event.clientY - box.top) * scaleY * 256), -POSITION_MAX, POSITION_MAX),
            buttons: finger ? ([1, 2].includes(phase) ? 1 : 0) : (event.buttons & 31) | (event.buttons & 32 ? 1 : 0),
            mods: (event.shiftKey ? 1 : 0) | (event.altKey ? 2 : 0) | (event.ctrlKey ? 4 : 0) | (event.metaKey ? 8 : 0), tool, valid: 0};
        if (!finger) {
            // Pointer Events synthesize pressure=.5 for a pressed device with
            // no pressure sensor. Observe a non-default sample before marking
            // pressure valid; similarly, all-zero tilt/twist can be defaults.
            if (Number.isFinite(event.pressure) && event.pressure > 0 && event.pressure < 1 && event.pressure !== .5) state.axes |= 1;
            if ((Number.isFinite(event.tiltX) && event.tiltX !== 0) || (Number.isFinite(event.tiltY) && event.tiltY !== 0)) state.axes |= 2;
            if (Number.isFinite(event.twist) && event.twist !== 0) state.axes |= 4;
            if (Number.isFinite(event.tangentialPressure) && event.tangentialPressure !== 0) state.axes |= 8;
            if (state.axes & 1 && Number.isFinite(event.pressure)) { report.valid |= 1; report.p = Math.round(clamp(event.pressure, 0, 1) * 65535); }
            if (state.axes & 2 && Number.isFinite(event.tiltX) && Number.isFinite(event.tiltY)) {
                report.valid |= 2; report.tx = Math.round(clamp(event.tiltX, -90, 90) * 100); report.ty = Math.round(clamp(event.tiltY, -90, 90) * 100);
            }
            if (state.axes & 4 && Number.isFinite(event.twist)) { report.valid |= 4; report.twist = Math.round(((event.twist % 360 + 360) % 360) * 100) % 36000; }
            if (state.axes & 8 && Number.isFinite(event.tangentialPressure)) {
                report.valid |= 8; report.tangent = Math.round(clamp(event.tangentialPressure, -1, 1) * 65535);
            }
        }
        // Pointer Events width/height are axis-aligned bounds, not an oriented
        // contact ellipse. They cannot establish major/minor/angle, so we do
        // not advertise or fabricate the native contact-shape measurements.
        state.tool = tool;
        return report;
    }

    emit(report) {
        if (!report) return;
        this.ink?.sample(report);
        this.send(osc('tablet-pointer', report));
    }

    handle(event, type = event.type) {
        if (!this.supported || !this.focused()) return false;
        const finger = event.pointerType === 'touch';
        if ((event.pointerType !== 'pen' || !(this.mode & 1)) && (!finger || !(this.mode & 2))) return false;
        if (!Number.isSafeInteger(event.pointerId) || event.pointerId < 0) return true;
        let state = this.contacts.get(event.pointerId);
        if (!state) {
            if (finger && type !== 'pointerdown') return true;
            state = {contact: event.pointerId, down: false, axes: 0, tool: 1, last: null, pointerType: event.pointerType};
            this.contacts.set(event.pointerId, state);
        }
        if (type === 'pointerleave' && state.down) return true; // Capture keeps active strokes alive outside the window.
        const phase = {pointerdown: 1, pointerup: 3, pointercancel: 4, pointerleave: 5}[type] ?? (state.down ? 2 : 0);
        if (type === 'pointerdown') { state.down = true; this.canvas.setPointerCapture?.(event.pointerId); }
        const samples = type === 'pointermove' && typeof event.getCoalescedEvents === 'function' ? event.getCoalescedEvents() : [];
        for (const sample of samples.length ? samples : [event]) {
            const report = this.report(sample, phase, state);
            if (report) { state.last = report; this.emit(report); }
        }
        if (finger && state.last) this.send(osc('tablet-pointer', {v: 1, op: 'event', kind: 2, phase: 6, device: 0,
            ...timeFields(event.timeStamp, this.now)}));
        if ([3, 4, 5].includes(phase)) {
            state.down = false;
            if (finger || phase !== 3) this.contacts.delete(event.pointerId);
        }
        event.preventDefault?.();
        return true;
    }

    cancelAll() {
        for (const state of this.contacts.values()) if (state.down && state.last) {
            const report = {...state.last, ...timeFields(this.now(), this.now), phase: 4, buttons: 0};
            delete report.ink;
            if (report.kind === 1 && report.valid & 1) report.p = 0;
            this.emit(report);
            if (report.kind === 2) this.send(osc('tablet-pointer', {v: 1, op: 'event', kind: 2, phase: 6, device: 0, t: report.t}));
        }
        this.contacts.clear();
    }

    reset() { this.cancelAll(); this.mode = 0; this.ink?.reset(); this.haptics.stop(); }
    attach() {
        const listen = (target, type, callback) => {
            target?.addEventListener?.(type, callback);
            this.listeners.push(() => target?.removeEventListener?.(type, callback));
        };
        for (const type of ['pointerdown', 'pointermove', 'pointerup', 'pointercancel', 'pointerleave']) listen(this.canvas, type, event => this.handle(event, type));
        return this;
    }
    attachLifecycle() {
        const listen = (target, type, callback) => {
            target?.addEventListener?.(type, callback);
            this.listeners.push(() => target?.removeEventListener?.(type, callback));
        };
        listen(this.environment, 'blur', () => { this.cancelAll(); this.haptics.stop(); });
        listen(this.canvas.ownerDocument, 'visibilitychange', () => { if (!this.focused()) this.cancelAll(); });
    }
    dispose() { this.reset(); this.listeners.forEach(remove => remove()); this.listeners.length = 0; }
}

export const encodeReport = fields => encoder.encode(osc('tablet-pointer', fields));
