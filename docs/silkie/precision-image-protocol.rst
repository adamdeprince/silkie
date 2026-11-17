High precision images
=====================

Eight bits per channel show visible steps in slow, dark gradients, and they
cannot carry HDR video as it is produced. Silkie accepts three raw pixel
formats with more bits per channel through the existing `kitty graphics
protocol <https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_, and a key
that says which transfer function the values are encoded with, so that an
HDR10 frame (ten-bit PQ in Rec. 2020) can be sent as it comes out of a
decoder:

==========  ==================  ===============  ======================================
Format      Pixel               Bytes per pixel  Alpha
==========  ==================  ===============  ======================================
``f=316``   16-bit gray         2                none, opaque
``f=632``   RGB10A2             4                2 bits: 0, 1/3, 2/3, 1
``f=664``   16-bit RGBA         8                16 bits
==========  ==================  ===============  ======================================

The numbers follow the scheme of :doc:`packed-image-protocol`: the base of
the family (``300`` gray, ``600`` RGBA) plus the bits per pixel. Silkie
keeps these pixels at their full precision all the way to the GPU.

Discovery
---------

The :doc:`capabilities-protocol` lists ``316``, ``632`` and ``664`` in its
``format`` field. A :ref:`feature query <graphics_query_features>` reports a
format and the transfer key together, for example::

    <ESC>_Ga=q,i=31,e=t,f=632,E=16,G=2<ESC>\

is answered with ``<ESC>_Gi=31,e=t,f=632,G=2,E=16;OK<ESC>\`` by a terminal
that shows HDR10 images. A terminal that does not know the ``E`` key may
reject a command carrying it without an answer, so ask first and send the key
only when it is echoed back.

Byte order and layout
---------------------

Every multi-byte value is **little-endian**, as the half floats of ``f=64``
and the words of RGB565 are. That is also the order in which GPUs and every
CPU Silkie runs on hold these formats, so the bytes go to the GPU without
being converted. Pixels run left to right, rows top to bottom, with no
padding between rows: the payload is ``s * v * bytes_per_pixel`` bytes, before
any ``o=z`` compression. Width and height are required in ``s`` and ``v``.

``f=316``, 16-bit gray
    One unsigned 16-bit value per pixel, ``0`` black and ``65535`` white. The
    value ``0x8000`` is sent as the bytes ``00 80``. Gray has no gamut, the
    ``G`` key is ignored.

``f=632``, RGB10A2
    One 32-bit word per pixel, with red in bits 0–9, green in bits 10–19, blue
    in bits 20–29 and alpha in bits 30–31::

        word = r | (g << 10) | (b << 20) | (a << 30)

    This is the layout of OpenGL's ``GL_RGB10_A2`` with
    ``GL_UNSIGNED_INT_2_10_10_10_REV``, Vulkan's
    ``VK_FORMAT_A2B10G10R10_UNORM_PACK32``, Metal's ``rgb10a2Unorm`` and
    DXGI's ``R10G10B10A2_UNORM``, so a frame from any of them can be sent
    unchanged. Worked examples, as they appear on the wire:

    ===============================  ============  ===============
    Pixel                            Word          Bytes
    ===============================  ============  ===============
    opaque red, ``r=1023``           ``c00003ff``  ``ff 03 00 c0``
    opaque gray, ``r=g=b=512``       ``e0080200``  ``00 02 08 e0``
    opaque white                     ``ffffffff``  ``ff ff ff ff``
    transparent black                ``00000000``  ``00 00 00 00``
    ===============================  ============  ===============

    Alpha is straight, not pre-multiplied, as for ``f=32``.

``f=664``, 16-bit RGBA
    Four unsigned 16-bit values per pixel, red, green, blue and alpha in that
    order, each little-endian. Opaque white is eight ``ff`` bytes; red at full
    intensity and half opacity is ``ff ff 00 00 00 00 00 80``. Alpha is
    straight.

The transfer function: the ``E`` key
------------------------------------

The values of a raw image are encoded with the sRGB curve, as they always
were. The ``E`` key names another curve, with the code points of
ITU-T H.273, which HDR video, AVIF and the ``cICP`` chunk of PNG use as well:

=========  ===================================================================
``E``      Transfer function
=========  ===================================================================
``0``      No key: sRGB, the default.
``13``     sRGB, the same as no key.
``8``      Linear: the values are proportional to light, ``1.0`` (the largest
           value) is SDR white.
``16``     PQ, SMPTE ST 2084: absolute luminance up to 10000 nits.
``18``     HLG, ITU-R BT.2100 hybrid log-gamma.
=========  ===================================================================

Any other value is refused with ``EINVAL``. The key belongs in the first
chunk of a chunked transmission. It applies to the formats above and to every
other raw format of RGB, RGBA or gray values: ``f=24``, ``f=32`` and the packed
formats of :doc:`packed-image-protocol`, which are expanded to eight bits
first and keep the curve. ``f=64`` is linear by definition: ``E=8`` is
accepted for it and changes nothing, any other value is refused. The key is
refused for ``f=100`` (a PNG says what it is itself), for ``f=8`` and ``f=16``
(their colors are those of the terminal), for the palette images ``f=401`` to
``f=408`` of :doc:`palette-image-protocol` (their palettes are sRGB), for ``f=200`` (each paint of a
vector image names its own colors) and for GPU surfaces, ``t=g``.

Silkie's linear values have SDR white, the white of ordinary text, at ``1.0``,
and show what is brighter with the headroom of the display, see
:doc:`hdr-protocol`. PQ is absolute, so Silkie takes the reference white of
ITU-R BT.2408, 203 nits, to be SDR white: a PQ value of 203 nits is shown as
bright as text, 406 nits at twice that, and 1000 nits at 4.93 times, which
the display shows if it has that much headroom and clips otherwise. HLG is
relative; Silkie shows it as BT.2100 does on a display with a nominal peak of
1000 nits (system gamma 1.2), which puts the reference white of HLG, a signal
of 0.75, at 203 nits and so at SDR white, and the largest signal at 4.93 times
SDR white. The luminance HLG's system gamma is applied to uses the primaries
of the image, Rec. 2020 when ``G=2``.

The ``G`` key of :doc:`wide-gamut-protocol` combines with ``E``: an image is
decoded with its curve, then converted from its gamut to that of the window.
**HDR10 is** ``f=632,E=16,G=2``. Colors of a PQ or HLG image keep their
brightness above SDR white through the conversion; colors outside the gamut of
the window are clipped to it. ``T=1`` (tone mapping, see :doc:`hdr-protocol`)
works with PQ and HLG images: the peak it compresses from is the brightest
decoded value of the image.

Choosing a format
-----------------

``f=632``
    Ten bits per channel in four bytes, the size of ``f=32``. Enough for
    smooth SDR gradients, and with ``E=16`` or ``E=18`` the format of HDR video.
    Its two bits of alpha suit opaque frames and hard masks, not soft edges.

``f=664``
    Sixteen bits per channel and full alpha, in eight bytes. For gradients
    and masks rendered at high precision, for linear light (``E=8``) where
    eight or ten bits leave the shadows coarse, and for PQ or HLG content with
    soft alpha.

``f=316``
    Sixteen-bit gray in two bytes, for scientific and medical images,
    depth maps and monochrome HDR.

``f=64``
    Half floats in eight bytes, linear extended sRGB: for values that are
    already linear floats, values above SDR white without a curve, and colors
    outside every gamut (negative components). It is also the only high
    precision format that animation frames can use.

Animation frames and composition (``a=f`` and ``a=c``) are not supported for
the formats above, nor for an image with a transfer function other than sRGB;
the terminal answers ``EINVAL``. Use ``f=64`` for animations that need more
than eight bits.

In Silkie
---------

The pixels are kept as they were sent, and uploaded to textures of their own
precision: ``GL_R16`` for 16-bit gray, ``GL_RGB10_A2`` for RGB10A2 and
``GL_RGBA16`` for 16-bit RGBA. All three are required, filterable formats of
the desktop OpenGL 3.1 and later that Silkie draws with, on macOS, Linux and
the BSDs. An eight bit image with a transfer function other than sRGB goes
into an ``GL_RGBA8`` texture rather than an sRGB one. The graphics shader
expands gray to RGB and decodes the curve into Silkie's linear,
extended-range values, so a slow gradient reaches an HDR window, which has a
floating point framebuffer, without being cut to eight bits. A window without
HDR stores eight bits per channel, and other output paths are unchanged. When
an image is scaled, the shader decodes each of the neighbouring pixels and
interpolates in linear light, as the GPU does for sRGB textures, so that edges
and gradients are not darkened by interpolating encoded values. When an image
is shrunk, the pixels a screen pixel covers are averaged, up to four in each
direction. An image drawn at its natural size needs no interpolation.

Vector images that draw one of these images (``img=`` in
:doc:`vector-protocol`) receive its decoded linear values as half floats.
Pointer images (the pointer image protocol) are SDR: values brighter than
SDR white are shown at SDR white.
