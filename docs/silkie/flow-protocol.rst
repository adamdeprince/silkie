Writing direction and reading order
===================================

A pane has two writing directions. The inline direction advances one logical
column. The line direction advances one logical row. Programs choose both with
OSC 777. Glyphs and images stay upright.

Set :opt:`text_flow` in ``silkie.conf``, for example ``text_flow down left``.
Programs can override it for the duration of an interaction.

For example, this shell command writes two columns of Chinese. The first
starts at the top right. The second appears immediately to its left::

    printf '\033]777;flow;v=1:op=set:inline=down:line=left\033\\\033[H春夏\r\n秋冬'

The logical home position is row 1, column 1. Changing direction keeps the
cursor with the content, so the example explicitly homes the cursor. At the
last logical line, another line feed scrolls the contents toward the first
line. Scrollback retains the logical lines. Switching between horizontal and
vertical writing reflows the pane to its new dimensions.

.. list-table::
   :header-rows: 1

   * - Inline
     - Next line
     - Home
     - Old lines scroll off
   * - right
     - down
     - top left
     - top
   * - left
     - down
     - top right
     - top
   * - right
     - up
     - bottom left
     - bottom
   * - left
     - up
     - bottom right
     - bottom
   * - down
     - right
     - top left
     - left
   * - up
     - right
     - bottom left
     - left
   * - down
     - left
     - top right
     - right
   * - up
     - left
     - bottom right
     - right

Coordinates
-----------

Cursor addressing, wrapping, margins, selection, image placements and mouse
cell reports use the logical grid. A carriage return goes to its first
column. Programs can query the dimensions with CSI 18 t or the PTY window
size. Wide characters still occupy two inline cells. A placement's columns
span the inline axis and its rows span the line axis. Image source rectangles
use the original image's pixel coordinates.

Pane-relative pixel positions in mouse, scroll, gesture, tablet tool and
direct-touch reports use the writing axes. Device-surface coordinates,
including indirect touch positions, describe the hardware. A captured
contact is cancelled in its old coordinates before the axes change.

Reading order
-------------

An application can direct screen reader navigation independently of writing
direction. Set the direction **before** writing the character whose successor
it describes. For example, write a label above its value and have the reader
continue down from the last character of the label::

    printf '\033]777;flow;v=1:op=reading:next=down\033\\X'
    printf '\033]777;flow;v=1:op=reading:next=auto\033\\'

The hint stays active for subsequent writes. Each character retains its hint
through scrolling and reflow. Overwriting the character replaces its hint.
Use ``right``, ``left``, ``down``, or ``up`` for a physical direction and
``auto`` for ordinary logical order.

Use ``next=end`` before writing the last character to include it and end
the pane’s accessible text there. For example, the following displays
``Done. Extra`` and exposes ``Done.`` to the screen reader::

    printf 'Done\033]777;flow;v=1:op=reading:next=end\033\\.'
    printf '\033]777;flow;v=1:op=reading:next=auto\033\\ Extra'

The reader starts at chain heads in logical order, then visits any remaining
cycles. A link to a blank or past the viewport edge ends that chain. The
reader tracks every character position it has emitted during this read.
If a link points to any of those positions, reading stops immediately,
before repeating the character or starting another chain. Direct speech
requests continue to speak the supplied string.

Protocol
--------

Discover support through the entry ``flow.1`` in :doc:`capabilities-protocol`.
Frames follow :doc:`extension-framing`. The default is ``inline=right``,
``line=down``, ``next=auto``.

* ``op=set:inline=DIRECTION:line=DIRECTION`` sets both writing axes. They
  must be perpendicular.
* ``op=reading:next=DIRECTION`` changes the hint for subsequent writes.
* ``op=query:id=N`` returns the current settings. Either setter also accepts
  an optional request ID and replies with the resulting settings.

A state reply has the following form::

    <OSC> 777 ; flow ; v=1:op=state:id=42:inline=down:line=left:next=auto <ST>

A hard reset, the shared extension reset, or a shell integration prompt mark
restores the configured writing directions and automatic reading order. Existing characters retain their individual reading
hints until overwritten or erased.
