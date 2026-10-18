HDR brightness protocol
==========================

.. versionadded:: 0.50.0

Many modern displays can show colors brighter than the white used for normal
content, which Apple calls *extended dynamic range* (EDR). This protocol lets
programs request that extra brightness for text, for images and for the cursor,
without changing how anything else is displayed.

Everything is expressed relative to *SDR white*, the white of normal text,
which is rendered exactly as bright as it always has been. A brightness of
``100`` percent is SDR white, ``200`` percent is twice as bright in linear
light, and so on. How much extra brightness the display can show is called the
*headroom*, also a multiple of SDR white: a headroom of ``1`` means the display
has no extra range, a headroom of ``4`` means it can show up to four times SDR
white. The headroom changes with the brightness setting of the display and
when a window moves between screens.

The terminal clamps every request to the headroom it currently has. On a
display without HDR, or when the user has HDR turned off, every request is
clamped to ``1`` and the output is identical to what it was before this
protocol existed. Requests are never used to make anything dimmer, so programs
written without HDR in mind look the same in a terminal that implements this
protocol as in one that does not.

Text
------

Two new SGR attributes set the brightness of the foreground and the background
of subsequently written cells, as a percent of SDR white, using the colon
sub-parameter form::

    <ESC>[56:<percent>m      foreground brightness
    <ESC>[57:<percent>m      background brightness

For example, to show text in orange at two and a half times SDR white::

    <ESC>[38;2;255;136;0m<ESC>[56:250mHello<ESC>[m

The brightness is independent of how the color was chosen. It applies to
colors set with ``38``/``48``, to the sixteen basic colors, to colors from the
256 color palette and to the default foreground and background, so a program
can ask for the default text color, brighter, without knowing what that color
is. The text decorations of a cell (underline, strikethrough) use the
foreground brightness. Under reverse video the brightness follows the color it
was set with, so a reversed cell with a foreground brightness of 200 percent
shows a background at 200 percent.

The attributes are reset by ``<ESC>[0m``, by ``56`` and ``57`` without a
sub-parameter, and by a value of ``100`` or less. Values are rounded to quarter
steps (``125``, ``150``, ``175``...) and clamped to ``1675``. A request larger
than the headroom is shown at the headroom. The *dim* attribute (``2``) and
brightness are independent, dim text at 200 percent is dim relative to a
white twice as bright as usual.

Terminals that do not know these attributes ignore them, since that is what
most terminals do with unknown SGR parameters and their sub-parameters, so a
program can emit them unconditionally. A terminal that drops a whole SGR
sequence containing an unknown parameter would also lose any color set in the
same sequence, so send the brightness in its own ``<ESC>[...m`` after the
color, as in the example above. The percent is only recognized as a colon
sub-parameter, ``<ESC>[56;250m`` is ``56`` with no value followed by an
unrelated parameter.

Images
--------

The :doc:`graphics-protocol` gains a pixel format for HDR images, ``f=64``.
The data is RGBA with four 16-bit floating point numbers (IEEE 754 half
precision, little endian) per pixel, in *linear* extended sRGB where ``1.0`` is
SDR white and values above it are brighter. Alpha is straight (not
pre-multiplied), as for ``f=32``. The other keys work as for ``f=32``: the
width and height must be given with ``s`` and ``v``, the data may be
compressed with ``o=z``, and it may be sent by any transmission medium.
Animation frames of such an image must also be ``f=64`` (and ``f=64`` frames
cannot be added to other images), the terminal responds with ``EINVAL``
otherwise. Frames are blended in linear light, and the background color of a
frame (the ``Y`` key) is taken as linear values, ``255`` being SDR white. The
gamut key of the :doc:`wide-gamut-protocol`, ``G``, is ignored for an HDR
image: its colors are extended sRGB by definition, which reaches the colors
outside sRGB with values below zero and above one.

Pixel values larger than the headroom are shown at the headroom, which
clips the bright parts of an image on a display with little headroom. An
image can ask instead for its bright parts to be compressed into the headroom
with ``T=1`` on the command that transmits it. Then, with *H* the headroom,
*P* the largest color component in the image (at least ``1.0``, and including
the animation frames loaded so far) and *k* = 0.75 *H* the knee, a pixel
whose largest component *m* is above the knee is scaled by *f(m)/m* where::

    f(m) = k + (m - k) / (1 + s (m - k))      s = 1 / (H - k) - 1 / (P - k)

so that everything up to the knee is unchanged, *P* lands exactly at *H*, the
curve is continuous at the knee and the hue of a pixel is preserved. When *P*
is within the headroom nothing is changed. ``T=0``, the default, clips.

To check whether a terminal supports ``f=64``, use the query action described
in the graphics protocol with ``f=64`` and a single pixel of data, a terminal
that does not know the format responds with ``EINVAL``.

The :doc:`vector-protocol` uses the same HDR presentation path. Brightness
suffixes and linear extended-sRGB paints work in every vector primitive,
including styled text, strokes, gradients and symbols. Nested clips and
group opacity preserve HDR values; embedded ``f=64`` images retain their
linear colors. Vector images also accept ``T=1`` for tone mapping.

Cursor
---------

In Silkie the brightness of the block cursor is a configuration option,
:opt:`cursor_brightness`. A cursor brighter than any text makes it easy to
find on a crowded screen.

Querying the headroom
------------------------

A program that wants to know how much extra brightness is available, for
example to pick between several renderings of an image, can ask for the
headroom of the window it is running in with the ``XTGETTCAP`` query
mechanism, see :doc:`kittens/query_terminal`. The query name is
``kitty-query-hdr_headroom`` and the answer is the headroom as a decimal
number, ``1`` when there is none. For example, encoding the query name as
hex::

    <ESC>P+q6b697474792d71756572792d6864725f68656164726f6f6d<ESC>\

The headroom changes over time, so re-query it before relying on a value
obtained a while ago. This query sends no notification of changes; a program
that wants to be told turns on the events of the :doc:`display-protocol`,
whose ``headroom`` field is the same value. On macOS the display fades its
extended range in over the first second or so after an HDR window appears, so
a value obtained at startup is often smaller than one obtained a moment later.

Configuration
----------------

In Silkie, HDR output is off by default and enabled with the :opt:`hdr` option.
The user can cap how bright anything gets with :opt:`hdr_max_headroom`, which
the terminal applies before clamping to the headroom of the display, and that
cap is what the query above reports.

HDR content is displayed on macOS, and on Wayland when two things hold:

* The compositor implements the ``color-management-v1`` protocol with
  parametric image descriptions, the extended linear transfer function and
  settable luminances. Mutter 50 does. A window then draws linear values and
  tells the compositor that they reach from black to as many times the
  reference white of the display as the content gets bright.
* The graphics driver has EGL window surfaces with floating point color
  (``EGL_EXT_pixel_format_float``), since eight bits per channel cannot hold
  values above white. Mesa's drivers for graphics hardware are expected to
  have them, which is untested. The NVIDIA driver (version 595) does not, and
  neither does Mesa when it renders in software, so there the window stays as
  it is without :opt:`hdr`.

The headroom on Wayland is the brightest luminance the compositor says the
display of the window can show, divided by the luminance of its reference
white. It is ``1`` on a display that is not in HDR mode and changes when the
display is switched into or out of it. A compositor that does not know the
peak of a display, a virtual one for instance, reports the limit of the signal
instead, which makes the headroom about ``49``; :opt:`hdr_max_headroom` is what
keeps Silkie from using that.

The values of an HDR window on Wayland are in the sRGB primaries unless
:opt:`wayland_colorspace` asks for a wider gamut, see
:doc:`wide-gamut-protocol`. The compositor is then told of the wide primaries
in the same description that says the values are linear and how bright they
get, since a window has only one, and what is drawn is converted to those
primaries. A compositor that takes linear values only in the sRGB primaries
leaves the window an HDR window in sRGB. This combination has been checked
against Mutter 50 in the same way and to the same extent as HDR itself.

``kitty --debug-rendering`` says whether a window got a floating point surface
and what the compositor was told. The Wayland support has been run against
Mutter 50 with every step checked except the floating point surface itself,
for want of a driver that has one, and it has not been seen on an HDR monitor.
On X11 and on Wayland without the above the headroom is always ``1``.
