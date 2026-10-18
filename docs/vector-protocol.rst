Vector images
================

.. versionadded:: 0.50.0

The :doc:`graphics-protocol` moves pixels. A program that draws, a document
viewer, a plotting library or a pen application, has to rasterize everything
itself and send the pixels again whenever the window is resized, the view is
zoomed or a stroke is added, and over a slow link that is what makes such
programs feel remote. Vector images are a pixel format of the graphics
protocol, ``f=200``, whose data is not pixels but a small scene: paths, shaped
text, glyph outlines, pen strokes, raster images and gradients. The terminal keeps the
scene and rasterizes it itself, at the exact size the image is shown at, so it
is crisp at any zoom, and a program changes it with patches a few bytes long.
The format is rich enough to render a PDF page, see `Rendering a PDF`_.

Because a vector image is an ordinary image to the rest of the protocol,
everything else works unchanged: placement with ``a=p``, anchoring to cells so
the image scrolls with the text and into the scrollback, the alternate screen,
``z`` ordering with the text, Unicode placeholders, relative placements,
chunked transmission with ``m=1``, compression with ``o=z``, every transmission
medium, deletion with ``a=d`` and the query action ``a=q``. A relay or
multiplexer that stores images stores vector images the same way.

Transmitting a vector image
-------------------------------

The data is UTF-8 text, one *record* per line, transmitted with ``a=t`` or
``a=T`` and ``f=200``. The ``s`` and ``v`` keys are required and give the
*intrinsic size* of the image in *user units*, the coordinate system of the
records. The image is shown like a raster image of that many pixels, so with
no ``c`` or ``r`` key it occupies ``s`` by ``v`` pixels, and with them the
scene is scaled to fit the cells. For example, a red square on a white page::

    <ESC>_Ga=T,f=200,i=1,s=200,v=100,c=40,r=10;<base64 of the records below><ESC>\

    p id=1 d=M0,0L200,0L200,100L0,100Z fill=#ffffff
    p id=2 d=M20,20L80,20L80,80L20,80Z fill=#ff0000

Transmitting again with the same image id replaces the whole scene, as it
does for any image. With ``o=z`` the uncompressed size need not be given. The
terminal answers ``OK`` or, for a scene it cannot parse, ``EINVAL`` with the
line number and the problem, and nothing of such a transmission is kept.

Editing a vector image
-------------------------

The action ``a=e`` applies the records in its payload to the existing vector
image given by ``i`` or ``I`` as a patch: an element with the id of an
existing element replaces it, a new id adds an element, ``x`` records delete
and ``a`` records append geometry. The patch is applied as a whole or, when a
record is malformed, not at all, with ``EINVAL`` as the response. A patch to an
image that does not exist answers ``ENOENT`` and to a raster image
``EINVAL``. Chunking and compression work as for ``a=t``::

    <ESC>_Ga=e,i=1;<base64 of "g id=0 t=2,0,0,2,-100,-50">\ <ESC>\

That one record zooms the whole scene to twice its size around the point
``100,50``, which is how a viewer pans and zooms a page: the terminal
re-rasterizes, nothing is sent again. Because the terminal rasterizes at the
size of the placement, a program that wants a sharper image after the user
zoomed need not do anything either.

Records
-----------

A record is a type, one character, followed by fields of the form
``key=value`` separated by single spaces, with no quoting and no spaces inside
a value. Unknown keys and unknown record types are ignored, so the format can
grow. Blank lines are ignored. Numbers are decimal and may have a fraction and
an exponent. Lists of numbers are separated by commas.

Every element has an ``id``, a 32 bit unsigned integer chosen by the program,
unique across all record types of the image, and every drawable element has
``g``, the id of the group it belongs to, defaulting to the root group, which
has id ``0`` and always exists. Elements are drawn in the order they were
created, within their group, and groups among their siblings likewise.
Replacing an element keeps its place in that order.

``g`` *group*
    ``g id= [p=<parent group>] [t=<transform>] [clip=<path id>] [box=x,y,width,height] [o=<opacity>] [blend=normal|multiply|screen] [v=0|1]``

    Groups nest through ``p`` and carry the state a PDF or SVG renderer
    pushes and pops: a transform, a clip, an opacity and a blend mode.
    ``clip`` names a ``p`` or ``s`` element whose geometry, in the group's
    coordinate system and with its fill rule, clips everything in the group,
    intersected with the clips of the enclosing groups. ``o`` is a percent,
    ``100`` by default; a group with less is composited as a whole, so
    overlapping children do not show through each other. ``blend`` applies to
    every drawing operation inside the group, ``multiply`` makes a highlighter.
    ``v=0`` hides the group and its subtree without deleting it. The root
    group accepts all of these except ``p``, so ``g id=0 t=...`` pans and
    zooms the whole scene.

    ``box`` is a rectangular clip in this group's coordinate system. Width
    and height must be nonnegative; a zero dimension clips everything away.
    It intersects ``clip`` and every enclosing group's clip. See
    `Nested window clips`_. Replacing a group without ``box`` removes its box.

``t`` *text*
    ``t id= [g=] text=<base64 UTF-8 JSON> [t=<transform>]``

    Shapes and retains a string or styled runs as vector outlines. The JSON
    layout object is described under `Text layout and measurement`_. Text
    participates in group transforms, clipping, opacity, blending and normal
    element replacement and deletion. Its outlines are generated once when
    the record is accepted, then rasterized at the placement's resolution.

``p`` *path*
    ``p id= [g=] d=<path> [fill=<color>|none] [rule=nz|eo] [stroke=<color>] [w=<width>] [cap=butt|round|square] [join=miter|round|bevel] [ml=<miter limit>] [dash=<lengths>] [dp=<dash phase>] [t=<transform>]``

    The path data in ``d`` is the SVG path syntax with commas, never spaces,
    between numbers: ``M``, ``L``, ``H``, ``V``, ``C``, ``S``, ``Q``, ``T``
    and ``Z``, in absolute and (lowercase) relative forms, with a command
    repeated when it is followed by more numbers than it takes. Arcs are not
    supported, send them as curves. A path with neither ``fill`` nor
    ``stroke`` is filled black; a path with only ``stroke`` is not filled.
    ``rule`` is the fill rule, nonzero by default. ``w``, ``cap``, ``join``,
    ``ml`` (``4`` by default) and ``dash`` have their SVG meanings, with the
    stroke geometry computed in the element's own coordinate system, so a
    stroke transformed non-uniformly is stretched like the path. When
    ``vector_dash_limit`` is set, the stroke of one path is cut into that
    many dashes at most: a subpath whose dashes would take the count past
    the limit is stroked solid, as are the subpaths after it that would too.
    ``0``, the default, strokes every dash. A pattern that fine against a
    path that long costs far more than it can show.

``s`` *symbol*
    ``s id= d=<path>``

    Geometry that is not drawn by itself but placed by ``u`` records, as
    many times as needed. A page of text sends each glyph outline once as a
    symbol and then one ``u`` per glyph.

``u`` *use*
    ``u id= [g=] s=<symbol id> [t=<transform>] [fill=] [stroke=] [w=]``

    Draws a symbol with the given style, filled black by default.

``i`` *image*
    ``i id= [g=] img=<image id> [t=<transform>] [o=<opacity>]``

    Draws a raster image transmitted earlier through the graphics protocol,
    by its image id, at its pixel size, so a pixel is one user unit before
    the transform. It is sampled bilinearly. An image that does not exist or
    is itself a vector image is not drawn.

``k`` *pen stroke*
    ``k id= [g=] pts=<x,y,width,...> [fill=<color>] [cap=round|butt]``

    Ink: a ribbon through the points with the width given at each point,
    joined and, by default, capped round. This is the record for live drawing,
    one ``a`` record per pen event appends to it. A terminal can apply these
    records itself as the pen moves, see :doc:`ink-protocol`.

``a`` *append*
    ``a id= d=<path>`` or ``a id= pts=<x,y,width,...>``

    Appends geometry to a path or symbol (``d``) or to a pen stroke
    (``pts``). Appended path data is parsed on its own, so start it with an
    absolute command.

``l`` and ``r`` *gradients*
    ``l id= x1= y1= x2= y2= stops=<offset,color,offset,color,...>``

    ``r id= cx= cy= r= [fx= fy=] stops=<...>``

    A linear gradient along a line, or a radial gradient around a center
    with an optional focus, in the coordinate system of the element that
    uses it. Offsets run from ``0`` to ``1``. A gradient is used as a color
    with ``fill=@<id>`` or ``stroke=@<id>``.

``x`` *delete*
    ``x id=<element>``, ``x g=<group>`` or ``x all``

    Deletes one element, a group with everything inside it, or the whole
    scene. Deleting an element that other elements refer to, a symbol, a
    gradient or a clip, leaves those elements in place but without it.

Colors are ``#rrggbb`` or ``#rrggbbaa`` in sRGB, or ``none``. The floating-point
forms ``srgb:r/g/b[/a]``, ``p3:r/g/b[/a]`` and ``rec2020:r/g/b[/a]`` use
components from 0 to 1, the named primaries and the sRGB transfer curve,
matching :doc:`wide-gamut-protocol`. ``linear:r/g/b[/a]`` uses linear extended
sRGB: RGB may be negative or exceed 1; alpha is 0 to 1 and defaults to 1.
All numbers must be finite. Slashes keep these colors unambiguous in
comma-separated gradient stops.

Every color form accepts ``*<percent>`` to multiply linear RGB by that
percentage, without changing alpha: ``#ff8800*250`` or ``p3:1/0.2/0*300``.
This works in fills, strokes, pen strokes, symbols, text and decorations,
and gradient stops. Gradient references ``@<id>`` work for text paints too.
The compositor and gradient interpolation operate in linear extended sRGB,
preserving HDR and out-of-sRGB channels through nested groups and clips.
Embedded raster images retain their tagged gamut and ``f=64`` HDR values.
The GPU texture is RGBA16F (finite channels saturate at its representable
range, ±65504), with display gamut/headroom conversion at presentation.
``T=1`` on the image selects the :doc:`hdr-protocol` tone mapper. SDR output
uses the terminal's existing HDR-to-SDR presentation path.

Transforms are the six numbers of an affine matrix,
``t=a,b,c,d,e,f``, mapping ``x, y`` to ``a*x + c*y + e, b*x + d*y + f``, the
SVG and PDF convention.

Nested window clips
-------------------

Clips are retained device-space masks after the owning group's transform.
A descendant's translation, rotation, scale or inverse transform cannot
escape an enclosing clip. There is no fixed nesting limit; the optional
``vector_depth_limit`` configuration applies as to other nested groups.
For a window manager such as odin-seat, put each window's content under a
group with its outer clip, then nest content and widget clips::

    g id=1000 t=1,0,0,1,50,30 box=0,0,400,240
    g id=1001 p=1000 box=10,40,380,190
    g id=1002 p=1001 t=0.866,0.5,-0.5,0.866,0,0 box=0,0,200,100
    p id=1003 g=1002 d=M-100,-100L1000,-100L1000,1000L-100,1000Z fill=#336699*200

The blue drawing remains inside all three boxes. Moving the outer group
moves its clip and contents together. A window manager assigns element IDs
and attaches child scenes beneath its window group when merging scenes;
``box`` constrains that subtree, rather than other root-level elements.

Font resources in the vector protocol
--------------------------------------

``a=v,f=200,i=<request id>`` is the vector service. Its payload is UTF-8 JSON,
carried by the graphics protocol's normal base64 encoding, chunking,
compression and direct/file/temporary-file/shared-memory transports. It does
not need ``s`` or ``v`` and creates no image or placement. ``i`` correlates
the reply and occupies no image ID. Continuation chunks may omit the action
and retain the first chunk's parameters. Use ordinary kitty graphics chunks
of at most 4096 base64 bytes. For example::

    <ESC>_Ga=v,f=200,i=77;<base64 of {"op":"fonts"}><ESC>\
    <ESC>_Gi=77;OK;<base64 UTF-8 JSON result><ESC>\

Errors are ``EINVAL:<base64 of {"error":"description"}>``; transport errors
use the ordinary graphics error responses. ``q`` has the ordinary quiet
semantics; use ``q=0`` when a result is needed. Successful result bodies grow
with the data and are not truncated to the ordinary short graphics message.

``{"op":"fonts"}``
    Lists installed faces (family, style, PostScript name, weight, native
    slant in degrees, and monospace status), configured roles, and uploaded
    session font IDs. ``version:1`` identifies this text/resource protocol.
    Family and PostScript names can be used as ``font`` selectors.

``{"op":"inspect","font":"Iosevka"}``
    Reports the selected face's units per em, glyph count, weight, native
    slant, and variable axes, each with tag, minimum, default and maximum.
    Role, weight and italic selection are also accepted.

``{"op":"upload","id":"body","data":"<base64 font bytes>","index":0}``
    Loads a TrueType/OpenType outline font into memory. ``index`` selects a
    collection member and defaults to zero. The nonempty string ID has a
    separate namespace from image and element IDs. Reusing it atomically
    replaces the font after validation. The reply has its ID and inspection
    metadata. Select it as ``"font":"@body"``. Font bytes are never written
    to the image disk cache or registered with the operating system.

``{"op":"release","id":"body"}``
    Releases the session handle. Already-retained text keeps its outlines;
    subsequent layouts using that handle fail until it is uploaded again.
    Uploads are local to the PTY, shared across its main and alternate
    buffers, and freed when that session closes. Image deletion does not
    release fonts. There is no fixed font-count limit; applications should
    release resources when finished. Transport allocation errors are reported.

Text layout and measurement
---------------------------

The text record's decoded JSON is a layout object, for example::

    {"font":"Iosevka","size":24,"x":10,"y":30,"angle":-12,
     "runs":[
       {"text":"Window ","fill":"#dddddd"},
       {"text":"title","weight":700,"italic":true,"slant":0,
        "underline":"double","fill":"p3:1/0.2/0*250"}
     ]}

Use ``text`` for a single string or ``runs`` for an array of objects, each
with a ``text`` string and style overrides. Top-level style fields are the
defaults for each run; runs advance consecutively on the same baseline.
Split runs at intended shaping boundaries: a style change also ends a
ligature or script shaping run. JSON escaping preserves spaces, newlines,
Unicode and control characters without introducing extra scene records.

``font`` / ``role``
    Select an installed family/PostScript name or an ``@`` session handle,
    or use a configured :doc:`font-protocol` role such as ``sans`` or
    ``serif``. Default: the terminal's configured regular font (Iosevka).
    Vector text also accepts proportional outline fonts.

``size`` / ``weight`` / ``italic`` / ``slant``
    Positive em size in scene units (default 16), weight 100–900 (default
    400), italic letterforms (boolean, default false), and optional absolute
    slant in degrees from -15 to 15. Negative slant leans right; zero can
    make italic letterforms upright; positive synthesizes inverse slant.
    Installed static families select the nearest face; variable fonts use
    their weight axis. Uploaded static faces retain their native weight and
    letterforms; upload each desired face or a variable font.

``axes`` / ``features``
    A dictionary of supported four-character OpenType axis tags and design
    values, and an array of HarfBuzz feature strings such as ``["liga=0",
    "ss01=1"]``. Explicit axis values must lie within the font's range.

``direction`` / ``language`` / ``script`` / ``fallback``
    Optional shaping direction (``ltr``, ``rtl``, ``ttb`` or ``btt``), BCP47
    language and ISO15924 script. HarfBuzz guesses unspecified properties.
    An optional array of font selectors supplies fallback faces; the
    configured regional CJK face is also tried for missing clusters.
    Combining marks and joined sequences stay together when choosing a
    fallback. This is a run layout API: applications supply visual run order
    for mixed bidirectional paragraphs, and line breaks rather than automatic
    paragraph wrapping. Bitmap-only and embedded color glyph painting are
    not part of the outline text format.

``fill`` / ``stroke`` / ``stroke_width`` / ``background``
    Vector paints, including gradients and HDR/wide-gamut colors. Fill
    defaults to black, stroke to ``none`` and stroke width to 1; text strokes
    have round joins. Background is optional and fills each run's logical box.

``underline`` / ``underline_color`` / ``strike`` / ``overline``
    Underline style ``none`` (default), ``single``, ``double``, ``curly``,
    ``dotted`` or ``dashed``; its optional paint defaults to the fill.
    Strike and overline are booleans. Decorations are vector geometry and
    accept the same HDR/wide-gamut paints and transforms as the glyphs.

``x`` / ``y`` / ``angle``
    Top-level baseline origin (default 0,0) and rotation in degrees (default
    0). Positive rotation is clockwise in the scene's downward y axis.
    ``\n`` starts another line. The record's affine transform and all parent
    transforms apply afterward. Use group boxes to clip text to a viewport.

Send the same layout object with ``"op":"measure"`` through ``a=v`` to
measure without creating an image or rasterizing pixels. The result gives
``width`` and ``height`` of the unrotated logical layout in user units,
``advance:[x,y]`` before the origin/rotation, ``ascent`` and ``descent``,
and ``ink:[left,top,right,bottom]`` after the layout origin/rotation, or
``null`` for no painted geometry. Ink includes decorations, backgrounds and
stroke reach; curve extrema are measured from the shaped outlines.
``missing`` lists Unicode scalar indices for unresolved glyphs, across the
concatenated runs. Rendering uses the same layout code. Measurement precedes
scene transforms, gradient lookup, opacity and clipping; it reports the full
string even when a window shows only a clipped part.

Python client helpers
----------------------

``kittens.tui.vector`` provides ``service_request``, ``font_upload``,
``measure_text``, ``parse_service_response``, ``text_record``, ``clip_group``
and ``scene``. Transmission helpers yield correctly chunked graphics
sequences for the application's output queue. The response parser accepts
either a framed APC or the ``G``-prefixed body from an input parser and
returns the request ID and decoded result. It raises ``ValueError`` for an
error response. For example, after uploading ``body`` and receiving its
successful reply::

    from kittens.tui.vector import clip_group, measure_text, scene, text_record

    layout = {"font": "@body", "size": 24, "text": "Window title",
              "x": 8, "y": 28, "fill": "p3:1/0.3/0*200"}
    for frame in measure_text(77, layout):
        output_queue.write(frame)
    # Handle request 77's result in the application's normal input loop.
    records = [clip_group(100, [0, 0, 400, 240]),
               clip_group(101, [4, 4, 392, 32], parent=100),
               text_record(102, layout, group=101)]
    for frame in scene(1, 400, 240, records):
        output_queue.write(frame)

Rendering
------------

The terminal rasterizes the scene with antialiasing, with the same
appearance on a gray or color display, whenever it is first shown, the scene
changes, or the size it is shown at changes, with user units scaled so the
intrinsic size fills the placement. Scenes count toward the image storage
quota by the size of their elements, not by pixels.

Silkie rasterizes on the CPU. Drawing all of a scene that covers a large window
at a high pixel density takes tens to hundreds of milliseconds, which is what
the first showing, a change of size and some patches cost. Most patches cost
far less, because kitty works out the rectangle a patch can have changed and
draws only that again, with a result that is the same, pixel for pixel, as
drawing everything:

* Appending to a pen stroke, or to a path that is stroked and has
  ``fill=none``, draws the rectangle around the appended geometry and not
  the one around the stroke. This is what live ink costs.
* Adding, replacing or deleting a path, text, a use or a pen stroke draws the
  rectangle around where the element was and where it now is. So does
  appending to a filled path, since a fill is closed across the gap between
  the ends of a subpath and more of it changes than was appended.
* A ``g`` record, the root group's included, a symbol, a gradient or an image
  record, a deletion of any of those, ``x g=``, ``x all`` and any change to a
  path that a group is clipped by draw everything, as other elements depend on
  them. Panning or zooming through the transform of the root group is in this
  class.

A program that animates should therefore keep what changes small and in
elements of its own. Kitty keeps no rasterized copy of a scene other than the
texture it is drawn from, so nothing but the elements counts toward the quota.

Rendering a PDF
------------------

The records map onto the PDF content stream operators closely enough that a
viewer can render pages without rasterizing anything itself:

* ``q`` and ``Q`` are a ``g`` record and the end of it; ``cm`` is its ``t``.
* ``W n`` is a ``p`` record with the clip path and ``clip=`` on a group that
  encloses what follows.
* The path construction and painting operators become ``p`` records, with
  ``rule=eo`` for the even-odd variants, and the graphics state parameters
  for line width, cap, join, miter limit and dash become ``w``, ``cap``,
  ``join``, ``ml`` and ``dash``.
* Text: each glyph used is sent once as an ``s`` record holding its outline
  from the embedded font, and every ``Tj`` becomes ``u`` records with the
  text matrix as ``t``; text render modes that stroke use ``stroke``.
* ``Do`` with an image XObject transmits the image through the graphics
  protocol once and draws it with an ``i`` record; the image space of PDF,
  the unit square, is the transform ``t=w,0,0,-h,0,h`` for an image of
  ``w`` by ``h`` pixels followed by the current matrix.
* ``ca`` and ``CA`` are ``o`` on a group, ``BM`` is ``blend`` for the normal,
  multiply and screen modes, and axial and radial shadings are ``l`` and
  ``r`` gradients.

Soft masks, tiling and shading patterns as fills, knockout and isolated
groups, and the other blend modes are not supported in this version of the
format; a viewer rasterizes those parts itself and sends them as raster
images.

Relays and multiplexers
---------------------------

A relay that resynthesizes the screen for a client, in the manner of mosh,
keeps a vector image as a table of records keyed by id. Records with a new id
are added, records with an existing id replace it, ``x`` records remove ids
(a group's ``x g=`` removing everything whose parent chain leads to it), ``a``
records are concatenated to the geometry of their target, and a transmission
with ``a=t`` or ``a=T`` empties the table before adding. The table, written
out as records, is the whole state of the image and can be sent to a fresh
terminal after a reconnection, as one ``a=t`` transmission. The patches
themselves need not be kept. Session font uploads must also be replayed before
text records that reference them. A relay that does not understand the records can
still forward them as opaque image data, keeping the transmissions and the
patches in order.

Querying support
-------------------

To find out whether a terminal supports vector images, send a query with
``f=200`` and a small scene::

    <ESC>_Ga=q,i=31,f=200,s=1,v=1;cCBpZD0xIGQ9TTAsMEwxLDBMMSwxWg==<ESC>\

A terminal that does answers ``OK``, one that does not answers ``EINVAL`` or
nothing at all, in which case the usual timeout applies.

That query establishes base scene support. For shaped text, font resources,
measurement, box clips and HDR paints, query ``a=v,f=200`` with
``{"op":"fonts"}`` and inspect ``version`` and ``features``. An older vector
implementation can accept base scenes while lacking these additions.
