Spatial display and depth
=========================

Silkie can present text, raster images and retained vector scenes as stereo
content on macOS. The user selects the presentation; applications supply only
optional depth attributes. Ordinary, unmarked content remains on the terminal
plane, and spatial presentation is off by default.

macOS presentation
------------------

XREAL 1S support is **experimental and untested**, and is disabled by default.
To opt in, enable **Settings → Experimental and untested → Experimental XREAL
1S**, or add this configuration under an Experimental and untested comment::

    # Experimental and untested
    experimental_xreal_1s yes

This option gates both side-by-side output and XREAL head tracking. Without it,
those menu entries are disabled and actions cannot activate them. Disabling the
option returns active XREAL presentations to ordinary rendering and stops their
tracking connection. Red/blue output in Follow Head mode works independently.

Choose **View → Spatial Display**:

* **Off** uses ordinary terminal rendering.
* **Follow Head** draws a left-eye view and a right-eye view without using
  head tracking.
* **Anchored Wall** presents a larger terminal canvas and uses the XREAL
  orientation stream to keep it anchored as the user turns their head.
* **Recenter** establishes a new forward direction from the next valid pose.
* **Spatial Display Settings** opens the persistent configuration controls.

Select **Side-by-Side (XREAL)** or **Red/Blue Glasses** independently of the
tracking mode. Red/blue output works on an ordinary display, with a red lens
over the left eye and a blue lens over the right eye. It converts each eye's
colors to luminance before assigning the red and blue channels, so colored
syntax remains visible through both lenses. Color distinctions are lost.
The same depth and vertical-phase limits apply to both output formats.

The modes apply to the current OS window. Switching preserves its sessions,
text and graphics; changing the canvas size resizes the terminal and notifies
applications in the usual way. Shortcuts and remote actions may use::

    map f8 set_spatial_mode off
    map f9 set_spatial_mode follow
    map f10 set_spatial_mode anchored
    map f11 recenter_spatial

On XREAL 1S, select **Full SBS** in the glasses' **3D Mode** menu, use an
extended display, move Silkie to that display, and make it fullscreen. Select
**Follow** and turn the glasses' **Stabilizer** off while Silkie handles head
motion; otherwise motion can be compensated twice. Follow Head also requires
Follow in the glasses. Silkie does not change the glasses' firmware settings.
XREAL documents Full SBS at 3840 × 1080; Silkie takes the framebuffer dimensions
from macOS and gives half its width to each eye. See
`XREAL's 3D mode guide <https://tutorials.xreal.com/docs/glasses/one-series/osd/3d-mode/>`_.

Anchored Wall uses **3DoF orientation tracking**: turning, nodding and tilting.
It does not implement positional tracking or read the XREAL Eye camera. The
menu reports tracking availability. On connection loss the wall returns to
center; reconnecting establishes a fresh reference so an old pose is not
reused. Orientation-only tracking can drift in yaw; use Recenter when needed.

The renderer uses Silkie's existing native OpenGL path and AppKit menus.
The XREAL One-series tracking adapter uses the macOS socket APIs through
Python's standard library. It connects to the glasses' USB-network interface
at ``169.254.2.1:52998`` only while an anchored window exists. The interface
must be available to macOS, and macOS local-network permission may be needed.
No camera is opened and no vendor application is required by Silkie.
The stream is an independently implemented decoder of the framing documented
in `Screen-XREAL-1S <https://github.com/nikryden/Screen-XREAL-1S/tree/main/src/XrealScreen.Device.XrealOne>`_.
Firmware and hardware compatibility require testing with the connected glasses.

Configuration and divergence limits
-----------------------------------

These options live in the Spatial Display settings page and ``silkie.conf``::

    experimental_xreal_1s no
    spatial_mode off
    spatial_output sbs
    spatial_divergence_exponent 0
    spatial_field_of_view 52
    spatial_wall_scale 2
    spatial_positive_divergence 0.5
    spatial_negative_divergence 0
    spatial_vertical_phase_limit 0
    spatial_tracking_address 169.254.2.1:52998

``spatial_mode`` is ``off``, ``follow`` or ``anchored``. ``spatial_output`` is
``sbs`` for side-by-side or ``red-blue`` for anaglyph output. The action
``set_spatial_output red-blue`` changes the current window's output.
The field of view is horizontal, per eye, in degrees.
``spatial_wall_scale`` multiplies the logical
width and height of an anchored canvas relative to one eye's view. It must be
at least one. There is no configured maximum scale; the canvas must fit the
GPU's actual maximum texture dimension, which Silkie checks when enabling it.
Moving to a display with a larger backing framebuffer temporarily reduces
the rendered wall scale if required by that GPU limit; a smaller framebuffer
restores the requested scale.

The positive and negative divergence settings are independent nonnegative
**magnitudes in degrees**, each less than 180. Positive divergence separates
the right-eye image to the right of the left-eye image and places content
behind the terminal plane. Negative divergence crosses the views and places
content in front. ``0`` disables that direction. Positive divergence defaults
to 0.5 degrees. Negative divergence defaults to **zero**, so applications cannot
introduce it until the user explicitly raises that limit. These are rendering preferences, not a
per-user calibration or a claim of visual comfort.

The divergence multiplier is **2^X**, where ``spatial_divergence_exponent``
sets X in the range **[-6, 6]**, in **0.1** steps in Settings. X = 0 gives
1×, X = -1 gives 0.5×, and X = 1 gives 2×. The endpoints give 1/64× and 64×.
Both angular and pixel requests are multiplied before applying the divergence
limits; raising the multiplier cannot bypass those limits.

Depth is measured relative to the neutral terminal plane, using the configured
field of view. The limits clamp the requested disparity at rendering time;
the stored attribute is preserved. They apply equally to text, image
placements and vector elements. Projection of the anchored wall transforms
the eye views together. Custom end-shader pipelines are bypassed during
spatial presentation so that a screen-space effect does not combine the eyes.

``spatial_vertical_phase_limit`` is the nonnegative maximum relative vertical
separation in physical display pixels. Its default is **zero**, which suppresses
all application-requested vertical phase. A nonzero value explicitly enables
it. Positive phase places the right-eye copy below the left-eye copy; negative
phase reverses that direction. As with depth, phase is clamped relative to the
neutral plane before wall projection. Screenshots capture the physical output,
including both eyes for side-by-side output, rather than the larger wall canvas.

Text depth attribute
---------------------

The ``depth`` selector follows :doc:`extension-framing` and advertises
``depth.1`` in :doc:`capabilities-protocol`. Its version is ``1``.

To set the requested divergence of subsequently written cells::

    <OSC>777;depth;v=1:op=set:d=-150<ST>

``d`` is a signed 32-bit integer in **millidegrees**, so ``-150`` requests
negative 0.15 degrees and ``250`` requests positive 0.25 degrees. Zero is the
terminal plane. This is binocular disparity, not a painter's z-index or a
distance in meters. For example, a syntax highlighter can annotate a keyword
without changing its text or color::

    printf '\033]777;depth;v=1:op=set:d=-150\033\\if\033]777;depth;v=1:op=reset\033\\'

For pixel-based depth, use ``p`` instead of ``d``. ``p`` accepts a finite
fractional value, such as ``p=-0.25``. One reference pixel corresponds to one
physical pixel on a 1920-pixel-wide eye view. Other eye widths scale it
proportionally, including the Retina backing scale supplied by macOS. A
1920-pixel eye view applies -0.125 pixels to the left copy and +0.125 pixels to
the right for ``p=0.25`` at multiplier 1×; a 3840-pixel eye view doubles those
offsets. Providing ``d`` and ``p`` together rejects the update. Setting either
replaces the other form of depth.

The optional ``y`` parameter requests relative vertical separation in physical
pixels and also accepts finite fractions. For example,
``op=set:p=-0.25:y=0.125`` sets both attributes. Pixel attributes are stored as
32-bit floating-point values; unrepresentable values are rejected. Text uses
tile-isolated bilinear glyph coverage, including decorations and color glyphs,
so fractional offsets do not snap to an integer or sample neighboring glyphs.
Vector geometry uses the retained renderer's anti-aliasing.
Omitting either parameter preserves that attribute. Both defaults are zero.
Malformed parameters reject the entire update.

Reset both attributes with ``op=reset`` or SGR ``0``. ``op=set:d=0`` resets
only depth; ``op=set:y=0`` resets only vertical phase. Ordinary color and
font changes preserve both. Saving and restoring the cursor preserves them,
and it is retained through scrolling, scrollback and reflow. ANSI-formatted
copying includes depth changes; plain-text copying contains only the text.
Terminal reset and the shell-prompt extension reset also restore zero depth.

An unsupported terminal can ignore the complete OSC and still display the
text following its terminator. The text is outside the OSC payload. Send this
attribute separately from other escape sequences, reset it after the styled
run, and use the capability listing when support must be known. Programs do
not need to know whether stereo presentation is currently enabled: Silkie
retains the attribute while rendering flat.

Raster and vector graphics
---------------------------

Graphics placements inherit the current text depth and vertical phase when created or replaced.
An existing named placement can be placed again at a different depth without
retransmitting its image. The graphics protocol's existing ``z`` key continues
to control painting order; it is independent of stereo depth.

Any drawable vector record or group accepts ``depth=<millidegrees>``. A group
sets the default for its descendants; a child's explicit depth overrides it::

    g id=1 depth=-150
    p id=2 g=1 d=M10,10h80v40h-80Z fill=#4488ff
    t id=3 g=1 depth=250 text=<base64-layout-JSON>

``depth_px=<reference-pixels>`` supplies fractional pixel depth instead of
``depth=<millidegrees>``. ``phase=<pixels>`` sets fractional vertical phase
with the same group inheritance rules.
Explicit depth or phase makes the scene own its spatial attributes; placement
attributes are then neutral, so an explicit flat child can override its group.

This covers paths, symbols placed with ``u``, shaped text, pen strokes and
raster images inside a scene. An explicit vector depth belongs to the scene's
terminal plane, rather than adding to a placement's depth. Scenes containing
explicit depth are rasterized separately for the two eyes and cached; changes
invalidate both views. Without explicit depth the scene uses its placement's
depth as a whole. Off renders the same retained scene with no disparity.

Stereo depth does not change terminal text layout or graphics painting order.
Mouse coordinates are mapped through the wall projection back to the logical
terminal canvas. Per-cell depth remains a visual styling attribute.

Linux
------

The depth attribute is accepted and retained on Linux, where rendering remains
flat. Spatial display and tracking backends are currently macOS-only. A future
Linux backend can use the same attribute and divergence controls.
