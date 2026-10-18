OSC 777 extension framing
============================

.. versionadded:: 0.50.0

The terminal extensions of this fork, such as the :doc:`tablet-protocol`,
share one framing. This page is what they have in common: how a frame is
written and read, the units and the clock of what they report, which reports
may be merged when a link cannot keep up, and how a mode is turned on and
refused. The page of each extension names its selector and its fields and
says only what is particular to it.

Syntax
---------

A frame is an ``OSC 777`` escape code whose first field is the selector that
names the extension::

    <OSC> 777 ; <selector> ; <fields> <ST>

``<OSC>`` is ``<ESC> ]`` and ``<ST>`` is ``<ESC> \``; ``BEL`` is also
accepted as a terminator. The fields are ASCII ``key=value`` pairs separated
by colons, in any order. Keys are case sensitive. A number is a decimal
integer with no leading ``+``, and with a leading ``-`` only where the field
is signed. Spaces in the examples on these pages are for clarity only. Every
frame has ``v``, the version of the extension's protocol, and ``op``, which
says what the frame is.

Frames from a program
-------------------------

A frame from a program is rejected as a whole, and changes nothing, when it
has

* a version the terminal does not know,
* a key that appears more than once,
* a value out of the range its page gives, or a number that is not written
  as above,
* no value for a field its page requires.

Storage grows with the frame. There is no fixed field-length limit; a
configured ``escape_limit`` can bound an entire OSC sequence.

No frame from a program, whether it is rejected, incomplete or accepted,
ever reaches the program or anything else as keystrokes.

Growth
---------

A key the receiver does not know is ignored, whichever way the frame goes,
so fields can be added to an extension without a new version. A program
ignores a frame whose ``op`` it does not know, and a frame from the terminal
with a version it does not know. Only a change in the meaning of something
that exists needs a new version.

Units
--------

* A position is in 1/256 of a physical pixel, relative to the top left of
  the cell area, signed where it can fall outside it. A physical pixel is a
  pixel of the framebuffer the terminal draws into, never a logical unit of
  the system, which stands for several of them on a high density display.
  The ``Pixels`` section of the display protocol says more, where a terminal
  has that protocol.
* A length on a device, such as the size of a tablet or a touchpad, is in
  1/256 millimeter.
* An angle is in hundredths of a degree.
* A time is as described next.

Time
-------

Every report from the terminal about something that happened carries ``t``:
milliseconds on one monotonic clock that the terminal uses for every
extension and for every window. The ``t`` of a pen report and the ``t`` of a
frame report can therefore be subtracted, to learn how long a stroke took to
reach the screen. The origin of the clock is the terminal's choice and stays
fixed for as long as it runs, so ``t`` means nothing by itself. ``t`` is when
the system says the thing happened, or, where the system does not say, when
the terminal learnt of it. ``tu``, where it is present, is microseconds past
``t``, ``0`` to ``999``. It is sent only where the system stamps what
happened more finely than a millisecond, and a report without it is on a
grid of whole milliseconds.

In kitty the clock is the one kitty measures all its time on, which starts
when kitty starts.

Delivery
-----------

A terminal sends reports in the order the things they report happened, and
never drops one. A terminal, or a relay between it and the program, that
cannot keep up, because the program is not reading or a link is slow, may
merge reports, and only these:

* consecutive ``move`` or ``hover`` reports of one contact of the tablet
  pointer protocol, with no other report of that contact between them and
  the same ``buttons``. The merged report has the absolute values of the
  last of them and the sum of the relative ones, ``wheel`` and ``wclicks``.
  The frame reports (phase ``6``) of groups that were merged are merged with
  them.
* consecutive ``update`` reports of one gesture, with ``scale`` multiplied
  and rotation and translation summed.
* consecutive scroll reports of one device in one phase, with the distances
  summed.

A merged report carries the ``t`` of the last report merged into it.
Nothing else is ever merged or dropped: not a ``down``, an ``up``, a
``cancel`` or an ``out``, not a change of buttons, not the description of a
device, not a reply or a report of a mode, not the beginning or the end of a
gesture, not a change of scroll phase, and no report of the frame
presentation protocol. The page of each extension says which of its reports
may be merged.

Silkie itself merges nothing. What it writes to a program waits in memory
until the program reads it, up to 100 MiB for each window. A write that
would take that past 100 MiB is dropped whole, and Silkie logs an error
saying so; a program that has left that much unread has stopped reading.

Modes and consent
--------------------

An extension that reports anything does so only after a program turns it on
with a mode, for one terminal window. The mode is reset by ``RIS``. A
program turns it off when it exits, suspends or fails, and on again when it
is resumed. A ``mode`` request without a value reads the mode back.

A terminal may let its user refuse a mode, or some of its bits, for every
program or for some. A refused bit reads back as off, and nothing it would
have turned on is reported, so a program reads the mode back rather than
assume that what it asked for was granted. A terminal also keeps only the
bits it knows, which a program learns in the same way.

In Silkie the user can refuse the hardware identifiers of the tablet pointer
protocol, and does so by default, see :opt:`tablet_hardware_ids`.
