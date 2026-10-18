Silkie configuration
=====================

These options configure Silkie's extensions in ``kitty.conf``. For the file
location, syntax, and inherited settings, see `kitty's configuration reference
<https://sw.kovidgoyal.net/kitty/conf/>`_.

The option defaults and descriptions below come from Silkie's configuration
definitions.

E-paper and color switching
---------------------------

For an external e-paper monitor, including a BOOX monitor that the operating
system treats as an ordinary display, add this to ``kitty.conf``::

    eink_mode yes

Silkie uses black text on white and reports ``kind=eink`` to applications
through the :doc:`display-protocol`. Applications can use that information to
avoid animation, smooth scrolling and frequent redraws. Software must support
the protocol to use it.

Press ``ctrl+shift+f12`` to switch between black on white and white on black.
The display is still reported as e-paper in either scheme. Press
``ctrl+shift+alt+f12`` to toggle e-paper mode. These shortcuts use ``kitty_mod``
(by default ``ctrl+shift``) and affect all Silkie windows. The actions are also
available from the command palette. To choose your own shortcuts::

    map f6 set_color_scheme toggle
    map f7 set_eink_mode toggle

Use ``color_scheme light`` or ``color_scheme dark`` for a persistent color
choice on any display. ``set_color_scheme auto`` returns to configured colors
and automatic themes, or to black on white while e-paper mode is enabled.
Explicit light and dark choices take precedence over automatic themes and
OS appearance changes. Reloading the configuration restores the saved choices.

E-paper mode declares the display kind without assuming its color depth or
refresh timing. For a panel known to have sixteen levels of gray, also set::

    display_info color=gray bits=4

Leave that line out for a color e-paper monitor, or declare its actual
capabilities. ``display_info kind=eink`` remains available as a reporting-only
override that leaves the color scheme unchanged. These declarations apply to
all windows in the Silkie process, so toggle the mode when moving between an
ordinary monitor and an e-paper monitor.

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
