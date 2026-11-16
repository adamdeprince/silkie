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
``format`` field, including ``301,302,304,308,316`` for the formats below. This listing
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

Each packed format is ``300`` plus its number of bits per pixel, so that the
numbers stay clear of those of the kitty graphics protocol, which uses ``f=8``
and ``f=16`` for its indexed color formats. Earlier development versions of
Silkie used ``f=1``, ``f=2``, ``f=4``, ``f=8`` and ``f=16`` for these formats;
those numbers are no longer accepted for them.

Grayscale: ``f=301``, ``f=302``, ``f=304``, ``f=308``
-----------------------------------------------------

The number of bits per grayscale pixel, ``bits``, is ``f - 300``: one, two,
four or eight. Values are unsigned
intensities: zero is black, and the largest value is white. Pixels are opaque;
there is no alpha channel or palette. The values have the sRGB transfer curve,
not linear-light intensity. The ``G`` gamut key is ignored for grayscale.

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

RGB565: ``f=316``
-----------------

Each opaque pixel is a little-endian 16-bit word: red occupies bits 15–11,
green bits 10–5, and blue bits 4–0. Each row is exactly ``s * 2`` bytes, with
no padding. Pixel and row order are the same as grayscale.

Channels use the sRGB transfer curve. The ``G`` gamut key has the same meaning
as for ``f=24``. Expansion to the existing eight-bit RGB representation uses
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

For inherited framing, chunking, placement, and animation fields, see the
`upstream graphics protocol manual
<https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_.
