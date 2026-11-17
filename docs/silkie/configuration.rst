Silkie configuration
=====================

Silkie reads ``silkie.conf`` from its own configuration directory, normally
``~/.config/silkie/``. It does not automatically read kitty's configuration or
use ``KITTY_CONFIG_DIRECTORY``.

Configuration paths
-------------------

Set ``SILKIE_CONFIG_DIRECTORY`` to use a different directory. Otherwise,
Silkie searches for ``silkie.conf`` in the following order and uses the first
file whose directory is writable:

* ``$XDG_CONFIG_HOME/silkie/``, when ``XDG_CONFIG_HOME`` is set;
* ``~/.config/silkie/``;
* ``~/Library/Preferences/silkie/`` on macOS;
* ``silkie/`` under each directory in ``XDG_CONFIG_DIRS``, in order.

If no configuration exists, Silkie creates ``$XDG_CONFIG_HOME/silkie/`` or
``~/.config/silkie/``. System defaults come from
``/etc/xdg/silkie/silkie.conf`` and are read before the user configuration.
Relative includes, themes, kitten configuration files, and profiles use the
Silkie configuration directory. The Settings window writes ``settings.conf``
there, which is read after ``silkie.conf``.

Use ``--config /path/to/file.conf`` to load a file explicitly, or
``--config NONE`` to skip configuration files. To reuse existing kitty settings,
copy them to ``silkie.conf`` or add an explicit include, for example::

    include ../kitty/kitty.conf

The configuration syntax and inherited settings are described in
`kitty's configuration reference <https://sw.kovidgoyal.net/kitty/conf/>`_.
The paths above are specific to Silkie.

Quitting on macOS
-----------------

Silkie does not bind ``Cmd-Q`` to quit by default. To explicitly enable it,
add this to ``silkie.conf``::

    map cmd+q quit

The application menu's Quit command remains available without a keyboard
shortcut unless you configure one.

Spatial display
----------------

On macOS, **View → Spatial Display** switches between ordinary rendering,
stereo content that follows the head, and an anchored terminal wall. Settings
place XREAL 1S support under **Experimental and untested**, disabled by default.
Enable ``experimental_xreal_1s yes`` to use side-by-side output or XREAL tracking.
Red/blue output without tracking does not require that opt-in. Spatial settings
include side-by-side and red/blue output, independent positive and negative
divergence limits, and a 2^X divergence multiplier with X from -6 to 6 in
0.1 steps. Fractional pixel depth scales with the physical eye resolution,
including Retina backing scale. Negative divergence and vertical phase are
disabled by default and require explicit user configuration. Applications
can assign depth to text and graphics while leaving unmarked content flat.
See :doc:`spatial-protocol` for XREAL setup, configuration, the optional text
attribute, and current tracking support.

Numeric controls enforce the setting's domain: opacity stays within its
documented range, dimensions and durations are nonnegative, and documented
negative sentinels and signed scroll multipliers remain available. Cursor HDR
brightness and HDR headroom start at 1× and step by 0.1, with no invented upper
bound. Composite values retain their complete text rather than losing optional
parameters to a numeric widget.

SSH agent forwarding
--------------------

Set ``ssh_agent_forwarding yes`` to offer the local SSH agent over the terminal
channel. Odin Seat and Skiff can keep a stable ``SSH_AUTH_SOCK`` while the
provider follows the attached terminal. See :doc:`ssh-agent-protocol` for
permissions, connection lifetime, fallback behavior, and protocol details.

The option defaults and descriptions below come from Silkie's configuration
definitions.

E-paper and color switching
---------------------------

Silkie knows which monitor each window is on, and treats a window on an
e-paper monitor as such, even when the operating system thinks it is an
ordinary display, as it does for every e-paper monitor seen so far. It knows
some e-paper monitors by the model in their EDID, never by serial number
(among them the ONYX BOOX Mira and Mira Pro, several DASUNG Paperlike models
and the Modos Glider). Declare any other
with :opt:`monitor_info` in ``silkie.conf``, by its model, connector, EDID
manufacturer and product code, or a part of its description::

    monitor_info MIRA253 kind=eink color=gray bits=4
    monitor_info "OBX:0x0011" kind=eink color=gray
    monitor_info DP-7 kind=eink color=rgb

``kitten @ ls`` shows what Silkie knows of the monitor of each OS window, under
``display``, and ``silkie --debug-rendering`` logs it when a window moves to
another monitor. What follows applies to the windows on that monitor only: a
window dragged to an ordinary monitor looks and behaves as usual again once it
has stayed there for a moment, and comes back when it returns.

E-paper (``kind=eink``) and gray (``color=gray`` or ``mono``) are independent,
as there is color e-paper:

* On e-paper, which ghosts when it changes, the cursor and blinking text do not
  blink, there is no cursor trail, animated images stay on the frame they show,
  an indeterminate progress bar stands still, a keyboard scroll jumps rather
  than glides and the output is not dithered, since dither shows as grain that
  ghosts. For now custom shaders, the visual bell, pixel scrolling and the
  repaint timing stay as configured. Programs are told ``kind=eink`` through the :doc:`display-protocol`,
  so that they can avoid animation and frequent redraws themselves.
* On a display that shows only gray, text is black on white, or white on black
  with ``color_scheme dark``, over the colors of themes, profiles,
  ``launch --color``, sessions and programs, which come back when the window
  moves to another display. Images are shown as they are, and the monitor
  reduces them to its levels of gray.

To treat every window as on e-paper whatever its monitor, add::

    eink_mode yes

With it a display whose colors nothing declares, in Silkie's table,
:opt:`monitor_info` or :opt:`display_info`, is taken to show gray, so a color
e-paper monitor needs its colors declared, for example
``monitor_info "<its model>" kind=eink color=rgb`` or ``display_info color=rgb``.

Press ``ctrl+shift+f12`` to switch between black on white and white on black.
The display is still reported as e-paper in either scheme. Press
``ctrl+shift+alt+f12`` to toggle :opt:`eink_mode`. These shortcuts use
``kitty_mod`` (by default ``ctrl+shift``). The actions are also available from
the command palette. To choose your own shortcuts::

    map f6 set_color_scheme toggle
    map f7 set_eink_mode toggle

Use ``color_scheme light`` or ``color_scheme dark`` for a persistent color
choice on any display. ``set_color_scheme auto`` returns to configured colors
and automatic themes. Explicit light and dark choices take precedence over
automatic themes and OS appearance changes. Reloading the configuration
restores the saved choices.

With ``color_scheme auto``, Silkie follows the OS appearance: it uses
:file:`dark-theme.auto.conf`, :file:`light-theme.auto.conf` or
:file:`no-preference-theme.auto.conf` from the Silkie config directory when
present. With ``automatic_light_theme yes`` and no light theme file, a light
OS appearance selects a built-in light theme, as long as :file:`silkie.conf`
sets no colors of its own; by default the colors stay as configured. To
customize the built-in theme, copy :file:`builtin-light-theme.conf` from the
Silkie package to :file:`light-theme.auto.conf`. Colors from profiles, ``launch --color``, the
ssh kitten and programs stay on top of any theme. The black on white of a gray
display shows over all of them.

:opt:`display_info` declares properties of every display that only change what
programs are told, for example ``display_info kind=eink`` reports e-paper
without changing anything Silkie does.

Display channel precision
-------------------------

The :doc:`display-protocol` reports whether output is RGB, grayscale, or
black-and-white, and the precision of each active channel. System-reported
framebuffer precision does not prove physical panel precision or calibration.
For a panel whose capabilities the system cannot report, declare them::

    display_info color=rgb red_bits=5 green_bits=6 blue_bits=5

Use ``display_info color=gray gray_bits=4`` for sixteen gray levels, or
``display_info color=mono`` for black and white. The report identifies these
values as configured and uses zero for unknown widths. The display protocol
documents the query, replies, subscriptions, and platform detection limits.

When the user changes the terminal's color scheme, profile colors, or output
color capabilities, Silkie asks applications to redraw as it does after a
window resize: it sends ``SIGWINCH`` to each affected terminal's foreground
process group. Applications subscribed to in-band resize notifications also
receive the usual resize report, with the current dimensions even if they have
not changed. Existing color-scheme and display subscriptions keep working.

Moving a window between outputs can change the color model, channel precision,
gamut, or HDR headroom and trigger the same redraw. Refresh-rate changes alone
do not. Application-issued palette changes through OSC escape sequences do
not send resize notifications, so an application's own redraw cannot cause a
palette-change redraw loop.

Fonts and CJK text
------------------

Silkie's default ``font_family`` is ``Iosevka``. Install Iosevka and, for the
serif role, Iosevka Slab. See :doc:`font-protocol` for protocol selection,
font installation links and CJK behavior. Font files are not bundled.

``font_role``
    Repeatable role-to-font mappings, using kitty's font specification syntax.
    The defaults are ``sans Iosevka`` and ``serif Iosevka Slab``. ``default``
    is reserved for ``font_family``. Configured roles can carry features and
    variable-axis settings, and there is no fixed role count.

``cjk_font``
    Default: ``auto``. Prefer regional Sarasa Term, Noto Sans CJK or Noto
    Sans Mono CJK, then regional system fonts and platform glyph fallback.
    An explicit font specification overrides that preference.

``cjk_font_locale``
    Default: ``auto``. Choose ``jp``, ``kr``, ``sc``, ``tc`` or ``hk`` for
    regional CJK glyph forms. Automatic selection follows the process locale,
    defaulting to ``sc`` when unspecified.

Input limits
------------

These limits apply to data a program writes to the terminal. Each one is a
maximum. ``0``, the default, means there is no limit. Set one only to contain
a program that tries to exhaust memory or time. Goblin View uses
``csi-limit`` and ``escape-limit``. Goblin Skiff uses ``--csi-limit``,
``--escape-limit`` and ``--vector-depth-limit``. The dash, raster and curve
limits apply when Silkie rasterizes a vector image.

``csi_limit``
    The maximum number of bytes in one CSI sequence, everything after ``ESC [``
    up to and including its final byte. A longer sequence is ignored. ``0``
    means a CSI may be of any length. Cursor motion and editing that name a
    count are still clamped to the screen.

``escape_limit``
    The maximum number of bytes in one OSC, APC, DCS, PM or SOS sequence, not
    counting its introducer and terminator. A longer sequence is discarded.
    ``0`` means these sequences may be of any length. Clipboard writes are
    still bounded by ``clipboard_max_size``.

``vector_depth_limit``
    The maximum number of groups a vector element may be nested inside. Deeper
    elements are not drawn. ``0`` means any depth. A cycle in the parent chain
    is rejected either way.

``vector_dash_limit``
    The maximum number of dash boundaries stroked for one path. A path that
    would need more is drawn solid. ``0`` means every dash is stroked.

``vector_raster_limit``
    The maximum number of pixels in the raster of one vector image. A larger
    requested size is scaled down to fit. ``0`` means the image is rasterized
    at the size it is shown.

``vector_curve_limit``
    The maximum number of line segments used to flatten one curve or circle.
    ``0`` means as many segments as the flattening error asks for.
