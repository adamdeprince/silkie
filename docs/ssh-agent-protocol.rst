SSH agent transport
===================

The ``ssh-agent.1`` extension carries SSH agent requests through a terminal
channel. Odin Seat and Skiff expose a stable Unix socket as ``SSH_AUTH_SOCK``;
each request travels to the currently attached terminal's provider. Reattaching
from a different terminal changes the available keys without replacing the
socket pathname in the application's environment.

The agent's private keys stay at the provider. The wire payload is a complete
`SSH agent packet (RFC 9987) <https://www.rfc-editor.org/rfc/rfc9987.html>`_,
including its four-byte, big-endian length. Key lists and signing operations
are forwarded on demand, without caching key lists or signatures.

Enabling access
---------------

In ``silkie.conf``::

   ssh_agent_forwarding yes

The default is ``no``. Enabling it gives programs that can write terminal output
access to public keys and signing, as SSH agent forwarding does. Key addition,
removal, smartcard loading, and agent locking are refused by this transport.
Supported agent requests are identities (11), signing (13), and extension (27)
requests named ``session-bind@openssh.com`` or ``query``.

Silkie uses its local ``SSH_AUTH_SOCK``. If there is no available socket and
``ssh-agent`` is installed, it starts an agent, supplies its socket to new local
shells, and stops that owned process when Silkie exits. Load keys with
``ssh-add`` in a local shell; Silkie does not load private key files itself.

Odin Seat gives new windows its session socket automatically. To update a
shell that predates this feature, set its environment once::

   export SSH_AUTH_SOCK="$(odin-seat agent-socket)"

``agent-socket -s NAME`` selects a session explicitly. The provider is the most
recently attached display client. When it disconnects, the previous attached
client becomes the provider. ``odin-seat -A attach`` explicitly authorizes the
attached client's local ``SSH_AUTH_SOCK`` as a fallback, including a freshly
forwarded agent over ordinary SSH. A client without a provider does not silently
borrow another attached client's keys. Pane focus does not select the agent.

Skiff gives its remote child a stable socket automatically and prefers the
terminal provider. ``skiff -A`` additionally authorizes fallback to the client's
local ``SSH_AUTH_SOCK`` when the terminal refuses or does not answer the query.
This supports terminals without this extension. The fallback also opens and
preserves a separate agent connection for each forwarded socket connection.

Framing and discovery
---------------------

Frames use OSC 777 and the ``ssh-agent`` selector::

   ESC ] 777 ; ssh-agent ; v=1:op=query:id=probe-1 ST
   ESC ] 777 ; ssh-agent ; v=1:op=cap:id=probe-1:caps=1:g=providerA ST

``caps=0`` means no provider is available or permitted. ``caps=1`` requires a
generation token ``g``. The common ``caps`` extension lists ``ssh-agent.1`` to
advertise protocol support; this is separate from permission and availability.

Fields follow :doc:`extension-framing`: ASCII ``key=value`` pairs separated by
colons, no duplicate keys, unknown keys ignored, and ST or BEL termination.
For this selector, ``id``, ``c``, and ``g`` are nonempty tokens made of ASCII
letters, digits, periods, underscores, and hyphens, rather than numeric query
IDs. There is no fixed token length or outstanding-request count.

Requests and replies
--------------------

An application connects to the local ``SSH_AUTH_SOCK`` and uses the standard
agent protocol. Its socket proxy queries the terminal for each packet, then
sends a request naming that provider generation::

   ESC ] 777 ; ssh-agent ; v=1:op=request:id=request-1:c=connection-1:g=providerA:expires=1794787200000:data=AAAAAQs= ST

Here ``data`` is standard padded base64 of a complete packet. ``AAAAAQs=`` is
the five-byte identities request: length 1 followed by message type 11.

.. list-table:: Request fields
   :header-rows: 1
   :widths: 18 82

   * - Field
     - Meaning
   * - ``id``
     - Unique request identifier, echoed by the response.
   * - ``c``
     - Connection token, stable for the lifetime of one local agent connection.
   * - ``g``
     - Provider generation returned by the preceding capability query.
   * - ``expires``
     - Unsigned 64-bit Unix milliseconds; the last time this request may begin.
   * - ``data``
     - Base64 of exactly one complete SSH agent packet, including its length.

The terminal returns ``op=reply`` with the same ``id``, ``c``, and ``g``, and
``data`` containing exactly one complete response packet. A transport failure
returns an SSH_AGENT_FAILURE packet and an ``error`` token, such as
``unavailable``, ``changed``, ``expired``, ``closed``, or ``timeout``.
Ordinary failures from the underlying agent remain ordinary agent responses.

On local socket close, the proxy sends::

   ESC ] 777 ; ssh-agent ; v=1:op=close:id=close-1:c=connection-1:g=providerA ST

Close has no response. The provider closes that agent connection and refuses
subsequent requests using its token. Connection tokens must not be reused.
Duplicate request IDs on a connection are refused rather than executed again.

Connections, reconnects, and routing
------------------------------------

Each connection token maps to one persistent connection to the real agent.
In particular, `OpenSSH session bindings
<https://github.com/openssh/openssh-portable/blob/master/PROTOCOL.agent>`_
remain on the same backend connection as subsequent signing requests. Opening
a fresh backend connection for every packet would lose those constraints.

A socket connection is pinned to the generation it first used. If a later
query reports another generation, the proxy fails and closes that connection;
a new connection obtains the new client's keys. In-flight requests are failed
when Odin Seat selects another display, and are not retried against its agent.
Closed or expired requests cannot begin later when queued terminal output is
finally delivered. Close cannot undo an operation already delivered to an
underlying agent.

Odin Seat scopes request and connection IDs per nested window, sends agent
traffic only to the selected display, and routes the reply to the originating
window or socket. It drops stale replies instead of treating them as keyboard
input. Its socket path, listener, selection order, and routing scope survive a
live server upgrade; existing agent connections fail and can be reopened.

Skiff carries agent frames on its reliable one-shot terminal event channel,
outside screen snapshots, input prediction, motion collection, and input
recordings. A new client announces its attachment and cancels the old socket
requests. Provider generations prevent queued operations intended for another
terminal from being executed after reconnecting.

Socket directories are private to the user (mode 0700); sockets have mode 0600.
Multiple local connections and multipart reads and writes are supported.
Storage grows with input and active connections; the SSH packet length is
limited by its actual unsigned 32-bit wire field, and Unix socket paths by the
operating system. Skiff also follows the signed message-size representation
of its protobuf transport. Allocation and representation failures are explicit.

Liveness
--------

The current socket proxies allow five seconds for capability negotiation and
two minutes for an agent request. Skiff waits one second before using an
authorized local fallback or reporting an unsupported terminal. An expired
negotiation or request fails without automatically retrying a signing action.
The client and provider clocks must be synchronized within the request's
remaining validity interval because ``expires`` is an absolute timestamp.

Backend sockets are nonblocking. Hardware-key confirmation and slow agents do
not block terminal drawing or other agent connections. Silkie polls pending
agent work from its event loop and releases connection state when the socket
or terminal window closes.
