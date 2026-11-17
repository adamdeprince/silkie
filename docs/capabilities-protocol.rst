Extension capabilities protocol
==================================

.. versionadded:: 0.50.0

A family of terminal extensions share one framing: ``OSC 777`` followed by a
selector naming the extension, a protocol version and ``key=value`` fields.
The tablet pointer, scroll, gesture and frame presentation protocols are
written this way. Each of them can be probed with its own ``query``, but a
program at the far end of a slow link pays a round trip per probe, and a
program resuming after a reconnection through a relay wants to know which of
the modes it had turned on are still in effect. This protocol answers both
with one exchange. It is an open terminal extension like the others: small,
unobtrusive, easy to implement, and no change to what the TTY is.

Framing
-------------

Messages are ``OSC 777`` with the ``caps`` selector::

    <OSC> 777 ; caps ; v=1 : op=query : id=42 <ST>

They follow the rules that every extension of the family shares, which
:doc:`extension-framing` gives: the syntax of the fields, which frames are
rejected, the units, the clock, and which reports may be merged under load.
``v``, the protocol version, is ``1``, and ``op`` is one of ``query``,
``cap`` and ``reset``. The terminal sends only replies, which are never
merged or dropped.

Listing the extensions
-------------------------

A program asks which selectors the terminal speaks::

    <OSC> 777 ; caps ; v=1 : op=query : id=42 <ST>

``id`` is a non-negative 31 bit integer of the program's choosing, echoed in
the reply so replies can be matched to queries. The terminal answers::

    <OSC> 777 ; caps ; v=1 : op=cap : id=42 : caps=3 : ext=caps.1,frame.1,gesture.1 : media=d,f,t,s : format=24,32,100 <ST>

``caps`` is a bit set of what the terminal does with this selector: ``1``
the listing, ``2`` the reset request described below. ``ext`` is a comma
separated list, in no particular order, of every selector the terminal
accepts, each followed by a period and the highest version of its protocol
the terminal speaks. ``caps.1`` is always among them. The list says only that
a selector is understood; what it can do with the display and the input
devices at hand is still learned from that selector's own ``query``, which a
program now sends only for the selectors it intends to use, and may send in
the same write as this query since replies arrive in order.

The answer is the same for every window of a terminal and does not change
while the terminal runs, so a program needs it once. A terminal that does not
support this protocol ignores the query, so a program must time out and may
then fall back to probing the selectors it needs one at a time.

Graphics media and formats
-----------------------------

The same reply says which transmission media and pixel formats of the
:doc:`graphics protocol <graphics-protocol>` the terminal accepts. ``media``
lists the values of the ``t`` key: ``d`` direct, ``f`` a file, ``t`` a
temporary file, ``s`` shared memory, and any medium an extension adds, such
as ``g`` for a GPU surface. ``format`` lists the values of the ``f`` key:
``24`` RGB, ``32`` RGBA, ``100`` PNG, ``8`` and ``16`` indexed color (where
supported), and any format an extension adds, such as ``64`` for HDR images, ``200`` for vector images, ``301``, ``302``,
``304`` and ``308`` grayscale and ``516`` RGB565 (see
:doc:`packed-image-protocol`), ``401``, ``402``, ``404`` and ``408``
palette images (see :doc:`palette-image-protocol`) and ``316`` 16-bit gray,
``632`` RGB10A2 and ``664`` 16-bit RGBA (see :doc:`precision-image-protocol`). Both describe what the
terminal accepts, not what is wise over a given connection, and the graphics
protocol's own query action (``a=q``) remains the check for one particular
image. A relay answers with what it can carry to the terminal behind it:
files, shared memory and GPU surfaces do not cross machines, so over a
network relay only ``d`` is available.

The accepted source formats are independent of the display's reported color
model and precision. A one-bit display can accept RGB images, and an RGB
display can accept packed one-bit pixels; :doc:`display-protocol` describes
output capabilities rather than restricting input.

Reading a mode
-----------------

Most selectors have a per-window mode that a program turns on with
``op=mode : mode=1`` and off with ``mode=0``. A ``mode`` request without a
``mode`` field is a read, answered with the current state::

    <OSC> 777 ; frame ; v=1 : op=mode <ST>
    <OSC> 777 ; frame ; v=1 : op=mode : mode=1 <ST>

and the ``cap`` reply of every selector that has a mode carries a ``mode``
field with the same value, so a single ``query`` tells a program both what the
selector offers and whether its mode is on. A terminal may let its user refuse
a mode, or some of its bits, for every program or for some; a refused bit
reads back as off and nothing it would have turned on is reported, so a
program reads a mode back after setting it rather than assume it was
granted. The reply uses the version number
the request asked for. A selector the terminal does not speak answers neither,
which is what the listing above is for.

Turning every mode off
-------------------------

Each selector asks a program to turn its mode off before it exits, suspends
or fails. A program that is killed does none of that, and what it leaves
behind is worse than a stale color: with the scroll mode on the wheel no
longer scrolls the scrollback, and with tool reports on a pen no longer
clicks. One request turns every mode of the window off::

    <OSC> 777 ; caps ; v=1 : op=reset <ST>

It has the effect of ``op=mode : mode=0`` sent to every selector that has a
mode, including whatever else turning that mode off does, such as the
``cancel`` reports for the contacts and gestures in progress. Selectors
without a mode are not affected, nothing that is not an extension mode is
touched, and there is no reply. It is what a shell, a wrapper script or a
supervisor sends on behalf of a program that has gone away, and a program may
send it when it starts, to begin from a known state. A terminal that honours
the request sets bit ``2`` of ``caps`` in the listing; one that does not
ignores it.

kitty also does this by itself whenever a shell marks the start of a prompt
with ``OSC 133 ; A``, as its shell integration does. A prompt means the shell
is reading input, so whatever program set the modes is no longer in charge of
the window, and without this a drawing program that crashed would leave the
user at a prompt they cannot scroll. When no mode is on, which is the usual
case, a prompt costs nothing and sends nothing. What follows from it:

* A program run by the shell is not affected, since the mark comes before the
  command starts. A program stopped with job control finds its modes off
  when it is resumed, which every selector already tells it to assume.
* A program that marks prompts of its own and uses a mode, a shell with a
  drawing widget, for instance, turns the mode on after it has marked the
  prompt, each time.

Relays and multiplexers
---------------------------

A multiplexer, or a relay that resynthesizes the screen rather than passing
bytes through, answers the listing itself with the intersection of the
selectors it can carry and the selectors the terminal behind it listed, so a
program never sees a selector that cannot reach the display. It keeps the
modes with the rest of the terminal state. A program that reconnects reads the
modes it depends on rather than assuming they survived, since the terminal at
the other end may have been reset with ``RIS`` in the meantime, and turns them
on again as needed.

A multiplexer applies the reset request to the pane it came from and then
works out again what it needs from the terminal behind it; it does not pass
the request on while another pane still has a mode on. The same goes for a
prompt mark: a multiplexer that forwards the marks of its panes to a terminal
that resets on them finds that terminal's modes off after a prompt in any
pane, so it either keeps the marks to itself or turns the modes back on. A
relay that passes bytes through passes the request through.
