Silkie browser build
======================

The browser build runs a portable Silkie terminal backend compiled to
WebAssembly, with a Linux shell and applications executed by CheerpX.
The website sources are in ``docs/silkie/`` and the generated website is
in the repository's top-level ``html/`` directory.

Build
-------

Install Rust through rustup, Python 3.11 or newer, e2fsprogs, and the Python
packages in ``docs/silkie/requirements.txt``. On macOS, Homebrew's e2fsprogs
installation is detected even when its executables are not on PATH. Install
the cross-compilation targets::

   rustup target add wasm32-unknown-unknown i686-unknown-linux-musl

Keep the game checkouts beside Silkie as ``../silkie-sprint``,
``../silkie-scramble``, ``../presidential``, ``../silkie-loophole``, and
``../ground-stop``, then run::

   make browser-demo
   make browser-serve

Open http://127.0.0.1:8000/. The homepage starts the terminal automatically
in a compact 4:3 window. Normal text is 14px; fullscreen uses 17px.
The standalone browser-demo.html uses the same homepage template. The page fetches the immutable
CheerpX 1.4.0 runtime from its Community License CDN. Loading needs internet
access; guest files stream from the website on demand, while shell commands
and games execute locally without Tailscale. External SSH uses the visitor's optional Tailscale login. There
is no Silkie-operated relay, auth key, or server-side shell.

The game links below the terminal open ``browser-demo.html?game=silkie-sprint``
or the equivalent ``?game=silkie-scramble``, ``?game=presidential``,
``?game=silkie-loophole``, and ``?game=ground-stop`` in a dedicated terminal filling its window and
launch the selected game automatically. Each game has separate persistent
browser storage, so all five can run beside the homepage shell. Repeated clicks
return to the existing game window. Only these allowlisted game names can auto-launch.
Direct links work without going through the homepage. The Fullscreen button
enters browser fullscreen after a click, as required by browsers.
Presidential opens its normal campaign menu and uses inline graphics without
extra arguments; the other games receive ``--no-tempfile``. The guest includes
Presidential's workspace crates and embedded data, and Loophole's and Ground
Stop's asset and license notices alongside their sources.
The homepage keeps its gameplay screenshot above the fold, including on phones.

``silkie-features`` reads the current tty width whenever it prints the report.
It wraps at word boundaries with hanging indents, preserves Unicode and clickable
OSC 8 labels, and adapts after resizing or entering fullscreen.

The guest builder pins downloaded Alpine packages by URL and SHA-256 and
compiles the games from each checkout's Cargo.lock. It creates an ext2 image
without root privileges or Docker. Its size is calculated from the installed
files and proportional writable headroom. See ``python3 web/build-guest.py
--help`` to change the source directory, output, cache, or headroom. Rebuilding
is necessary to enlarge a full image; this does not silently discard guest
files. Browser storage quotas also apply to the writable overlay.

Sprint and Scramble sprite atlases are painted natively during the guest build
at every sprite resolution supported by the games. The guest loads these PNGs
from ``/usr/share/silkie/sprites`` through ``SILKIE_SPRITE_CACHE``, avoiding
sprite painting under CPU emulation. The cache also includes the exact sRGB,
Display P3, and HDR protocol transfers, avoiding emulated gamut conversion,
PNG encoding, and compression during startup. Prepared art is cached by the
source and asset hashes; missing entries retain the normal rendering path. The ext2
image includes every resolution; HTTP range reads fetch only the art in use.

Presidential's and Ground Stop's browser executables link ``guest/math`` for the
math functions their simulations call most: logarithms, powers and exponentials,
and for Ground Stop also inverse trigonometry, rounding, ``fmod``, ``hypot`` and
scaling. ``GUEST_MATH`` in ``build-guest.py`` lists each game's functions. The
shim uses the pinned Rust ``libm`` implementations compiled for SSE2, avoiding
expensive i386 musl x87 routines under CheerpX. The simulations retain full
floating-point precision; native releases use their ordinary math library. The
shim's sources and lockfile ship with the guest's other sources.

Individual build targets are ``make wasm``, ``make browser-guest``, and
``make html``. The HTML target stages browser JavaScript and CSS but does
not compile Rust or rebuild the image. Use ``make browser-demo`` after
changing the terminal core or guest. Generated guest images are ignored by
Git; build them before uploading the website. The generated ``guest.json``
identifies the image and selects a separate local overlay for each image
version.

Components
------------

* ``wasm/``: the portable terminal screen/parser core, with dynamically
  growing input, escape sequences, and scrollback. It shares Silkie's
  generated Unicode width tables, but is an initial backend rather than
  a compilation of the CPython/GLFW desktop implementation.
* ``terminal.mjs``: Canvas rendering, keyboard/mouse input, inline kitty
  graphics, OSC 8 links, selection/copy, and the byte-oriented terminal interface.
* ``vector.mjs``: retained vector scenes, atomic patches, clipping, gradients,
  group opacity, and variable-width ink geometry.
* ``input.mjs`` and ``ink.mjs``: native protocol reports from browser pen/touch
  events, pressure/tilt data, cancellation, and terminal-side retained strokes.
* ``color.mjs``: native brightness, wide-gamut and alpha paints; tagged raster
  conversion and optional HDR presentation with verified float16/WebGPU support.
* ``runtime.mjs``: filesystem mounts, shell lifecycle, capability report,
  and Tailscale callbacks. ``app.mjs`` connects it to the website controls.
* ``guest/`` and ``build-guest.py``: pinned Linux dependencies, startup
  commands, inline-graphics game wrappers, and the ext2 image build.
* ``serve.py``: local static HTTP hosting with isolation headers, streaming
  byte ranges, appropriate MIME types, and no SSH relay functionality.

Applications discover actual protocol support. Retained vectors and ink,
cell alpha, wide-gamut decoding/conversion, HDR image decoding, and native
pen/touch reports are implemented. Display-P3 output and extended HDR output
are enabled only when the actual browser/display pipeline supports them;
SDR presentation clips or tone-maps extended samples. Numeric physical HDR
headroom is unavailable through browser APIs and is reported as unknown.
Native haptic actuator targeting and stable tablet hardware identifiers are
not exposed by Pointer Events or the Vibration API; their native capabilities
remain zero. Generic device vibration is a separate optional backend.
The startup report lists only browser restrictions, not implementation gaps.
The initial kitty graphics subset handles inline still bitmap/vector images and placements
with ``C=1`` (the game path), not cursor-moving placements, animation,
composition, or Unicode placeholders. Full grapheme clustering, including
emoji ZWJ sequences, also remains to be ported.
Keyboard negotiation grants disambiguation, press/repeat/release events,
and all-key reporting. Each physical key belongs to the keyboard session
that received its press. Late releases and autorepeats cannot cross into
another application, and focus loss releases held keys in the current session.

The game wrappers run through ``silkie-console-session``, a small native
guest launcher. It holds the outer tty in raw mode before starting a game.
After the game exits, it sends a nonce-tagged private terminal query and
reads exactly through its acknowledgement before restoring the shell tty.
The frontend acknowledges synchronously, so queued game reports precede
that boundary and subsequent shell input follows it. This uses no timing
delay or tty flush, and leaves post-acknowledgement typeahead unread.

The runtime updates tty dimensions and explicitly signals the tracked foreground
application after a resize,
coalescing repeated events while a signal helper runs. It never signals host
processes.

CheerpX's custom console emits bare LF even with termios OPOST/ONLCR enabled.
The frontend starts in ANSI newline mode (LNM, CSI 20 h) to display shell
output correctly. This acts only on terminal text interpretation, preserving
graphics and other control-string payload bytes. Applications can explicitly
select normal linefeed behavior with CSI 20 l.

Validation
------------

Run the JavaScript and Python checks with::

   make browser-test

The core tests instantiate the production WASM and exercise split UTF-8,
large control strings, retained scrollback, and graphics replies. The
runtime checks exercise byte transport, repeated shells, resize, and login
failure/retry. Server tests use loopback sockets, including repeated
concurrent reads from large image files. The guest builder also validates
the game ELF architecture and static linkage.

For an actual browser check after building the website, run::

   node web/browser-smoke.mjs

The runner launches its own loopback server and a clean Chrome profile. It
checks the shell, Unicode input, window resize signals, repeated gameplay and
quit, and the Tailscale login link. It does not sign into an account. Set
``CHROME_BIN`` or pass ``--chrome /path/to/chrome`` on systems where Chrome
is not in the default macOS location. Authenticated Tailscale SSH still needs
verification with the visitor's own destination.

Use ``node web/browser-smoke.mjs --mode keyboard`` for the focused keyboard
regression. It checks exact press/repeat/release bytes inside Linux, held-key
cancellation when switching tabs, and 20 alternating game exits. Cases include
rapid quits, keys held across application boundaries, and shell typeahead sent
immediately after the handoff acknowledgement. These checks also run in the
complete browser suite.

Use ``node web/browser-smoke.mjs --mode launch`` to check all five game launch
links, concurrent homepage/game windows, repeated clicks, and report formatting
at desktop and phone widths.

Use ``node web/browser-smoke.mjs --mode startup`` to measure both games' first
graphics in compact, fullscreen, and fullscreen Retina terminals, capture their
title screens, and verify that removing the loading banner preserves the complete
menu text. The JSON report separates graphics decoding and vector rasterization
timings. Use ``--mode loophole-resize`` for repeated compact/fullscreen scenery
coverage and a font-size-only notification with the grid dimensions held fixed.

Desktop terminal behavior is documented upstream at
https://sw.kovidgoyal.net/kitty/. Silkie's additions are in
https://github.com/adamdeprince/silkie/tree/silkie/docs/silkie.

R2 deployment
---------------

Build the guest and website with ``make browser-demo`` before publishing.
``python3 packaging/publish_r2.py`` verifies the guest image against its manifest,
uploads it under an immutable SHA-256 filename, stages the corresponding manifest,
and uploads assets before HTML. Existing remote images remain available to running
guests. The local ``html/`` tree keeps its normal development image filename.
Use ``--dry-run`` to inspect the upload plan.

The production ``distribution-static`` R2 gateway adds ``Cross-Origin-Opener-Policy:
same-origin``, ``Cross-Origin-Embedder-Policy: require-corp``, and
``Cross-Origin-Resource-Policy: same-origin`` for ``silkie.goblinreactor.com``.
The scoped change is recorded in ``packaging/r2-wasm-headers.patch``; preserve it
when updating the shared gateway. These headers are required for the browser
guest's shared WebAssembly memory.

Check a published deployment with ``node web/browser-smoke.mjs --url
https://silkie.goblinreactor.com/ --mode launch``. Supplying ``--url`` uses the
live origin instead of starting a local HTTP server.

Terminal links and focus
-------------------------

The startup banner includes clickable OSC 8 links for Tailscale and SSH help.
Login URLs are printed as terminal links as well. Returning from the login
window restores focus and rebinds the console input callback after connection.
Mouse drag selects text, including retained scrollback; Shift-drag selects
when an application is using the mouse protocol. Copy uses the browser's
normal clipboard event; selection data is not sent to the shell. Keyboard
release reports use browser ``keyup`` events and are enabled for applications
that request them.
