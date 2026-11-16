SSH in the browser demo
========================

.. note::

   The Silkie WebAssembly demo is under development. This guide records its
   networking design and the private relay setup. The :doc:`browser-demo`
   includes connection controls; end-to-end SSH validation is still pending.

The browser demo uses CheerpX to run a Linux shell, basic commands,
``silkie-sprint``, ``silkie-scramble``, and an OpenSSH client. Networking will
be optional, using the visitor's own Tailscale network. **Silkie will not
operate a public SSH relay.** The shell and bundled games must work without
connecting to Tailscale.

A web page cannot open a raw TCP socket to port 22. CheerpX provides that
connectivity through its `Tailscale integration
<https://cheerpx.io/docs/guides/Networking>`_. A netcat relay can then forward
SSH to a machine reachable from the visitor's network. Netcat alone does not
provide a browser transport: the relay recipe below also uses Tailscale.

Connect through Tailscale
--------------------------

The terminal startup banner contains a clickable **Connect to Tailscale**
link. It prints a second link for the visitor's Tailscale login, which
registers the browser guest in their tailnet.
The page shows connection status and the guest's assigned address.
Signing in to the host computer's Tailscale app does not automatically
connect the guest; it needs its own login.

To prepare an SSH destination:

1. Install `Tailscale <https://tailscale.com/download>`_ on the destination
   machine and sign in to the same tailnet.
2. Enable the machine's SSH server. On macOS, this is **Remote Login** in
   System Settings. On Linux, enable the distribution's OpenSSH server.
3. Allow the browser guest to reach the destination's SSH port in the
   tailnet access policy and the destination's firewall.
4. Once the browser guest is connected, run this in its shell, substituting
   your account and the destination's Tailscale IPv4 address::

      ssh your-user@100.101.102.103

Use the destination's Tailscale address, not ``localhost``: inside the demo,
``localhost`` refers to the Linux guest. An exit node is unnecessary for
connections to tailnet machines. See `Tailscale's connection guide
<https://tailscale.com/docs/how-to/connect-to-devices>`_.

This uses ordinary SSH over Tailscale networking. Tailscale SSH is a separate,
optional service with its own access rules; it is not required for this setup.
Verify the destination's host-key fingerprint before accepting a new key.

Run your own netcat relay
--------------------------

Use a relay when an SSH destination does not run Tailscale but another
computer can reach it. The relay computer must run Tailscale and be able
to open a TCP connection to the destination's SSH port.

The connection is::

   browser guest -> Tailscale -> your relay:2222 -> your SSH server:22

These commands use **Nmap Ncat**, a netcat implementation whose executable
is named ``ncat``. Other programs named ``nc`` have different listener and
execution options. Install Ncat on the relay computer; for example::

   # macOS (Homebrew)
   brew install nmap

   # Debian / Ubuntu
   sudo apt-get install ncat

On the relay computer, replace ``192.168.1.50`` with the SSH server's LAN
address, then run::

   ncat --listen "$(tailscale ip -4)" 2222 --keep-open \
     --sh-exec "ncat 192.168.1.50 22"

The listener binds only to the relay's Tailscale IPv4 address. Its tailnet
policy and local firewall must allow the browser guest to reach TCP port
2222. The destination must allow connections from the relay. Keep the
destination in the command fixed; each incoming connection starts a separate
forwarder to that destination. ``--keep-open`` permits reconnects and
independent simultaneous sessions.

If the SSH server runs on the relay computer itself, use ``127.0.0.1`` in
place of ``192.168.1.50``. If ``tailscale ip -4`` is unavailable, use the
relay's Tailscale IPv4 address explicitly as the listener address.

In the browser shell, substitute the relay's Tailscale IPv4 address and the
destination's account name::

   ssh -o HostKeyAlias=lan-server -p 2222 your-user@100.101.102.103

``HostKeyAlias`` identifies the destination in SSH's known-hosts file;
choose a distinct, stable name for each destination. Verify the fingerprint
against the destination SSH server, rather than the relay. SSH authentication
and encryption run between the guest and that destination; Ncat forwards the
encrypted stream without logging in on the visitor's behalf.

Leave the relay command running while using it. Press Ctrl+C to stop accepting
connections, and close any existing SSH sessions. No router port forwarding,
public listener, or Tailscale exit node is needed. This recipe uses
Tailscale's normal infrastructure; Silkie supplies no relay service.

See the Ncat manuals for `command execution
<https://nmap.org/ncat/guide/ncat-exec.html>`_ and `port forwarding
<https://nmap.org/ncat/guide/ncat-tricks.html>`_.

Browser integration requirements
---------------------------------

The CheerpX integration must use the visitor's interactive login. Configure
``networkInterface.loginUrlCb``, ``stateUpdateCb``, and ``netmapUpdateCb``
when creating the guest, then call ``cx.networkLogin()`` from the explicit
connect action. Present the login URL as a link the visitor can open and
show authentication failures and connection changes on the page. See
`CheerpX networking <https://cheerpx.io/docs/guides/Networking>`_ and the
`Linux.create reference
<https://cheerpx.io/docs/reference/CheerpX.Linux/create>`_.

Do not embed a project-owned Tailscale auth key or a default public proxy in
the page or guest image. The shell startup report must explain that external
SSH requires the visitor's Tailscale connection and link to this guide.
Guest SSH keys and known-hosts data belong to the visitor's guest filesystem,
not a Silkie server.

Validation status
-------------------

The Ncat recipe has been checked on loopback with Ncat 7.991: 24 sessions,
four concurrent clients, and repeated binary exchanges totalling more than
12 MiB passed. A separate full-duplex echo test sending a single 1 MiB burst
stalled. This remains unresolved; the streaming check does not establish
reliability for every traffic pattern. Browser-to-server SSH, bulk transfers,
and reconnects through CheerpX and Tailscale still need end-to-end validation
before the demo is released.
