import {createTranslator} from './i18n.mjs';

// CheerpX owns process execution; the terminal owns protocol parsing and drawing.
// Keep this boundary byte-oriented so graphics and split UTF-8 survive intact.
export function featureReport(capabilities = {}, language = 'en') {
    const t = createTranslator(language);
    const esc = '\x1b';
    const link = (uri, label) => `${esc}]8;;${uri}${esc}\\${label}${esc}]8;;${esc}\\`;
    return [
        t('Silkie — a Linux shell running in this tab.'),
        t('Commands: ls, cat, grep, find, ssh.'),
        t('Games: {games}.', {games: DEMO_GAMES.join(', ')}),
        '',
        t('Browser restrictions:'),
        t('  - No direct access to native GPU surfaces (dma-buf/IOSurface).'),
        t('  - No unrestricted host files, processes, or desktop windows.'),
        t('  - No exact physical presentation timestamps or HDR headroom.'),
        t('  - No native tablet hardware IDs or targeted pen/trackpad haptics.'),
        t('  - No raw TCP sockets; SSH uses your own Tailscale network.'),
        '',
        link('silkie:tailscale', t('Connect to Tailscale')) + ' · ' + link('silkie:ssh-help', t('SSH setup')),
        t('Run silkie-features to show this report again.'),
        '',
    ].join('\n');
}

export async function readGuestManifest(url, fetcher = fetch, language = 'en') {
    const t = createTranslator(language);
    const response = await fetcher(url, {cache: 'no-cache'});
    if (!response.ok) throw new Error(t('The Linux image has not been built. Run make browser-demo.'));
    const manifest = await response.json();
    if (!/^[a-f0-9]{64}$/.test(manifest.sha256)) throw new Error(t('The guest image manifest has no valid SHA-256 identity. Rebuild the Linux image.'));
    const image = new URL(manifest.image || 'guest.ext2', url);
    if (image.origin !== new URL(url).origin) throw new Error(t('The Linux image must be served from the demo website.'));
    return {...manifest, image: image.href};
}

export const DEMO_GAMES = Object.freeze(['presidential', 'silkie-loophole', 'ground-stop', 'silkie-sprint', 'silkie-scramble']);

function checkedGame(game) {
    if (game !== null && !DEMO_GAMES.includes(game)) throw new Error(`Choose a game: ${DEMO_GAMES.join(', ')}.`);
    return game;
}

export function demoGameFromURL(url) {
    const games = new URL(url).searchParams.getAll('game');
    return games.length === 1 && DEMO_GAMES.includes(games[0]) ? games[0] : null;
}

export function demoStorageName(identity, game = null) {
    checkedGame(game);
    // Each game has its own persistent filesystem so its window can run beside
    // the homepage shell. Use exactly this identity for both IDB and Web Locks.
    return `silkie-demo-${identity}${game ? `-${game}` : ''}`;
}

export function createGameLauncher({url, openWindow}) {
    const windows = new Map();
    return game => {
        checkedGame(game);
        if (!game) throw new Error('Choose a game to open.');
        let popup = windows.get(game);
        if (!popup || popup.closed) {
            const target = new URL(url);
            target.search = '';
            target.hash = '';
            target.searchParams.set('game', game);
            // These are fixed, same-origin demo pages. A stable browsing-context
            // name lets another click reuse the game rather than clone its disk.
            popup = openWindow(target.href, `silkie-${game}`, 'popup,width=960,height=720');
            if (popup) windows.set(game, popup);
        }
        popup?.focus();
        return popup;
    };
}

export class DemoSession {
    constructor({CheerpX, manifest, terminal, game = null, language = 'en', onState = () => {}, onNetwork = () => {}, onLogin = () => {}}) {
        checkedGame(game);
        Object.assign(this, {CheerpX, manifest, terminal, game, onState, onNetwork, onLogin});
        this.t = createTranslator(language);
        this.language = language;
        this.running = false;
        this.input = null;
        this.guest = null;
        this.initialization = null;
        this.login = null;
        this.connectRequested = false;
        this.networkAddress = '';
        this.initializationFailed = false;
        this.shellActive = false;
        this.pendingResize = false;
        this.signalingResize = false;
    }

    async initialize() {
        if (this.initialization) return this.initialization;
        this.initialization = this.createGuest();
        try { return await this.initialization; }
        catch (error) {
            this.initializationFailed = true;
            throw new Error(this.t('{error} Reload the page to retry initialization.', {error: error.message || error}), {cause: error});
        }
    }

    async createGuest() {
        const {CheerpX: cx, manifest} = this;
        this.onState('loading', this.t('Loading the Linux filesystem…'));
        const image = await cx.HttpBytesDevice.create(manifest.image);
        const storage = await cx.IDBDevice.create(demoStorageName(manifest.sha256, this.game));
        const overlay = await cx.OverlayDevice.create(image, storage);
        const data = await cx.DataDevice.create();
        await data.writeFile('/browser-features.txt', featureReport(this.terminal.capabilities, this.language));
        this.onState('loading', this.t('Starting Linux…'));
        this.guest = await cx.Linux.create({
            mounts: [
                {type: 'ext2', path: '/', dev: overlay},
                {type: 'dir', path: '/run/silkie', dev: data},
                {type: 'devs', path: '/dev'},
                {type: 'proc', path: '/proc'},
            ],
            networkInterface: {
                loginUrlCb: url => {
                    // A stale callback must not launch or display a login after
                    // a failed/cancelled request or without a user connect action.
                    if (!this.connectRequested) return;
                    try {
                        const target = new URL(url);
                        if (target.protocol !== 'https:') throw new Error(this.t('Tailscale supplied an insecure login URL.'));
                        this.onLogin(target.href);
                    } catch (error) { this.failLogin?.(error); }
                },
                stateUpdateCb: state => {
                    if (!this.connectRequested) return;
                    if (state === 6) {
                        this.onLogin(null);
                        this.onNetwork('connected', this.networkAddress || this.t('Connected to your Tailscale network.'));
                    } else if (state === 4) {
                        this.onLogin(null);
                        this.onNetwork('disconnected', this.t('Tailscale disconnected.'));
                    } else if (state === 3) this.onNetwork('connecting', this.t('Waiting for device approval in your Tailscale account.'));
                    else this.onNetwork('connecting', this.t('Connecting to your Tailscale network…'));
                },
                netmapUpdateCb: map => {
                    if (!this.connectRequested) return;
                    const addresses = map?.self?.addresses || [];
                    this.networkAddress = addresses.join(', ');
                    if (this.networkAddress) this.onNetwork('address', this.networkAddress);
                },
            },
        });
        this.resize(this.terminal.cols, this.terminal.rows);
        return this.guest;
    }

    resize(cols, rows) {
        if (!this.guest) return;
        this.input = this.guest.setCustomConsole(
            bytes => this.terminal.write(new Uint8Array(bytes)), cols, rows,
        );
        if (this.shellActive) {
            this.pendingResize = true;
            void this.signalResize();
        }
    }

    async signalResize() {
        if (this.signalingResize) return;
        this.signalingResize = true;
        try {
            // Notify applications after updating the custom console size.
            // Signal only processes owned by this guest's
            // demo user. These are virtual Linux PIDs, never host processes.
            // Coalesce while a notification is running, preserving the final
            // resize without spawning a growing queue of helper processes.
            while (this.pendingResize && this.shellActive) {
                this.pendingResize = false;
                // Notify the tracked foreground application, rather than
                // interrupting a background shell that is waiting for a game.
                const result = await this.guest.run('/usr/local/libexec/silkie-console-session', ['--notify-resize'], {
                    uid: this.manifest.uid ?? 1000, gid: this.manifest.gid ?? 1000,
                });
                if (result.status !== 0) throw new Error(`guest signal helper exited ${result.status}`);
            }
        } catch (error) {
            this.onState('resize-error', this.t('Terminal size changed, but application notification failed: {error}', {error: error.message || error}));
        } finally { this.signalingResize = false; }
    }

    send(bytes) {
        if (!this.running || !this.input) return;
        for (const byte of bytes) this.input(byte);
    }

    async start() {
        // Cover initialization too: two clicks must not create two Linux guests.
        if (this.running) return;
        this.running = true;
        try {
            const guest = await this.initialize();
            this.onState('running', this.game ? this.t('{game} running in your browser.', {game:this.game}) : this.t('Shell running in your browser.'));
            this.terminal.focus();
            this.shellActive = true;
            const result = await guest.run(this.manifest.entry || '/usr/local/bin/silkie-demo-shell', [], {
                env: [
                    'HOME=/home/demo', 'USER=demo', 'LOGNAME=demo',
                    'SHELL=/bin/bash', 'TERM=xterm-256color', 'LANG=C.UTF-8',
                    'PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin',
                    'SILKIE_SPRITE_CACHE=/usr/share/silkie/sprites',
                    ...(this.game ? [`SILKIE_DEMO_GAME=${this.game}`] : []),
                ],
                cwd: this.manifest.cwd || '/home/demo',
                uid: this.manifest.uid ?? 1000, gid: this.manifest.gid ?? 1000,
            });
            this.onState('exited', this.game ? this.t('{game} exited ({status}). Play again to restart.', {game:this.game,status:result.status}) : this.t('Shell exited ({status}). Start another shell to continue.', {status:result.status}));
        } catch (error) {
            this.onState(this.initializationFailed ? 'fatal' : 'error', error.message);
            throw error;
        } finally { this.running = false; this.shellActive = false; }
    }

    async connect() {
        if (!this.guest) throw new Error(this.t('Start the shell before connecting to Tailscale.'));
        if (this.login) return this.login;
        this.connectRequested = true;
        this.onNetwork('connecting', this.t('Preparing your Tailscale login…'));
        const failure = new Promise((resolve, reject) => { this.failLogin = reject; });
        this.login = Promise.race([Promise.resolve().then(() => this.guest.networkLogin()), failure]);
        try { await this.login; }
        catch (error) {
            this.connectRequested = false;
            this.onLogin(null);
            this.onNetwork('error', this.t('Tailscale login failed: {error}', {error:error.message || error}));
            throw error;
        } finally { this.login = null; this.failLogin = null; }
    }
}
