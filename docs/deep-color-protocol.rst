Deep colors
==============

.. versionadded:: 0.50.0

Terminal colors have eight bits per channel. That is enough to pick a color,
but not to draw a smooth gradient across a wide window, to place colors
precisely in a wide gamut, where each step is coarser, or to grade brightness
finely above SDR white with the :doc:`hdr-protocol`. This extension lets a
program give the color of text, its background and its underline with sixteen
bits per channel. It is a new form of the SGR color attributes that other
terminals skip, so nothing changes for programs that do not use it.

The escape code
------------------

::

    <ESC>[38:0:<id>:<r>:<g>:<b>m      foreground
    <ESC>[48:0:<id>:<r>:<g>:<b>m      background
    <ESC>[58:0:<id>:<r>:<g>:<b>m      underline and strikethrough

``r``, ``g`` and ``b`` are 0 to 65535, encoded with the sRGB transfer curve
like the components of every other terminal color, so ``65535`` is SDR white,
the brightest value of an ordinary color, and ``257`` times an eight bit value
is that value exactly. ``id`` is the gamut of the :doc:`wide-gamut-protocol`:
``0`` for sRGB, ``1`` for Display P3 and ``2`` for Rec. 2020, any other id
being ``0``. It is required: write ``0`` for sRGB, never leave it empty, since
some parsers, Silkie's among them, drop empty sub-parameters and would see too
few. A group with fewer than four values after the ``0``, or with a component
above 65535, is malformed and changes nothing. The parameters after it in the
same sequence still apply.

The ``0`` is the color space type of ITU-T T.416, the standard the
``38:2`` and ``38:5`` forms come from, which defines ``0`` as *implementation
defined*, ``1`` transparent, ``2`` RGB, ``3`` CMY, ``4`` CMYK and ``5``
indexed. Only the colon form exists. In the semicolon form the parameters after
the type are unrelated attributes, and terminals disagree on how many of them
an unknown type consumes, so ``<ESC>[38;0;...m`` is not this extension. Silkie
ignores such a sequence as it ignores any SGR with an unknown color type.

A deep color is a color like any other. It replaces the color it is given
for, and ``39``, ``49``, ``59``, ``0`` or any other color of the same kind
replaces it. Its gamut belongs to it: selecting an ordinary color selects that
color's gamut again. It combines with the brightness of ``56`` and ``57`` of
the :doc:`hdr-protocol`, with the cell background alpha of
:doc:`cell-alpha-protocol` and with reverse video, under which it follows the
color it was given for, as the gamut does. Unlike ``58:2``, whose id is
ignored, a deep underline color keeps its gamut. A deep background is never the
default background, so it is opaque even when it has the value of the default
background in a window with :opt:`background_opacity`, and it is never one of
the :opt:`transparent_background_colors`.

Being understood elsewhere
-----------------------------

What other terminals do with an unknown color type decided the form. Each of
them was checked in its source (in November 2026: xterm 412, the main branches
of VTE, foot, WezTerm, Alacritty's vte crate 0.15, iTerm2, Windows Terminal,
Ghostty, contour, tmux and mintty, and kitty 0.47):

* xterm, VTE, foot, Alacritty, iTerm2, Windows Terminal, Ghostty, tmux and
  mintty ignore the group ``38:0:...`` and go on with the next parameter.
  xterm does not support ``58`` at all and skips the group in the same way.
* kitty and WezTerm ignore the group and every parameter after it in the same
  sequence.
* contour resets the color and runs the components as SGR attributes of their
  own.
* In the semicolon form, xterm, VTE, Alacritty, Windows Terminal, tmux and
  contour consume the type, foot, iTerm2, Ghostty and mintty run it as an
  attribute, and kitty and WezTerm drop the rest of the sequence.

The other forms considered were worse. A component above 255 in ``38:2`` is
masked to eight bits, giving a wrong color, by foot, iTerm2, kitty, Ghostty and
mintty, and rejected by the others. Carrying the low bits in sub-parameters
added after ``38:2:<id>:<r>:<g>:<b>`` fails in VTE, which rejects ``38:2``
unless the id is empty, in kitty and iTerm2, which drop an empty id and then
read the wrong fields, in Windows Terminal, which rejects a non-zero id, and in
tmux, which ignores a group of eight or more fields, besides overwriting the
tolerance fields T.416 puts there. A fresh type number avoids all that, but
``6`` is WezTerm's eight bit RGBA, read there as a wrong color, ``12`` and
``13`` are iTerm2's, ``3`` and ``4`` are mintty's CMY and CMYK, and Ghostty
reads the type as an SGR attribute when it is ``4``, ``38``, ``48`` or ``58``.
``0`` is the value T.416 sets aside for what an implementation chooses, and
none of these terminals gives it a meaning.

Hence how a program should send it:

* Send the deep color in a sequence of its own, after the ordinary color it
  refines, which every terminal understands::

      <ESC>[38:2:1:255:128:0m<ESC>[38:0:1:65535:32768:1m

  A terminal that drops the rest of a sequence then loses nothing, and a
  terminal that limits the number of parameters in a sequence (Ghostty 24,
  contour 16, iTerm2 16 sub-parameters, tmux 64 bytes) is not pushed over its
  limit. Everywhere but contour the program gets the eight bit color or the
  deep one and never a wrong one. Where the eight bit color is the nearest
  one, ``(c * 255 + 32767) / 65535`` for each component ``c``, the two are
  the same color to within half a step of eight bits.
* The group has six fields, which keeps it within the six sub-parameters
  Windows Terminal stores for a parameter and below the eight at which tmux
  ignores a group.
* contour cannot be made safe by any colon form. A program that cares asks
  first, see below, and only sends deep colors to a terminal that lists them.

Asking for it
---------------

A terminal that implements this extension lists ``deep`` in the ``sgr`` field
of the reply to the query of the :doc:`capabilities-protocol`::

    <OSC> 777 ; caps ; v=1 : op=query : id=1 <ST>
    <OSC> 777 ; caps ; v=1 : op=cap : id=1 : ... : sgr=alpha,deep,gamut,hdr : ... <ST>

Whether the display shows more than eight bits is a separate question, which
the ``bits`` of the ``display`` query of the :doc:`display-protocol` answers. A
terminal is free to accept deep colors and show them at the precision of its
display, as it may accept Display P3 colors on an sRGB display.

In Silkie
------------

The color word of a cell has room for an eight bit color and no more, and
making it wider would grow every cell of every window and its scrollback for
the few that use deep colors. A deep color is therefore interned in a table
that each screen has, and the cell holds its index in that table. Equal colors
share an entry. The table grows as colors are added and is garbage collected:
every few thousand new colors, and never more often than the number of colors
in use, the colors that no cell, cursor or saved cursor refers to any more,
those of lines that scrolled out of the scrollback for instance, are freed.
Cells that use ordinary colors are stored, drawn and copied exactly as before.

The table can hold sixteen million colors at a time, which the 24 bits of the
cell's color word allow, or fewer if the GPU cannot make a texture that large
(4096 times its largest texture size). When it is full of colors still in use,
or memory runs out, a new deep color is stored as the nearest eight bit color
with its gamut instead, which is what a cell can hold by itself.

Deep colors survive everything colors survive: the scrollback, resizing and
rewrapping, alternate screens, saved cursors, ``DECCARA`` and copying with
formatting. Text copied with formatting, and the scrollback given to the pager,
write each deep color as its nearest eight bit color and then the deep form in
a sequence of its own, as recommended above. A ``DECRQSS`` report of the SGR
state is a single list of parameters and has only the deep form.

The cell shader reads the table from a sixteen bit integer texture, so a deep
color reaches the blending of the cell, its conversion to the gamut of the
window and the brightness of the :doc:`hdr-protocol` at full precision, in
floating point. Whether all of it reaches the display depends on the window:
an eight bit window, which is the default, ends at eight bits. The few cell
colors Silkie works out on the CPU rather than in the shader, such as the
colors of the window edges, use the nearest eight bit color.
