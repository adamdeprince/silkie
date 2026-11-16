// SPDX-License-Identifier: GPL-3.0-only
// Color-management boundary shared by cells, raster graphics and vectors.
// The matrices match kitty/vector-color.h (D65 primaries). No gamut clipping
// happens until an output path needs it; HDR samples stay floating point.
const SRGB_XYZ = [.4124564,.3575761,.1804375,.2126729,.7151522,.0721750,.0193339,.1191920,.9503041];
const XYZ_SRGB = [3.2404542,-1.5371385,-.4985314,-.9692660,1.8760108,.0415560,.0556434,-.2040259,1.0572252];
const GAMUT_XYZ = [SRGB_XYZ,
    [.4865709,.2656677,.1982173,.2289746,.6917385,.0792869,0,.0451134,1.0439444],
    [.6369580,.1446169,.1688810,.2627002,.6779981,.0593017,0,.0280727,1.0609851]];
const XYZ_P3 = [2.4934969,-.9313836,-.4027108,-.8294890,1.7626641,.0236247,.0358458,-.0761724,.9568845];
const clamp = value => Math.min(1, Math.max(0, value));
const numeric = value => /^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/.test(value);
const multiply = (matrix, value) => [0, 1, 2].map(row => matrix[row * 3] * value[0] + matrix[row * 3 + 1] * value[1] + matrix[row * 3 + 2] * value[2]);
export const decodeSRGB = value => value <= .04045 ? value / 12.92 : ((value + .055) / 1.055) ** 2.4;
export const encodeSRGB = value => value <= .0031308 ? value * 12.92 : 1.055 * value ** (1 / 2.4) - .055;
export function toLinearSRGB(rgb, gamut = 0) {
    const decoded = rgb.map(decodeSRGB);
    return gamut === 1 || gamut === 2 ? multiply(XYZ_SRGB, multiply(GAMUT_XYZ[gamut], decoded)) : decoded;
}
export function fromLinearSRGB(rgb, colorSpace = 'srgb') {
    return (colorSpace === 'display-p3' ? multiply(XYZ_P3, multiply(SRGB_XYZ, rgb)) : rgb).map(encodeSRGB);
}

export function parseVectorColor(paint) {
    if (paint === 'none') return null;
    if (typeof paint !== 'string') throw new Error('EINVAL: paint must be a string');
    const [base, brightnessText, ...extra] = paint.split('*');
    const brightness = brightnessText === undefined ? 1 : Number(brightnessText) / 100;
    if (extra.length || (brightnessText !== undefined && !numeric(brightnessText)) || !Number.isFinite(brightness) || brightness < 0) throw new Error('EINVAL: invalid paint brightness');
    let components, gamut = 0, linear = false;
    if (/^#[0-9a-f]{6}(?:[0-9a-f]{2})?$/i.test(base)) {
        components = [1, 3, 5].map(offset => parseInt(base.slice(offset, offset + 2), 16) / 255);
        components.push(base.length === 9 ? parseInt(base.slice(7), 16) / 255 : 1);
    } else {
        const match = /^(linear|srgb|p3|rec2020):(.+)$/.exec(base);
        if (!match) throw new Error('EINVAL: unsupported paint color');
        const words = match[2].split('/');
        if (words.some(value => !numeric(value))) throw new Error('EINVAL: invalid paint components');
        components = words.map(Number);
        if (![3, 4].includes(components.length) || components.some(value => !Number.isFinite(value))) throw new Error('EINVAL: invalid paint components');
        if (components.length === 3) components.push(1);
        linear = match[1] === 'linear';
        if ((!linear && components.slice(0, 3).some(value => value < 0 || value > 1)) || components[3] < 0 || components[3] > 1) throw new Error('EINVAL: paint component is outside its range');
        gamut = {srgb: 0, p3: 1, rec2020: 2}[match[1]] || 0;
    }
    const rgb = linear ? components.slice(0, 3) : toLinearSRGB(components.slice(0, 3), gamut);
    const result = [...rgb.map(value => value * brightness), components[3]];
    if (result.some(value => !Number.isFinite(value))) throw new Error('EINVAL: paint overflow');
    return result;
}

export function linearPaint(rgba, capabilities = {}) {
    if (!rgba) return null;
    const alpha = clamp(rgba[3] ?? 1);
    if (capabilities.cssColor4 !== false && capabilities.colorSpace === 'display-p3') {
        // Gamut conversion precedes clipping: a P3 red has negative green/blue
        // in linear sRGB, but must not be clipped to an ordinary sRGB red.
        const encoded = fromLinearSRGB(rgba.slice(0, 3), 'display-p3').map(value => capabilities.hdr ? value : clamp(value));
        return `color(display-p3 ${encoded.join(' ')} / ${alpha})`;
    }
    const rgb = rgba.slice(0, 3).map(value => capabilities.hdr ? value : clamp(value));
    if (capabilities.cssColor4 !== false) return `color(srgb-linear ${rgb.join(' ')} / ${alpha})`;
    return `rgba(${rgb.map(value => Math.round(clamp(encodeSRGB(value)) * 255)).join(',')},${alpha})`;
}
export const parseVectorPaint = (paint, capabilities) => linearPaint(parseVectorColor(paint), capabilities);
export function interpolateVectorPaint(first, second, fraction, capabilities) {
    const a = parseVectorColor(first), b = parseVectorColor(second);
    if (!a || !b || !Number.isFinite(fraction)) throw new Error('EINVAL: invalid gradient color');
    const t = clamp(fraction);
    return linearPaint(a.map((value, index) => value + (b[index] - value) * t), capabilities);
}

// Display metadata has explicit unknown values in Silkie's protocol. Browsers
// expose scaling/gamut, but not a calibrated physical headroom, PPI or refresh
// period. Report those as zero rather than deriving guesses from CSS pixels or
// requestAnimationFrame timing. Subscription mode may be refused by a terminal.
export function respondColorOSC(payload, capabilities, {send, environment = globalThis} = {}) {
    if (!payload.startsWith('777;display;')) return false;
    const fields = Object.create(null);
    for (const pair of payload.slice('777;display;'.length).split(':')) {
        const separator = pair.indexOf('=');
        if (separator <= 0 || separator === pair.length - 1) return true;
        const key = pair.slice(0, separator), value = pair.slice(separator + 1);
        if (Object.hasOwn(fields, key) || !/^[\x21-\x7e]+$/.test(key + value)) return true;
        fields[key] = value;
    }
    if (fields.v !== '1' || !['query', 'mode'].includes(fields.op)) return true;
    if (fields.mode !== undefined && !['0', '1'].includes(fields.mode)) return true;
    if (fields.op === 'mode') {
        send('\x1b]777;display;v=1:op=mode:mode=0\x1b\\');
        return true;
    }
    if (!/^\d+$/.test(fields.id || '') || Number(fields.id) > 2147483647) return true;
    const matches = query => Boolean(environment.matchMedia?.(query).matches);
    const scale = Number.isFinite(environment.devicePixelRatio) && environment.devicePixelRatio > 0 ? environment.devicePixelRatio : 0;
    const gamut = capabilities.colorSpace === 'display-p3' && matches('(color-gamut: p3)')
        ? (matches('(color-gamut: rec2020)') ? 'rec2020' : 'p3') : 'srgb';
    const color = matches('(monochrome)') ? (matches('(monochrome: 1)') ? 'mono' : 'gray') : 'rgb';
    const headroom = Number.isFinite(capabilities.headroom) && capabilities.headroom > 0 ? capabilities.headroom : 0;
    send(`\x1b]777;display;v=1:op=cap:id=${Number(fields.id)}:mode=0:kind=unknown:color=${color}:bits=0:gamut=${gamut}:headroom=${headroom}:scale=${scale}:period=0:ppi=0\x1b\\`);
    return true;
}

export function cellPaint(cell, foreground, capabilities = {}, {opaque = false} = {}) {
    const reverse = Boolean(cell[3] & 8);
    const useForeground = foreground !== reverse;
    const color = cell[useForeground ? 1 : 2];
    const gamut = cell[useForeground ? 6 : 7] || 0;
    const requestedBrightness = (cell[useForeground ? 4 : 5] ?? 100) / 100;
    const brightness = capabilities.hdr ? requestedBrightness : 1;
    const dim = foreground && (cell[3] & 64) ? .5 : 1;
    const rgb = toLinearSRGB([(color >> 16 & 255) / 255, (color >> 8 & 255) / 255, (color & 255) / 255], gamut);
    const alpha = foreground || reverse || opaque ? 1 : (cell[8] ?? 100) / 100;
    return linearPaint([...rgb.map(value => value * brightness * dim), alpha], capabilities);
}

export function halfToFloat(bits) {
    const exponent = bits >> 10 & 31, mantissa = bits & 1023;
    const value = exponent === 0 ? mantissa * 2 ** -24 : exponent === 31 ? (mantissa ? NaN : Infinity) : (1024 + mantissa) * 2 ** (exponent - 25);
    return bits & 0x8000 ? -value : value;
}
function allocate(Type, length) {
    try { return new Type(length); }
    catch (error) {
        if (error instanceof RangeError) throw new Error('ENOMEM: graphics storage allocation failed', {cause: error});
        throw error;
    }
}

export function decodeGraphicsPixels(fields, data, {makeCanvas = () => document.createElement('canvas'), capabilities = {}} = {}) {
    const format = Number(fields.f || 32), channels = format / 8;
    const width = Number(fields.s), height = Number(fields.v), expected = width * height * channels;
    if (![24, 32, 64].includes(format) || !Number.isSafeInteger(width) || !Number.isSafeInteger(height)
        || width <= 0 || height <= 0 || !Number.isSafeInteger(expected) || data.length !== expected) {
        throw new Error('EINVAL: image dimensions do not match payload');
    }
    const gamut = Number(fields.G) === 1 ? 1 : Number(fields.G) === 2 ? 2 : 0;
    const sampleCount = width * height * 4;
    if (!Number.isSafeInteger(sampleCount)) throw new Error('EINVAL: graphics size overflow');
    if (format !== 64 && (gamut === 0 || (gamut === 1 && Number(fields.T) !== 1))) {
        // Ordinary game atlases already contain encoded sRGB/P3 bytes. Let Canvas
        // convert the tagged ImageData into its output color space, avoiding
        // two JavaScript color transforms and a float allocation per pixel.
        let output;
        if (format === 32) output = new Uint8ClampedArray(data.buffer, data.byteOffset, data.byteLength);
        else {
            output = allocate(Uint8ClampedArray, sampleCount);
            for (let source = 0, target = 0; source < data.length; source += 3, target += 4) {
                output[target] = data[source]; output[target + 1] = data[source + 1];
                output[target + 2] = data[source + 2]; output[target + 3] = 255;
            }
        }
        const canvas = makeCanvas(); canvas.width = width; canvas.height = height;
        const ctx = canvas.getContext('2d', {colorSpace: capabilities.colorSpace === 'display-p3' ? 'display-p3' : 'srgb',
            colorType: capabilities.hdr && capabilities.float16 ? 'float16' : 'unorm8'});
        if (!ctx) throw new Error('ENOMEM: unable to create graphics surface');
        ctx.putImageData(new ImageData(output, width, height, {colorSpace: gamut === 1 ? 'display-p3' : 'srgb', pixelFormat: 'rgba-unorm8'}), 0, 0);
        return canvas;
    }
    const samples = allocate(Float32Array, sampleCount);
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    let peak = 1;
    for (let pixel = 0; pixel < width * height; pixel++) {
        const index = pixel * 4;
        const rgb = format === 64 ? [0, 2, 4].map(offset => halfToFloat(view.getUint16(pixel * 8 + offset, true)))
            : toLinearSRGB([0, 1, 2].map(offset => data[pixel * channels + offset] / 255), gamut);
        const alpha = format === 64 ? halfToFloat(view.getUint16(pixel * 8 + 6, true)) : format === 32 ? data[pixel * 4 + 3] / 255 : 1;
        if (rgb.some(value => !Number.isFinite(value)) || !Number.isFinite(alpha)) throw new Error('EINVAL: non-finite HDR pixel');
        samples.set([...rgb, clamp(alpha)], index);
        peak = Math.max(peak, ...rgb);
    }
    // The browser does not expose physical HDR headroom numerically. Preserve
    // extended values for its HDR compositor; the SDR fallback has headroom 1.
    const hdr = capabilities.hdr && capabilities.float16;
    const headroom = 1, knee = .75 * headroom;
    const toneMap = !hdr && Number(fields.T) === 1 && peak > headroom;
    const slope = toneMap ? 1 / (headroom - knee) - 1 / (peak - knee) : 0;
    const colorSpace = capabilities.colorSpace === 'display-p3' ? 'display-p3' : 'srgb';
    const output = allocate(hdr ? Float16Array : Uint8ClampedArray, samples.length);
    for (let index = 0; index < samples.length; index += 4) {
        let rgb = [samples[index], samples[index + 1], samples[index + 2]];
        const maximum = Math.max(...rgb);
        if (toneMap && maximum > knee) {
            const mapped = knee + (maximum - knee) / (1 + slope * (maximum - knee));
            rgb = rgb.map(value => value * mapped / maximum);
        }
        const encoded = fromLinearSRGB(rgb, colorSpace);
        for (let channel = 0; channel < 3; channel++) output[index + channel] = hdr ? encoded[channel] : Math.round(clamp(encoded[channel]) * 255);
        output[index + 3] = hdr ? samples[index + 3] : Math.round(samples[index + 3] * 255);
    }
    const canvas = makeCanvas(); canvas.width = width; canvas.height = height;
    const ctx = canvas.getContext('2d', {colorSpace, colorType: hdr ? 'float16' : 'unorm8'});
    if (!ctx) throw new Error('ENOMEM: unable to create graphics surface');
    const image = new ImageData(output, width, height, {colorSpace, pixelFormat: hdr ? 'rgba-float16' : 'rgba-unorm8'});
    ctx.putImageData(image, 0, 0);
    return canvas;
}

const HDR_SHADER = `
@group(0) @binding(0) var image: texture_2d<f32>;
@group(0) @binding(1) var filtering: sampler;
struct Vertex { @builtin(position) position: vec4f, @location(0) uv: vec2f }
@vertex fn vs(@builtin(vertex_index) index: u32) -> Vertex {
    var position = array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));
    var result: Vertex; result.position = vec4f(position[index],0.,1.);
    result.uv = position[index] * vec2f(.5,-.5) + vec2f(.5,.5); return result;
}
fn linear(value: vec3f) -> vec3f {
    return select(pow((value + .055) / 1.055, vec3f(2.4)), value / 12.92, value <= vec3f(.04045));
}
@fragment fn fs(vertex: Vertex) -> @location(0) vec4f {
    let value = textureSample(image, filtering, vertex.uv);
    return vec4f(linear(value.rgb),value.a);
}`;

export async function createColorContext(canvas, {environment = globalThis, makeCanvas = () => document.createElement('canvas')} = {}) {
    const matches = query => Boolean(environment.matchMedia?.(query).matches);
    const options = {alpha: false, colorSpace: 'display-p3', colorType: 'float16'};
    const backing = makeCanvas(); backing.width = backing.height = 1;
    const ctx = backing.getContext('2d', options) || backing.getContext('2d');
    if (!ctx) throw new Error('Canvas 2D is unavailable');
    const attributes = ctx.getContextAttributes?.() || {};
    const capabilities = {
        cellAlpha: true, wideGamut: true, hdrImages: true,
        colorSpace: attributes.colorSpace === 'display-p3' ? 'display-p3' : 'srgb',
        gamut: attributes.colorSpace === 'display-p3' && matches('(color-gamut: p3)') ? 'p3' : 'srgb',
        float16: false, hdr: false, headroom: null,
        cssColor4: environment.CSS?.supports?.('color', 'color(srgb-linear 1 0 0)') ?? false,
    };
    try {
        if (attributes.colorType === 'float16' && typeof environment.Float16Array === 'function') {
            const pixel = new environment.ImageData(new environment.Float16Array([2, 0, 0, 1]), 1, 1,
                {colorSpace: capabilities.colorSpace, pixelFormat: 'rgba-float16'});
            ctx.putImageData(pixel, 0, 0);
            capabilities.float16 = ctx.getImageData(0, 0, 1, 1, {pixelFormat: 'rgba-float16'}).data[0] > 1;
            if (capabilities.float16 && capabilities.cssColor4) {
                ctx.fillStyle = 'color(srgb-linear 2 0 0)';
                ctx.fillRect(0, 0, 1, 1);
                capabilities.float16 = ctx.getImageData(0, 0, 1, 1, {pixelFormat: 'rgba-float16'}).data[0] > 1;
            }
        }
    } catch { /* Optional float16 precision cannot be assumed from requested attributes. */ }
    if (capabilities.float16 && capabilities.cssColor4 && matches('(dynamic-range: high)') && environment.navigator?.gpu) {
        let hdrDevice;
        try {
            const adapter = await environment.navigator.gpu.requestAdapter();
            if (!adapter) throw new Error('No WebGPU adapter');
            const device = await adapter.requestDevice();
            hdrDevice = device;
            const probe = makeCanvas().getContext('webgpu');
            const configuration = {device, format: 'rgba16float', colorSpace: capabilities.colorSpace, alphaMode: 'opaque', toneMapping: {mode: 'extended'}};
            probe.configure(configuration);
            if (probe.getConfiguration?.().toneMapping?.mode !== 'extended') throw new Error('Extended HDR presentation unavailable');
            probe.unconfigure();
            const module = device.createShaderModule({code: HDR_SHADER});
            const pipeline = await device.createRenderPipelineAsync({layout: 'auto', vertex: {module, entryPoint: 'vs'}, fragment: {module, entryPoint: 'fs', targets: [{format: 'rgba16float'}]}});
            // Test the actual 2D -> GPU bridge as well. Some implementations
            // expose both APIs but clamp external-image copies to SDR.
            const usage = environment.GPUTextureUsage;
            device.pushErrorScope('validation');
            const sample = device.createTexture({size: [1, 1], format: 'rgba16float', usage: usage.COPY_DST | usage.COPY_SRC | usage.RENDER_ATTACHMENT});
            // WebGPU buffer-copy rows require 256-byte alignment, even for a
            // single RGBA16F pixel. This is a format constraint, not a cap.
            const readback = device.createBuffer({size: 256, usage: environment.GPUBufferUsage.COPY_DST | environment.GPUBufferUsage.MAP_READ});
            device.queue.copyExternalImageToTexture({source: backing}, {texture: sample, colorSpace: capabilities.colorSpace}, [1, 1]);
            const copy = device.createCommandEncoder();
            copy.copyTextureToBuffer({texture: sample}, {buffer: readback, bytesPerRow: 256}, [1, 1]);
            device.queue.submit([copy.finish()]);
            await readback.mapAsync(environment.GPUMapMode.READ);
            const retainedHDR = halfToFloat(new Uint16Array(readback.getMappedRange())[0]) > 1;
            readback.unmap(); readback.destroy(); sample.destroy();
            const copyError = await device.popErrorScope();
            if (copyError || !retainedHDR) throw new Error('HDR values were clipped by external-image transfer');
            const output = canvas.getContext('webgpu'); output.configure(configuration);
            capabilities.hdr = true;
            let texture, group;
            const sampler = device.createSampler({magFilter: 'nearest', minFilter: 'nearest'});
            const resize = (width, height) => {
                backing.width = width; backing.height = height;
                texture?.destroy();
                const usage = environment.GPUTextureUsage;
                texture = device.createTexture({size: [width, height], format: 'rgba16float', usage: usage.COPY_DST | usage.TEXTURE_BINDING | usage.RENDER_ATTACHMENT});
                group = device.createBindGroup({layout: pipeline.getBindGroupLayout(0), entries: [{binding: 0, resource: texture.createView()}, {binding: 1, resource: sampler}]});
            };
            return {ctx, capabilities, resize, dispose() {texture?.destroy(); output.unconfigure(); device.destroy();}, present() {
                if (!texture) resize(canvas.width, canvas.height);
                device.queue.copyExternalImageToTexture({source: backing}, {texture, colorSpace: capabilities.colorSpace}, [backing.width, backing.height]);
                const commands = device.createCommandEncoder();
                const pass = commands.beginRenderPass({colorAttachments: [{view: output.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1]}]});
                pass.setPipeline(pipeline); pass.setBindGroup(0, group); pass.draw(3); pass.end();
                device.queue.submit([commands.finish()]);
            }};
        } catch {
            hdrDevice?.destroy();
            // SDR/P3 is the fallback; do not advertise an HDR presentation path.
        }
    }
    const visible = canvas.getContext('2d', {alpha: false, colorSpace: capabilities.colorSpace, colorType: 'unorm8'});
    if (!visible) throw new Error('Unable to create the terminal canvas');
    const actual = visible.getContextAttributes?.() || {};
    capabilities.colorSpace = actual.colorSpace === 'display-p3' ? 'display-p3' : 'srgb';
    capabilities.gamut = capabilities.colorSpace === 'display-p3' && matches('(color-gamut: p3)') ? 'p3' : 'srgb';
    capabilities.float16 = false;
    return {ctx: visible, capabilities, resize() {}, present() {}, dispose() {}};
}
