Checking colour management without a display
===============================================

These scripts run kitty inside a headless Mutter, which implements the
``color-management-v1`` protocol and can be switched into HDR, and inside a
headless sway, which (at 1.11) does not have the protocol at all. They are how
the display gamut, the wide gamut window and HDR on Wayland were checked. No
picture is ever seen: what is checked is the conversation with the compositor
(``WAYLAND_DEBUG=1``), what kitty reports, and pixels read back with
``kitten @ screenshot``.

Everything lives in a private runtime directory, ``/tmp/silkie-color-run``,
with its own session bus, and nothing is read from or written to the user's
settings. Never stop a compositor by name: other sessions, including a real
desktop, may be running the same programs.

Starting and stopping::

    mkdir -p -m 700 /tmp/silkie-color-run
    (setsid nohup rig/color/start-mutter.sh > /tmp/silkie-color-run/mutter.log 2>&1 &
     echo $! > /tmp/silkie-color-run/mutter.pid)
    rig/color/force-hdr.sh true        # the virtual monitor becomes BT.2020 PQ
    rig/color/force-hdr.sh false
    rig/color/stop-mutter.sh           # stops only that session

Running kitty in it for some seconds, with whatever options are under test::

    WAYLAND_DEBUG=1 rig/color/run-kitty.sh /path/to/built/tree 10 \
        -o wayland_colorspace=displayp3 --debug-rendering python3 some-client.py

``start-sway.sh`` and ``run-kitty-sway.sh`` do the same for sway.

kitty exits with status 0 even after a protocol error, which is fatal to its
connection, so a run is checked by looking for ``wl_display#1.error`` in the
log, not by its exit status.

The rest:

``display-client.py``
    Runs inside the terminal and logs the replies of the display protocol.

``gradient-card.py``
    Runs inside the terminal: slow, dark ramps as 16 bit float images beside
    8 bit ones, to judge banding, deep color and dithering by eye on a real
    display. ``--help`` says what to compare.

``hdr-client.py``
    Runs inside the terminal: asks for the HDR headroom, prints text at
    ``SGR 56:400`` and shows an ``f=64`` image with values above 1.0.

``color-harness.py``
    Patches a scratch copy of a tree so that a series of colour descriptions
    is set, chosen with ``SILKIE_COLOR_TEST``, and what the compositor made of
    each is printed. For checking the plumbing of the ``wayland-color`` branch.

``hdr-harness.py``
    Patches a scratch copy of a tree so that, with ``SILKIE_HDR_TEST`` set, an
    ordinary 8 bit window is treated as an HDR one. No driver on the machine
    this was written on offers floating point window surfaces, so this is the
    only way everything around the surface could be exercised there. The
    picture is wrong on purpose.

``eglprobe.c``
    Lists the EGL window configurations a Wayland compositor and driver offer,
    floating point ones included.

The two harnesses change source text they expect to find, so they need
updating when that code changes; they refuse to run when it has. Neither is
ever applied to a tree that is committed.
