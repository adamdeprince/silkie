Packed image formats
====================

Silkie accepts compact grayscale pixels and RGB565 through the existing
`kitty graphics protocol <https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_.
These formats reduce bytes on the connection without requiring a PNG encoder
or compression. They describe the incoming image, independently of the output
display: a color display accepts a one-bit image, and a monochrome display
still accepts RGB, RGBA, and HDR images.

The :doc:`display-protocol` describes the output. Applications can use it to
choose a source format, quantize or dither before transmission when useful,
and retain a richer source format when they need one. Silkie does not reject
an image because its precision or color model differs from the display.

Discovery
---------

The :doc:`capabilities-protocol` lists accepted graphics formats in its
``format`` field, including ``301,302,304,308,516`` for the formats below. This listing
does not depend on the display's color model or precision. The graphics
protocol's ``a=q`` action also probes support for a particular image::

    <ESC>_Ga=q,i=42,f=301,s=8,v=1;VQ==<ESC>\

The one-byte payload is hexadecimal ``55``: eight alternating black and white
pixels. A terminal accepting it replies ``<ESC>_Gi=42;OK<ESC>\`` without
keeping the image. Applications should handle rejection or timeout and fall
back to an accepted format. A :ref:`feature query <graphics_query_features>`,
for example ``<ESC>_Ga=q,i=42,e=t,f=301<ESC>\``, asks the same without sending
pixels.

Format numbers
--------------

Silkie numbers its raw formats by family, the base of the family plus the
number of bits per pixel: ``300`` for gray, ``400`` for :doc:`palette images <palette-image-protocol>`,
``500`` for RGB without alpha and ``600`` for RGBA. Eight-bit gray is
``f=308``, RGB565 is ``f=516`` and RGB10A2 is ``f=632``. The numbers stay
clear of those of the kitty graphics protocol, which uses ``f=8`` and ``f=16``
for its `indexed color formats
<https://sw.kovidgoyal.net/kitty/graphics-protocol/#indexed-color-data>`_,
whose pixels are indices into the terminal's color palette. Silkie supports
those formats as well. The formats of more than eight bits per channel,
``f=316``, ``f=632`` and ``f=664``, are described in
:doc:`precision-image-protocol`. ``f=64`` (half float RGBA, see
:doc:`hdr-protocol`) and ``f=200`` (vector images) keep the numbers they were
released with.

The numbers have moved twice during development. Versions before
2026-11-16 used ``f=1``, ``f=2``, ``f=4``, ``f=8`` and ``f=16`` for the packed
formats; those numbers are no longer accepted for them. Versions from then
until the high precision formats were added used ``f=316`` for RGB565, which
is now ``f=516``; ``f=316`` is now 16-bit gray, so an RGB565 image sent with
the old number is shown as gray instead of being refused. A program can tell
the two apart with a feature query for ``f=516``.

Grayscale: ``f=301``, ``f=302``, ``f=304``, ``f=308``
-----------------------------------------------------

The number of bits per grayscale pixel, ``bits``, is ``f - 300``: one, two,
four or eight. Values are unsigned
intensities: zero is black, and the largest value is white. Pixels are opaque;
there is no alpha channel or palette. The values have the sRGB transfer curve,
not linear-light intensity, unless the ``E`` key of
:doc:`precision-image-protocol` names another curve. The ``G`` gamut key is
ignored for grayscale.

Pixels run left to right, rows top to bottom. Within each byte the leftmost
pixel uses the most significant bits. Each row starts at a byte boundary;
unused low bits in its last byte are ignored. There is no additional stride
or padding between rows.

For width ``s``, height ``v``, and ``bits`` as above, the decoded payload size is::

    row_bytes = ceil(s * bits / 8)
    payload_bytes = row_bytes * v

For example, nine one-bit pixels require two bytes per row. A final byte
``80`` or ``ff`` gives the same ninth white pixel; its seven padding bits do
not begin the next row. In ``f=302``, byte ``1b`` contains four pixels at levels
``0,1,2,3``, corresponding to RGB byte values ``0,85,170,255``.

Silkie expands each intensity equally into red, green, and blue internally.
For these four formats, the exact byte value is
``sample * (255 / ((1 << bits) - 1))``. No output-dependent quantization is
applied to the stored image.

RGB565: ``f=516``
-----------------

Each opaque pixel is a little-endian 16-bit word: red occupies bits 15–11,
green bits 10–5, and blue bits 4–0. Each row is exactly ``s * 2`` bytes, with
no padding. Pixel and row order are the same as grayscale.

Channels use the sRGB transfer curve, unless the ``E`` key of
:doc:`precision-image-protocol` names another one. The ``G`` gamut key has the
same meaning as for ``f=24``. Expansion to the existing eight-bit RGB representation uses
round-to-nearest independently for each channel::

    red   = (r5 * 255 + 15) / 31
    green = (g6 * 255 + 31) / 63
    blue  = (b5 * 255 + 15) / 31

The divisions are integer divisions. Red, green, and blue pixels are the
byte pairs ``00 f8``, ``e0 07``, and ``1f 00`` respectively. This byte order
does not depend on the sender's or terminal's CPU.

Transport and lifecycle
-----------------------

Width and height are required in the existing ``s`` and ``v`` keys. The
payload is base64 encoded as usual. Direct, file, temporary-file, and shared
memory transports all accept these packed formats, as do chunked direct
transfers. File offsets and sizes count packed bytes, not expanded RGB bytes.

``o=z`` is optional: when present, zlib compression wraps the packed bytes
before base64 encoding. A sender can send a one-bit image directly without
compressing it. An eight-pixel one-bit row requires one decoded payload byte,
compared with 24 bytes for the same row in RGB.

The ordinary image operations, placements, clipping, deletion, vector-image
references, and animation-frame uploads work as for other raster inputs.
Packed pixels expand before caching and GPU upload; their wire precision
does not restrict later frame formats or depend on display capabilities.
Existing rules for mixing ordinary and HDR animation frames still apply.

Images of indices into a palette the program sends, with alpha in every
palette slot, are described in :doc:`palette-image-protocol`.

For inherited framing, chunking, placement, and animation fields, see the
`upstream graphics protocol manual
<https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_.
