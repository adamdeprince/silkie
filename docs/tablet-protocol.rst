Tablet pointer protocol
==========================

.. versionadded:: 0.50.0

Pens and fingers carry far more than the mouse protocol can express:
pressure, tilt, rotation of the barrel, which end of the pen is in use,
hovering, several fingers at once, the shape of each finger on the glass.
A tablet has keys, rings and dials of its own beside the pen. The tablet
pointer protocol is an open terminal extension that reports all of it, from
graphics tablets, pen displays, touchscreens and touchpads, with room for
whatever a device measures. It belongs to no terminal: silkie (this
kitty fork) implements it and this page is its specification. The goals are
those of the other extensions here: small, unobtrusive, easy to implement,
and no change to what the TTY is.

Framing
-------------

Frames are ``OSC 777`` with the ``tablet-pointer`` selector::

    <OSC> 777 ; tablet-pointer ; v=1 : op=query : id=42 <ST>

They are framed as :doc:`extension-framing` says, which is also where the
units, the clock of ``t``, the reports that may be merged and the refusal of
a mode are described for every extension. ``v``, the protocol version, is
``1``, and ``op`` is one of ``query``, ``cap``, ``mode``, ``device``,
``label`` and ``event``.

The protocol grows without a new version. Beyond what every extension
ignores, a program ignores a report whose ``kind`` it does not know and any
bit of ``valid`` it does not know, and treats a ``tool`` value it does not
know as ``0``. A new kind of report is sent only to a program that asked for
it with a mode bit of its own, so a program is never sent what it was not
written for.

Discovery
-------------

A program asks the terminal whether it supports the protocol::

    <OSC> 777 ; tablet-pointer ; v=1 : op=query : id=42 <ST>

``id`` is a non-negative 31 bit integer of the program's choosing, echoed in
the reply so replies can be matched to queries. A supporting terminal
answers::

    <OSC> 777 ; tablet-pointer ; v=1 : op=cap : id=42 : caps=101 : mode=0 : w=1200 : h=800 : cw=12 : ch=24 <ST>

``caps`` is a bit set of what the terminal can report:

======= ===================================================================
``1``   Tools
``2``   Fingers on a touchscreen
``4``   Fingers on a touchpad
``8``   The shape of a finger contact
``16``  Pads
``32``  Tool actions
``64``  The hardware identifiers of tools and pads
``128`` Estimated and predicted samples
``256`` Fingers the system judges accidental
======= ===================================================================

It describes the terminal on the system it runs on, not the hardware
attached right now. A bit is set only for what the terminal knows it can
report. Where the system does not say beforehand whether it gives the shape
of a contact, bit ``8`` stays clear until a contact has had one, so a
program does not wait for the bit: a report has a shape when its ``valid``
says so. What goes beyond tools and fingers, the bits from ``16`` up, is sent
only when asked for with the mode. ``mode`` is the current state of the
mode described below. ``w,h`` are the size of the cell area in pixels (1 to
65535) and ``cw,ch`` the cell size (1 to 4096). Pixels, here and in the
reports, are physical pixels: those of the framebuffer the terminal draws
into, not the scaled units of the window system, so a position is as fine as
the display is. A program queries again after a resize. A terminal that does not support the protocol ignores the
query, so a program must time out and fall back to mouse reporting. A query
does not change the mode.

Mode
--------

Reports are off by default and turned on per terminal window with::

    <OSC> 777 ; tablet-pointer ; v=1 : op=mode : mode=3 <ST>

``mode`` is a bit set of what the program wants, and ``0`` turns everything
off:

======= ===================================================================
``1``   Reports of tools
``2``   Reports of fingers
``4``   Reports of pads, see `Pads`_
``8``   The hardware identifiers of tools and pads, see `Devices`_
``16``  Reports of tool actions, see `Tool actions`_
``32``  Estimated and predicted samples, see `Estimates and predictions`_
``64``  Fingers the system judges accidental, see `Fingers`_
======= ===================================================================

While bit ``1`` or ``2`` is on, that kind of input over the window is
reported with this protocol instead of as mouse events, so with ``mode=1`` a
pen draws while a finger still taps and scrolls as a mouse, and with
``mode=3`` a tap is not a click and a drag does not select. A physical
mouse, and the mouse and scroll events a touchpad produces by itself,
continue to use whatever mouse reporting the program asked for. Programs
that merely want pen input to act as a mouse need not use the protocol at
all. Bit ``4`` takes nothing away: what the system itself does with the
controls of a pad, such as a shortcut the user has put on a key, goes on.

The mode is local to the window, is reset by ``RIS``, and changing it
cancels every active contact and lets go of every held key of a pad.
Programs must turn it off when they exit, suspend or fail, and turn it back
on when resumed. Multiplexers keep it with the rest of the terminal state,
restore it on reconnect, and turn it off when detaching. A terminal may let
its user refuse a mode, or some of its bits, for every program or for some.
A refused bit reads back as off and nothing it would have turned on is
reported. kitty refuses bit ``8``, the hardware identifiers, unless its user
allows them with :opt:`tablet_hardware_ids`, since a serial number says
whose pen it is and the program asking may be on another machine. A refusal
shows in the mode, not in ``caps``, which says what the terminal can report
on the system it runs on.

A ``mode`` request without a ``mode`` field reads the state instead of
changing it, and is answered with::

    <OSC> 777 ; tablet-pointer ; v=1 : op=mode : mode=3 <ST>

A terminal keeps only the bits it supports and that its user has not
refused, so a program that asked for more than it was given finds out by
reading the mode back. A program
resuming after a reconnection reads the mode to learn whether it survived,
rather than assuming it did.

Devices
-----------

Before the first report of a tool, each time the tool comes into proximity
over a window that has tool reports on, the terminal describes it::

    <OSC> 777 ; tablet-pointer ; v=1 : op=device : device=3 : kind=1 : tool=1 : axes=7 : name=V2Fjb20gSW50dW9zIFBybyBN <ST>

``device`` is the id the reports of that tool carry, ``kind`` is ``1`` and
``tool`` is the type of the tool, with the values it has in reports. The
other fields are present when the system provides them:

================ ==================================================================
``axes``         The measurements the reports of this tool can carry, a bit set
                 with the bits of ``valid``, so a program can set up a brush
                 before the first stroke
``name``         The name of the tablet the tool is on, or of the tool itself,
                 at most 127 bytes of UTF-8 encoded as base64 with padding
``serial``       The serial number of the tool, an unsigned 64 bit integer
``hwid``         The hardware id of the tool, an unsigned 64 bit integer; on
                 Wacom tablets it tells the model of the pen
``vid``, ``pid`` The vendor and product ids of the tablet, unsigned 32 bit
                 integers
``bat``          The charge of the battery of the tool, ``0`` to ``100``
``tablet``       Which of the tablets that are attached the tool is on, a number
                 from ``1``, see `Tablets`_
``w``, ``h``     The size of the active surface of that tablet in 1/256
                 millimeter
``direct``       ``1`` when that surface is a display, ``0`` when it is not
``pcolor``,      The color, line width and line style that are stored in the tool
``pwidth``,      itself, see `Settings stored in a pen`_
``pstyle``
================ ==================================================================

``serial``, ``hwid``, ``vid`` and ``pid`` are the hardware identifiers, and
they are sent only while mode bit ``8`` is on. A serial number names one
physical pen, and so its owner, to every program that turns reports on,
including one on a remote host at the far end of an ssh session. A program
that keeps settings per pen asks for them; a program that only draws has no
need to know whose pen it is.

The description is sent again when the tool comes back after leaving
proximity, when it moves over another window, and when the program changes
the mode while the tool is in proximity. It is also sent again whenever
something it says is no longer so while the tool is in use: the battery
runs down, the user picks another color on the pen, the system learns more
of the tool than it knew at first. A description replaces the one before it
as a whole, so a field it leaves out is no longer known, and it changes
nothing about a stroke that is open. A program must not depend on
getting one: a terminal that knows nothing about a tool beyond its reports
need not describe it. A pad is described in the same way, with ``kind=3``
and fields of its own, see `Pads`_. The device of a finger report is not
described.

Tablets
~~~~~~~~~~

More than one tablet can be attached at once, and a pen can be carried from
one to the other. ``tablet`` says which of them a tool is on: a number from
``1`` that the terminal gives each tablet, that stays with the tablet for as
long as it is attached, and that every tool on the tablet and every pad that
belongs to it carries, so a program can tell that these keys and that pen
are parts of one device. It is absent, or ``0``, when the system does not
say which tablet a tool is on. It is a count and not a hardware identifier:
it says nothing of the tablet that outlasts its being plugged in, so it
needs no mode bit.

``w`` and ``h`` are the width and height of the active surface of the
tablet, the area in which it tracks a tool, and are present together or not
at all. ``direct`` says whether that surface is a display. On a display the
tool points at what it draws and ``x,y`` are all a program needs. On a
tablet that is not one, the hand moves in one place and the mark appears in
another, where the system has put the pointer after a mapping of its own,
and a program that wants to map the tablet to its canvas itself, or to know
how far the hand really moved, needs the place on the tablet. That is
``rx`` and ``ry`` in the reports of the tool, with ``valid`` bit ``256``:
``0`` to ``65535`` across and down the active surface from its top left
corner. With ``w`` and ``h`` they are a position in millimeters. ``x,y``
are reported all the same, and are where the system put the pointer.

The pointer input of Windows has all of these: every pointer names the
device it comes from, and the device says whether it is a pen built into a
display or one on a tablet of its own, the rectangle of its surface in
hundredths of a millimeter, and where on it the pen is. So has the kernel of
Linux, whose devices give the range and the resolution of their position and
say whether they are a display. What a terminal gets depends on what stands
between it and those. The ``tablet-v2`` protocol of Wayland says which
tablet a tool and a pad are on and no more, but it names the device node of
the tablet, by which a terminal on the same machine can ask the kernel for
the size and for whether the tablet is a display. X11 has the position and
its range, with a resolution where the driver states one, and no tablet that
tools belong to. AppKit numbers the tablets, and gives a position in units
of the tablet whose range it does not tell.

Settings stored in a pen
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Some pens keep a color, a line width and a line style in the pen itself,
set with a switch on it or written to it by a program, so that a pen picked
up at another device draws there as it did at the last. They are the
*Preferred Color*, *Preferred Line Width* and *Preferred Line Style* of the
HID usage tables for digitizers, which the pens of the Universal Stylus
Initiative implement. A terminal that has them puts them into the
description of the tool:

================ ==================================================================
``pcolor``       The color, six hexadecimal digits ``RRGGBB`` in sRGB
``pwidth``       The width of the line in 1/256 millimeter; ``0`` is the thinnest
                 line there is, one physical pixel at any magnification
``pstyle``       ``1`` ink, as of a ballpoint, a gel or a fountain pen, ``2``
                 pencil, ``3`` highlighter with a flat tip, ``4`` marker with a
                 chisel tip, ``5`` round brush
================ ==================================================================

HID gives the color in one of two forms. A value of 24 bits is red, green
and blue of eight bits each, in that order, and is passed on as it is; the
usage tables do not say what color space it is in, and it is taken for
sRGB, which is the space of the other form. A value of 8 bits is an index
into the named colors of CSS in alphabetical order, with *gray* spelled one
way only: ``0`` AliceBlue to ``140`` YellowGreen, as Appendix A of the HID
Usage Tables 1.7 lists them. The terminal sends the color the name stands
for, so index ``7``, Black, is ``pcolor=000000`` and index ``9``, Blue, is
``pcolor=0000ff``. Index ``255``, an index the appendix reserves, and the
*No Preferred Color* flag beside a value of 24 bits each mean that the pen
has no color, and ``pcolor`` is left out.

The width comes in the units the pen declares for it. The usage tables
recommend steps of a tenth of a millimeter and leave the pen free to
declare others, so a terminal converts from what the report descriptor
gives as the physical range and unit of the value, and leaves ``pwidth``
out when the descriptor gives none. The line styles *Ink*, *Pencil*,
*Highlighter*, *Chisel Marker* and *Brush* are ``1`` to ``5``; *No
Preference* is no style, and ``pstyle`` is left out.

These are what the pen would like and not orders, and a program is free to
draw otherwise. HID also says of each whether the pen lets it be changed,
which matters only to something that writes to the pen and is not passed
on. A pen whose setting changes while it is in use is described again.
Only the reports of the pen itself carry these settings, so a terminal has
them where it reads the digitizer directly; the tablet interfaces of
Windows, macOS, Wayland and X11 do not pass them on.

Reports
-----------

A report is a single frame per event::

    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=1 : phase=1 : device=3 : t=408211 : contact=0 : x=25600 : y=51200 : buttons=1 : mods=0 : tool=1 : valid=3 : p=32000 : tx=-1200 : ty=600 <ST>

In the reports of tools and fingers ``kind``, ``phase``, ``device``, ``t``,
``contact``, ``x`` and ``y`` are required. The other fields in this table
default to zero, except ``tool``, which defaults to ``1``:

================ ==================================================================
``kind``         ``1`` tool, ``2`` finger. ``3`` is a pad and ``4`` a tool
                 action, which have fields of their own, see `Pads`_ and
                 `Tool actions`_
``phase``        ``0`` hover, ``1`` down, ``2`` move, ``3`` up, ``4`` cancel,
                 ``5`` the tool left proximity, ``6`` frame, see `Fingers`_,
                 ``7`` correction, see `Estimates and predictions`_
``device``       Identifies the tool or touch device for as long as it is in
                 proximity, a non-negative 31 bit integer, ``0`` when the
                 system does not tell devices apart
``t``            The time of the event in milliseconds on the clock the
                 terminal uses for every extension, see
                 :doc:`extension-framing`: when the system says it happened,
                 or when the terminal learnt of it where the system does not
                 say
``tu``           Microseconds past ``t``, ``0`` to ``999``, present only when
                 the system stamps events more finely than a millisecond. A
                 report without it is on a grid of whole milliseconds
``contact``      Identifies one contact of a device, stable for its lifetime.
                 A pen is normally contact ``0``, fingers get the system's
                 touch ids, which it may reuse once a finger has lifted
``x``, ``y``     Position in 1/256 of a physical pixel, signed, zero based,
                 relative to the top left of the cell area, within
                 ±1073741823; for a finger on a touchpad (``src=1``) the
                 position on the pad instead, ``0`` to ``65535`` on each axis
                 with the origin at its top left
``buttons``      Held buttons: tip or left ``1``, barrel or right ``2``, second
                 barrel or middle ``4``, back ``8``, forward ``16``, third
                 barrel ``32``. A finger has ``1`` while it is down
``mods``         Shift ``1``, Alt ``2``, Control ``4``, Super ``8``
``tool``         ``0`` finger or unknown, ``1`` pen, ``2`` eraser, ``3`` brush,
                 ``4`` pencil, ``5`` airbrush, ``6`` mouse, ``7`` lens, ``8``
                 totem, see `Totems`_
``valid``        The measurements the report carries, a bit set, see below
``src``          For a finger, ``1`` when it is on a touchpad rather than on
                 the screen; absent otherwise
``stage``        For a finger on a touchpad that measures force, how hard the
                 pad is pressed, see `Fingers`_
``conf``         For a finger, ``1`` when the system takes the contact to be
                 meant and ``0`` when it takes it for an accident, such as a
                 palm; absent when the system has not said which. A contact
                 with ``0`` is reported only while mode bit ``64`` is on, see
                 `Fingers`_
``w``, ``h``     For a finger on a touchpad, the size of the pad in 1/256
                 millimeter when the system knows it, so a program can turn
                 positions into distances or map the pad onto its canvas with
                 the right aspect ratio
================ ==================================================================

A measurement is present exactly when its bit is set in ``valid``. A clear
bit means the device does not measure it, never that the value is zero, and a
terminal must not make a measurement up. The measurements, with their
``valid`` bit:

======================= ===== ====================================================
``p``                   1     Pressure, 0 to 65535
``tx``, ``ty``          2     Tilt in hundredths of a degree, −9000 to 9000,
                              positive towards the right and bottom of the screen
``twist``               4     Rotation of the tool around its axis in hundredths
                              of a degree, 0 to 35999 clockwise
``tangent``             8     Tangential pressure, the airbrush wheel, −65535 to
                              65535
``z``                   16    Height in device units, −1000000 to 1000000
``dist``                32    Distance from the tablet, 0 to 65535
``wheel``, ``wclicks``  64    Rotation of the wheel on the tool since the last
                              report, in hundredths of a degree and in clicks. A
                              mouse or a lens has one, and so may any other tool
``major``, ``minor``,   128   The shape of a finger or a totem on the screen: the
``angle``                     axes of the ellipse of the contact in 1/256 of a
                              physical pixel and its orientation in hundredths of
                              a degree, clockwise from the vertical
``rx``, ``ry``          256   Where the tool is on the active surface of its
                              tablet, 0 to 65535 from the top left corner, see
                              `Tablets`_
``grip``                512   How hard the barrel of the tool is held, 0 to 65535
======================= ===== ====================================================

A wheel is reported for whatever tool has one, not only for the two that
always do: ``tablet-v2`` and libinput make it a capability of the tool,
whatever its type. AppKit and XInput have no wheel in what they report of a
tool.

``grip`` is the force of the hand on the barrel of a pen that measures it,
the *Barrel Pressure* of the HID usage tables. It is a measurement like
``p``, comes with every report of the tool, and can thicken a line or open a
nozzle as steadily as the hand tightens. The squeeze of `Tool actions`_ is
something else: what a system makes of that force when it crosses a
threshold, which happens once and has no size. A pen can give either, both
or neither. As with the settings stored in a pen, only the reports of the
pen itself carry ``grip``, and the tablet interfaces of Windows, macOS,
Wayland and X11 do not pass it on.

Fingers
~~~~~~~~~~

Finger reports have ``kind=2``, ``tool=0``, ``buttons=1`` while down, one
``contact`` per finger, and usually no measurements beyond the shape. A
touch system delivers all the fingers that moved together before any of them
is acted on, and the terminal keeps that grouping: after the reports of one
such group it sends a frame report, which carries only ``kind=2``,
``phase=6``, ``device`` and ``t``, with ``tu`` when the reports have it::

    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=2 : phase=1 : device=0 : t=408211 : contact=0 : x=25600 : y=51200 : buttons=1 : mods=0 : tool=0 : valid=0 <ST>
    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=2 : phase=1 : device=0 : t=408211 : contact=1 : x=38400 : y=51200 : buttons=1 : mods=0 : tool=0 : valid=0 <ST>
    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=2 : phase=6 : device=0 : t=408211 <ST>

A program that cares about the relation between fingers, as a pinch
recognizer does, collects reports until the frame and applies them at once.
A program that paints one stroke per finger can act on each report as it
comes. Frames are sent for fingers only; a tool reports one contact at a
time.

A touchpad that measures how hard it is pressed reports that with ``p`` and
``stage``. The force is on the pad as a whole, not under each finger, so
every finger on the pad carries the same two values, and when only the force
changes the fingers are reported again as a ``move`` that did not move.
``stage`` is ``0`` while the pad is pressed less than a click, ``1`` for a
click and ``2`` for a click pressed harder, and ``p`` runs from 0 to 65535
within each stage, so it drops back when the stage goes up.

Not everything that touches a screen or a touchpad is meant to. A palm
resting on the glass, the side of the hand that holds a pen, or a thumb lying
on the edge of a touchpad is a contact too, and some systems judge which a
contact is from its size, its shape and where the pen is. A finger report
says what the system has judged with ``conf``: ``1`` for a contact it takes
to be meant and ``0`` for one it takes to be an accident. ``conf`` is absent
when the system has not said, which says nothing either way. Some systems do
not judge at all. Others only mark the contacts they take for accidents and
say nothing of the rest, and a contact such a system has not marked has no
``conf``: it was not found to be meant, and a palm the system missed looks
the same. Only a system that says of a contact that it is meant gives it
``conf=1``. The judgement can change while the contact is down, as when a
palm comes down edge first and looks like a finger for a moment, or a thumb
that was resting begins to move.

A contact that is not meant is a contact all the same, and what to do with
it is the program's choice: one program ignores it, another lets the palm
rest and smudge. But a program that knows nothing of ``conf`` would take a
resting palm for a finger, so the contacts the system judges accidental are
reported only while mode bit ``64`` is on. Without it a program is not
shown a contact for as long as the system takes it for an accident:

* A contact that begins as an accident is not reported. If the system stops
  taking it for one before it lifts, it is reported from then on, and begins
  with a ``down``. If it lifts first, nothing is reported of it.
* A contact that is being reported, and that the system comes to take for an
  accident, is cancelled: a report with ``phase=4`` and ``conf=0``, after
  which the program takes back what it made of the contact, as after every
  other cancel. That is so also when the contact lifts as an accident, whose
  last report is then a ``cancel`` and not an ``up``. A cancelled contact is
  not reported again, unless the system stops taking it for an accident
  before it lifts, and then it begins with a new ``down``.
* A frame report follows a group only when something of the group was
  reported.

With bit ``64`` every contact is reported as it happens, with the ``conf``
the system gave it, and nothing is cancelled for being an accident: a program
reads ``conf`` in every report, and one that had begun to draw for a contact
that turns out to be an accident takes that back itself. The bit changes nothing where the
system does not judge. A terminal that can report these contacts says so with
``caps`` bit ``256``.

Windows gives the judgement as the confidence flag of a pointer, and the HID
usage tables have it as *Touch Valid* in the report of a contact, where a
terminal that reads the digitizer finds it. Android judges too, and says so
late: it cancels the one finger it has come to take for a palm, which is a
``cancel`` of that contact with ``conf=0``. AppKit has a mark for the
touches on a trackpad that it takes to be resting there and says nothing of
the others, and was not seen to set it, see `Implementations`_. Wayland and
X11 do not say.

Tool actions
~~~~~~~~~~~~~~~

Some things done with a tool are not a button held down: a double tap on the
barrel of a pen, a squeeze of it, a click of the button on its end. While
mode bit ``16`` is on they are reported with ``kind=4``::

    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=4 : device=3 : t=408211 : action=1 : x=25600 : y=51200 : mods=0 <ST>

================ ==================================================================
``device``       The tool, as in its reports, ``0`` when the system does not
                 say which tool it was
``t``, ``tu``    The time of the action, as in the other reports
``action``       ``1`` a double tap on the barrel, ``2`` a squeeze, ``3`` a
                 click of the button on the end of the tool, ``4`` a double
                 click of that button, ``5`` that button pressed and held,
                 ``6`` the tool was put into its holder, ``7`` it was taken
                 out of it, ``8`` the tool is moved through the air with its
                 button held
``state``        For an action that lasts, as a squeeze does, ``1`` when it
                 begins and ``0`` when it ends, and for action ``8`` also
                 ``2`` for each report in between; absent for the others
``dx``, ``dy``   With ``state=2``, how far the tool has moved through the air
                 since the last report of the action, −32767 to 32767,
                 positive to the right and down
``x``, ``y``     The position of the pointer, as in a tool report
``mods``         As in a tool report
================ ==================================================================

``device``, ``t``, ``action``, ``x``, ``y`` and ``mods`` are always present.
An action has no ``phase`` and no ``contact`` and belongs to no stroke. It
goes to the window that has a stroke of the tool captured, and otherwise to
the window under the pointer. What an action does is the program's choice; a
double tap usually switches between the pen and the eraser. A program
ignores an ``action`` it does not know.

A pen that has a holder in the device, a slot or a magnet that also charges
it, tells the system when it is put in and taken out, which is a good moment
to put a palette away or bring it up. Windows tells applications of both.
Neither action lasts, and ``x,y`` is where the pointer happens to be.

A pen with a radio and a motion sensor can be waved like a wand: while its
button is held away from the screen it reports how it moves, as the S Pen
of the Samsung models that pair it over Bluetooth does. That is action
``8``: ``state=1`` when the button goes down, any number of reports with
``state=2`` that each carry ``dx`` and ``dy``, and ``state=0`` when it comes
up. The two are what the sensor in the pen makes of the motion and have no
unit, so only their signs and their proportions mean anything, and a
terminal that is given fractions scales them to whole numbers by one factor
throughout. A program adds them up to move a cursor or reads a flick from
their run. The pen is
not over the tablet while this happens, so there are no tool reports and
``x,y`` stays where the pointer was.

Pads
~~~~~~~

A pad is the keys, touch rings, touch strips and dials on the body of a
tablet, or on a remote of their own. While mode bit ``4`` is on they are
reported with ``kind=3``, one control to a report::

    <OSC> 777 ; tablet-pointer ; v=1 : op=event : kind=3 : device=5 : t=408211 : mods=0 : ring=0 : angle=9000 : src=1 <ST>

``device``, ``t`` and ``mods`` are always present and mean what they do in
the other reports, as does ``tu``. A pad report has no ``phase``, no
``contact`` and no position. The rest of it is one of these:

============================ ======================================================
``button``, ``state``        A key went down, ``state=1``, or came up, ``state=0``
``ring``, ``angle``          Where the finger is on a touch ring, in hundredths of
                             a degree, ``0`` to ``35999``, clockwise from the top
                             of the ring as the tablet is turned
``strip``, ``pos``           Where the finger is on a touch strip, ``0`` at its
                             top or left end to ``65535`` at the other
``ring`` or ``strip``,       The finger lifted off the ring or strip, in place of
``stop=1``                   ``angle`` or ``pos``. What comes next from that
                             control begins a new movement, so the jump from the
                             last position to the next one is not a turn
``dial``, ``delta``          How far a dial was turned since its last report,
                             signed, ``120`` for each detent and less for a dial
                             that turns smoothly. A dial has no position
``group``, ``mode``          A group of controls went into another mode
============================ ======================================================

The value of ``button``, ``ring``, ``strip`` and ``dial`` is the number of
the control, each kind counted from zero across the pad in the order the
system lists them. ``src=1`` is added to a ring or strip report when the
system says a finger is what moves it.

The controls of a pad are in groups, and each group has a mode, a number
from zero that the user changes on the pad, usually with a key the system
keeps for that, and that the pad may show with a light. A mode is nothing
but a number: it is there so that the same controls can do one set of things
in one mode and another in the next, and the program decides what. A
``group`` report gives the number of the group and the mode it is now in.
Terminals also send one for every group when the pad comes to the window, so
a program does not have to wait for the user to switch before it knows.

Pad reports go to the window that has the keyboard focus, not to the one
under the pointer. Before the first of them, each time the pad comes to a
window that has pad reports on, the terminal describes the pad::

    <OSC> 777 ; tablet-pointer ; v=1 : op=device : device=5 : kind=3 : buttons=9 : rings=1 : strips=0 : dials=0 : groups=1 : modes=4 : name=V2Fjb20gSW50dW9zNSB0b3VjaCBNIFBlbg== <ST>

================ ==================================================================
``buttons``,     How many keys, rings, strips and dials the pad has
``rings``,
``strips``,
``dials``
``groups``       How many groups its controls are in
``modes``        How many modes each group has, separated by commas, in the order
                 of the groups
``mode``         The mode each group is in, in the same form, present when the
                 terminal knows it at that moment
``bgroups``,     The group each key, ring, strip and dial is in, see below
``rgroups``,
``sgroups``,
``dgroups``
``tablet``       The tablet the pad belongs to, the number the tools on that
                 tablet carry, see `Tablets`_
``name``         The name of the tablet the pad belongs to, as for a tool
``vid``, ``pid`` The ids of that tablet, sent only while mode bit ``8`` is on
================ ==================================================================

A mode is the mode of a group, so a program that shows what a control does
has to know which group the control is in. ``bgroups`` says it for the keys,
``rgroups`` for the rings, ``sgroups`` for the strips and ``dgroups`` for
the dials: the number of the group of each control of that kind, in the
order of their numbers and separated by commas, with ``-`` for a control
that is in no group, as a key is that the system keeps for switching modes.
``bgroups=0,0,0,-,1,1`` is three keys in group 0, one in none and two in
group 1. A list has an entry for every control of its kind. It is absent
when the pad has no control of that kind and when the system does not say,
and with a pad of a single group there is nothing to be in doubt about.
The system may take a key out of its group for a use of its own, or give it
back, while the pad is in use, and the pad is then described again, as a
tool is when what was said of it changes. The groups come from libinput,
which Wayland compositors pass on through ``tablet-v2``: each group lists
its keys and announces its rings, strips and dials. Nothing else that
silkie reads has them.

A pad holds nothing open except its keys. When the mode is changed, the
terminal is reset, the keyboard focus goes to another window or the pad goes
away, the terminal reports every key that is down as released,
``state=0``, with the ``t`` of its press, and the release that the pad makes
later is not reported. A key that was already down when the reports began
never reports its release. A program must tolerate a release for a key it
does not have down.

A program can say what a control does at the moment, for the system to show
the user where it has a display for that, such as an overlay of the layout
of the pad::

    <OSC> 777 ; tablet-pointer ; v=1 : op=label : device=5 : button=2 : text=VW5kbw== <ST>

``device`` is the pad and exactly one of ``button``, ``ring``, ``strip`` and
``dial`` names the control, with a number from ``0`` to ``255``. ``text`` is
UTF-8 without control characters, encoded as base64 with padding; a ``text``
that is empty or left out takes the label away. A window system may carry
only so much of a label: a Wayland request holds 4075 bytes of it, and kitty
cuts a longer one there, between characters. A label belongs to the mode
the group of the control is in, so a program sets its labels when the pad is
described and again after each ``group`` report. It is accepted only from
the program the pad is reporting to, there is no reply, and a terminal with
nowhere to show labels ignores it.

Totems
~~~~~~~~~

A totem, ``tool=8``, is a dial or puck that stands on the screen and is
tracked by it. It is reported as a tool: ``x,y`` is its centre, ``twist``
how far it has been turned, and the contact shape, with ``valid`` bit
``128``, the area of the screen it covers. Pressing it down is button ``1``,
so a totem that is pressed and turned is a stroke whose ``twist`` changes.

Estimates and predictions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Some systems hand over a sample before all of it has been measured: the
pressure of a pen can arrive by radio after the position of its tip, and
until it does the system fills in an estimate. Some also predict, giving
samples for where the tool is about to be, which a program draws to hide
the delay to the screen and then replaces. A terminal that can pass these on
does so only while mode bit ``32`` is on, with three more fields in the
reports of tools and fingers:

================ ==================================================================
``seq``          A count of the samples of this device, an unsigned 32 bit
                 integer that wraps, by which a later report names an earlier
                 one
``est``          The measurements in this report that are estimates, a bit set
                 with the bits of ``valid``
``pred``         ``1`` for a predicted sample; absent otherwise
================ ==================================================================

An estimate is put right by a correction: a report with ``phase=7`` and the
``seq`` of the report it corrects, which repeats that report with the
measured values. It is not a new point of the stroke. Its own ``est`` names
what is still an estimate, and is absent once nothing is. A correction can
come after the ``up`` of its stroke, and it may never come, in which case
the estimate stands.

A predicted sample is not part of the stroke. A program that draws it takes
every predicted sample of a contact away again when the next report of that
contact arrives that is not predicted. A predicted sample has no ``seq`` and
is never corrected.

Without bit ``32`` a terminal sends an estimate as an ordinary value and
sends no corrections and no predictions, so a program that knows nothing of
this draws what it is given.

Lifecycle
-------------

A stroke begins with ``down`` and ends with ``up`` or ``cancel``; in between
every report of that contact is a ``move``. The stroke stays captured by the
window it began in even when the tool or finger leaves it, so coordinates can
be negative or exceed the cell area, and they must not be clamped into
another window. The fingers of one sequence, from the first finger landing
to the last lifting, all go to the window under the first finger, or for a
touchpad to the window under the pointer when the first finger landed. A
contact the system judges accidental does not keep a sequence open, or a
thumb left lying on a touchpad would hold every later finger in one window:
a finger that lands, or a contact that stops being an accident, while every
contact that is down is an accident begins a new sequence, with the window
that is under it, or under the pointer, at that moment. When that is another
window, the contacts that were down are cancelled in the window they were
reported to, and in the new one each of them begins with a ``down``.
``hover`` reports follow a tool in proximity without a contact and are not
captured. A tool leaving proximity cancels any stroke it had open, and then,
when the system reports it, sends phase ``5``. That report goes to the
window that was last told of the tool, wherever the tool was when it left:
a pen whose stroke was dragged out of the window and ended there is over no
window, and the program still has to learn that it is gone.

The terminal cancels contacts, with ``buttons=0`` and ``p=0``, when the mode
changes, the terminal is reset, the window's size in cells changes, the
system takes the fingers over for a gesture of its own, or the device goes
away. After a cancel the rest of that stroke is not reported, not even its
``up``. If a finger that was cancelled because the system took it for an
accident is reported again, see `Fingers`_, that is a new stroke, which
begins with a ``down``.
Programs must tolerate a lost ``hover``, a repeated cancel, and a fresh
``down`` for a contact they still have open, which replaces the earlier
stroke.

A terminal or relay that cannot keep up may merge some reports, as
:doc:`extension-framing` says. Of this protocol's reports only ``move`` and
``hover`` may be merged: consecutive ones of one contact, with no other
report of that contact between them and the same ``buttons``, into one with
the absolute values of the last and the sum of ``wheel`` and of ``wclicks``,
and with them the frame reports of the groups they were in. Every other
report, of a pad, a device, an action or a correction included, arrives as
it was sent.

A multiplexer translates ``x,y`` of on-screen contacts by the pane's origin
(borders included) and scroll offset, forwards every other field unchanged,
delivers each stroke and each finger sequence only to the pane it began in,
and cancels contacts when panes move or resize. It keeps the mode of each
pane and asks the terminal for the union of them. It keeps the last
description of each tool in proximity and sends it to a pane before that
pane's first report of the tool, without the hardware identifiers unless
that pane asked for them. When the terminal describes a device again, the
multiplexer keeps the new description in place of the old one and sends it
on to the pane the device is reporting to. ``rx``, ``ry``, ``grip`` and
``conf`` are forwarded unchanged, none of them being a place on the screen.
Two panes can differ in mode bit ``64``, so a multiplexer asks the terminal
for it when any pane has it, and for a pane that has not it does what a
terminal does: it keeps back the contacts the system judges accidental,
cancels one that becomes an accident, begins one that stops being one with
a ``down``, and sends a frame only after a group it sent something of.
``tablet`` is the terminal's number and is forwarded unchanged; a relay
that brings the tablets of several terminals together gives them numbers of
its own, so that two tablets never share one and a pad keeps the number of
the tools of its tablet. A tool action goes to the pane that has a stroke
of the tool, and otherwise to the pane under the pointer, with ``x,y``
translated as for a contact and ``dx,dy`` unchanged. Corrections follow the report they correct to
its pane, and ``seq`` is forwarded unchanged. Pad reports go unchanged to
the pane that has the focus, after the description of the pad, and when the
focus moves to another pane the keys that are down are reported released to
the pane it left. A label is passed on only from the pane the pad is
reporting to. A multiplexer or relay that
detaches or loses its connection while contacts are live sends ``cancel``
for each of them to the program, so a program never holds a dead contact,
and a relay that reconnects forwards nothing it did not see: the fingers
down at that moment are simply reported from their next ``down``.

Implementations
-------------------

* silkie, this fork of kitty: tools on macOS, Wayland and X11; fingers on
  the touchpad on macOS and on touchscreens on Wayland, with contact shape,
  and on X11, without;
  pads on Wayland, with the groups of their controls, and the keys of pads
  on X11; tool actions on macOS;
  the number of the tablet on macOS and Wayland; the size of a tablet and
  whether it is a display on Wayland and X11; the place of a tool on its
  tablet on X11. ``kitty --debug-input`` logs the tablet, pad and touch
  events it sees.
* Inkline, Goblin Skiff and GoblinView carry a codec for an earlier draft of
  this protocol and are being brought up to this specification.

Implementation notes for silkie
----------------------------------

silkie reads tablets and touch devices through the window system, so a
program gets what the system reports. It keeps the mode bits ``1``, ``2``,
``4``, ``8``, ``16`` and ``64``, and ``caps`` says which of them the system
it runs on can do anything for. None of the systems it runs on hands over
estimates or predictions, so it does not take bit ``32``, and it reports no
totems and no battery charge. ``tu`` is sent on macOS only: Wayland and X11
stamp tablet and touch events in whole milliseconds.

Of what a tablet says of itself silkie sends what the window system gives
it, which the notes below go through: ``tablet`` on macOS and Wayland, the
groups of the controls of a pad on Wayland, ``w``, ``h`` and ``direct`` on
Wayland and X11, where they are had from the kernel or the driver, and ``rx``
and ``ry`` on X11. It can write every field of this page,
and its tests do, but nothing it reads has ``grip``, ``pcolor``, ``pwidth``,
``pstyle``, or the actions ``6`` to ``8``, so it never sends them, and
``conf`` it has from a mark of AppKit that no touch was seen to carry, so it
does not claim ``caps`` bit ``256`` anywhere. It describes a tool or a pad
again when what it said of it has changed, which at present only a pad on
Wayland does.

Tilt is defined here on the axes of the screen. A system that turns a
tablet, for a left-handed user or for a pen display that stands rotated,
turns the position with it, and not always the tilt, which the tablet
measures on its own axes. silkie turns tilt where the system says how the
tablet is turned, and passes it on as it is given where it does not:

* libinput turns a tablet only by a half turn, for left-handed use, and
  then turns tilt and twist with the position itself.
* On Wayland, ``tablet-v2`` gives tilt on the axes of the tablet, and a
  compositor that maps a tablet to a rotated output, or turns it by a
  calibration of its own, says nothing of that. Tilt is passed on as
  given, so on a pen display turned a quarter turn it is on the axes of
  the panel, not of the screen.
* On X11 the wacom driver's ``Wacom Rotation`` and the server's
  ``Coordinate Transformation Matrix`` turn the position and leave tilt on
  the axes of the tablet. silkie reads both, again whenever they change,
  and turns tilt by them, when the matrix keeps the axes at right angles,
  as a quarter turn, a half turn, a mirror or a map to part of the screen
  does. Tilt is left out for a matrix that turns the tablet by any other
  angle or flattens it. Twist is not turned.
* AppKit gives tilt as the tablet driver reports it, after whatever
  orientation the user set in the driver.

None of this was tried with a turned tablet.

* On macOS, tablet data arrives with the mouse events of the tablet driver.
  Tilt is scaled from the driver's fraction of maximum tilt assuming 60
  degrees, as Wacom tablets report, and ``z`` is the driver's absolute Z.
  While a program takes a tool's reports, AppKit's merging of tablet points
  is turned off, so every sample the tablet sends is reported. AppKit tells
  a pen, an eraser and a puck apart; the airbrush and the lens cursor of a
  Wacom tablet are recognised from the driver's own code for the tool. No
  name is known for the device frame. ``tablet`` is one more than the index
  AppKit gives the tablet among those connected. AppKit also gives where the
  tool is in units of the tablet, but neither the range of those units nor
  the size of the tablet, so there is no ``rx``, ``ry``, ``w`` or ``h``. The
  one tool action is the double tap on an Apple Pencil paired with an iPad
  that the Mac uses as a display, and the system does not say which tool it
  came from, so ``device`` is ``0`` unless a single tool is in proximity.
  Fingers are the indirect touches of the trackpad, as AppKit reports them
  to the content view; the pad size is the device size AppKit gives, taken
  to be in points at 72 to the inch. Trackpad fingers are reported alongside
  the mouse and scroll events the system makes from them. A trackpad that
  measures force gives ``p`` and ``stage`` once it has been pressed for the
  first time. AppKit has a mark for the touches it takes to be resting on
  the pad, which its documentation gives as a thumb or another touch at
  rest, and silkie asks it for those too: a touch with the mark is
  ``conf=0``, and one that comes to rest or stops resting is reported even
  when it has not moved. Of a touch without the mark AppKit says nothing, so
  such a touch has no ``conf``. On the trackpad of a 16 inch MacBook Pro
  under macOS 26 the mark was never seen: a palm laid on the pad, and a
  thumb held still at its edge while another finger moved, both came as
  touches like any other. So silkie does not claim ``caps`` bit ``256`` on
  macOS, a program there gets no ``conf`` and judges palms for itself, and
  no other interface of AppKit says what a touch is. The code is left in for
  a trackpad that does set the mark: a program that turns on mode bit ``64``
  regardless is sent such a touch with ``conf=0``, a program that does not
  has it kept back, as AppKit itself keeps it back from a view that has not
  asked, with one difference that is meant: a finger that comes to rest ends
  with a ``cancel`` where AppKit would have ended it with an ``up``, since a
  finger that came to rest did not tap. ``kitty --debug-input`` writes
  ``accidental`` after a touch that has the mark. The Wacom tool types, the
  double tap and the force are written from the documentation of AppKit and
  have not been tried with the hardware. There are no pad reports: AppKit
  has no events for the controls of a pad, which an application gets only
  through the request interface of Wacom's own driver.
* On Wayland, tools come through the ``tablet-v2`` protocol, which reports
  every measurement above, identifies the tool and names the tablet, and
  fingers through ``wl_touch``, with ``shape`` and ``orientation`` on a seat
  of version 6 or later whose compositor sends them, which sway and GNOME
  did not when this was written; ``caps`` claims the shape once a contact
  has had one. The pointer cursor follows a tool only on compositors with
  ``cursor-shape-v1``. GNOME applies its own stylus button mapping first (by
  default the lower button is the middle button), sway passes the buttons
  through. A tool does not move the pointer as far as silkie is concerned: a
  compositor can give tablets a cursor of their own, so what is reported at
  the pointer, such as a scroll, stays where the pointer was. Pads come
  through ``tablet-v2`` as well: keys, rings, strips and modes from its
  first version and dials from its second, which the compositor has to
  offer, and a label is passed to the compositor as the feedback for that
  control. The ``name`` and ids of a pad are those of the tablet it is
  attached to. ``caps`` claims tools and pads only on a compositor that has
  the protocol. ``tablet`` numbers the tablets of the seat in the order the
  compositor announced them: a tool has the number of the tablet it came
  into proximity over, a pad that of the tablet it was given the focus with.
  The group of a key is the group that lists it, and the group of a ring, a
  strip or a dial the one that announced it. When a compositor lists the
  keys of a group again while the pad has the focus, the pad is described
  again. The protocol has neither the size of a tablet, nor whether it is a
  display, nor where on it a tool is, though the compositor knows all three.
  It does name the device node of the tablet, and of a node the system says
  the first two to anyone who asks, with no access to the device itself.
  udev keeps the size, which it works out from the range and the resolution
  of the position the device reports, as ``ID_INPUT_WIDTH_MM`` and
  ``ID_INPUT_HEIGHT_MM`` in its database, in whole millimeters rounded down.
  The kernel has the properties of the device in sysfs, in which
  ``INPUT_PROP_DIRECT`` is a display and ``INPUT_PROP_POINTER`` a tablet
  that is not one. silkie reads both through libudev, which it loads when a
  tablet names a node and does without where there is none, and sends them
  as ``w``, ``h`` and ``direct``. Of the nodes a tablet has, only that of
  the pen is read, not that of the pad or of the fingers on the tablet. A
  node is a name on the machine the compositor runs on, so what is found at
  it counts only when the kernel has that device under the vendor and
  product ids the compositor gave for the tablet, or under its name when it
  gave no ids: a terminal on another machine than its compositor says
  nothing. What the kernel does not say is left out, as the size is for a
  device that states no resolution. ``rx`` and ``ry`` cannot be had this
  way. This was tried against a stand-in for libudev that answers as udev
  and the kernel do for two tablets, and the reading of the real one against
  the devices of a machine that has no tablet; no tablet was at hand. Pads
  have been tried against a stand-in for the compositor that sends what
  wlroots sends, not with a tablet.
* On X11, tools come through XInput 2 devices whose names include the tool
  type, as the libinput and wacom drivers name them; that name is the
  ``name`` of the device frame, and the ids of the tablet are not known.
  ``tangent`` carries the device's wheel axis and ``twist`` its rotary Z
  axis. The position of a tool is made from the position its own device
  reports, scaled to the screen as the server does it, because the server
  puts the position of the pointer before an event into that event, which is
  where the tool was one report earlier. That position is also where ``rx``
  and ``ry`` come from. The server puts it through the matrix by which the
  user maps the tablet to the screen or to one monitor, so silkie reads that
  matrix, again whenever it is changed, and undoes it; a matrix that cannot
  be undone leaves ``rx`` and ``ry`` out, and so does the area ratio of
  the libinput driver, which stretches one axis of the position and cuts off
  what falls past its end, by a factor silkie cannot learn exactly. ``w``
  and ``h`` are the range of the position divided by the resolution the
  driver states for it; no driver that states one was at hand, so those
  sizes are untried. The libinput driver states none, and both drivers name
  the device node of a tool in its ``Device Node`` property and its vendor
  and product ids in ``Device Product ID``. Where the driver states no
  resolution, silkie reads the size and ``direct`` of the node from the
  kernel as on Wayland, the size in whole millimeters rounded down, and
  believes them only when its connection to the X server is a local socket,
  the node being one on the machine of the server, and when the kernel has
  the device under those ids. This was tried against devices of the
  server's test driver given those properties and a stand-in for libudev,
  not with a tablet. X has no tablet that tools
  belong to, each tool being a device of its own, so there is no ``tablet``.
  The X server does not report a tool leaving proximity, so phase ``5``
  arrives only when the device goes away. Xwayland's tablet devices are
  recognised too, but under GNOME no motion reached them in testing; use the
  Wayland backend there. Nothing of a tablet is reported for them: their
  position is one on the desktop and their resolution is made up. Fingers on
  a touchscreen come through the touch events of XInput 2.2, and ``caps``
  claims them when the server has that version. X reports each finger in an
  event of its own, so every finger report is followed by a frame report of
  its own and fingers that moved together are not grouped. There is no
  contact shape, a touch the system takes over ends with an ordinary ``up``
  since the server does not say that it was taken, and the fingers on a
  touchpad are not reported. The server also moves the pointer to where the
  first finger lands, which is not passed on as mouse reports. This was
  tried against Xwayland, which makes these events from the touches of its
  compositor, not with a touchscreen. Pads come from the libinput and
  wacom drivers, which give a pad a device of its own of the type ``PAD``,
  and ``caps`` claims them when the server has one of those drivers. Of a
  pad, X has only its keys, as the buttons of that device: keys 0 to 2 are
  buttons 1 to 3, the rest are from button 8 on, and buttons 4 to 7 are the
  scrolling the drivers make of the rings and strips by default, which is
  not reported. ``buttons`` in the description is the number of buttons of
  the device less those four, which is the number of keys under the
  libinput driver; the wacom driver can give a device more buttons than its
  pad has keys. Rings and strips are not reported, and the description has
  none: the libinput driver gives every pad the same axes for two rings and
  two strips whatever it has, so how many it has cannot be told, and the
  wacom driver passes on raw values whose direction and units it does not
  state. There are no groups and no modes, which the libinput driver
  publishes only in properties of its own that silkie does not read, and a
  label has nowhere to go. A pad reports to the window with the keyboard
  focus, but the server delivers its keys only to a window under the
  pointer, so a pad key pressed with the pointer over another application
  is not reported. This was tried against a device of the server's test
  driver marked as a pad with the ``Wacom Tool Type`` property of the wacom
  driver, not with a pad.
