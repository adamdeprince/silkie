.. meta::
   :description: Silkie is a terminal emulator built on kitty, extending it with pen and touch input, vector graphics, HDR, and presentation feedback. Install Silkie and explore its specifications.

A terminal for richer interaction.
==================================

.. raw:: html

   <figure class="hero-game">
     <a href="games.html">
       <img src="_static/games/sprint-tug.png" width="1100" height="393"
            alt="Loki and Skaði playing tug with a Silkie hen in Silkie Sprint"
            fetchpriority="high">
     </a>
     <figcaption><a href="games.html">Meet the games for Silkie and kitty.</a></figcaption>
   </figure>

.. container:: hero-lead

   Silkie is a terminal emulator for macOS and Linux, built on
   `kitty <https://sw.kovidgoyal.net/kitty/>`_. It extends the terminal with
   pen and touch input, vector graphics, HDR color, and display timing feedback.

   Run your shell and terminal applications. Build software that can draw,
   respond to a stylus, and understand the display it is running on.

   .. container:: home-actions

      `Install Silkie <#install>`_
      :doc:`Try the browser demo <browser-demo>`
      :doc:`Read the specifications <developer-guide>`

   .. container:: source-note

      Open source · Main branch: `silkie
      <https://github.com/adamdeprince/silkie/tree/silkie>`_

.. include:: _includes/browser-terminal.rst

.. container:: lineage

   .. rubric:: Specifications and documentation

   .. container:: lineage-silkie

      **03 / Silkie extensions**

      Pen, touch, ink, vectors, color, and timing.
      :doc:`Explore Silkie's additions <reference>`.

   .. container:: lineage-kitty

      **02 / kitty + kitten tools**

      The terminal emulator Silkie forks, with graphics, keyboard protocols,
      and tools called kittens. `Read kitty's docs
      <https://sw.kovidgoyal.net/kitty/>`_ and `kitten docs
      <https://sw.kovidgoyal.net/kitty/kittens_intro/>`_.

   .. container:: lineage-terminal

      **01 / Terminal foundations**

      The text stream and VT/xterm-style control sequences that kitty extends.
      :ref:`Read the underlying references <terminal-foundations>`.

Features
----------

Silkie adds protocols that applications can discover and use as needed.
Support depends on the operating system, input device, display, and compositor;
each specification describes its capabilities and limits.

Fonts and typography
~~~~~~~~~~~~~~~~~~~~~

Mix Iosevka's sans-serif and slab-serif faces, select weights from Thin to
Heavy, and control slant in either direction. Regional CJK font preferences
preserve character widths and the terminal grid. Font files are installed
separately; see :doc:`font-protocol` for configuration and escape sequences.

Pen, touch, and gestures
~~~~~~~~~~~~~~~~~~~~~~~~

Give applications pressure and tilt information, touch contacts, smooth
scrolling, and gesture events. Inspect what a device reports with the
:doc:`show_pointer kitten <show-pointer>`.

:doc:`Tablet input <tablet-protocol>` · :doc:`Scrolling <scroll-protocol>` ·
:doc:`Gestures <gesture-protocol>` · :doc:`Haptics <haptic-protocol>`

Vector graphics and ink
~~~~~~~~~~~~~~~~~~~~~~~

Send paths, styled text and scenes for the terminal to render at the displayed
size, with session fonts, text measurement, nested window clips and HDR color.
Let the terminal draw pen strokes as input arrives, without waiting for
each sample to make a round trip to the application.

:doc:`Vector images <vector-protocol>` · :doc:`Terminal-side ink <ink-protocol>` ·
:doc:`GPU surfaces <gpu-surface-protocol>`

Send one-, two-, four-, or eight-bit grayscale pixels and RGB565 without
requiring compression; :doc:`packed-image-protocol` describes the compact
wire encodings, independent of the display's color model.
:doc:`palette-image-protocol` adds images of one- to eight-bit indices into a
palette of RGBA colors that can be replaced without resending the pixels. Send
16-bit gray, RGB10A2 and 16-bit RGBA at their full precision, encoded with
sRGB, linear, PQ or HLG, so that HDR10 frames go to the GPU as they are; see
:doc:`precision-image-protocol`. Images of any size are drawn, from tiles when they
are larger than a GPU texture, with mipmaps when shown smaller than they are,
and stay without a storage quota; see :doc:`image-tiling`.

Color beyond ordinary SDR
~~~~~~~~~~~~~~~~~~~~~~~~~

Use wider color spaces, request brightness above SDR white on supported
displays, and give text colors with sixteen bits per channel. Query the
available headroom and control transparency at the cell level.

:doc:`HDR <hdr-protocol>` · :doc:`Wide gamut <wide-gamut-protocol>` ·
:doc:`Deep colors <deep-color-protocol>` · :doc:`Cell alpha <cell-alpha-protocol>`

Display and presentation feedback
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Learn about the display, receive input timing information, and track when a
frame is presented. Use those reports to coordinate drawing with the terminal.

:doc:`Display information <display-protocol>` · :doc:`Latency <latency-protocol>` ·
:doc:`Frame presentation <frame-protocol>`

Games
-------

Play **Presidential**, a political satire and economic simulation, or
**Silkie Loophole**, a raccoon's trick-or-treating adventure in Montpelier.
**Ground Stop** brings every aircraft in America down at Montpelier's
last working airport. **Silkie Sprint** crosses roads and rivers;
**Silkie Scramble** gathers chicks while outwitting garden predators.
All five run in the browser's Linux terminal and support native Silkie
and kitty terminals.

:doc:`Play the games, watch gameplay, and find installation instructions <games>`.

Install
---------

Binary packages are available from `Homebrew <https://brew.goblinreactor.com/#silkie>`_
for Apple Silicon macOS, and the `APT repository <https://apt.goblinreactor.com/#silkie>`_
for Linux. See :doc:`install` for the supported targets and installation instructions.
To set up package sources and install both Silkie and its Iosevka font::

   curl -fsSL https://silkie.goblinreactor.com/install.sh | bash

The installer asks before adding or changing Goblin Reactor package sources.
It installs unmodified Iosevka with its SIL Open Font License notice.
Silkie uses Iosevka's wider Extended faces by default.

The macOS app is not signed with an Apple Developer ID or notarized by Apple,
so macOS would refuse to open it as downloaded. On macOS the install script
and the Homebrew cask clear the quarantine flag from ``silkie.app`` after
installing it, so Silkie opens normally. If macOS still blocks it, approve
Silkie in System Settings > Privacy & Security, or run
``xattr -dr com.apple.quarantine /Applications/silkie.app``.

To build from source, use the ``silkie`` branch. You will need Git, a C compiler, and
Go; Linux also needs the system libraries listed in the
:doc:`installation guide <install>`. The build script downloads the remaining
build dependencies.

.. include:: _includes/source-build.rst

Launch the installed application with ``silkie`` and use ``silkie-kitten`` for
its companion tools. The source build above runs directly from the checkout.
Upstream kitty packages install upstream
kitty; use the Silkie packages or source above to get these additions.

.. container:: home-actions

   :doc:`Installation details <install>`
   :doc:`Configure Silkie <configuration>`

Documentation
---------------

Silkie's specifications describe the additions. Read them alongside the
upstream references to understand the terminal as a whole.

Find the specifications
~~~~~~~~~~~~~~~~~~~~~~~~

The developer guide brings together terminal standards, kitty and kitten
manuals, Silkie's extension specifications, and links to the source.

:doc:`Developer guide <developer-guide>`

Use the extension reference
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Find escape sequences, fields, units, replies, and platform limits.
Start with capability discovery and shared framing, then choose a protocol.

:doc:`All Silkie specifications <reference>` ·
:doc:`Capability discovery <capabilities-protocol>`

Follow the upstream manuals
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

kitty documents inherited terminal behavior. Its kittens are tools and plugins
that use and extend kitty, such as image display, SSH integration, and remote control.

`kitty manual <https://sw.kovidgoyal.net/kitty/>`_ ·
`Kitten tools <https://sw.kovidgoyal.net/kitty/kittens_intro/>`_ ·
`kitty protocols <https://sw.kovidgoyal.net/kitty/protocol-extensions/>`_

.. toctree::
   :hidden:
   :maxdepth: 2

   install
   games
   browser-demo
   browser-ssh
   developer-guide
   reference
