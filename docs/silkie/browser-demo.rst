Try Silkie in your browser
===========================

.. container:: hero-lead

   A real Linux shell, running locally in this tab. Try basic commands,
   play Presidential, Silkie Loophole, Ground Stop, Sprint, and Scramble, or connect to your own machines
   over SSH. The terminal starts automatically.

   Click in the terminal to type. Drag to select text, then copy it as usual.
   Use **Fullscreen** for more space and a slightly larger font.
   The game links below the terminal open a game filling its own window, ready to play.
   Use **Fullscreen** there to hide the browser controls too.

   :doc:`Install Silkie <install>` for the native terminal, or explore the
   :doc:`extension reference <reference>`.

.. include:: _includes/browser-terminal.rst

Try these commands
--------------------

.. code-block:: sh

   ls
   printf 'Hello from Silkie!\n'
   silkie-features
   silkie-sprint
   silkie-scramble
   presidential
   silkie-loophole
   ground-stop

Click inside the terminal to type. Sprint and Scramble use arrows or WASD;
press P to pause, then Q to quit. In Loophole, walk with arrows or WASD,
press Space to interact, and ? for help. In Ground Stop, click an aircraft,
steer it with WASD, Z, and X, and press Enter to land it; press Q twice to quit.
Presidential opens a campaign menu;
use F1–F9 to explore the panels and ? for help. The homepage shell and each game window can
run together, with separate files and saves in this browser's local storage.
Clicking a game link again returns to its existing window. Keep one window
per game to protect its filesystem. Browser storage quotas still apply.
``silkie-features`` formats its report for the current terminal width,
including after resizing or switching to fullscreen.

Connect to your machines
-------------------------

Choose **Connect to Tailscale** inside the terminal and follow the login link
to join your own network. Then use ``ssh your-user@your-tailscale-address``
in the shell. The page shows the guest's connection status and address.
SSH requires a destination SSH server and permission to reach it.

Silkie operates no public relay. You can use Ncat on your own Tailscale
machine to relay to an SSH server on your LAN. See :doc:`browser-ssh` for
setup, commands, and host-key verification.

.. raw:: html

   <p class="demo-attribution">Linux execution powered by
   <a href="https://cheerpx.io/">CheerpX by Leaning Technologies</a>,
   loaded from its hosted runtime under the
   <a href="https://cheerpx.io/docs/licensing">Community License</a>.
   Silkie forks <a href="https://sw.kovidgoyal.net/kitty/">kitty</a>.
   Read the <a href="reference.html">Silkie extension reference</a> and
   <a href="https://sw.kovidgoyal.net/kitty/protocol-extensions/">upstream protocol manuals</a>.
   kitty's companion tools and plugins are called
   <a href="https://sw.kovidgoyal.net/kitty/kittens_intro/">kittens</a>.
   <a href="https://github.com/adamdeprince/silkie/tree/silkie">Browse the source</a>.</p>
