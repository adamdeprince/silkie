Large images and image storage
==============================

Silkie draws images of any size the
`kitty graphics protocol <https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_
can describe, and keeps them until the program deletes them. There is no
largest width or height, no quota for image data and no quota for animation
frames. The only bounds are the arithmetic, which is checked for overflow,
and the memory of the computer and of its GPU.

Sizes
-----

kitty refuses images wider or taller than 10000 pixels, PNG images included,
and a payload of more than 400 MB. Silkie accepts any width and height up to
the 32 bits of the ``s`` and ``v`` keys, PNG images up to the 2\ :sup:`31` - 1
pixels the format allows, and payloads of any size. A size whose bytes do not
fit in the address space is refused with ``EOVERFLOW``, and one that does not
fit in memory with ``ENOMEM``. A :ref:`feature query <graphics_query_features>`
echoes ``s`` and ``v`` whatever their value, so
``<ESC>_Ga=q,i=31,e=t,f=24,s=20000,v=100<ESC>\`` tells a program that a
20000 pixel wide image is accepted.

Data sent directly arrives in a buffer that grows with it: a command that
claims a large size and sends little data allocates no more than it sends.

The keys of the protocol are 32 bits, so the size ``S`` and offset ``O`` of
data read from a file or shared memory object cannot name more than 4 GiB.
Without ``S``, the whole file is read, whatever its size.

Drawing large images
--------------------

A GPU has a largest texture, often 8192 or 16384 pixels square, and Silkie
uses at most 8192 on macOS, where a computer can switch between GPUs. An
image wider or taller than that is split into tiles of at most 4096 pixels
square, each in a texture of its own, and every placement is drawn as the
parts of the tiles it shows. An image that fits a texture is drawn from one,
as before.

Crops (the ``x``, ``y``, ``w`` and ``h`` keys), scaling, Unicode
placeholders, z-index layers, stereo depth, HDR, wide gamut, indexed color
and palette images, and the high precision formats all work the same with
tiles. Each tile has an apron of one pixel all round, the pixels of its
neighbors, so filtering where two tiles meet reads the same pixels it would
in one texture and no seam shows.

Tiles are uploaded to the GPU only when a placement shows them, and a tile
that has not been on screen for five seconds is freed, so an image larger
than the memory of the GPU can be shown a part at a time. When the GPU runs
out of memory, the tiles not on screen are freed and the upload is tried
again; a tile that still does not fit is not drawn.

Images drawn smaller than they are
----------------------------------

kitty samples an image drawn smaller than it is at a few pixels round the
center of each screen pixel, which makes a large image drawn small alias:
thin lines vanish and patterns shimmer. Silkie gives an image mipmaps,
smaller copies of it, as soon as a placement shows it smaller than it is, and
the GPU filters between them (trilinear filtering).

* The GPU makes the mipmaps of eight bit sRGB, linear and half float images.
* Images the shader decodes, those with a PQ or HLG transfer function and the
  high precision formats of :doc:`precision-image-protocol`, are decoded on
  the CPU into linear half floats, which the GPU mipmaps in linear light.
  The tiles of such an image always hold its decoded pixels.
* The pixels of indexed color and palette images are indices into a palette
  that can change, so their mipmaps are made of indices: each is the most
  common index of the four pixels it stands for. The shader looks up the
  colors of the two levels nearest the size drawn and blends them, as
  trilinear filtering would. A detail one pixel wide in an index of its own
  can vanish at a small size rather than shimmer.

Storage
-------

Images stay until the program deletes them or they leave the scrollback, as
kitty does without its quota. When memory runs out while an image is
transmitted, decoded or composed, Silkie evicts images that are not on
screen, transient ones (``N=1``) first and then the least recently used,
until they have freed as much as was needed, and tries once more. If that
fails, or the images it could evict would not free enough, the command fails
with ``ENOMEM``; the terminal goes on. Image data the terminal has written to
its disk cache holds no memory, so eviction frees memory only of data still
in RAM.

Errors
------

=============  =========================================================
Code           Cause
=============  =========================================================
``EOVERFLOW``  dimensions whose size in bytes does not fit in the address
               space
``ENOMEM``     memory ran out, after evicting what could be evicted
=============  =========================================================
