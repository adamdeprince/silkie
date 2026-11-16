Desktop notification routing
============================

Silkie inherits kitty's `OSC 99 desktop notification protocol
<https://sw.kovidgoyal.net/kitty/desktop-notifications/>`_. Applications can
request a notification, ask to focus their terminal when it is activated,
and receive the notification ID and selected button. The `notify kitten
<https://sw.kovidgoyal.net/kitty/kittens/notify/>`_ provides a command-line sender.

OSC 9 and AI completion alerts
------------------------------

Silkie also accepts the iTerm2-style ``OSC 9 ; message ST`` notification,
terminated by either BEL or ``ESC \\``. The entire message, including any
semicolons, becomes the notification title. Clicking it focuses the originating
terminal without sending input to the application. See the `iTerm2 escape-code
manual <https://iterm2.com/documentation-escape-codes.html>`_ for the wire format.

The ``OSC 9 ; 4 ; state ; percent ST`` form reports progress instead. Clearing
or completing a progress bar does not request a desktop notification. The
abbreviated ``OSC 9 ; 4 ST`` form clears progress as well.

To test desktop notification delivery, run this in a Silkie shell::

   printf '\033]9;Silkie notification test\007'

On macOS, allow Silkie notifications when prompted. If no banner appears,
check System Settings → Notifications → Silkie, banner settings, and Focus.
Notification permission failures are logged to Silkie's standard error.
Configured notification filters also apply to OSC 9.

Claude Code must emit a notification sequence for Silkie to display one.
If automatic terminal detection does not select a compatible protocol, set
``"preferredNotifChannel": "iterm2"`` for OSC 9 or ``"kitty"`` for OSC 99 in
``~/.claude/settings.json``. See `Claude Code's terminal notification setup
<https://code.claude.com/docs/en/terminal-config#get-a-terminal-bell-or-notification>`_
for notification timing and multiplexer configuration.

Return paths through multiplexers
---------------------------------

Silkie adds optional ``r`` metadata to OSC 99. It identifies the windows a
notification crossed, allowing a click to restore the originating terminal
and nested multiplexer windows. This field does not change the notification
title or body.

Each hop is ``component/instance/window``. Component and instance are nonempty
ASCII tokens containing letters, digits, underscores, or hyphens; window is
a decimal integer. Commas separate hops, listed from the originating application
outward. Neither delimiter needs base64 encoding::

   i=foo:r=odin-seat/sessionA/3,odin-seat/sessionB/8

A multiplexer appends its own hop to every outgoing fragment. On activation,
Silkie preserves that route and appends ``silkie/<instance>/<window>``; its
instance token identifies this notification manager and window is the native
terminal channel ID. For example::

   ESC ] 99 ; i=foo:r=odin-seat/sessionA/3,odin-seat/sessionB/8,silkie/terminalC/12:a=focus,report ; 2 ST

The reply identifies button 2. ``a`` contains the effective requested actions,
including actions inherited across multipart notifications. An empty ``a=``
means no actions. A route causes an activation reply even when ``report`` was
not requested, so multiplexers can restore windows for ``focus``. Each hop
focuses its own window only when ``focus`` is present; the final multiplexer
delivers the reply to the application only when ``report`` was requested.
Report-only notifications do not take focus.

Silkie first focuses its native terminal when requested. Multiplexers then
restore windows from outside inward and preserve the complete route in the
application's reply. IDs are scoped by native channel and route, so two
windows may use the same application ID. Multipart fragments must repeat the
same ID and route. Close and capability replies echo the route; a capability
reply advertises ``r=1`` in its payload. Requests without ``r`` retain kitty's
reply format.

Odin Seat convention
--------------------

Odin Seat appends ``odin-seat/<session-token>/<window-id>``. The random session
token survives a live server upgrade; window IDs remain stable within that
session. A closed window or stale session is never replaced by the currently
focused window when routing a click.

For hosts that do not echo ``r``, Odin Seat also wraps the notification's
``i`` in an opaque address. Nested multiplexers unwrap one address at a time;
the original application ID and full return path reach the caller. No pending
notification table or fixed ID length is required. Treat the wrapped ID as
opaque rather than constructing one in an application.

Odin Seat converts OSC 9 and OSC 777 ``notify;title;body`` to OSC 99 with focus
and report actions. It consumes the final click reply for these senders, which
did not request an input protocol. An OSC 99 caller can request ``report`` to
receive its original ID, selected button, and full route. Older hosts can focus
converted notifications through the wrapped ID, but need this Silkie extension
to return the full route and requested actions.

Codex configuration
--------------------

For a Codex version supporting these settings, place this in
``~/.codex/config.toml`` and start a new Codex session::

   [tui]
   notifications = true
   notification_method = "osc9"
   notification_condition = "unfocused"

Odin Seat forwards DECSET 1004 focus reports to applications. A pane is focused
when it is Odin Seat's active visible window and an attached host terminal is
focused. Switching panes, detaching, and returning through a notification update
that state, including through nested Odin Seat sessions.
