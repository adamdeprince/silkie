.. meta::
   :description: Silkie is a terminal emulator built on kitty, extending it with pen and touch input, vector graphics, HDR, and presentation feedback. Install Silkie and explore its specifications.

A terminal for richer interaction.
==================================

.. container:: hero-lead

   Silkie is a terminal emulator for macOS and Linux, built on
   `kitty <https://sw.kovidgoyal.net/kitty/>`_. It extends the terminal with
   pen and touch input, vector graphics, HDR color, and display timing feedback.

   Run your shell and terminal applications. Build software that can draw,
   respond to a stylus, and understand the display it is running on.

   .. container:: home-actions

      `Install Silkie <#install>`_
      :doc:`Read the specifications <developer-guide>`

   .. container:: source-note

      Open source · Main branch: `silkie
      <https://github.com/adamdeprince/silkie/tree/silkie>`_

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

Color beyond ordinary SDR
~~~~~~~~~~~~~~~~~~~~~~~~~

Use wider color spaces and request brightness above SDR white on supported
displays. Query the available headroom and control transparency at the cell level.

:doc:`HDR <hdr-protocol>` · :doc:`Wide gamut <wide-gamut-protocol>` ·
:doc:`Cell alpha <cell-alpha-protocol>`

Display and presentation feedback
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Learn about the display, receive input timing information, and track when a
frame is presented. Use those reports to coordinate drawing with the terminal.

:doc:`Display information <display-protocol>` · :doc:`Latency <latency-protocol>` ·
:doc:`Frame presentation <frame-protocol>`

Install
---------

Binary packages are available from `Homebrew <https://brew.goblinreactor.com/#silkie>`_
for Apple Silicon macOS, and the `APT repository <https://apt.goblinreactor.com/#silkie>`_
for Linux. See :doc:`install` for the supported targets and installation instructions.

To build from source, use the ``silkie`` branch. You will need Git, a C compiler, and
Go; Linux also needs the system libraries listed in the
:doc:`installation guide <install>`. The build script downloads the remaining
build dependencies.

.. include:: _includes/source-build.rst

The executable retains the name ``kitty``. Running this checkout's executable
launches Silkie with its extensions. Upstream kitty packages install upstream
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
   developer-guide
   reference
