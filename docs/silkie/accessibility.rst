Accessibility
=============

Navigate terminal text
----------------------

VoiceOver on macOS and Orca on Linux can navigate each visible pane as
text. Silkie exposes the viewport, the terminal caret, and the selection.
Moving keyboard focus to another pane also updates screen reader focus.
Use the terminal's scroll controls to bring earlier output into the viewport.
Silkie uses `AppKit text navigation
<https://developer.apple.com/documentation/appkit/nsaccessibilitynavigablestatictext>`_
and `AT-SPI Text
<https://gnome.pages.gitlab.gnome.org/at-spi2-core/devel-docs/doc-org.a11y.atspi.Text.html>`_.

Programs can use the :doc:`display-protocol` to read the current high
contrast and color-independent rendering hints. Set :opt:`high_contrast`
or :opt:`differentiate_without_color` to ``auto``, ``off``, or ``on`` in
``silkie.conf``.

See :doc:`flow-protocol` for per-character reading order and vertical writing.

Magnify a pane
--------------

Use **View → Magnify Pane**, or press ``Ctrl+Alt+=`` to zoom in,
``Ctrl+Alt+−`` to zoom out, and ``Ctrl+Alt+0`` to return to actual size.
``Ctrl+Alt+Arrow`` pans a magnified pane and ``Ctrl+Alt+C`` centers it on
the text cursor. On a touchscreen that delivers direct touch events, spread
two fingers to zoom and move both fingers together to pan. When a pane is
already magnified, a trackpad pinch zooms around the pointer.

Magnification preserves the program's rows, columns, and text layout. Silkie
rerasterizes glyphs and vector graphics at the requested resolution, subject
to renderer and hardware limits.

By default, once a raster image becomes visible during zoom, Silkie reports
pixel dimensions at the next whole-number scale, capped at three times
the base size. It reports 2× for zoom above 1× through 2×, and 3× above
2×. At actual size it reports the base resolution. Silkie updates the
PTY pixel dimensions when the reported scale changes, which
delivers a ``SIGWINCH`` on systems that signal pixel-size changes, and sends
an in-band resize report to applications that enabled one. A program can
query the reported cell size with ``CSI 16 t`` and send an image at that
pixel density while keeping the same placement in cells. Silkie scales the
available image locally to the selected zoom level.

To save bandwidth, keep the reported pixel dimensions at their base values
by setting :opt:`magnify_raster_resolution` in ``silkie.conf``::

    magnify_raster_resolution no

Bind ``magnify_pane`` in ``silkie.conf`` to customize the controls. A numeric
argument sets the magnification. Optional one-based row and column arguments
set the center in the visible terminal grid. For example::

    map kitty_mod+f11 magnify_pane 3 12 40
    map kitty_mod+f12 magnify_pane reset

Silkie also samples desktop zoom on its existing display-monitoring wakeup.
It reads GNOME Zoom's settings, queries KWin versions that report the target
zoom, and checks whether macOS Accessibility Zoom is active. The macOS zoom
factor is available on a best-effort basis. With desktop zoom, final
sharpness depends on how the compositor scales the window. Android tablets
running Silkie in an X11 or Wayland session can use the pane magnifier. The
Android system magnifier belongs to the Android host and its factor is
available to Android accessibility services.

Review terminal images
----------------------

Choose **View → Review Images** or press ``kitty_mod+alt+i`` to read the
images in the active pane's viewport. The review uses a selectable,
read-only text view on macOS or with the Linux GTK backend. When the
native backend is unavailable, Silkie uses the configured scrollback pager.
Close the review to return keyboard focus to the pane.

The review lists images from top to bottom and left to right. It includes
essential, informative, and unclassified images. Each entry identifies its
purpose and location and includes the name, description, and details the
program supplied. To include decorative images, choose **Review All Images**.
Scroll the pane first to review images further back in its output.

Both actions can be bound in ``silkie.conf``::

    map kitty_mod+alt+i review_images
    map kitty_mod+alt+o review_all_images

Describe image placements
-------------------------

Any program using the `kitty graphics protocol
<https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_ can annotate a
placement. This works with raster graphics and :doc:`vector-protocol`,
including SVG. Give the image and its placement nonzero ``i`` and ``p`` IDs
and send the annotation after placing the image.

Discover support by sending the :doc:`capabilities-protocol` query and
looking for ``a11y.1`` in ``ext``. The selector uses :doc:`extension-framing`.

::

    <OSC> 777 ; a11y ; v=1 : op=set-image : i=17 : p=3 : purpose=essential : name=Latency%20by%20request%20rate <ST>

``set-image`` replaces the annotation as a unit. ``clear-image`` takes the
same placement IDs and removes the annotation. Omitting a descriptive field
in ``set-image`` clears its previous value.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Field
     - Value
   * - ``i``
     - Image ID, required. An unsigned 32-bit integer greater than zero.
   * - ``p``
     - Placement ID, required. An unsigned 32-bit integer greater than zero.
   * - ``purpose``
     - ``essential`` for an image that carries the subject or result,
       ``informative`` for supporting material, or ``decorative`` for visual
       ornament. Omit the field to leave the purpose unclassified.
   * - ``name``
     - Optional short, single-line name.
   * - ``description``
     - Optional summary of what the image communicates.
   * - ``details``
     - Optional longer plain-text explanation, data table, or reference URL.
       The review presents URLs as text that the user can copy.
   * - ``id``
     - Optional reply correlation ID from zero to 2147483647.

Percent-encode the UTF-8 bytes of all three text fields, especially colons,
percent signs, and non-ASCII bytes. Percent escapes take two hexadecimal
digits. Invalid escapes, invalid UTF-8, and control characters reject the
entire update. A description or details may include tabs and line breaks
encoded as ``%09`` and ``%0A``.

When the request includes ``id``, a successful update replies with ``OK``.
A missing image or placement replies with ``ENOENT``. An allocation failure
can reply with ``ENOMEM``. Malformed requests receive no reply.

::

    <OSC> 777 ; a11y ; v=1 : op=ack : id=42 : status=OK <ST>
    <OSC> 777 ; a11y ; v=1 : op=clear-image : i=17 : p=3 : id=43 <ST>

An annotation belongs to the active screen and follows its placement into
scrollback. Moving an existing placement keeps the annotation. Deleting a
placement or replacing the image data removes it along with the placement.
Virtual placements describe the visible runs of their Unicode placeholders.
The review shows one entry per visible virtual placement.

Label text and make announcements
---------------------------------

Look for ``a11y.2`` in the :doc:`capabilities-protocol` reply to discover
text labels and announcements. Image operations also accept version 2.

After drawing text, send ``set-text`` to label a range of terminal cells::

    <OSC> 777 ; a11y ; v=2 : op=set-text : range=7 : row=2 : column=1 : end_row=2 : end_column=4 : role=prompt : label=Search <ST>

``range`` is a nonzero unsigned 32-bit ID local to the pane. The start is
inclusive and the end is exclusive. Rows and columns are one-based
coordinates on the active terminal screen, independent of user scrollback
position. The end column may be one past the last column. ``role`` is
``prompt``, ``status``, ``completion``, or ``decorative``. An optional
percent-encoded UTF-8 ``label`` replaces the text exposed to screen readers.
Decorative ranges are omitted from accessible text.

A new range replaces any overlapping ranges. An annotation expires when
its cells change, the pane is resized, or it switches terminal screens.
Use ``clear-text`` with the same ``range`` ID to remove it explicitly.

Send ``announce`` when an event occurs::

    <OSC> 777 ; a11y ; v=2 : op=announce : text=Search%20failed : priority=assertive <ST>

``text`` is percent-encoded, nonempty, single-line UTF-8. The default
``priority=polite`` requests a normal announcement. ``assertive`` requests
an interruption. Each command requests one announcement. Repainting the
pane or updating a text label never requests it again.

An optional ``id`` requests a version-2 acknowledgment. Statuses are ``OK``,
``EINVAL`` for coordinates outside the grid, ``ENOENT`` for an unknown
range ID, ``ENOTSUP`` when native announcements are unavailable, and
``ENOMEM`` if allocating text fails. Malformed requests receive no reply.

Speech over the terminal
------------------------

Programs can queue speech with per-utterance voice settings. Silkie uses
`AVSpeechSynthesizer <https://developer.apple.com/documentation/AVFAudio/AVSpeechSynthesizer>`_
on macOS and `Speech Dispatcher <https://github.com/brailcom/speechd>`_ on
Linux. The interface supports queued and interruptible speech, character
echo, and reusable voice styles as in
`Emacspeak Voice Lock <https://tvraman.github.io/emacspeak/manual/Voice-Lock.html>`_.

Look for ``speech.1`` in the :doc:`capabilities-protocol` reply and query
the terminal host for its backend::

    <OSC> 777 ; speech ; v=1 : op=query : id=17 <ST>
    <OSC> 777 ; speech ; v=1 : op=cap : id=17 : available=1 : engine=avspeech : pitch_range=0 <ST>

``available=1`` indicates that a native speech backend is installed.
``engine`` is ``avspeech``, ``speech_dispatcher``, or ``none``.
``pitch_range=1`` means the backend accepts pitch-range settings.
The protocol uses :doc:`extension-framing`. Percent-encode text and names
as with the accessibility protocol above.

To speak a styled sequence, define styles and queue text using them::

    <OSC> 777 ; speech ; v=1 : op=set-style : style=comment : pitch=-25 : rate=-10 <ST>
    <OSC> 777 ; speech ; v=1 : op=queue : text=Line%2017 <ST>
    <OSC> 777 ; speech ; v=1 : op=queue : style=comment : text=Save%20the%20buffer <ST>
    <OSC> 777 ; speech ; v=1 : op=dispatch <ST>

``speak`` adds text to playback without a separate dispatch. ``character``
takes a single Unicode character for key echo. Set ``interrupt=1`` to
clear the pane's speech before adding new output. ``sound`` takes a native
sound ``name`` and ``silence`` takes a ``duration`` in milliseconds.
Use ``queued=1`` with either to include it in a later dispatch.

``stop`` interrupts playback and clears the pane's queues. ``pause`` and
``resume`` control playback. ``reset`` also clears the pane's styles.
``clear-style`` removes the named style. A style can set any subset of
``voice``, ``language``, ``rate``, ``pitch``, ``pitch_range``, ``volume``,
``punctuation``, and ``split_caps``. An individual utterance can override
those fields. Rate, pitch, and pitch range are from −100 to 100 with zero
neutral. Volume is from 0 to 100. Punctuation is ``none``, ``some`` or
``all``. ``split_caps=1`` separates words in mixed-case identifiers.
Voice identifiers and speech rendering depend on the installed synthesizer.
An optional ``id`` requests an acknowledgment of the command.

SVG example
-----------

This Python program places a small SVG plot and describes its result. Its
standard output should go to a Silkie terminal with SVG and image annotation
support::

    from base64 import b64encode
    from urllib.parse import quote

    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 48">'
        '<path d="M4 42 H116 M4 42 V4" stroke="#879581"/>'
        '<path d="M4 34 L78 33 L112 7" fill="none" stroke="#77bb42" stroke-width="3"/>'
        '</svg>'
    )
    print(f'\x1b_Ga=T,f=201,i=17,p=3;{b64encode(svg.encode()).decode()}\x1b\\', end='')
    name = quote('Latency by request rate', safe='')
    summary = quote('Latency stays below 20 ms through 800 requests per second, then rises sharply.', safe='')
    print(f'\x1b]777;a11y;v=1:op=set-image:i=17:p=3:purpose=essential:name={name}:description={summary}\x1b\\', end='', flush=True)

The application supplies the descriptions in the review. A future
on-demand interpretation action could use the placement ID to identify the
image the user selects.
