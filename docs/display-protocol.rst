Display protocol
==================

.. versionadded:: 0.50.0

A program that draws knows nothing about the screen its output ends up on:
whether it is a color LCD or sixteen levels of gray on e-ink, how much of the
color spectrum it covers, how bright it can go, how many physical pixels a cell
has and how often it refreshes. Each of these changes what the best rendering
is, from the choice of a palette and a dithering strategy to whether animation
is worth doing at all. The display protocol lets a program ask, and be told
when the answer changes, for example because the window was moved to another
screen. It is an open terminal extension in the manner of the other kitty
protocols: small, unobtrusive, easy to implement, and no change to what the
TTY is.

Framing
-------------

Frames are ``OSC 777`` with the ``display`` selector::

    <OSC> 777 ; display ; v=1 : op=query : id=42 <ST>

They follow the rules every OSC 777 extension shares, which
:doc:`extension-framing` gives: the syntax of the fields, which frames are
rejected, the units and the clock. The values in the terminal's reports are
words, or decimal numbers that may have a fraction, such as
``headroom=1.5``, with no leading ``+``. ``v``, the protocol version, is
``1``, and ``op`` is one of ``query``, ``cap``, ``mode`` and ``event``.

Query
-------------

A program asks about the display its window is on::

    <OSC> 777 ; display ; v=1 : op=query : id=42 <ST>

``id`` is a non-negative 31 bit integer of the program's choosing, echoed in
the reply so replies can be matched to queries. A supporting terminal
answers::

    <OSC> 777 ; display ; v=1 : op=cap : id=42 : mode=0 : kind=lcd : color=rgb : bits=8 : gamut=p3 : headroom=1 : scale=2 : period=8333 : ppi=254 : red_bits=8 : green_bits=8 : blue_bits=8 : gray_bits=0 : precision=framebuffer : output_bits=16 : output_type=float : output_dither=0 <ST>

A terminal that does not support the protocol ignores the query, so a program
must time out and fall back to assuming an ordinary color display.

The fields of the reply are:

``mode``
    The current state of the mode described below, ``0`` or ``1``.

``kind``
    One of ``lcd``, ``oled``, ``eink`` and ``unknown``. On e-ink, updates are
    slow and leave ghosts, so a program avoids animation and smooth scrolling
    and redraws in large, infrequent steps. It also avoids dithering, whose
    fine pattern shows as grain and ghosts as it changes, and reduces colors to
    the levels of the panel by taking the nearest. ``kind`` says nothing of
    color: there is color e-paper, and ``color`` is what says whether a display
    shows only gray.

``color``
    One of ``rgb`` (independent red, green and blue channels), ``gray``
    (one grayscale channel) and ``mono`` (only black and white).

``bits``
    Bits per color channel, ``0`` when unknown or RGB channels have different
    widths. This field remains for older clients; use the individual widths
    below when available. Sixteen levels of gray are
    ``color=gray : bits=4``. Together with ``color`` this is what a program
    needs to choose a palette and whether and how to dither; antialiasing
    works fine with a few levels and is not what this field decides.

``red_bits``, ``green_bits``, ``blue_bits``, ``gray_bits``
    Individual channel precision in bits, ``0`` when unknown or not applicable.
    RGB reports use the first three fields and set ``gray_bits=0``; grayscale
    and monochrome reports use ``gray_bits`` and set all three RGB widths to
    ``0``. For example RGB565 is ``color=rgb : bits=0 : red_bits=5 :
    green_bits=6 : blue_bits=5 : gray_bits=0``. A sixteen-level grayscale
    display is ``color=gray : bits=4 : gray_bits=4``. A monochrome display has
    ``color=mono : bits=1 : gray_bits=1``.

``precision``
    Where the channel widths came from: ``framebuffer`` for the system's
    display mode or window visual, ``configured`` for a user's declaration,
    ``model`` for what the terminal knows of the model of the monitor, or
    ``unknown``. A program treats a value it does not know as ``unknown``. A framebuffer width does not establish the physical
    panel's precision: the compositor, display link, panel, or dithering may
    change what is finally shown. These fields describe available color
    representation, not a measurement of calibration or color error.

``output_bits``
    Bits per color channel of the surface the terminal itself draws into and
    hands to the system, ``0`` when unknown or its channels have different
    widths. The fields above describe the display; this one describes how
    finely the terminal keeps what a program sends, a gradient in an image
    with 16 bits per channel for example, on its way there. What finally
    reaches the panel is the lesser of the two.

``output_type``
    What the channels of that surface hold: ``fixed``, integers standing for
    fractions of full intensity, ``float``, floating point numbers, or
    ``unknown``. A ``float`` surface with ``output_bits=16`` holds half floats,
    with 11 significant bits, in linear light; the system quantizes it for the
    display.

``output_dither``
    The bits of the steps the terminal dithers what it draws to, of the
    narrowest channel, ``0`` when it does not dither. Colors between those
    steps show as a fine pattern rather than being rounded into bands; colors
    that are exactly a step are never dithered. This can be less than
    ``output_bits``: a deep surface on a display that is not known to show
    more than 8 bits is dithered to the steps of 8 bits, as the system rounds
    it to such a display without dithering. A program that dithers its own
    output can tell from this to which steps, and whether the terminal would
    do it again.

The individual widths, ``precision`` and the ``output`` fields are additive
version-1 fields. Clients must ignore unknown fields and treat missing
individual widths as unknown; an older terminal's nonzero ``bits`` still
applies equally to its active channels.

``gamut``
    One of ``srgb``, ``p3``, ``rec2020`` and ``unknown``: the widest color
    space the display covers.

``headroom``
    How much brighter than the white of normal text the terminal can show,
    as a multiple: ``1`` when there is no extra brightness. See the HDR
    brightness protocol for how to use it.

``scale``
    The scaling of the framebuffer that the system asks applications for: how
    many physical pixels it wants used for one of its own logical units,
    ``2`` on most high density displays and often a fraction such as ``1.25``
    or ``1.5`` on others. ``0`` when unknown. See `Pixels`_ for what it is
    for.

``period``
    The refresh period of the display in microseconds, ``0`` when unknown.

``ppi``
    Physical pixels per inch: how many of the pixels a program draws in fit
    in an inch of the screen, so that it can draw at a true size. ``0`` when
    unknown.

The monitor the window is on, where the terminal knows it, which a program
can use to recognize a monitor whose properties it knows better than the
system, or to show the user which monitor the window is on. Each of these
fields is left out when it is not known:

``mfg``, ``product``
    The manufacturer ID and the product code of the monitor's EDID: three
    capital letters, such as ``OBX``, and a number from ``0`` to ``65535``.
    ``product`` is sent only with ``mfg``.

``make``, ``model``
    The manufacturer and the model as the system gives them, else from the
    EDID, its manufacturer ID and its name descriptor, such as ``MIRA253``.

No serial number is sent: it would tell whose monitor it is, and the program
asking may be running on another machine.

``connector``
    The system's name for the connection the monitor is on, such as ``DP-7``.

``description``
    A description of the monitor meant for people, as the system gives it.

These four are strings, UTF-8 without control characters encoded as base64
with padding, as the other extensions of this terminal send strings. For
example a monitor reported as ``mfg=OBX : product=17 : make=T0JY :
model=TUlSQTI1Mw== : connector=RFAtNw==`` is an ONYX BOOX Mira Pro, make
``OBX`` and model ``MIRA253``, on ``DP-7``. A move to another monitor is a
change, and sends an event even when every other field is the same.

Values the terminal cannot determine are ``unknown`` or ``0``, never guessed.
A terminal that knows nothing about its display, a multiplexer that is not
attached, for example, answers with every field unknown and the program is no
worse off than before it asked.

Pixels
----------

A pixel, in this protocol and in every other one of this terminal that
measures in pixels (the size of the window and of a cell, the size and
placement of an image, the position of a pointer, a pen or a finger, the
distance of a scroll), is a physical pixel: one pixel of the framebuffer the
terminal draws into. Nothing is expressed in the logical units of the system,
which stand for ``scale`` pixels each, and the terminal never stretches what a
program draws to make up the difference. A line a program draws one pixel
wide is therefore one pixel wide, not a soft blob spread over several, and a
program gets every pixel the display has.

``scale`` is what the system would have had an application do: the size it
wants things drawn at for them to look the size the user chose. A program uses
it to decide how big to draw, a rule that is one logical unit thick as
``scale`` pixels for example, and never to convert a position or a size, which
are in physical pixels already. ``ppi`` relates those same pixels to the
screen itself.

Mode
--------

A program that wants to be told about changes turns them on per terminal
window with::

    <OSC> 777 ; display ; v=1 : op=mode : mode=1 <ST>

and off with ``mode=0``. The mode is local to the window and is reset by
``RIS``. Programs must turn it off when they exit, suspend or fail, and turn
it back on when resumed. Multiplexers keep it with the rest of the terminal
state and turn it off when detaching. A terminal may let its user refuse the
mode, for every program or for some. A refused mode reads back as off and
nothing is reported, so a program reads the mode back rather than assume it
was granted.

A ``mode`` request without a ``mode`` field reads the state instead of
changing it, and is answered with::

    <OSC> 777 ; display ; v=1 : op=mode : mode=1 <ST>

A program resuming after a reconnection reads the mode to learn whether it
survived, rather than assuming it did.

Events
-----------

Turning the mode on sends one event with the current values, so a program
needs no separate query. After that, while the mode is on, the terminal sends
one event whenever any field changes: the window was moved to a display with
a different scale or refresh rate, the display's brightness setting changed
the headroom, its color profile was switched::

    <OSC> 777 ; display ; v=1 : op=event : t=408211 : kind=lcd : color=rgb : bits=8 : gamut=srgb : headroom=1 : scale=1 : period=16666 : ppi=96 : red_bits=8 : green_bits=8 : blue_bits=8 : gray_bits=0 : precision=framebuffer : output_bits=10 : output_type=fixed : output_dither=8 <ST>

The fields are those of the ``cap`` reply, without ``id`` and ``mode``, and
``t``, the time the terminal learnt of the change, in milliseconds on the
clock the terminal uses for every extension, see :doc:`extension-framing`.
Changes are relative to the last event sent, so turning the mode off and on
again reports the current values once and then only changes. A change can be
reported a short while after it happens, and a program that has turned the
mode off may still receive an event that was already on its way. An event is
never merged with another or dropped, by the terminal or by a relay.

Relays and multiplexers
---------------------------

A relay such as a mosh-like client, that draws the terminal's screen on a
display of its own, answers queries for the display it has, since the
terminal at the far end cannot know it. When the relay reconnects to a
different client it sends one event to every window with the mode on, so a
program that laid out its output for a phone can redo it for a desktop, and
the other way round.

A multiplexer answers for the display of the client it is attached to, with
every field unknown while detached, and sends an event when attached to a
client with a different display.

Configuration
----------------

The operating system reports most of these values, but not all. Nothing tells
a terminal that it is on e-ink: e-paper monitors say in their EDID that they
are RGB color displays, and operating systems pass that on. Silkie therefore
goes by which monitor the window is on, as the system identifies it:

* On Wayland, the make and model of ``wl_output`` and the name and description
  of ``wl_output`` (version 4) or ``xdg_output``. No Wayland protocol gives the
  EDID. Mutter, for example, gives an ONYX BOOX Mira Pro as make ``OBX``, model
  ``MIRA253``, name ``DP-7`` and description ``OBX 25"``. On Linux Silkie also
  reads the EDID of the connector with that name from
  :file:`/sys/class/drm`, for the product code, but only when exactly one
  connector has the name and its EDID has the model the compositor gave, so
  that a nested or remote compositor never gets the EDID of another monitor.
* On X11, the name of the RandR output and its ``EDID`` property.
* On macOS, the name of the screen, as model, and the EDID manufacturer and
  product codes that Core Graphics gives as its vendor and model numbers.

Silkie has a table of e-paper monitors, matched on these, which says what each
is: e-paper, and whether it shows only gray, and with how many levels where
that is known. The :opt:`monitor_info` option declares the properties of a
monitor, overriding the table and the OS. A window on e-paper (``kind=eink``)
avoids what ghosts: blinking, animation and dithering, so ``output_dither`` is
``0`` there. For now custom shaders, the visual bell, pixel scrolling and the
repaint timing are as on any display, and Silkie does not reduce images to the
levels of gray of the panel: the monitor does that. A window on a display that shows only gray (``color=gray`` or
``mono``) gets black text on white, or white on black with an explicit dark
color scheme. The two are independent. All of this follows the window from
monitor to monitor, once a move has lasted a moment, as for the depth of the
display below, and programs with the mode on get an event. :opt:`eink_mode`
treats every display as e-paper, taking a display whose colors nothing declares
to show gray, and takes precedence over ``kind=...`` in the options. Runtime
changes with ``set_eink_mode`` notify display subscribers immediately. The
:opt:`display_info` option declares individual properties of every display,
overriding the rest, but only in what programs are told: using only
``display_info kind=eink`` changes the report without changing what Silkie
does. Nothing guesses whether an e-paper panel is grayscale or color. Silkie
reports ``kind`` on macOS for built-in displays, as ``lcd``, and for the
monitors it knows, and ``gamut`` on macOS and on Wayland compositors that
implement the color management protocol. There the compositor gives the
primaries of the display, and ``gamut`` is the widest of the three whose area
in the CIE 1931 chromaticity diagram the display covers at least nine tenths
of, ``srgb`` when it covers neither of the wider two. ``headroom`` is the
headroom of the :doc:`hdr-protocol`, after the cap of :opt:`hdr_max_headroom`,
so it is ``1`` unless :opt:`hdr` is enabled and the window is actually showing
HDR, which that page says when to expect on macOS and on Wayland.

For a known ten-bit RGB panel or a sixteen-level grayscale panel, configure::

    display_info color=rgb red_bits=10 green_bits=10 blue_bits=10

or::

    display_info color=gray gray_bits=4

For a black-and-white panel, use ``display_info color=mono``. Monochrome means
exactly one bit, so wider ``bits`` or ``gray_bits`` declarations are ignored;
use ``color=gray`` for more than two levels. The legacy ``bits=8`` declaration
sets all active channel widths; individual declarations take precedence
regardless of their order. Changing from RGB to grayscale clears detected RGB
precision rather than guessing the gray panel's depth.

macOS reports the screen's color-space model and bits per sample. X11 reports
the actual window visual's RGB masks or grayscale depth, including unequal
RGB channel widths. These are labeled ``precision=framebuffer`` and do not
detect a panel that accepts RGB input but displays only gray. Wayland does
not provide channel precision through the output and color-management
protocols currently used by Silkie, so it reports zero widths rather than
assuming eight bits. Declare physical panel properties explicitly when the
system cannot supply them. Applications may query the fields with::

    printf '\033]777;display;v=1:op=query:id=42\033\\'

Responses and subscribed events both carry the channel widths; moving to a
display with a different width triggers an event even if its other properties
are unchanged.

The ``output`` fields are what OpenGL says about the surface of the window
once it has been drawn, ``0`` and ``unknown`` before that, and are not
affected by :opt:`display_info`. On e-paper nothing is dithered, whatever
:opt:`output_dither` says, and ``output_dither`` is ``0``. Which surface a window gets is chosen with
:opt:`deep_color`, and whether and to which steps it is dithered with
:opt:`output_dither`. They change, with an event, when the window is moved to
a display of another depth: the steps dithered to follow the display, and on
macOS a window that has a floating point surface only for HDR, with
:opt:`deep_color` set to :code:`never`, switches between it and an 8 bit one. A move is acted on once it has lasted a moment, so a window dragged
across the boundary between two displays does not flip back and forth.

On macOS the system reports 8 bits per sample even for a MacBook Pro's 10 bit
XDR panel, so ``bits`` stays ``8`` there while the window has a floating point
surface, ``output_bits=16 : output_type=float``, and the system, not the
terminal, quantizes what it shows for the panel. A screen that supports
extended dynamic range is taken to be deep.

What is known about the depth of a display, and when, differs. macOS and X11
know it before a window exists: the screen a window opens on, and the depth of
the X screen, which is the same for all its monitors, so on X11 nothing
changes as a window moves. On Wayland a window is on no output until it is
shown, and the compositor says which output governs it, the one whose image
description it prefers for the window, only after that. Until then Silkie
goes by the image descriptions of all outputs, which color management gives
for every output from the start. No Wayland protocol gives the depth of an
SDR output: an output is known to be deep only when its description is HDR
(PQ, HLG or linear transfer, or a peak luminance above its reference white).
A Wayland window therefore gets a 10 bit surface in any case, which costs no
more than an 8 bit one, and is dithered to 8 bit steps until its output is
known to be deep.

``scale`` is the scale the window system gives the window: the backing scale
factor on macOS, the preferred scale of the surface on Wayland, fractional
where the compositor says so, and the scale of the desktop taken from
``Xft.dpi`` on X11. On X11, and on Wayland with a compositor that states its
fractional scale, the pixels Silkie draws are the pixels of the panel. On macOS
they are those of the framebuffer the system keeps for the display, which are
the panel's own at the default resolution. At one of the scaled resolutions
(*More Space* and its neighbours in the display settings) the system resamples
that framebuffer onto the panel, as a Wayland compositor does for a window it
could only give a whole number scale. A pixel Silkie draws is then not exactly
a pixel of the panel, which no application can avoid, and on macOS ``ppi``
counts the pixels Silkie draws.

``ppi`` is ``0`` for a display that does not say how large it is, as a
virtual one in a remote session or a virtual machine does not. Window systems
assume 96 or 72 to the inch for such a display, which is a guess and is not
passed on.
