#!/usr/bin/env node
// Actual browser/guest smoke test. Uses a clean Chrome profile and no packages.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {DEMO_GAMES} from './runtime.mjs';

const root = resolve(fileURLToPath(new URL('..', import.meta.url)));
const options = {
    chrome: process.env.CHROME_BIN || (process.platform === 'darwin' ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' : 'chromium'),
    root: join(root, 'html'), page: 'index.html', port: '0', url: '', timeout: 180000, mode: 'all',
    screenshot: join(process.platform === 'darwin' ? '/private/tmp' : tmpdir(), 'silkie-browser-demo.png'),
    report: join(tmpdir(), 'silkie-browser-smoke.json'),
};
for (let i = 2; i < process.argv.length; i += 2) {
    const key = process.argv[i].replace(/^--/, '');
    if (!(key in options) || !process.argv[i + 1]) throw new Error(`Unknown/missing option: ${process.argv[i]}`);
    options[key] = key === 'timeout' ? Number(process.argv[i + 1]) : process.argv[i + 1];
}
if (!['all', 'commands', 'newline', 'signal', 'resize', 'ui', 'links', 'keyboard', 'games', 'launch', 'startup', 'titles', 'loophole-resize', 'chinese'].includes(options.mode)) {
    throw new Error(`Unknown test mode: ${options.mode}`);
}

class CDP {
    constructor(url) {
        this.next = 0;
        this.pending = new Map();
        this.events = [];
        this.ws = new WebSocket(url);
        this.ready = new Promise((resolve, reject) => {
            this.ws.addEventListener('open', resolve, {once: true});
            this.ws.addEventListener('error', reject, {once: true});
        });
        this.ws.addEventListener('message', event => {
            const message = JSON.parse(event.data);
            if (message.id) {
                const waiting = this.pending.get(message.id);
                if (!waiting) return;
                this.pending.delete(message.id);
                clearTimeout(waiting.timer);
                if (message.error) waiting.reject(new Error(JSON.stringify(message.error)));
                else waiting.resolve(message.result);
            } else this.events.push(message);
        });
        this.ws.addEventListener('close', () => {
            for (const waiting of this.pending.values()) {
                clearTimeout(waiting.timer);
                waiting.reject(new Error('Chrome debugging connection closed'));
            }
            this.pending.clear();
        });
    }
    async send(method, params = {}, sessionId = this.sessionId) {
        await this.ready;
        if (this.ws.readyState !== WebSocket.OPEN) throw new Error('Chrome debugging connection is not open');
        const id = ++this.next;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error(`Chrome command timed out: ${method}`));
            }, 30000);
            this.pending.set(id, {resolve, reject, timer});
            this.ws.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
        });
    }
    async evaluate(expression) {
        const result = await this.send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
        if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
        return result.result.value;
    }
}

async function until(action, description, timeout = options.timeout) {
    const deadline = Date.now() + timeout;
    do {
        const value = await action();
        if (value) return value;
        await delay(100);
    } while (Date.now() < deadline);
    throw new Error(`Timed out waiting for ${description}`);
}

async function stop(child) {
    if (!child || child.exitCode !== null || child.signalCode !== null) return;
    const done = new Promise(resolve => child.once('exit', resolve));
    child.kill('SIGTERM');
    const timer = setTimeout(() => child.kill('SIGKILL'), 3000);
    await done;
    clearTimeout(timer);
}

let chrome, server, cdp, profile;
let sequence = 0;
const results = [];
const diagnostics = {browserErrors: [], server: '', chrome: ''};
const sanitize = value => String(value).replace(/https:\/\/login\.tailscale\.com\/[^\s"']+/g, '[Tailscale login URL]');

async function test(name, action) {
    try {
        const detail = await action();
        results.push({name, status: 'pass', detail});
        console.log(`PASS ${name}${detail ? `: ${detail}` : ''}`);
        return true;
    } catch (error) {
        results.push({name, status: 'fail', error: sanitize(error.message)});
        console.log(`FAIL ${name}: ${sanitize(error.message)}`);
        return false;
    }
}

async function output() { return cdp.evaluate('globalThis.__smokeChunks.join("")'); }
async function send(text) {
    await cdp.evaluate(`globalThis.silkieDemo.send(new TextEncoder().encode(${JSON.stringify(text)}))`);
}
async function command(text) {
    const marker = `__SILKIE_SMOKE_${++sequence}__`;
    const before = await cdp.evaluate('globalThis.__smokeChunks.length');
    await send(`${text}; smoke_status=$?; printf '\\n${marker}:%s\\n' "$smoke_status"\r`);
    const expression = `globalThis.__smokeChunks.slice(${before}).join("")`;
    const transcript = await until(async () => {
        const value = await cdp.evaluate(expression);
        return new RegExp(`(?:\\r?\\n)${marker}:\\d+\\r?\\n`).test(value) ? value : false;
    }, `shell command ${sequence} completion`);
    const status = Number(transcript.match(new RegExp(`${marker}:(\\d+)`))[1]);
    assert.equal(status, 0, `Guest command failed (${status}): ${transcript}`);
    return transcript;
}
async function key(key, code, virtualKeyCode) {
    await cdp.send('Input.dispatchKeyEvent', {type: 'keyDown', key, code, windowsVirtualKeyCode: virtualKeyCode});
    await cdp.send('Input.dispatchKeyEvent', {type: 'keyUp', key, code, windowsVirtualKeyCode: virtualKeyCode});
}

try {
    profile = await mkdtemp(join(tmpdir(), 'silkie-browser-smoke-'));
    let url = options.url;
    if (url) {
        url = new URL(url).href;
        if (!url.endsWith('/')) url += '/';
    } else {
        server = spawn('python3', [join(root, 'web/serve.py'), '--port', options.port, '--root', options.root], {stdio: ['ignore', 'pipe', 'pipe']});
        server.stdout.on('data', bytes => { diagnostics.server += bytes; });
        server.stderr.on('data', bytes => { diagnostics.server += bytes; });
        server.on('error', error => { diagnostics.server += error.message; });
        url = await until(() => {
            if (server.exitCode !== null) throw new Error(`HTTP server exited: ${diagnostics.server}`);
            return diagnostics.server.match(/http:\/\/127\.0\.0\.1:\d+\//)?.[0];
        }, 'local server', 10000);
    }
    chrome = spawn(options.chrome, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
        '--no-first-run', '--no-default-browser-check', '--disable-background-networking', '--disable-component-update',
        '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
        '--disable-sync', '--use-mock-keychain', '--password-store=basic', 'about:blank'], {stdio: ['ignore', 'ignore', 'pipe']});
    chrome.stderr.on('data', bytes => { diagnostics.chrome += bytes; });
    chrome.on('error', error => { diagnostics.chrome += error.message; });
    const endpoint = await until(async () => {
        if (chrome.exitCode !== null) throw new Error(`Chrome exited: ${diagnostics.chrome}`);
        try {
            const [port, path] = (await readFile(join(profile, 'DevToolsActivePort'), 'utf8')).trim().split('\n');
            return `ws://127.0.0.1:${port}${path}`;
        } catch (error) { if (error.code !== 'ENOENT') throw error; }
    }, 'Chrome debugging endpoint', 20000);
    cdp = new CDP(endpoint);
    const {targetId} = await cdp.send('Target.createTarget', {url: 'about:blank'});
    const {sessionId} = await cdp.send('Target.attachToTarget', {targetId, flatten: true});
    cdp.sessionId = sessionId;
    await cdp.send('Target.activateTarget', {targetId});
    await cdp.send('Runtime.enable');
    await cdp.send('Page.enable');
    await cdp.send('Page.bringToFront');
    await cdp.send('Page.addScriptToEvaluateOnNewDocument', {source: String.raw`
        globalThis.__smokeChunks = [];
        globalThis.__smokeTrace = [];
        globalThis.__smokeInputs = [];
        const NativeWorker = Worker;
        globalThis.Worker = class extends NativeWorker {
            constructor(...args) {
                super(...args);
                this.addEventListener('message', event => {
                    if (event.data?.type === 77) __smokeTrace.push({type:'host-signal', time:performance.now(), epoch:Date.now(), pid:event.data.pid, value:event.data.value});
                });
            }
        };
        for (const type of ['keydown', 'keyup']) document.addEventListener(type, event => {
            if (event.key === 'q') __smokeTrace.push({type, time: performance.now(), flags:globalThis.silkieDemo?.terminal.keyboardFlags});
        }, true);
        globalThis.__smokeDrawImages = 0;
        const drawImage = CanvasRenderingContext2D.prototype.drawImage;
        CanvasRenderingContext2D.prototype.drawImage = function (...args) {
            if (this.canvas === document.querySelector('#silkie-terminal canvas')) globalThis.__smokeDrawImages++;
            return drawImage.apply(this, args);
        };
        const decoder = new TextDecoder();
        let demo;
        Object.defineProperty(globalThis, 'silkieDemo', {
            get() { return demo; },
            set(session) {
                demo = session;
                const send = session.send.bind(session);
                session.send = bytes => {
                    const text = new TextDecoder().decode(bytes);
                    __smokeInputs.push(text);
                    send(bytes);
                    if (text.startsWith('\x1b]777;browser-input;v=1:op=ack:') && globalThis.__typeAfterHandoff) {
                        const queued = __typeAfterHandoff; __typeAfterHandoff = null;
                        send(new TextEncoder().encode(queued));
                    }
                };
                const createGuest = session.createGuest.bind(session);
                session.createGuest = async () => {
                    const guest = await createGuest();
                    const run = guest.run.bind(guest);
                    guest.run = async (...args) => {
                        const event = {type:'guest-run', time:performance.now(), epoch:Date.now(), path:args[0], args:args[1]};
                        __smokeTrace.push(event);
                        const result = await run(...args);
                        event.completed=performance.now(); event.status=result.status;
                        return result;
                    };
                    return guest;
                };
                const write = session.terminal.write.bind(session.terminal);
                session.terminal.write = bytes => {
                    const text = typeof bytes === 'string' ? bytes : decoder.decode(bytes, {stream: true});
                    globalThis.__smokeChunks.push(text);
                    const controls = text.match(/\x1b\[[<>?][0-9;]*[a-zA-Z]/g);
                    const flagsBefore = session.terminal.keyboardFlags;
                    const result = write(bytes);
                    if (controls) __smokeTrace.push({type:'controls', time:performance.now(), controls, flagsBefore, flagsAfter:session.terminal.keyboardFlags});
                    return result;
                };
            }, configurable: true,
        });
    `});
    await cdp.send('Emulation.setDeviceMetricsOverride', {width: 1440, height: 1100, deviceScaleFactor: 1, mobile: false});
    await cdp.send('Page.navigate', {url: `${url}${options.page}`});
    await until(() => cdp.evaluate('Boolean(document.querySelector("#demo-status"))'), 'homepage terminal');
    console.log(`Browser opened ${url}${options.page}`);
    assert(await cdp.evaluate('isSecureContext && crossOriginIsolated && !!SharedArrayBuffer'));
    await until(async () => {
        const status = await cdp.evaluate('document.querySelector("#demo-status").textContent');
        if (/Reload the page|failed|not been built|needs HTTPS|not available|could not|error/i.test(status)) throw new Error(status);
        return (await output()).includes('\x1b]8;;silkie:tailscale\x1b\\');
    }, 'Linux shell startup');
    console.log('PASS Linux shell auto-started and printed browser restrictions');
    results.push({name: 'Linux shell startup', status: 'pass'});
    await send("stty -echo; printf '\\n__SILKIE_SMOKE_READY__\\n'\r");
    await until(async () => /\r?\n__SILKIE_SMOKE_READY__\r?\n/.test(await output()), 'shell test setup');

    await test('basic Linux commands', async () => {
        const text = await command("printf 'GUEST_OS:'; uname -m; id -u; command -v ssh silkie-sprint silkie-scramble presidential silkie-loophole; ssh -V; printf 'FILE_TEST' > smoke.txt; cat smoke.txt; rm smoke.txt");
        assert.match(text, /GUEST_OS:i[3-6]86/);
        assert.match(text, /1000/);
        assert.match(text, /FILE_TEST/);
        assert.match(text, /OpenSSH_/);
        for (const game of DEMO_GAMES) assert(text.includes(`/usr/local/bin/${game}`), `${game} is not installed`);
        const manifest = await cdp.evaluate('silkieDemo.manifest.games');
        const hashes = await command(`sha256sum ${DEMO_GAMES.map(game => `/usr/local/libexec/${game}`).join(' ')}`);
        for (const game of DEMO_GAMES) {
            assert(hashes.includes(`${manifest[game].sha256}  /usr/local/libexec/${game}`), `${game} differs from the published manifest`);
        }
        return 'i386 shell, user 1000, OpenSSH executable ran, all four game commands installed';
    });
    await test('shell newline rendering', async () => {
        const text = await command("printf 'NEWLINE_ONE\\nNEWLINE_TWO\\n'");
        assert(text.includes('NEWLINE_ONE\nNEWLINE_TWO\n'), 'The guest byte stream must retain its original bare LF bytes');
        await cdp.evaluate('silkieDemo.terminal.flush()');
        const lines = await cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.split("\\n")');
        assert(lines.some(line => line.startsWith('NEWLINE_ONE')), 'First shell line does not start at column zero');
        assert(lines.some(line => line.startsWith('NEWLINE_TWO')), 'Second shell line does not start at column zero');
        return 'bare LF bytes preserved; both displayed lines start at column zero';
    });
    if (options.mode === 'chinese') {
        await test('Chinese shell, controls and language routes', async () => {
            assert.equal(await cdp.evaluate('document.documentElement.lang'), 'zh-CN');
            assert.match(await output(), /浏览器限制：/);
            assert.match(await command('silkie-features'), /运行 silkie-features 可再次显示此报告/);
            assert.equal(await cdp.evaluate('document.querySelector("#demo-status").textContent'), 'Shell 正在浏览器中运行。');
            await cdp.evaluate('silkieDemo.terminal.flush()');
            const initial = await cdp.send('Page.captureScreenshot', {format:'png',captureBeyondViewport:false});
            await writeFile(options.screenshot, Buffer.from(initial.data,'base64'));
            const labels = await cdp.evaluate('Array.from(document.querySelectorAll("#silkie-terminal [aria-label]"), element => element.getAttribute("aria-label"))');
            assert(labels.includes('终端输入'), JSON.stringify(labels));
            assert(labels.includes('终端输出'), JSON.stringify(labels));
            assert.equal(await cdp.evaluate('document.querySelector("#demo-fullscreen").textContent'), '全屏');
            await cdp.send('Runtime.evaluate', {expression:'document.querySelector("#demo-fullscreen").click()',userGesture:true});
            await until(() => cdp.evaluate('Boolean(document.fullscreenElement)'), 'Chinese fullscreen');
            assert.equal(await cdp.evaluate('document.querySelector("#demo-fullscreen").textContent'), '退出全屏');
            await cdp.evaluate('document.exitFullscreen()');
            await until(() => cdp.evaluate('document.querySelector("#demo-fullscreen").textContent === "全屏"'), 'Chinese fullscreen exit');
            const route = await cdp.evaluate(`(() => {
                const open = window.open;
                let opened;
                window.open = url => { opened = url; return {closed:false, focus(){}}; };
                try { document.querySelector('a[data-demo-game]').click(); return opened; }
                finally { window.open = open; }
            })()`);
            assert.equal(new URL(route).pathname, '/zh-CN/browser-demo.html');
            assert(DEMO_GAMES.includes(new URL(route).searchParams.get('game')));
            const english = await cdp.evaluate('document.querySelector("a[hreflang=en]").href');
            assert.equal(new URL(english).pathname, '/index.html');
            await cdp.send('Emulation.setDeviceMetricsOverride', {width:390,height:844,deviceScaleFactor:1,mobile:true});
            await delay(300);
            assert(await cdp.evaluate('document.documentElement.scrollWidth <= innerWidth'), 'Chinese mobile layout overflows');
            const mobileReport = await command('silkie-features');
            assert.match(mobileReport, /silkie-sprint/);
            assert.match(mobileReport, /silkie:tailscale/);
            await cdp.evaluate('silkieDemo.terminal.flush()');
            const shot = await cdp.send('Page.captureScreenshot', {format:'png',captureBeyondViewport:false});
            await writeFile(options.screenshot.replace(/\.png$/, '-mobile.png'), Buffer.from(shot.data,'base64'));
            await cdp.send('Emulation.setDeviceMetricsOverride', {width:1440,height:1100,deviceScaleFactor:1,mobile:false});
            await delay(200);
            await command('silkie-features');
            await cdp.evaluate('silkieDemo.terminal.flush()');
            const desktop = await cdp.send('Page.captureScreenshot', {format:'png',captureBeyondViewport:false});
            await writeFile(options.screenshot, Buffer.from(desktop.data,'base64'));
            return 'Chinese shell report, accessibility labels, fullscreen, game popup route, English switch and mobile layout';
        });
    }
    if (options.mode === 'loophole-resize') {
        await test('cell-size changes notify the guest even when rows and columns stay fixed', async () => {
            const measured = await cdp.evaluate(`(async () => {
                const {createTerminal} = await import('./_static/demo/terminal.mjs');
                const element = document.createElement('div');
                element.style.cssText = 'position:absolute;left:-10000px;top:0';
                document.body.append(element);
                const metrics = size => {
                    const ctx = document.createElement('canvas').getContext('2d');
                    ctx.font = size + 'px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
                    return [Math.round(ctx.measureText('M').width * devicePixelRatio) / devicePixelRatio, Math.ceil(size * 1.35)];
                };
                const size = font => { const [cw,ch] = metrics(font); element.style.width=(64.5*cw)+'px'; element.style.height=(15.5*ch)+'px'; };
                size(14);
                const notifications=[], replies=[];
                const terminal=await createTerminal(element,{fontSize:14,onResize:(...grid)=>notifications.push(grid),onData:bytes=>replies.push(new TextDecoder().decode(bytes))});
                try {
                    const before=notifications.length;
                    size(17); terminal.setFontSize(17);
                    terminal.write('\\x1b[16t'); await terminal.flush();
                    return {before, notifications, replies, pixels:metrics(17).map(value=>Math.round(value*devicePixelRatio))};
                } finally { terminal.dispose(); element.remove(); }
            })()`);
            assert(measured.notifications.length > measured.before, 'Font-only resize did not notify the guest');
            assert(measured.notifications.every(grid => grid[0] === 64 && grid[1] === 15), JSON.stringify(measured));
            assert(measured.replies.includes(`\x1b[6;${measured.pixels[1]};${measured.pixels[0]}t`), JSON.stringify(measured));
            return 'same 64 × 15 grid, new physical cell dimensions, guest notified and query answered';
        });
        await test('Loophole sections stay contiguous through fullscreen and repeated resizing', async () => {
            await cdp.evaluate(`(async () => {
                const {KittyGraphics} = await import('./_static/demo/terminal.mjs');
                const draw = KittyGraphics.prototype.draw;
                KittyGraphics.prototype.draw = function(ctx, layer, cw, ch, dpr, offset) {
                    if (layer === 'behind-background') {
                        const rects = [...this.placements.values()].filter(p => Number(p.z) === -2000000000)
                            .map(p => this.placement(p, this.images.get(p.imageId), cw, ch, dpr, offset));
                        globalThis.__loopholeGeometry = {rects, cw, ch};
                    }
                    return draw.call(this, ctx, layer, cw, ch, dpr, offset);
                };
            })()`);
            const marker = `__LOOPHOLE_RESIZE_${++sequence}__`;
            await send(`silkie-loophole --seed 42; printf '\\n${marker}\\n'\r`);
            await until(()=>cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("SILKIE LOOPHOLE")'), 'Loophole compact title');
            await cdp.evaluate('silkieDemo.terminal.focus()');
            await key('Enter','Enter',13);
            await until(()=>cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("breath")'), 'Loophole compact gameplay');
            const geometry = () => cdp.evaluate(`(() => {
                const g = globalThis.__loopholeGeometry;
                if (!g?.rects.length) return null;
                const width = silkieDemo.terminal.cols * g.cw;
                const top = g.ch, bottom = (silkieDemo.terminal.rows - 1) * g.ch;
                const edges = [...new Set([top, bottom, ...g.rects.flatMap(r => [Math.max(top,r.y), Math.min(bottom,r.y+r.height)])])].filter(y=>y>=top && y<=bottom).sort((a,b)=>a-b);
                const gaps = [];
                for (let i=1; i<edges.length; i++) {
                    if (edges[i]-edges[i-1]<1e-6) continue;
                    const y=(edges[i]+edges[i-1])/2;
                    const spans=g.rects.filter(r=>r.y<=y && r.y+r.height>=y).map(r=>[Math.max(0,r.x),Math.min(width,r.x+r.width)]).sort((a,b)=>a[0]-b[0]);
                    let end=0;
                    for(const [a,b] of spans) {if(a>end+1e-6) gaps.push({x:end,width:a-end,y});end=Math.max(end,b);}
                    if(end<width-1e-6) gaps.push({x:end,width:width-end,y});
                }
                return {gaps, cols:silkieDemo.terminal.cols,rows:silkieDemo.terminal.rows,font:silkieDemo.terminal.fontSize, ...g};
            })()`);
            await until(async()=>{const g=await geometry();return g && !g.gaps.length;},'contiguous compact scenery');
            for (let round=0; round<4; round++) {
                await cdp.send('Runtime.evaluate',{expression:'document.querySelector("#demo-fullscreen").click()',userGesture:true});
                const fullscreen = round % 2 === 0;
                await until(()=>cdp.evaluate(`Boolean(document.fullscreenElement) === ${fullscreen} && silkieDemo.terminal.fontSize === ${fullscreen ? 17 : 14}`), 'fullscreen font transition');
                const started = Date.now();
                const g = await until(async () => {
                    await cdp.evaluate('silkieDemo.terminal.flush()');
                    const measured = await geometry();
                    return measured && !measured.gaps.length ? measured : false;
                }, 'contiguous scenery after fullscreen transition');
                g.settledMs = Date.now() - started;
                diagnostics[`loophole-resize-${round}`] = g;
                const screenshot = await cdp.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
                await writeFile(options.screenshot.replace(/\.png$/,`-resize-${round}.png`),Buffer.from(screenshot.data,'base64'));
                assert(g && !g.gaps.length, `Scenery gaps after ${fullscreen ? 'entering' : 'leaving'} fullscreen: ${JSON.stringify(g?.gaps)}`);
            }
            await cdp.evaluate('silkieDemo.terminal.focus()');
            await cdp.send('Input.dispatchKeyEvent',{type:'keyDown',key:'c',code:'KeyC',windowsVirtualKeyCode:67,modifiers:2});
            await cdp.send('Input.dispatchKeyEvent',{type:'keyUp',key:'c',code:'KeyC',windowsVirtualKeyCode:67,modifiers:2});
            await until(()=>cdp.evaluate(`document.querySelector('#silkie-terminal canvas').textContent.includes('${marker}')`),'Loophole resize-test exit');
            return 'compact gameplay and four fullscreen transitions have complete scenery coverage';
        });
    }
    if (options.mode === 'titles') {
        const homepageSession = cdp.sessionId;
        for (const game of DEMO_GAMES) await test(`${game} published title`, async () => {
            const target = new URL('browser-demo.html', url);
            target.searchParams.set('game', game);
            const {targetId: gameTarget} = await cdp.send('Target.createTarget', {url:target.href});
            try {
                const attached = await cdp.send('Target.attachToTarget', {targetId:gameTarget,flatten:true});
                cdp.sessionId = attached.sessionId;
                await cdp.send('Runtime.enable'); await cdp.send('Page.enable');
                await cdp.send('Target.activateTarget', {targetId:gameTarget});
                await cdp.send('Page.bringToFront');
                const title = game === 'presidential' ? 'New campaign' : game === 'silkie-loophole' ? 'SILKIE LOOPHOLE' : 'Enter:';
                await until(()=>cdp.evaluate(`globalThis.silkieDemo?.terminal.keyboardFlags > 0 && document.querySelector('#silkie-terminal canvas').textContent.includes(${JSON.stringify(title)})`), `${game} title`);
                if (game !== 'presidential') await until(()=>cdp.evaluate('silkieDemo.terminal.graphicsStatistics.placements > 0'), `${game} title graphics`);
                assert.equal(await cdp.evaluate('document.documentElement.dataset.demoGame'), game);
                return 'automatic launch and title rendered';
            } finally {
                cdp.sessionId = homepageSession;
                await cdp.send('Target.closeTarget', {targetId:gameTarget});
            }
        });
    }
    if (options.mode === 'all' || options.mode === 'ui') {
    await test('homepage layout, compact font and true fullscreen', async () => {
        const compact = await cdp.evaluate(`(() => {
            const box = document.querySelector('#demo-panel').getBoundingClientRect();
            return {ratio:box.width/box.height,font:silkieDemo.terminal.fontSize,header:!!document.querySelector('.site-header'),sidebar:!!document.querySelector('.sidebar-drawer')};
        })()`);
        assert(compact.header); assert(Math.abs(compact.ratio - 4/3) < .01, JSON.stringify(compact));
        assert.equal(compact.font, 14);
        await cdp.send('Runtime.evaluate', {expression:'document.querySelector("#demo-fullscreen").click()',userGesture:true});
        await until(() => cdp.evaluate('document.fullscreenElement?.id === "demo-panel" && silkieDemo.terminal.fontSize === 17'), 'fullscreen and larger font');
        await cdp.evaluate('document.exitFullscreen()');
        await until(() => cdp.evaluate('!document.fullscreenElement && silkieDemo.terminal.fontSize === 14'), 'normal window and font restored');
        return 'homepage template, automatic boot, 4:3 window; 14px normal / 17px fullscreen';
    });
    await test('drag selection and native copy event', async () => {
        await command("printf 'COPY_THIS_TEXT\\n'");
        await cdp.evaluate('document.querySelector("#demo-panel").scrollIntoView({block:"center",behavior:"instant"})');
        const points = await cdp.evaluate(`(() => {
            const canvas=document.querySelector('#silkie-terminal canvas'),box=canvas.getBoundingClientRect();
            const lines=canvas.textContent.split('\\n'),row=lines.findIndex(line=>line.startsWith('COPY_THIS_TEXT'));
            const cw=box.width/silkieDemo.terminal.cols,ch=box.height/silkieDemo.terminal.rows;
            return {x:box.left+cw/2,end:box.left+cw*13.5,y:box.top+ch*(row+.5),row};
        })()`);
        assert(points.row >= 0);
        await cdp.send('Input.dispatchMouseEvent',{type:'mousePressed',x:points.x,y:points.y,button:'left',clickCount:1});
        await cdp.send('Input.dispatchMouseEvent',{type:'mouseMoved',x:points.end,y:points.y,button:'left',buttons:1});
        await cdp.send('Input.dispatchMouseEvent',{type:'mouseReleased',x:points.end,y:points.y,button:'left',clickCount:1});
        const copied=await cdp.evaluate(`(() => {
            const data=new DataTransfer();
            document.querySelector('#silkie-terminal textarea').dispatchEvent(new ClipboardEvent('copy',{clipboardData:data,bubbles:true,cancelable:true}));
            return data.getData('text/plain');
        })()`);
        assert.equal(copied.trim(),'COPY_THIS_TEXT');
        await command("printf 'AFTER_SELECTION'");
        return 'selected terminal text copied through production clipboard handler';
    });
    }
    if (options.mode === 'newline') {
        const text = await command("stty opost onlcr; printf 'LINE1\\nLINE2\\n'; stty -a");
        diagnostics.newline = text;
        console.log('ONLCR diagnostic:', JSON.stringify(text));
    }
    if (options.mode === 'signal') {
        const text = await command("type kill; printf 'SHELL_PID:%s\\n' $$; trap 'printf SELF_WINCH_OBSERVED' WINCH; kill -s WINCH $$");
        const pid = text.match(/SHELL_PID:(\d+)/)?.[1];
        console.log('Signal diagnostic:', JSON.stringify(text));
        assert(pid, 'Shell did not print its PID');
        assert(text.includes('SELF_WINCH_OBSERVED'));
        for (const [target, label] of [[pid, 'SDK_DIRECT_WINCH'], ['-1', 'SDK_ALL_WINCH']]) {
            await test(label, async () => {
                await command(`trap 'printf ${label}_OBSERVED' WINCH`);
                const result = await cdp.evaluate(`silkieDemo.guest.run('/bin/kill', ['-s','WINCH',${JSON.stringify(target)}], {uid:1000,gid:1000})`);
                diagnostics[label] = result;
                console.log(label, 'helper status', JSON.stringify(result));
                await until(async () => (await output()).includes(`${label}_OBSERVED`), label, 5000);
                return `target=${target}, helper status=${result.status}`;
            });
        }
    }
    if (options.mode === 'all' || options.mode === 'launch') {
    const mainSession=cdp.sessionId;
    const mainWindow=await cdp.send('Browser.getWindowForTarget',{targetId},null);
    const popups=[];
    try {
        // Check the new arcade game's rendering before the simulation's setup.
        for (const game of ['silkie-loophole', ...DEMO_GAMES.filter(name => name !== 'silkie-loophole')]) {
            console.log('Starting Linux game window', game);
            cdp.sessionId=mainSession;
            await cdp.send('Target.activateTarget',{targetId});
            await cdp.evaluate(`document.querySelector('a[data-demo-game="${game}"]').scrollIntoView({block:'center',behavior:'instant'})`);
            const point=await cdp.evaluate(`(() => {const b=document.querySelector('a[data-demo-game="${game}"]').getClientRects()[0]; return {x:b.left+b.width/2,y:b.top+b.height/2}})()`);
            await cdp.send('Input.dispatchMouseEvent',{type:'mousePressed',...point,button:'left',clickCount:1});
            await cdp.send('Input.dispatchMouseEvent',{type:'mouseReleased',...point,button:'left',clickCount:1});
            const popup=await until(async()=>{
                const {targetInfos}=await cdp.send('Target.getTargets');
                return targetInfos.find(t=>t.url.includes('browser-demo.html?game='+game));
            },'game popup '+game);
            const attached=await cdp.send('Target.attachToTarget',{targetId:popup.targetId,flatten:true});
            const popupWindow=await cdp.send('Browser.getWindowForTarget',{targetId:popup.targetId},null);
            assert.notEqual(popupWindow.windowId,mainWindow.windowId,'Game must open in its own window');
            popups.push({game,targetId:popup.targetId,sessionId:attached.sessionId});
            cdp.sessionId=attached.sessionId;
            await cdp.send('Runtime.enable'); await cdp.send('Page.enable');
            await cdp.send('Target.activateTarget', {targetId:popup.targetId});
            await cdp.send('Page.bringToFront');
            const readyText = game === 'presidential' ? 'New campaign' : game === 'silkie-loophole' ? 'SILKIE LOOPHOLE' : 'Enter:';
            await until(()=>cdp.evaluate(`globalThis.silkieDemo?.terminal.keyboardFlags > 0 && document.querySelector("#silkie-terminal canvas").textContent.includes(${JSON.stringify(readyText)})`),'automatic game start '+game);
            console.log('PASS automatic game start', game);
            await cdp.evaluate(`(() => {
                globalThis.__gameOutput = [];
                const write = silkieDemo.terminal.write.bind(silkieDemo.terminal), decoder = new TextDecoder();
                silkieDemo.terminal.write = bytes => {
                    __gameOutput.push(typeof bytes === 'string' ? bytes : decoder.decode(bytes, {stream:true}));
                    return write(bytes);
                };
            })()`);
            const state=await cdp.evaluate('({game:document.documentElement.dataset.demoGame,running:silkieDemo.running,cols:silkieDemo.terminal.cols,rows:silkieDemo.terminal.rows,header:getComputedStyle(document.querySelector(".site-header")).display,panel:document.querySelector("#demo-panel").getBoundingClientRect().height,viewport:innerHeight,scroll:document.documentElement.scrollHeight})');
            assert.equal(state.game,game); assert(state.running); assert.equal(state.header,'none');
            assert(Math.abs(state.panel-state.viewport)<1,JSON.stringify(state));
            assert(state.scroll<=state.viewport,JSON.stringify(state));
            await key('Enter','Enter',13);
            if (game === 'presidential') {
                await until(()=>cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("NEW CAMPAIGN")'),'campaign setup');
                await key('Enter','Enter',13);
            }
            const playText = game === 'presidential' ? 'Control room' : game === 'silkie-loophole' ? 'breath' : 'Level 1';
            await until(async () => {
                const panic = await cdp.evaluate(`__gameOutput.join('').match(/(?:thread[^\\n]*panicked|fatal runtime error|memory allocation of)[\\s\\S]*/)?.[0]`);
                if (panic) throw new Error(`${game} crashed: ${panic}`);
                return cdp.evaluate(`document.querySelector("#silkie-terminal canvas").textContent.includes(${JSON.stringify(playText)})`);
            },'play from popup '+game);
            if (game === 'presidential') {
                await key('g', 'KeyG', 71);
                await until(()=>cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("GO: COMMIT AND ADVANCE ONE DAY")'), 'campaign day confirmation');
                await key('Enter', 'Enter', 13);
                await until(async () => {
                    const panic = await cdp.evaluate(`__gameOutput.join('').match(/(?:thread[^\\n]*panicked|fatal runtime error|memory allocation of)[\\s\\S]*/)?.[0]`);
                    if (panic) throw new Error(`presidential crashed advancing a day: ${panic}`);
                    return cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("DAY 2")');
                }, 'campaign advances and saves day two');
                console.log('PASS Presidential full campaign initialization and day advancement');
            }
            await cdp.send('Runtime.evaluate',{expression:'document.querySelector("#demo-fullscreen").click()',userGesture:true});
            await until(()=>cdp.evaluate('document.fullscreenElement?.id === "demo-panel" && silkieDemo.terminal.fontSize === 17'),'game fullscreen '+game);
            await cdp.evaluate('document.exitFullscreen()');
            await until(()=>cdp.evaluate('!document.fullscreenElement && silkieDemo.terminal.fontSize === 14'),'game fullscreen exit '+game);
            const shot=await cdp.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
            await writeFile('/private/tmp/'+game+'-launch-window.png',Buffer.from(shot.data,'base64'));
            console.log('PASS game launch window',game,JSON.stringify(state));
            if (game !== 'presidential') {
                await key('p','KeyP',80);
                await until(()=>cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("Paused")'),'pause background game '+game);
            }
        }
        cdp.sessionId=mainSession;
        await cdp.send('Target.activateTarget',{targetId});
        const homepage=await command("printf 'HOMEPAGE_ALONGSIDE_GAMES\\n'");
        assert(homepage.includes('HOMEPAGE_ALONGSIDE_GAMES'));
        const sprint=popups.find(p=>p.game==='silkie-sprint');
        await cdp.send('Runtime.evaluate',{expression:'document.querySelector("a[data-demo-game=silkie-sprint]").click()',userGesture:true});
        const after=await cdp.send('Target.getTargets');
        assert.equal(after.targetInfos.filter(t=>t.url.includes('game=silkie-sprint')).length,1);
        assert(after.targetInfos.some(t=>t.targetId===sprint.targetId));
        cdp.sessionId=sprint.sessionId;
        assert(await cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("Level 1")'),'Repeated click must preserve the game instead of restarting it');
        console.log('PASS homepage and all games run together; repeated click focuses existing game');
        for (const popup of popups) {
            cdp.sessionId=popup.sessionId;
            await cdp.send('Target.activateTarget',{targetId:popup.targetId});
            await cdp.evaluate('silkieDemo.terminal.focus()');
            await cdp.send('Input.dispatchKeyEvent',{type:'keyDown',key:'c',code:'KeyC',windowsVirtualKeyCode:67,modifiers:2});
            await cdp.send('Input.dispatchKeyEvent',{type:'keyUp',key:'c',code:'KeyC',windowsVirtualKeyCode:67,modifiers:2});
            if (popup.game === 'presidential') {
                await until(()=>cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("Quit?")'),'campaign quit confirmation');
                await key('y','KeyY',89);
            }
            await until(()=>cdp.evaluate('!silkieDemo.running && !document.querySelector("#demo-start").hidden'),'game exit '+popup.game);
            assert.equal(await cdp.evaluate('document.querySelector("#demo-start").textContent'),'Play again');
            if (['presidential', 'silkie-loophole'].includes(popup.game)) {
                for (let round = 0; round < 3; round++) {
                    await cdp.evaluate('document.querySelector("#demo-start").click()');
                    const text = popup.game === 'presidential' ? 'Continue the campaign' : 'SILKIE LOOPHOLE';
                    await until(()=>cdp.evaluate(`silkieDemo.running && silkieDemo.terminal.keyboardFlags > 0 && document.querySelector("#silkie-terminal canvas").textContent.includes(${JSON.stringify(text)})`),'restart saved game '+popup.game);
                    await key('q','KeyQ',81);
                    if (popup.game === 'presidential') {
                        await until(()=>cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("Quit Presidential?")'),'restart quit confirmation');
                        await key('y','KeyY',89);
                    }
                    await until(()=>cdp.evaluate('!silkieDemo.running'),'repeated exit '+popup.game);
                }
                console.log('PASS repeated game restart and saved state',popup.game);
            }
        }
    } finally {
        cdp.sessionId=mainSession;
        for (const popup of popups) await cdp.send('Target.closeTarget',{targetId:popup.targetId});
        await cdp.send('Target.activateTarget',{targetId});
    }
    for (const [width,height,mobile] of [[1280,900,false],[390,844,true],[360,640,true],[1440,1100,false]]) {
        await cdp.send('Emulation.setDeviceMetricsOverride',{width,height,deviceScaleFactor:1,mobile});
        await delay(200);
        const cols=await cdp.evaluate('silkieDemo.terminal.cols');
        const text=await command('silkie-features');
        const plain=text.replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g,'').replace(/\x1b\[[0-?]*[ -/]*[@-~]/g,'');
        for (const line of plain.split(/\r?\n/).filter(line=>!line.includes('__SILKIE_SMOKE_')&&!line.includes('silkie:'))) {
            assert([...line.replace(/\r/g,'')].length<=cols,JSON.stringify({cols,line}));
        }
        await cdp.evaluate('silkieDemo.terminal.flush()');
        assert(await cdp.evaluate('silkieDemo.terminal.links().some(link=>link.uri==="silkie:tailscale")'));
        console.log('PASS width-aware guest report',width,cols);
        await cdp.evaluate('document.querySelector("#demo-panel").scrollIntoView({block:"center",behavior:"instant"})');
        const shot=await cdp.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
        await writeFile(`/private/tmp/silkie-width-report-${width}.png`,Buffer.from(shot.data,'base64'));
    }
    await cdp.send('Runtime.evaluate',{expression:'document.querySelector("#demo-fullscreen").click()',userGesture:true});
    await until(()=>cdp.evaluate('!!document.fullscreenElement && silkieDemo.terminal.fontSize===17'),'fullscreen report size');
    const fullscreenColumns=await cdp.evaluate('silkieDemo.terminal.cols');
    const fullscreenReport=await command('silkie-features');
    const fullscreenText=fullscreenReport.replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g,'').replace(/\x1b\[[0-?]*[ -/]*[@-~]/g,'');
    for (const line of fullscreenText.split(/\r?\n/).filter(line=>!line.includes('__SILKIE_SMOKE_')&&!line.includes('silkie:'))) {
        assert([...line.replace(/\r/g,'')].length<=fullscreenColumns,JSON.stringify({fullscreenColumns,line}));
    }
    await cdp.evaluate('document.exitFullscreen()');
    console.log('PASS width-aware guest report in fullscreen',fullscreenColumns);
    }
    if (options.mode === 'all' || options.mode === 'resize') {
    await test('Unicode and astral input', async () => {
        const expected = 'SILKIE_UNICODE: café 中文 🐔';
        const before = await cdp.evaluate('__smokeChunks.length');
        await cdp.evaluate('globalThis.silkieDemo.terminal.focus()');
        await cdp.send('Input.insertText', {text: `printf '%s\\n' '${expected}'`});
        await key('Enter', 'Enter', 13);
        const actual = await until(async () => {
            const text = await cdp.evaluate(`__smokeChunks.slice(${before}).join("")`);
            return /SILKIE_UNICODE[^\n]*\n/.test(text) ? text : false;
        }, 'Unicode echo', 15000);
        diagnostics.unicode = actual;
        assert(actual.includes(expected), `Expected ${JSON.stringify(expected)}, received ${JSON.stringify(actual)}`);
        return 'UTF-8 text entered through actual textarea input';
    });
    await test('terminal resize and SIGWINCH', async () => {
        await command("trap 'printf WINCH_OBSERVED' WINCH");
        const previous = await cdp.evaluate('[silkieDemo.terminal.cols,silkieDemo.terminal.rows]');
        await cdp.send('Emulation.setDeviceMetricsOverride', {width: 1200, height: 900, deviceScaleFactor: 1, mobile: false});
        const dimensions = await until(async () => {
            const next = await cdp.evaluate('[silkieDemo.terminal.cols,silkieDemo.terminal.rows]');
            return next.join() !== previous.join() ? next : false;
        }, 'terminal resize');
        const text = await command('stty size');
        assert.match(text, new RegExp(`${dimensions[1]} ${dimensions[0]}`));
        await until(async () => (await output()).includes('WINCH_OBSERVED'), 'shell SIGWINCH', 10000);
        await command('trap - WINCH');
        return `${dimensions[0]} columns × ${dimensions[1]} rows; signal received`;
    });
    }
    if (options.mode === 'all' || options.mode === 'keyboard') {
    const receivedKeyboard = await test('guest receives held-key press, repeat, release, and focus-loss release', async () => {
        const exercise = async (expected, actions) => {
            const marker = `__KEY_BYTES_${++sequence}__`;
            const before = await cdp.evaluate('__smokeChunks.length');
            const inputsBefore = await cdp.evaluate('__smokeInputs.length');
            const script = `printf '\\033[>11u'; IFS= read -r -N ${expected.length} keys; printf '\\033[<u'; printf '${marker}'; printf '%s' "$keys" | od -An -tx1; printf '\\n${marker}_END\\n'`;
            const quoted = "'" + script.replaceAll("'", "'\\''") + "'";
            await send(`/usr/local/libexec/silkie-console-session /bin/bash -c ${quoted}\r`);
            await until(() => cdp.evaluate('silkieDemo.terminal.keyboardFlags === 11'), 'guest keyboard receiver');
            await cdp.evaluate('silkieDemo.terminal.focus()');
            await actions();
            const transcript = await until(async () => {
                const text = await cdp.evaluate(`__smokeChunks.slice(${before}).join("")`);
                return text.includes(`${marker}_END\n`) ? text : false;
            }, 'guest keyboard bytes');
            const hex = transcript.split(marker)[1].replace(/[^\da-f]/g, '');
            assert.equal(Buffer.from(hex, 'hex').toString(), expected);
            await until(() => cdp.evaluate('silkieDemo.terminal.keyboardFlags === 0'), 'receiver restored keyboard');
            await until(() => cdp.evaluate(`__smokeInputs.slice(${inputsBefore}).some(text=>text.startsWith("\\x1b]777;browser-input;v=1:op=ack:"))`), 'receiver input handoff');
            await command(':');
        };
        await exercise('\x1b[1;1:1A\x1b[1;1:2A\x1b[1;1:3A\x1b[119;1:1u\x1b[119;1:3u\x1b[32;1:1u\x1b[32;1:3u', async () => {
            await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38});
            await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38,autoRepeat:true});
            await cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key:'ArrowUp',code:'ArrowUp',windowsVirtualKeyCode:38});
            await key('w', 'KeyW', 87);
            await key(' ', 'Space', 32);
        });
        await exercise('\x1b[1;1:1D\x1b[1;1:3D', async () => {
            await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:'ArrowLeft',code:'ArrowLeft',windowsVirtualKeyCode:37});
            await cdp.evaluate('document.querySelector("#silkie-terminal textarea").blur()');
            await cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key:'ArrowLeft',code:'ArrowLeft',windowsVirtualKeyCode:37});
        });
        await exercise('\x1b[32;1:1u\x1b[32;1:3u', async () => {
            const beforeTabSwitch = await cdp.evaluate('__smokeInputs.length');
            await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:' ',code:'Space',windowsVirtualKeyCode:32});
            const alternate = await cdp.send('Target.createTarget', {url:'about:blank'});
            await cdp.send('Target.activateTarget', {targetId:alternate.targetId});
            await until(() => cdp.evaluate(`__smokeInputs.slice(${beforeTabSwitch}).includes("\\x1b[32;1:3u")`), 'release after switching tabs');
            await cdp.send('Target.activateTarget', {targetId});
            await cdp.send('Target.closeTarget', {targetId:alternate.targetId});
            await cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key:' ',code:'Space',windowsVirtualKeyCode:32});
        });
        return 'exact bytes checked inside Linux for arrows, WASD, space, repeat, release, and focus cancellation';
    });
    if (!receivedKeyboard) throw new Error('Guest keyboard receiver failed; stopping before sending more input.');
    const cleanKeyboardExits = await test('repeated game exits isolate queued and late keyboard releases from Bash', async () => {
        let held = false;
        for (let round = 0; round < 20; round++) {
            const game = round % 2 ? 'silkie-scramble' : 'silkie-sprint';
            const marker = `__KEY_EXIT_${round}__`;
            const before = await cdp.evaluate('__smokeChunks.length');
            await send(`${game} --kitty-only --seed 42; smoke_status=$?; printf '\\n${marker}:%s\\n' "$smoke_status"\r`);
            await until(() => cdp.evaluate('silkieDemo.terminal.keyboardFlags === 11'), `${game} release negotiation`);
            await until(() => cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("Enter:")'), `${game} title`);
            await cdp.evaluate('silkieDemo.terminal.focus()');
            if (held) {
                const inputs = await cdp.evaluate('__smokeInputs.length');
                await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:'q',code:'KeyQ',windowsVirtualKeyCode:81,autoRepeat:true});
                await cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key:'q',code:'KeyQ',windowsVirtualKeyCode:81});
                assert.equal(await cdp.evaluate('__smokeInputs.length'), inputs, 'old held key must not repeat/release into the next game');
                held = false;
            }
            if (round % 3 === 0) {
                await cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:'q',code:'KeyQ',windowsVirtualKeyCode:81});
                held = true; // Keep it physically held across the next launch.
            } else if (round % 3 === 1) {
                await Promise.all([
                    cdp.send('Input.dispatchKeyEvent', {type:'keyDown',key:'q',code:'KeyQ',windowsVirtualKeyCode:81}),
                    cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key:'q',code:'KeyQ',windowsVirtualKeyCode:81}),
                ]);
            } else {
                await cdp.evaluate(`globalThis.__typeAfterHandoff = "printf 'HANDOFF_TYPEAHEAD_${round}\\n'\\r"`);
                await key('q', 'KeyQ', 81);
            }
            const transcript = await until(async () => {
                const text = await cdp.evaluate(`__smokeChunks.slice(${before}).join("")`);
                return text.includes(`${marker}:0\n`) ? text : false;
            }, `${game} clean exit`, 30000);
            assert(!transcript.includes('command not found'), transcript.slice(-1500));
            assert.equal(await cdp.evaluate('silkieDemo.terminal.keyboardFlags'), 0);
            assert.deepEqual(await cdp.evaluate('silkieDemo.terminal.graphicsStatistics'), {images:0,vectorImages:0,placements:0}, 'retired game graphics cannot cover the shell');
            if (round % 3 === 2) await until(async () => (await output()).includes(`HANDOFF_TYPEAHEAD_${round}\n`), 'post-ACK shell typeahead');
            const shell = await command(`printf 'POST_RELEASE_${round}\\n'`);
            assert.match(shell, new RegExp(`POST_RELEASE_${round}`));
            assert(!shell.includes('command not found'), shell);
        }
        if (held) await cdp.send('Input.dispatchKeyEvent', {type:'keyUp',key:'q',code:'KeyQ',windowsVirtualKeyCode:81});
        return '20 alternating game launches: rapid down/up, keys held across exits, and shell typeahead immediately after ACK';
    });
    if (!cleanKeyboardExits) throw new Error('Keyboard exit handoff failed; stopping before sending more input.');
    }
    if (options.mode === 'startup') {
        await cdp.evaluate(`(async () => {
            const {KittyGraphics} = await import('./_static/demo/terminal.mjs');
            const {VectorScene} = await import('./_static/demo/vector.mjs');
            globalThis.__startupWork = [];
            for (const [prototype, method] of [[KittyGraphics.prototype, 'command'], [VectorScene.prototype, 'rasterize']]) {
                const original = prototype[method];
                prototype[method] = function (...args) {
                    const start = performance.now();
                    const record = () => __startupWork.push({method, start, ms: performance.now() - start,
                        header: method === 'command' ? args[0].split(';')[0] : args.slice(0, 2)});
                    const result = original.apply(this, args);
                    if (result?.then) return result.finally(record);
                    record(); return result;
                };
            }
        })()`);
        for (const [size, width, height, scale] of [['fullscreen-retina',1920,1080,2], ['fullscreen',1920,1080,1], ['compact',1200,900,1]]) for (const game of ['silkie-sprint', 'silkie-scramble']) {
            await test(`${game} title at ${size}`, async () => {
                await cdp.send('Emulation.setDeviceMetricsOverride', {width,height,deviceScaleFactor:scale,mobile:false});
                if (size.startsWith('fullscreen') && !await cdp.evaluate('Boolean(document.fullscreenElement)')) {
                    await cdp.send('Runtime.evaluate',{expression:'document.querySelector("#demo-fullscreen").click()',userGesture:true});
                    await until(()=>cdp.evaluate('Boolean(document.fullscreenElement) && silkieDemo.terminal.fontSize === 17'), 'startup fullscreen');
                } else if (size === 'compact' && await cdp.evaluate('Boolean(document.fullscreenElement)')) await cdp.evaluate('document.exitFullscreen()');
                await delay(200);
                if (size === 'compact') await cdp.evaluate('silkieDemo.terminal.resize(64, 15)');
                const marker = `__SILKIE_STARTUP_${++sequence}__`;
                const start = await cdp.evaluate('performance.now()');
                const before = await cdp.evaluate('__startupWork.length');
                const drawsBefore = await cdp.evaluate('__smokeDrawImages');
                await send(`${game} --seed 42; printf '\\n${marker}\\n'\r`);
                await until(() => cdp.evaluate(`__smokeDrawImages > ${drawsBefore} && silkieDemo.terminal.graphicsStatistics.vectorImages > 0`), `${game} title graphics`);
                const firstGraphics = await cdp.evaluate(`performance.now() - ${start}`);
                await delay(1000);
                await cdp.evaluate('silkieDemo.terminal.flush()');
                const text = await cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent');
                diagnostics[`${game}-${size}`] = {firstGraphics, text, geometry: await cdp.evaluate('({cols:silkieDemo.terminal.cols,rows:silkieDemo.terminal.rows,dpr:devicePixelRatio})'), work: await cdp.evaluate(`__startupWork.slice(${before})`)};
                const screenshot = await cdp.send('Page.captureScreenshot', {format:'png'});
                await writeFile(options.screenshot.replace(/\.png$/, `-${game}-${size}.png`), Buffer.from(screenshot.data, 'base64'));
                await key('q', 'KeyQ', 81);
                await until(() => cdp.evaluate(`document.querySelector('#silkie-terminal canvas').textContent.includes('${marker}')`), `${game} title quit`);
                const normalized = text.replace(/ +/g, ' ');
                assert(normalized.includes('Press Enter to play'), text);
                assert(normalized.includes(game === 'silkie-scramble' ? 'Gather the chicks. Mind the fox. Bring them home.' : 'Chase bugs, dodge prams, mind the hawk.'), text);
                if (game === 'silkie-sprint') assert(normalized.includes('Loki and Skaði only want cuddles'), text);
                return `first graphics in ${(firstGraphics / 1000).toFixed(2)}s; complete menu text`;
            });
        }
    }
    if (options.mode === 'all' || options.mode === 'games') {
    await cdp.send('Emulation.setDeviceMetricsOverride', {width:1200,height:900,deviceScaleFactor:1,mobile:false});
    await delay(200);
    let gameNumber = 0;
    for (const game of ['silkie-sprint', 'silkie-scramble', 'silkie-sprint', 'silkie-scramble']) {
        gameNumber++;
        await test(`${game} capability report`, async () => {
            const text = await command(`${game} --report`);
            assert.match(text, /graphics|Graphics|kitty/);
            return 'native guest executable completed capability discovery';
        });
        const played = await test(`${game} interactive graphics and quit`, async () => {
            const marker = `__SILKIE_GAME_${++sequence}__`;
            const before = await cdp.evaluate('__smokeChunks.length');
            const drawsBefore = await cdp.evaluate('__smokeDrawImages');
            const traceBefore = await cdp.evaluate('__smokeTrace.length');
            await send(`${game} --seed 42; smoke_status=$?; printf '\\n${marker}:%s\\n' "$smoke_status"\r`);
            await until(() => cdp.evaluate(`__smokeChunks.slice(${before}).join("").includes("\\x1b_G")`), `${game} bitmap output`);
            await until(() => cdp.evaluate(`__smokeDrawImages - ${drawsBefore} >= 2`), `${game} displayed bitmap frames`);
            await cdp.evaluate('silkieDemo.terminal.focus()');
            await key('Enter', 'Enter', 13);
            await until(() => cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("Level 1")'), `${game} entering play`);
            await until(() => cdp.evaluate('silkieDemo.terminal.graphicsStatistics.vectorImages > 0'), `${game} retained vector scenery`);
            if (game === 'silkie-scramble') {
                // Scramble has a timed Ready phase that deliberately ignores
                // movement. Wait for its instructions to clear before moving.
                await until(() => cdp.evaluate('!document.querySelector("#silkie-terminal canvas").textContent.includes("Seven chicks are out on the lawn")'), `${game} ready countdown`);
            }
            assert.equal(await cdp.evaluate('silkieDemo.terminal.keyboardFlags'), 11, 'Guest must grant press/repeat/release reporting');
            const playDraws = await cdp.evaluate('__smokeDrawImages');
            await key('ArrowUp', 'ArrowUp', 38);
            await key('ArrowRight', 'ArrowRight', 39);
            await key(' ', 'Space', 32);
            await until(() => cdp.evaluate(`__smokeDrawImages > ${playDraws} + 20`), `${game} continued gameplay drawing`);
            const beforeResize = await cdp.evaluate('[silkieDemo.terminal.cols,silkieDemo.terminal.rows,__smokeDrawImages]');
            // The documentation column has a maximum width. Vary height too,
            // ensuring each viewport change really changes the terminal grid.
            await cdp.send('Emulation.setDeviceMetricsOverride', {width: gameNumber % 2 ? 1320 : 1200, height: gameNumber % 2 ? 1000 : 900, deviceScaleFactor: 1, mobile: false});
            await until(() => cdp.evaluate(`silkieDemo.terminal.cols !== ${beforeResize[0]} || silkieDemo.terminal.rows !== ${beforeResize[1]}`), `${game} terminal resize`);
            await until(() => cdp.evaluate(`__smokeDrawImages > ${beforeResize[2]} + 20`), `${game} drawing after resize`);
            await delay(1000);
            await cdp.evaluate('silkieDemo.terminal.flush()');
            await cdp.evaluate('document.querySelector("#demo-panel").scrollIntoView({block:"center",behavior:"instant"})');
            const screenshot = await cdp.send('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false});
            await writeFile(options.screenshot, Buffer.from(screenshot.data, 'base64'));
            await writeFile(options.screenshot.replace(/\.png$/, `-${game}.png`), Buffer.from(screenshot.data, 'base64'));
            await cdp.evaluate('silkieDemo.terminal.focus()');
            await key('p', 'KeyP', 80);
            await until(() => cdp.evaluate('document.querySelector("#silkie-terminal canvas").textContent.includes("Paused")'), `${game} pause menu`);
            await key('q', 'KeyQ', 81);
            const text = await until(async () => {
                const value = await cdp.evaluate(`__smokeChunks.slice(${before}).join("")`);
                return new RegExp(`${marker}:\\d+\\r?\\n`).test(value) ? value : false;
            }, `${game} quit`, 30000);
            diagnostics[game] = {tail: text.slice(-5000), trace: await cdp.evaluate(`__smokeTrace.slice(${traceBefore})`), outputControls: text.match(/\x1b\[[<>?][0-9;]*[a-zA-Z]/g)};
            assert.match(text, new RegExp(`${marker}:0\\r?\\n`));
            assert.equal(await cdp.evaluate('silkieDemo.terminal.keyboardFlags'), 0, 'Game must restore shell keyboard flags');
            const shellText = await command("printf 'POST_GAME_OK\\n'");
            assert.match(shellText, /POST_GAME_OK/);
            assert(!shellText.includes('command not found'), shellText);
            return `entered play, sent arrows/space, drew further frames, paused and quit successfully; ${options.screenshot}`;
        });
        if (!played) throw new Error(`${game} did not complete its interactive test; stopping before sending commands into a running game.`);
    }
    await test('mobile layout', async () => {
        await cdp.send('Emulation.setDeviceMetricsOverride', {width:390,height:844,deviceScaleFactor:1,mobile:true});
        await delay(300);
        const layout = await cdp.evaluate('({width:innerWidth,scroll:document.documentElement.scrollWidth,cols:silkieDemo.terminal.cols,rows:silkieDemo.terminal.rows})');
        assert(layout.scroll <= layout.width, JSON.stringify(layout));
        assert(layout.cols > 0 && layout.rows > 0);
        const screenshot = await cdp.send('Page.captureScreenshot', {format:'png',captureBeyondViewport:false});
        await writeFile(join(dirname(options.screenshot), 'silkie-browser-demo-mobile.png'), Buffer.from(screenshot.data,'base64'));
        return `${layout.cols} × ${layout.rows} terminal grid, no horizontal page overflow`;
    });
    }
    if (options.mode === 'all' || options.mode === 'links') {
    await test('terminal Tailscale link and focus restoration', async () => {
        await command('silkie-features');
        await cdp.evaluate('document.querySelector("#demo-panel").scrollIntoView({block:"center",behavior:"instant"})');
        await cdp.evaluate('globalThis.__linkConnectCalls=0; const originalConnect=silkieDemo.connect.bind(silkieDemo); silkieDemo.connect=(...args)=>{__linkConnectCalls++;return originalConnect(...args)}');
        const point = await cdp.evaluate(`(() => {
            const link = silkieDemo.terminal.links().find(link => link.uri === 'silkie:tailscale');
            const box = document.querySelector('#silkie-terminal canvas').getBoundingClientRect();
            return link ? {x: box.left + link.x + link.width / 2, y: box.top + link.y + link.height / 2} : null;
        })()`);
        assert(point, 'Tailscale link must be visible in the terminal');
        await cdp.send('Input.dispatchMouseEvent', {type: 'mousePressed', ...point, pointerType: 'mouse', button: 'left', clickCount: 1});
        await cdp.send('Input.dispatchMouseEvent', {type: 'mouseReleased', ...point, pointerType: 'mouse', button: 'left', clickCount: 1});
        diagnostics.terminalLink = {point, state: await cdp.evaluate('({calls:__linkConnectCalls,status:document.querySelector("#demo-status").textContent,requested:silkieDemo.connectRequested,modes:silkieDemo.terminal.modes,viewport:{width:innerWidth,height:innerHeight,scale:visualViewport.scale}})')};
        assert(diagnostics.terminalLink.state.calls > 0, JSON.stringify(diagnostics.terminalLink));
        await until(() => cdp.evaluate('silkieDemo.terminal.links().some(link => link.uri.startsWith("https://login.tailscale.com/"))'), 'terminal login link', 60000);
        const typedCommand = async label => {
            const before = await cdp.evaluate('__smokeChunks.length');
            await cdp.send('Input.insertText', {text: `printf '${label}\\n'`});
            await key('Enter', 'Enter', 13);
            await until(async () => (await cdp.evaluate(`__smokeChunks.slice(${before}).join("")`)).includes(`${label}\n`), label);
        };
        await typedCommand('INPUT_DURING_LOGIN');
        // Exercise the same connected callback without enrolling an account.
        await cdp.evaluate('silkieDemo.onNetwork("connected", "Connected test callback"); window.dispatchEvent(new Event("focus"))');
        await until(() => cdp.evaluate('document.activeElement === document.querySelector("#silkie-terminal textarea")'), 'restored keyboard focus');
        await typedCommand('AFTER_LOGIN_CALLBACK');
        return 'clickable OSC8 link; login URL shown inside terminal; input works after connected callback; no account login attempted';
    });

    }
    diagnostics.browserErrors = cdp.events.filter(event => event.method === 'Runtime.exceptionThrown').map(event => sanitize(event.params.exceptionDetails.exception?.description || event.params.exceptionDetails.text));
    diagnostics.exceptionEvents = cdp.events.filter(event => event.method === 'Runtime.exceptionThrown');
    diagnostics.guestRuns = await cdp.evaluate('__smokeTrace.filter(event=>event.type==="guest-run")');
    diagnostics.workerSignals = await cdp.evaluate('__smokeTrace.filter(event=>event.type==="host-signal")');
    if (options.mode === 'chinese') {
        await test('Chinese documentation search', async () => {
            await cdp.send('Page.navigate', {url: new URL('zh-CN/search.html?q=' + encodeURIComponent('数位板'), url).href});
            await until(() => cdp.evaluate('Array.from(document.querySelectorAll("#search-results a"), link => link.href).some(href => href.includes("tablet-protocol.html"))'), 'Chinese tablet search result');
            assert.equal(await cdp.evaluate('document.documentElement.lang'), 'zh-CN');
            return 'Chinese query returns the translated tablet reference';
        });
    }
    assert.equal(diagnostics.browserErrors.length, 0, JSON.stringify(diagnostics.browserErrors));
} catch (error) {
    results.push({name: 'browser runner', status: 'fail', error: sanitize(error.stack || error)});
    console.error(sanitize(error.stack || error));
    if (cdp) {
        try {
            console.error('Page state:', await cdp.evaluate('({status:document.querySelector("#demo-status")?.textContent, network:document.querySelector("#demo-network")?.textContent, outputBytes:globalThis.__smokeChunks?.reduce((total,part)=>total+part.length,0), screen:document.querySelector("#silkie-terminal canvas")?.textContent})'));
            const shot = await cdp.send('Page.captureScreenshot', {format: 'png'});
            await writeFile(options.screenshot, Buffer.from(shot.data, 'base64'));
        } catch (diagnosticError) { console.error(sanitize(diagnosticError.message)); }
    }
} finally {
    if (cdp) cdp.ws.close();
    await stop(chrome);
    await stop(server);
    if (profile) await rm(profile, {recursive: true, force: true});
    const report = options.report;
    await writeFile(report, JSON.stringify({results, diagnostics}, (key, value) => typeof value === 'string' ? sanitize(value) : value, 2));
    console.log(`Smoke report: ${report}`);
    if (results.some(result => result.status === 'fail')) process.exitCode = 1;
}
