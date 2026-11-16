Window actions
===============

Split a pane vertically
-----------------------

Choose **Shell → Split Pane Vertically** to open a new pane to the right of
the active pane. Silkie switches the tab to the splits layout when needed,
enabling that layout for the tab if necessary. Repeated splits divide the
active pane without resetting the existing split arrangement.

To bind the action in ``silkie.conf``::

    map ctrl+shift+d split_pane_vertically

For inherited layout behavior, see `kitty's layouts documentation
<https://sw.kovidgoyal.net/kitty/layouts/>`_.

Kill an unresponsive window
---------------------------

Silkie adds the ``kill_window`` action. It sends ``SIGKILL`` to the foreground
process group and to the shell, then closes the active terminal window. Use it
when a program no longer responds; the killed programs cannot save their state.

To bind the action in ``silkie.conf``::

    map kitty_mod+k kill_window

On macOS, the Shell menu also provides **Kill Window**. For the mapping syntax
and inherited actions, see `kitty's keyboard mapping documentation
<https://sw.kovidgoyal.net/kitty/mapping/>`_.
