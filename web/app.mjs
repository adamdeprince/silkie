import {createTranslator} from './i18n.mjs';
import {CHEERPX_MODULE, GUEST_MANIFEST, TERMINAL_WASM} from './config.mjs';
import {DemoSession, createGameLauncher, demoGameFromURL, demoStorageName, readGuestManifest} from './runtime.mjs';
import {createTerminal} from './terminal.mjs';

const language = document.documentElement.lang;
const t = createTranslator(language);

const start = document.querySelector('#demo-start');
const status = document.querySelector('#demo-status');
const container = document.querySelector('#silkie-terminal');
const panel = document.querySelector('#demo-panel');
const fullscreen = document.querySelector('#demo-fullscreen');
const normalFont = 14, fullscreenFont = 17;
const game = demoGameFromURL(location.href);
if (game) {
    document.documentElement.dataset.demoGame = game;
    document.title = `${game} — Silkie`;
    start.textContent = t('Play again');
}
const launchGame = createGameLauncher({
    url: new URL('browser-demo.html', location.href),
    openWindow: (...args) => window.open(...args),
});
for (const link of document.querySelectorAll('a[data-demo-game]')) {
    link.addEventListener('click', event => {
        if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
        if (launchGame(link.dataset.demoGame)) event.preventDefault();
    });
}
let session, lease, releaseLease, pendingTerminal, loginUrl;
let booting = false, terminalFailure = false, returnFocus = false;

function message(text) { status.textContent = text; }
function fontSize() { return document.fullscreenElement === panel ? fullscreenFont : normalFont; }
function focusTerminal(rebind = false) {
    if (!session?.running || terminalFailure) return;
    if (rebind) session.resize(session.terminal.cols, session.terminal.rows);
    session.terminal.focus();
}
function onLogin(url) {
    loginUrl = url;
    if (!url || !session) return;
    session.terminal.write(`\r\n\x1b]8;;${url}\x1b\\${t('Continue Tailscale login ↗')}\x1b]8;;\x1b\\\r\n`);
    focusTerminal();
}
async function openLink(uri) {
    if (uri === 'silkie:tailscale') {
        if (!session?.guest) return;
        returnFocus = true;
        try { await session.connect(); }
        catch (error) { message(error.message || String(error)); }
        finally { focusTerminal(true); }
    } else {
        const url = uri === 'silkie:ssh-help' ? new URL('browser-ssh.html', location.href) : new URL(uri);
        if (!['http:', 'https:'].includes(url.protocol)) return;
        if (uri === loginUrl) returnFocus = true;
        window.open(url.href, '_blank', 'noopener,noreferrer');
    }
}
async function acquireLease(identity) {
    if (!navigator.locks) throw new Error(t('This browser needs Web Locks support to protect the guest filesystem.'));
    let acquired;
    const ready = new Promise(resolve => { acquired = resolve; });
    lease = navigator.locks.request(demoStorageName(identity, game), {ifAvailable: true}, async lock => {
        acquired(Boolean(lock));
        if (lock) await new Promise(resolve => { releaseLease = resolve; });
    });
    if (!await Promise.race([ready, lease.then(() => false)])) {
        throw new Error(`${t('{game} is open in another window. Switch to it or close it before starting this one.', {game:game || t('The shell demo')})}`);
    }
}
function onState(state, text) {
    if (terminalFailure) return;
    message(text);
    start.hidden = !['exited', 'error'].includes(state);
    start.disabled = ['running', 'loading', 'fatal', 'resize-error'].includes(state);
}
async function startShell() {
    if (booting || session?.running) return;
    booting = true; start.hidden = true; start.disabled = true;
    try {
        if (!globalThis.isSecureContext || !globalThis.crossOriginIsolated || !globalThis.SharedArrayBuffer) {
            throw new Error(t('This page needs HTTPS (or localhost) and cross-origin isolation. For local development, run make browser-serve.'));
        }
        if (!session) {
            const manifest = await readGuestManifest(GUEST_MANIFEST, fetch, language);
            await acquireLease(manifest.sha256);
            message(t('Loading the terminal…'));
            const terminal = await createTerminal(container, {
                language, wasmUrl: TERMINAL_WASM, convertEol: true, fontSize: fontSize(),
                onData: bytes => session?.send(bytes),
                onResize: (cols, rows) => session?.resize(cols, rows),
                onLink: uri => { void openLink(uri).catch(error => message(error.message)); },
                onCopyError: error => message(t('Copy failed: {error}', {error:error.message || error})),
                onError: error => {
                    terminalFailure = true;
                    if (session) session.input = null;
                    message(t('The terminal stopped: {error}. Reload the page to restart.', {error:error.message || error}));
                    start.hidden = true;
                },
            });
            pendingTerminal = terminal;
            message(t('Starting Linux…'));
            const CheerpX = await import(CHEERPX_MODULE);
            session = new DemoSession({
                CheerpX, manifest, terminal, game, language, onState, onLogin,
                onNetwork: (state, text) => {
                    message(text);
                    if (state === 'connected') {
                        requestAnimationFrame(() => focusTerminal(true));
                    }
                },
            });
            pendingTerminal = null;
            globalThis.silkieDemo = session;
        }
        await session.start();
    } catch (error) {
        message(error.message || String(error));
        start.disabled = terminalFailure || Boolean(session?.initializationFailed);
        start.hidden = start.disabled;
        if (!session) {
            pendingTerminal?.dispose(); pendingTerminal = null;
            releaseLease?.(); await lease?.catch(() => {}); releaseLease = null;
        }
    } finally { booting = false; }
}
start.addEventListener('click', () => { void startShell(); });
fullscreen.addEventListener('click', async () => {
    try {
        if (document.fullscreenElement) await document.exitFullscreen();
        else await panel.requestFullscreen();
    } catch (error) { message(t('Fullscreen unavailable: {error}', {error:error.message})); }
});
document.addEventListener('fullscreenchange', () => {
    const active = document.fullscreenElement === panel;
    fullscreen.textContent = active ? t('Exit fullscreen') : t('Fullscreen');
    fullscreen.setAttribute('aria-pressed', String(active));
    session?.terminal.setFontSize(fontSize());
    focusTerminal();
});
addEventListener('focus', () => {
    if (returnFocus) { returnFocus = false; requestAnimationFrame(() => focusTerminal(true)); }
});
// Document destruction releases the Web Lock; a cached document retains it.
void startShell();
