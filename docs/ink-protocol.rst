Terminal-side ink
====================

.. versionadded:: 0.50.0

A program that draws with a pen receives a report from the
:doc:`tablet-protocol`, appends a point to a pen stroke of a
:doc:`vector image <vector-protocol>` and waits for the terminal to draw it.
Every sample of the pen therefore travels from the terminal to the program
and back before any ink shows, and when the program is at the far end of a
network connection the network is crossed twice. The line lags behind the
pen, which is the one thing a drawing program cannot afford.

Terminal-side ink lets the terminal draw the stroke itself. The program gives
it a brush for one of its vector images, and from then on the terminal adds
the points of each stroke to that image as the pen moves, at once, while the
reports go to the program as they always did. The terminal draws no overlay
of its own: it applies to the image exactly the records the program would
have sent, so the stroke is an ordinary element of the program's scene, with
an id the program chose, which the program keeps, replaces with a better one
or deletes. Nothing else about vector images changes. It is an open terminal
extension like the others here: small, unobtrusive, easy to implement, and no
change to what the TTY is.

Framing
-------------

Frames are ``OSC 777`` with the ``ink`` selector::

    <OSC> 777 ; ink ; v=1 : op=query : id=42 <ST>

They follow the rules every OSC 777 extension shares, which
:doc:`extension-framing` gives: the syntax of the fields, which frames are
rejected, and the units.
Values are decimal integers, except the widths and the exponent of a brush,
which are numbers as the records of a vector image write them and may have a
fraction, and its color. ``v``, the protocol version, is ``1``, and ``op`` is
one of ``query``, ``cap``, ``brush`` and ``mode``. The terminal sends only
replies, which are never merged or dropped.

Discovery
-------------

A program asks the terminal whether it can ink::

    <OSC> 777 ; ink ; v=1 : op=query : id=42 <ST>

``id`` is a non-negative 31 bit integer of the program's choosing, echoed in
the reply so replies can be matched to queries. A supporting terminal
answers::

    <OSC> 777 ; ink ; v=1 : op=cap : id=42 : caps=1 : mode=0 <ST>

``caps`` is a bit set, of which only ``1`` is defined: the terminal draws
strokes. ``mode`` is ``1`` while ink is on and ``0`` otherwise. When the
terminal holds a brush the reply also carries it, with the fields of
`The brush`_ as the terminal has them, ``k`` being the id the next stroke
will get and ``n`` how many ids are left::

    <OSC> 777 ; ink ; v=1 : op=cap : id=42 : caps=1 : mode=1 : i=7 : g=0 : k=1003 : n=47 : fill=#1a1a80 : w0=1 : w1=5 : e=1 : tools=58 <ST>

A terminal that does not support the protocol ignores the query, so a
program must time out and go on drawing its strokes itself, which it must be
able to do in any case. A query changes nothing.

The brush
-------------

A program turns ink on by giving the terminal a brush::

    <OSC> 777 ; ink ; v=1 : op=brush : i=7 : k=1000 : n=50 : fill=#1a1a80 : w0=1 : w1=5 <ST>

============ ====================================================================
``i``        Required. The id of a vector image (``f=200``) of this window, as
             the graphics protocol knows it. The image need not exist yet; a
             stroke begins only over an image that does
``g``        The id of the group of that image the strokes go into, ``0``, the
             root group, by default
``k``        Required. The id the element of the first stroke gets, at least
             ``1``
``n``        How many ids, counting up from ``k``, the program sets aside for
             the terminal, ``1`` by default. The last of them may not exceed
             4294967295. The program must not use these ids for anything else
``fill``     The color of the strokes, as vector records write a color,
             ``#rrggbb``, ``#rrggbbaa`` or ``@`` and the id of a gradient.
             Black by default; ``none`` is refused
``w1``       Required. The width of the stroke under full pressure, in the
             units of the group, a finite number from 0 up
``w0``       The width under no pressure, the same as ``w1`` by default, which
             gives a stroke of constant width
``e``        The exponent of the curve between the two, a finite number
             greater than 0, ``1`` by default
``tools``    The tools that draw, a bit set with bit ``1 << tool`` for each
             ``tool`` value of the tablet pointer protocol: pen ``2``, eraser
             ``4``, brush ``8``, pencil ``16``, airbrush ``32``, mouse ``64``,
             lens ``128``, totem ``256`` and an unknown tool ``1``. By default
             ``58``, the pen, brush, pencil and airbrush. From 1 to 511
============ ====================================================================

A brush replaces the one before it, ids included, and turns ink on. The
eraser is left out by default because what erasing means is the program's
business; a program that wants the other end of the pen to paint, with the
color of its page for instance, gives a brush that says so.

Mode
--------

Ink is turned off with::

    <OSC> 777 ; ink ; v=1 : op=mode : mode=0 <ST>

The terminal keeps the brush, and ``mode=1`` turns ink on again with it and
the ids it has left, so a program can stop the terminal drawing while its own
eraser or selection tool is in use and start it again without sending the
brush. ``mode=1`` does nothing when there is no brush. A ``mode`` request
without a ``mode`` field reads the state and is answered with::

    <OSC> 777 ; ink ; v=1 : op=mode : mode=1 <ST>

The mode and the brush are local to the window. ``RIS`` turns ink off and
forgets the brush. Programs must turn ink off when they exit, suspend or
fail, and give the brush again when resumed; a program resuming after a
reconnection queries to learn what survived rather than assuming. A terminal
may let its user refuse ink, for every program or for some. A refused mode
reads back as off and nothing is drawn, so a program reads the mode back
rather than assume it was granted.

Ink needs the reports of tools: the window must have bit ``1`` of the mode of
the :doc:`tablet-protocol` on. Without it a pen is a mouse, there are no
strokes and nothing is drawn. A program that never sends a brush sees no
difference at all.

Strokes
-----------

While ink is on, a stroke of the tablet pointer protocol, from the ``down``
of a tool to its ``up``, becomes a stroke of ink when

* the tool is one of those the brush names,
* the ``down`` is over a placement of the image, and, when several
  placements of it are under the pen, over the one drawn on top of the
  others: the one with the highest ``z``, and among equals the one placed
  last,
* the group exists, is a group, and the transforms from the root group down
  to it can be inverted,
* an id is left, and the terminal has room for another stroke; a terminal
  may limit how many tools it draws for at once.

Then the terminal applies to the image, exactly as if the program had sent it
with the edit action of the vector format, the record::

    k id=<the next id> g=<the group> pts=X,Y,W fill=<the fill>

with the point the ``down`` report gives, see `From a report to a point`_,
and uses up that id. The report of the ``down`` that the program receives
carries one more field, ``ink``, with the id of the element::

    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=1 : phase=1 : device=3 : t=408211 : contact=0 : x=25600 : y=51200 : buttons=1 : mods=0 : tool=1 : valid=1 : p=13107 : ink=1000 <ST>

Every later report of that stroke, each ``move`` and the ``up``, appends the
point it gives::

    a id=<that id> pts=X,Y,W

The ``up`` ends the stroke. A ``down`` without an ``ink`` field began a
stroke the terminal is not drawing, whatever the reason, and the program
draws it itself as it would without this protocol. That is also how a
program learns that the ids have run out: it gives a brush with more.

The element is the program's from the moment it exists. Usually the program
leaves it as it is. It may replace it, with a ``k`` record of the same id,
by a stroke it has smoothed or thinned, or delete it with an ``x`` record,
and should do either after the ``up``: a record that replaces the element
while the stroke is being drawn is followed by the points the terminal goes
on appending to whatever has that id.

A stroke that the tablet pointer protocol cancels, with ``phase=4``, never
happened: the terminal deletes its element, as the record ``x id=<that id>``
does, before it sends the report of the cancel. The id is not given again.

Reports of hovering draw nothing, and fingers are not inked.

From a report to a point
----------------------------

The point a report adds is worked out from the report alone and from what the
terminal is showing when it makes the report, so that a program, or a relay
that mirrors the scene, arrives at the same numbers. They are real numbers,
computed in double precision; a difference in the last digit is of no
account.

Let ``x``, ``y`` be the position in the report, in 1/256 of a physical pixel
from the top left of the cell area. Let the placement the stroke began in be
shown in the rectangle with its top left corner ``L``, ``T`` physical pixels
from the top left of the cell area, ``W`` wide and ``H`` high, and let it show
the part of the image with its top left corner at ``sx``, ``sy`` and the size
``sw`` by ``sh``, in the user units of the image, which is the whole image
unless the placement was made with a source rectangle. The position in the user units of
the image is::

    X = sx + (x / 256 - L) * sw / W
    Y = sy + (y / 256 - T) * sh / H

Let ``M`` be the product of the transforms of the groups from the root group
down to the group of the stroke, each the matrix the ``t`` of its ``g``
record gives and the identity where there is none, the root first. The point
is given in the coordinate system of that group::

    (X', Y') = inverse(M) applied to (X, Y)

The width follows the pressure. With ``p`` the pressure of the report::

    f = p / 65535      when the report has a pressure, bit 1 of valid
    f = 1              when it has none
    width = w0 + (w1 - w0) * f ^ e

A tool that does not measure pressure thus draws at the width of full
pressure. The report of an ``up`` has a pressure of ``0`` when the tool
measures pressure, so a stroke ends at the width ``w0``.

Where a placement is shown is the graphics protocol's business. For a
placement at column ``c0`` and row ``r0`` of the screen, counted from zero
from the top left of what is shown now, with the offsets ``X``, ``Y`` into
that cell and a cell of ``cw`` by ``ch`` pixels, kitty has::

    L = c0 * cw + X
    T = r0 * ch + Y
    W, H = c * cw - X, r * ch - Y        with both c and r
    W, H = H * sw / sh, r * ch - Y       with r alone
    W, H = c * cw - X, W * sh / sw       with c alone
    W, H = sw, sh                        with neither

What happens during a stroke
--------------------------------

The stroke stays with the placement it began in. The tablet pointer protocol
keeps a stroke captured by its window when the pen leaves it, and the ink
follows in the same way: positions outside the placement give points outside
the image, by the same formulas, and whatever of the stroke falls outside is
not seen.

Each point is worked out with what holds when its report is made:

* When the placement moves, because the screen scrolls or the user scrolls
  back, ``L`` and ``T`` are those of where it is shown now, so the ink stays
  under the pen.
* When the transform of the group or of a group above it changes, later
  points are taken through the new transforms; the points already in the
  element move with the group, as they do for any element.
* A report whose position cannot be taken into the group, because the group
  has gone or a transform can no longer be inverted, adds no point. The
  stroke goes on with the next report.
* When the image or the placement goes away, deleted, scrolled out of the
  scrollback or replaced by a new transmission of the image, the stroke of
  ink is over. Nothing is deleted on its account, and the reports of the
  stroke go on to the program.
* When the window switches between the main and the alternate screen, the
  strokes on the images of the screen that is left are over in the same way.

A brush given during a stroke applies from the next stroke; the one being
drawn keeps the brush it began with. Turning ink off during a stroke leaves
what has been drawn and adds no more to it.

An example
--------------

A program has a vector image ``7`` of 400 by 300 units placed at its own
size with its corner 30 pixels across and 40 pixels down the cell area. It
finds out that the terminal inks and gives it a brush, a dark blue line
between one and five units wide, with fifty ids from 1000::

    <OSC> 777 ; ink ; v=1 : op=query : id=1 <ST>
    <OSC> 777 ; ink ; v=1 : op=cap : id=1 : caps=1 : mode=0 <ST>
    <OSC> 777 ; tablet-pointer ; v=1 : op=mode : mode=1 <ST>
    <OSC> 777 ; ink ; v=1 : op=brush : i=7 : k=1000 : n=50 : fill=#1a1a80 : w0=1 : w1=5 <ST>

The pen touches down 100 pixels across and 200 down at a fifth of full
pressure, moves five pixels right and two down while pressing harder, and
lifts a pixel further on. The program receives::

    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=1 : phase=1 : device=3 : t=408211 : contact=0 : x=25600 : y=51200 : buttons=1 : mods=0 : tool=1 : valid=1 : p=13107 : ink=1000 <ST>
    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=1 : phase=2 : device=3 : t=408215 : contact=0 : x=26880 : y=51712 : buttons=1 : mods=0 : tool=1 : valid=1 : p=39321 <ST>
    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=1 : phase=3 : device=3 : t=408219 : contact=0 : x=27136 : y=51712 : buttons=0 : mods=0 : tool=1 : valid=1 : p=0 <ST>

and by the time it does the terminal has applied to image ``7``, and drawn::

    k id=1000 pts=70,160,1.8 fill=#1a1a80
    a id=1000 pts=75,162,3.4
    a id=1000 pts=76,162,1

since ``70 = (25600 / 256 - 30) * 400 / 400`` and ``1.8 = 1 + (5 - 1) *
13107 / 65535``. The program adds the same three points to its own copy of
the scene under the id 1000. If it wants a smoother line it now sends, with
the edit action, a ``k`` record with that id and the points it prefers; if it
decides the stroke was a tap that selects something instead, it sends ``x
id=1000``. Otherwise it sends nothing: the stroke is already there.

Relays and multiplexers
---------------------------

A relay or a multiplexer that passes the bytes of both directions through
needs to know nothing of this protocol, and the ink is drawn by the terminal
at the end, which is where the pen is: this is the case the protocol exists
for.

One that keeps the scene of a vector image as a table of records, so as to
send it to a terminal that connects later, has to keep that table true. It
either answers the query itself with ``caps=0`` and passes no brush on, or it
adds to its table the records this page says the terminal applies, working
them out from the reports it forwards, the brush it passed on and the
placement as its client shows it. One that translates the positions of
reports for a pane does so before it works them out, and one that moves or
resizes a pane cancels the strokes in it, as the tablet pointer protocol
requires, which removes their ink.

A multiplexer keeps the mode and the brush of each pane with the rest of its
state and turns ink off when detaching.

Implementations
-------------------

* silkie, this fork of kitty, wherever it reports tools.

Implementation notes for silkie
----------------------------------

A report is turned into its record and applied before the report is written
to the program, and the image is drawn again with the next frame through the
partial redraw of :doc:`vector images <vector-protocol>`, which draws only
the rectangle around the new piece of the stroke. Silkie grows its active
stroke storage as tools begin drawing; there is no fixed limit on simultaneous
tools. The points of a stroke count toward the image storage quota
like those a program sends.
