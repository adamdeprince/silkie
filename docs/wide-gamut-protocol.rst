Wide gamut colors
=====================

.. versionadded:: 0.50.0

Terminal colors are sRGB. Most displays made in the last few years can show
colors outside that gamut, the deeper reds and greens of Display P3 and the
still larger Rec. 2020 gamut, but a program has no way to ask for them. This
extension lets a program say which gamut an RGB color is in, using a slot the
existing escape code already has, so that nothing changes for programs that
do not use it.

Choosing a gamut
-------------------

The colon form of the RGB color SGR has an optional color space id between
the ``2`` and the red component, defined in ITU-T T.416 but never given a
meaning by terminals, which skip it. This extension gives it one::

    <ESC>[38:2:<id>:<r>:<g>:<b>m      foreground
    <ESC>[48:2:<id>:<r>:<g>:<b>m      background

``id`` is ``0`` for sRGB, which is the same as leaving it out, ``1`` for
Display P3 and ``2`` for Rec. 2020. Any other id is treated as ``0``. The
components are 0 to 255, encoded with the sRGB transfer curve in every gamut,
so only the primaries change with the id and a terminal needs a single
curve. The brightest red of Display P3 is::

    <ESC>[38:2:1:255:0:0m

The gamut belongs to the RGB color it was given with. Selecting one of the
sixteen basic colors, a color from the 256 color palette or the default color
also selects sRGB, as does an RGB color without an id. Under reverse video
the gamut follows the color, so a reversed cell with a Display P3 foreground
shows a Display P3 background. Decorations, underline and strikethrough, use
the foreground and its gamut unless a decoration color is set with ``58``,
which is always sRGB.

Terminals that do not know the id skip it, so a program can emit the id
unconditionally and get the same color interpreted as sRGB elsewhere, which
is a little less saturated and never wrong. The id is only recognized in the
colon form. The semicolon form ``<ESC>[38;2;<r>;<g>;<b>m`` has no id, since
the parameters it is followed by are unrelated attributes.

Images
---------

The pixels of an image sent with the :doc:`graphics-protocol` are sRGB as
well. The ``G`` key of a command that transmits them says otherwise, with the
ids used for text::

    <ESC>_Gf=24,s=10,v=20,G=1;<payload><ESC>\

``G=1`` is Display P3 and ``G=2`` is Rec. 2020. ``G=0``, any other value and
no key at all are sRGB. As for text, the components are encoded with the sRGB
transfer curve in every gamut. The key applies to RGB and RGBA data, ``f=24``
and ``f=32``, and belongs in the first chunk of a chunked transmission. It
applies in the same way to the pixels of a GPU surface, ``t=g``. It is ignored
for ``f=64``, whose pixels are linear extended sRGB by definition, see the
:doc:`hdr-protocol`. A vector image, ``f=200``, also ignores ``G``: each paint
names its own color space in the :doc:`vector-protocol`. Vector text, paths,
gradients and embedded images share a linear extended-sRGB compositor and
retain wide-gamut and HDR values through clipping and blending.

A PNG image, ``f=100``, says what its colors are by itself, and the key is
ignored for it. A PNG is in Display P3 or Rec. 2020 when its embedded profile
has the primaries of that gamut, whatever the curve of the profile, or when
its ``cICP`` chunk names those primaries together with the sRGB transfer
function. Any other PNG is sRGB, converted to it if it carries a profile, as
it always was.

All the frames of an animated image are in the gamut of the image, which is
that of the data it was created with. A frame transmitted in another gamut is
converted to that of the image when it is loaded, so a program may send every
frame as it has it.

Unlike the id of a text color, the key is not skipped by a terminal that does
not know it: such a terminal may reject the whole command, without an answer.
A program therefore asks first, with the query action of the graphics protocol
and a single pixel::

    <ESC>_Gi=31,a=q,f=24,s=1,v=1,G=1;AAAA<ESC>\

and sends the key only to a terminal that answers ``OK``.

What the terminal does with it
------------------------------------

The terminal converts a color tagged with a gamut to the gamut it draws in,
through CIE XYZ, and clips what the display cannot show. Colors without an id
are what they always were, the terminal's own colorspace, and are not
touched, so programs that do not use the id are rendered exactly as they were
before this extension existed. When a terminal draws in sRGB, a Display P3 or
Rec. 2020 color is shown as the nearest sRGB color, which is close to, though
not the same as, reading its components as sRGB. Only when the terminal draws
in a wider gamut do the extra colors appear. An image in a gamut is converted
in the same way, and an image without one is drawn exactly as before.

A program that wants to know whether asking for wide gamut colors is
worthwhile asks for the ``gamut`` of the display with the ``display`` query
of the display information protocol, where the terminal has one, and
otherwise simply emits the id. Brightness above SDR white is a separate
attribute, the ``56`` and ``57`` of the HDR brightness protocol, and the two
combine: a Display P3 red at twice SDR white is
``<ESC>[38:2:1:255:0:0m<ESC>[56:200m``.

In Silkie
------------

In Silkie the gamut is stored with the color of the cell and survives
everything the color survives: scrollback, copying with formatting, and
``DECRQSS`` reports of the current SGR state, which include the id when it is
not ``0``. On macOS the window draws in the colorspace selected by
:opt:`macos_colorspace`, and tagged colors are converted to it: with ``srgb``
Display P3 and Rec. 2020 colors are clipped to sRGB, with ``displayp3``
Display P3 colors are shown as they are and Rec. 2020 colors are clipped to
Display P3, while untagged colors keep being interpreted as Display P3, as
that option has always done. With ``default`` the window is in the native
colorspace of the display and every color is passed through unconverted.

On Wayland the window draws in sRGB unless :opt:`wayland_colorspace` asks for
``displayp3``, ``rec2020`` or ``auto`` and the compositor implements color
management, the ``color-management-v1`` protocol. The compositor is then told
that the window is in that gamut, and colors and images tagged with a gamut
are converted to it: in a ``displayp3`` window Display P3 colors are shown as
they are and Rec. 2020 colors are clipped to Display P3. Unlike on macOS,
untagged colors stay sRGB: they are converted to the gamut of the window as
well, so everything that does not ask for a wider gamut looks as it does in
an sRGB window. ``auto`` picks the widest of the three gamuts whose primaries
the display reaches, going by what the compositor reports for it. The option
takes effect on existing windows when the configuration is reloaded.

The window is declared to the compositor with the primaries of its gamut and
a gamma of 2.2, the display the colors of a terminal are made for and what a
compositor takes a window that declares nothing to be. Grays therefore have
the same values in a wide window as in an sRGB one. The conversion between
primaries is done in the linear light of the sRGB curve, as all blending in
Silkie is, which can move a saturated untagged color by one step in 255 from
what an sRGB window shows. A compositor that offers no gamma of 2.2 is given
the sRGB curve instead. On a compositor without the protocol, and on X11, the
window draws in sRGB.

A window that also shows HDR, see :opt:`hdr` and the :doc:`hdr-protocol`, has
a single description like any other, and it carries both: the primaries of
the gamut chosen here, linear values in place of the gamma, and the
luminances the contents reach. Colors are converted to the gamut of the window
as above, and what is brighter than SDR white stays so in that gamut. When the
compositor does not take linear values in the wide primaries, the window
keeps its HDR and is drawn in sRGB; when the window cannot show HDR at all, it
is a wide window as described above.

The profile of a PNG is taken to be Display P3 or Rec. 2020 when each of its
three primaries is within 0.005 in x and in y of the primary of that gamut,
both adapted to the D50 white point profiles are made for. The pixels are
then converted from the curve of the profile to the sRGB curve and nothing
else. Other uses of PNG in Silkie, such as background images and window logos,
are sRGB as before.
