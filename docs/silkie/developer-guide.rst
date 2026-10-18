Writing software for Silkie
============================

Silkie is a fork of the **kitty terminal emulator**. kitty extends the
traditional VT/xterm-style terminal protocol with its own graphics, keyboard,
and other protocols. **Kittens** are kitty's companion tools and plugins;
they use terminal capabilities and can add functionality to kitty. Silkie
adds further terminal protocols and the :doc:`show_pointer tool <show-pointer>`.

Silkie's behavior is documented in several places: terminal standards,
kitty's manuals, and Silkie's extension specifications. This guide links to
each source and the relevant implementation files.

Source and version
--------------------

Silkie's source is on the `silkie branch
<https://github.com/adamdeprince/silkie/tree/silkie>`_.

Upstream kitty manuals may describe behavior that differs from your Silkie
build. The source for your version shows how a feature is implemented. Runtime
capability queries tell your application which extensions, versions, and
optional features are available.

Getting started
-----------------

If you are new to kitty, the `kitty overview <https://sw.kovidgoyal.net/kitty/>`_
introduces the terminal, and the `kitten overview
<https://sw.kovidgoyal.net/kitty/kittens_intro/>`_ introduces its tools and
plugins. The `terminal foundations`_ and `inherited kitty protocols`_ below
cover the standards and protocols that Silkie builds on.

To add a Silkie feature to your application, start with
:doc:`capabilities-protocol` and :doc:`extension-framing`, then choose the
protocols you need from the `Silkie extension map`_. Each specification
explains how to discover support, use the feature, and clean up afterward,
including any platform limits. Test your application in the target terminal
and provide a fallback when a feature is unavailable.

For kittens, terminal user interfaces, remote-control clients, and shell
integration, see `kitten and application APIs`_. The `implementation map`_
points to the relevant code if you want to investigate behavior or contribute
a change to Silkie.

.. _terminal-foundations:

Terminal foundations
----------------------

The compatibility model is a terminal receiving text and control sequences
over a byte stream. The following references explain the inherited vocabulary;
they are not a claim that Silkie implements every historical terminal feature.

* `ECMA-48: control functions
  <https://ecma-international.org/publications-and-standards/standards/ecma-48/>`_
  defines control-function syntax, including CSI, SGR, OSC, and string terminators.
* `XTerm control sequences
  <https://invisible-island.net/xterm/ctlseqs/ctlseqs.html>`_ covers the VT/xterm
  conventions used for cursor movement, screen modes, mouse reporting,
  and other common terminal interactions.
* `Silkie's terminfo source
  <https://github.com/adamdeprince/silkie/blob/silkie/terminfo/kitty.terminfo>`_
  describes the capabilities advertised to terminfo-based applications.

Inherited kitty protocols
---------------------------

The `upstream terminal protocol index
<https://sw.kovidgoyal.net/kitty/protocol-extensions/>`_ is the entry point for
kitty's extensions to the traditional terminal protocol. These are inherited
references, not Silkie-specific additions.

.. list-table:: Upstream specifications
   :header-rows: 1
   :widths: 32 68

   * - Area
     - Read upstream
   * - Images and keyboard input
     - `Graphics <https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_;
       `keyboard <https://sw.kovidgoyal.net/kitty/keyboard-protocol/>`_
   * - Text and screen presentation
     - `Underlines <https://sw.kovidgoyal.net/kitty/underlines/>`_;
       `text sizing <https://sw.kovidgoyal.net/kitty/text-sizing-protocol/>`_;
       `multiple cursors <https://sw.kovidgoyal.net/kitty/multiple-cursors-protocol/>`_;
       `unscrolling <https://sw.kovidgoyal.net/kitty/unscroll/>`_
   * - Colors and attributes
     - `Color control <https://sw.kovidgoyal.net/kitty/color-stack/>`_;
       `attributes in rectangular areas <https://sw.kovidgoyal.net/kitty/deccara/>`_
   * - Interaction and desktop integration
     - `Pointer shapes <https://sw.kovidgoyal.net/kitty/pointer-shapes/>`_;
       `drag and drop <https://sw.kovidgoyal.net/kitty/dnd-protocol/>`_;
       `desktop notifications <https://sw.kovidgoyal.net/kitty/desktop-notifications/>`_
   * - Data exchange
     - `Clipboard <https://sw.kovidgoyal.net/kitty/clipboard/>`_;
       `file transfer <https://sw.kovidgoyal.net/kitty/file-transfer-protocol/>`_
   * - Additional controls
     - `Miscellaneous protocols <https://sw.kovidgoyal.net/kitty/misc-protocol/>`_

Silkie extension map
----------------------

Each link below opens a Silkie specification with wire formats, replies,
and implementation details. The :doc:`reference` also lists them by category.

.. list-table:: Silkie specifications and their prerequisites
   :header-rows: 1
   :widths: 30 45 25

   * - Area
     - Silkie specifications
     - Read alongside
   * - Discovery and common rules
     - :doc:`capabilities-protocol`; :doc:`extension-framing`
     - OSC and ST control-string framing
   * - Input
     - :doc:`tablet-protocol`; :doc:`scroll-protocol`; :doc:`gesture-protocol`;
       :doc:`haptic-protocol`
     - Shared framing and each protocol's discovery rules
   * - Vector rendering and ink
     - :doc:`vector-protocol`; :doc:`ink-protocol`; :doc:`gpu-surface-protocol`
     - kitty graphics; tablet input for ink
   * - Color and transparency
     - :doc:`hdr-protocol`; :doc:`wide-gamut-protocol`; :doc:`cell-alpha-protocol`
     - SGR, kitty graphics, display information
   * - Font families and typography
     - :doc:`font-protocol`
     - SGR, kitty font configuration and text sizing
   * - Image cursors
     - :doc:`pointer-image-protocol`
     - kitty pointer shapes and graphics
   * - Display and timing
     - :doc:`display-protocol`; :doc:`latency-protocol`; :doc:`frame-protocol`
     - Shared framing and the clock/units defined by each protocol
   * - Settings and tools
     - :doc:`configuration`; :doc:`show-pointer`; :doc:`window-actions`
     - kitty configuration, key mappings, and kitten tools

For example, a vector drawing application needs kitty's graphics protocol for
transmission and placement, Silkie's vector format for its scene, and tablet
and ink protocols for pen input and local stroke rendering. The vector
specification alone does not define the whole graphics transport.

Kitten and application APIs
----------------------------

* `Kitten documentation <https://sw.kovidgoyal.net/kitty/kittens_intro/>`_:
  the tool catalog, with links to individual command manuals.
* `Create kittens <https://sw.kovidgoyal.net/kitty/kittens/custom/>`_:
  the plugin model and APIs for extending kitty through a kitten.
* `Remote control guide <https://sw.kovidgoyal.net/kitty/remote-control/>`_
  and `wire protocol <https://sw.kovidgoyal.net/kitty/rc_protocol/>`_:
  controlling terminal windows and state from software.
* `Shell integration <https://sw.kovidgoyal.net/kitty/shell-integration/>`_:
  shell-specific integration and the protocols it uses.
* `Configuration <https://sw.kovidgoyal.net/kitty/conf/>`_ and
  `key mappings <https://sw.kovidgoyal.net/kitty/mapping/>`_:
  inherited settings and actions. Apply :doc:`configuration` for Silkie's additions.
* :doc:`show-pointer`: Silkie's input inspector, useful when implementing
  the tablet, gesture, and scroll protocols.

Implementation map
--------------------

The source files below are on the **silkie** branch.

.. list-table:: Source locations
   :header-rows: 1
   :widths: 35 65

   * - Responsibility
     - Source
   * - Repository and specification sources
     - `Repository <https://github.com/adamdeprince/silkie/tree/silkie>`_;
       `docs <https://github.com/adamdeprince/silkie/tree/silkie/docs>`_
   * - Terminal sequence parsing and screen behavior
     - `kitty/vt-parser.c <https://github.com/adamdeprince/silkie/blob/silkie/kitty/vt-parser.c>`_;
       `kitty/screen.c <https://github.com/adamdeprince/silkie/blob/silkie/kitty/screen.c>`_
   * - Graphics, vectors, and ink
     - `kitty/graphics.c <https://github.com/adamdeprince/silkie/blob/silkie/kitty/graphics.c>`_;
       `kitty/vector.c <https://github.com/adamdeprince/silkie/blob/silkie/kitty/vector.c>`_;
       `kitty/fonts/vector.py <https://github.com/adamdeprince/silkie/blob/silkie/kitty/fonts/vector.py>`_;
       `kittens/tui/vector.py <https://github.com/adamdeprince/silkie/blob/silkie/kittens/tui/vector.py>`_;
       `kitty/ink.c <https://github.com/adamdeprince/silkie/blob/silkie/kitty/ink.c>`_
   * - Input and platform integration
     - `kitty/mouse.c <https://github.com/adamdeprince/silkie/blob/silkie/kitty/mouse.c>`_;
       `glfw <https://github.com/adamdeprince/silkie/tree/silkie/glfw>`_
   * - Kitten tools and input inspector
     - `kittens <https://github.com/adamdeprince/silkie/tree/silkie/kittens>`_;
       `kittens/show_pointer <https://github.com/adamdeprince/silkie/tree/silkie/kittens/show_pointer>`_
   * - Terminal application helpers
     - `tools/tui <https://github.com/adamdeprince/silkie/tree/silkie/tools/tui>`_
   * - Configuration declarations
     - `kitty/options/definition.py <https://github.com/adamdeprince/silkie/blob/silkie/kitty/options/definition.py>`_
   * - Terminal tests
     - `kitty_tests <https://github.com/adamdeprince/silkie/tree/silkie/kitty_tests>`_

Capability discovery and cleanup
---------------------------------

Query support before enabling an optional mode, apply a timeout when no reply
arrives, and keep a fallback for unsupported features. The OSC 777 capability
registry covers its selector family; graphics formats, SGR additions, and
other protocols have their own discovery rules in their specifications.
Disable modes you enabled when exiting or suspending, as the protocol requires.
Account for remote connections and multiplexers: the application's effective
capabilities depend on the whole path to the terminal.
