Palette images
==============

Silkie accepts images whose pixels are one-, two-, four- or eight-bit indices
into a palette the program sends with them, through the existing
`kitty graphics protocol <https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_.
Every palette slot is a full RGBA color with its own eight-bit alpha, so
transparency and translucency are not limited to one reserved index as in
GIF. The palette can be replaced later without sending the pixels again,
which makes color cycling, fades and highlights cheap.

Discovery
---------

The :doc:`capabilities-protocol` lists ``401,402,404,408`` in its ``format``
field. A :ref:`feature query <graphics_query_features>` asks about one
format, for example ``<ESC>_Ga=q,i=42,e=t,f=404<ESC>\``, and
``<ESC>_Ga=q,i=42,e=e,x=1,w=2<ESC>\`` asks whether palette updates of a
range of slots are understood. A normal ``a=q`` with data checks one
particular image.

Formats: ``f=401``, ``f=402``, ``f=404``, ``f=408``
---------------------------------------------------

The number of bits of each index, ``bits``, is ``f - 400``: one, two, four or
eight, so the palette has ``2 ** bits`` slots: 2, 4, 16 or 256. The numbers
mirror the :doc:`packed grayscale formats <packed-image-protocol>`, which
are ``300`` plus their bits per pixel.

Width and height are required in the ``s`` and ``v`` keys. The decoded
payload is the palette followed by the indices:

The palette
    Always every slot, in slot order, four bytes each: red, green, blue and
    alpha. Alpha is straight, not multiplied into the colors: ``0`` is
    transparent and ``255`` opaque. Colors use the sRGB transfer curve. The
    ``G`` gamut key, if present, tags the palette colors as it does the
    pixels of ``f=24`` and ``f=32``, see :doc:`wide-gamut-protocol`.

The indices
    Packed exactly as the grayscale formats pack their samples: pixels run left
    to right and rows top to bottom; within each byte the leftmost pixel uses
    the most significant bits; each row starts at a byte boundary and the
    unused low bits of its last byte are ignored.

Because the palette always has a slot for every value an index can take, no
index can be out of range. For width ``s`` and height ``v``::

    palette_bytes = 4 * 2 ** bits
    row_bytes = ceil(s * bits / 8)
    payload_bytes = palette_bytes + row_bytes * v

A payload of another size is an error, see `Errors`_.

Example
~~~~~~~

A three by two image in ``f=402``, two bits per index, has a four slot
palette and one byte per row:

=========  ==================  ====================================
Bytes      Meaning
=========  ==================  ====================================
00000000   slot 0              transparent
ff0000ff   slot 1              opaque red
0000ff80   slot 2              blue, half transparent
ffffffff   slot 3              opaque white
6c         row 0               ``01 10 11 00``: indices 1, 2, 3 and
                               two unused bits
34         row 1               ``00 11 01 00``: indices 0, 3, 1 and
                               two unused bits
=========  ==================  ====================================

Those eighteen bytes, base64 encoded, are sent as::

    <ESC>_Ga=T,i=7,f=402,s=3,v=2;AAAAAP8AAP8AAP+A/////2w0<ESC>\

Palette updates
---------------

The ``e`` action, which edits a :doc:`vector image <vector-protocol>`,
replaces palette slots of a palette image. Its payload is the new slots, four
bytes each as in the palette above, and no indices::

    <ESC>_Ga=e,i=7,x=1,w=1;AP8A/w==<ESC>\

makes slot 1 of the image above opaque green, which recolors every
placement of the image, and every vector image that draws it, without sending
its pixels again. The keys are:

``x``
    The first slot to replace, ``0`` if absent.

``w``
    The number of slots to replace, if absent every slot from ``x`` to the
    end of the palette. So an update with neither key replaces the whole
    palette, and its payload is the size of the palette.

``i`` or ``I``
    The image, as for other actions on an existing image.

``G``
    Optional. The colors of a palette are all in the gamut of the image, so
    if present it must be the gamut the image was sent with.

The payload is ``4 * w`` bytes. It can be sent directly, from a file, a
temporary file or shared memory, chunked with ``m``, and compressed with
``o=z``, exactly as image data. ``q`` suppresses responses as usual.

Animation frames and composition
--------------------------------

A palette image has a single frame, its palette being the way to change how
it looks. Sending an animation frame (``a=f``) for a palette image, a frame in
a palette format for another image, or composing frames (``a=c``) of a
palette image is refused with ``EINVAL``. Sending the image again with
``a=t`` or ``a=T``, in any format, replaces it as for other images.

Errors
------

=============  =========================================================
Code           Cause
=============  =========================================================
``EINVAL``     zero or missing ``s`` or ``v``; ``s`` or ``v`` above the
               largest image dimension the terminal accepts; a format
               number between the palette formats, such as ``f=403``; an
               animation frame or composition, see above; compressed
               data that does not inflate to the expected size; a palette
               update whose slots are not all in the palette, or with
               another gamut; ``a=e`` for an image that is neither a
               palette nor a vector image
``ENODATA``    less data than the format and size, or the update, need
``EFBIG``      more data than they need, sent directly
``EOVERFLOW``  dimensions whose size does not fit in memory
``ENOENT``     an update for an image that does not exist
``ENOMEM``     the terminal ran out of memory
=============  =========================================================

Differences from kitty's indexed color formats
----------------------------------------------

The kitty graphics protocol's indexed color formats, ``f=8`` and ``f=16``,
which Silkie also supports, index the terminal's own palette: the 256 SGR
colors, with the last eight slots being special colors such as the default
foreground and background. Such an image follows the color theme of the
terminal. A palette image has a palette of its own, chosen by the program,
with alpha in every slot rather than in every pixel as with ``f=16``; it
looks the same whatever the terminal colors are, and only changes when the
program updates its palette. Palette images also pack their indices into
one, two or four bits when the palette is small.

Rendering
---------

Silkie keeps the indices and the palette apart on the GPU and looks the colors
up when drawing, filtering after the lookup when the image is scaled. Palette
colors are standard dynamic range: they are not brightened in an HDR window,
and they are converted from the gamut of the image to that of the window
like other image pixels.
