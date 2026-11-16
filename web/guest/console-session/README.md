# Browser console session

`silkie-console-session PROGRAM [ARGS...]` runs a foreground application and
returns its exit status after the browser acknowledges the input handoff.
It is built for the Linux guest; native macOS tests exercise its POSIX terminal
lifecycle against real pseudo terminals.

The launcher saves the shell's termios settings and enters raw mode **before**
starting the child. Consequently, an application's own raw-mode cleanup restores
raw input instead of briefly echoing queued keyboard report fragments. The child
gets its own foreground process group where the kernel supports it. INT, TERM,
HUP, QUIT, and TSTP directed to the launcher are forwarded to that group, with
a child-PID fallback for virtual kernels without group signaling.

After the child exits, or before returning control to the shell for suspension,
the launcher writes:

```
ESC ] 777;browser-input;v=1:op=handoff:id=N ESC \
```

`N` is a fresh positive int31 drawn from `/dev/urandom`. The browser resets its
input modes and synchronously returns exactly:

```
ESC ] 777;browser-input;v=1:op=ack:id=N ESC \
```

The launcher consumes earlier application input through the matching reply's
terminator and restores the saved termios with `TCSANOW`. Each read requests one
byte, preserving subsequent shell typeahead in the terminal queue. It applies
no timeout or input size limit. A stopped child resumes in raw foreground mode;
a background resume cannot take the terminal away from the shell.

``silkie-console-session --notify-resize`` sends SIGWINCH only to the tracked
foreground application's PID. The browser uses this after changing tty
dimensions, avoiding interruption of a background shell waiting for a game.

The guest entry script uses ``silkie-console-session --shell PROGRAM [ARGS...]``
to establish foreground ownership where supported and publish the shell PID
before exec. Shell and launcher atomically publish the active application in
``/tmp/silkie-demo-foreground`` (overridable with ``SILKIE_CONSOLE_STATE``).
This is necessary because SDK-launched notification commands cannot infer the
original console's owner from their tty. During a handoff, the launcher publishes
its own PID, then restores the previous owner after the acknowledgement and tty
restoration. Resize delivery works at the prompt and while a game owns the terminal.

`silkie-console-session --features FILE` prints the startup/feature report using
the current stdout `TIOCGWINSZ` width on every invocation. Redirected output uses
80 columns. The formatter wraps words with hanging bullet and label indents,
reduces indentation on narrow screens, preserves Unicode grapheme clusters,
and keeps OSC 8 hyperlink targets attached to their wrapped labels. A grapheme
intrinsically wider than a one-column terminal is printed intact on its own
line. The formatter has no report, line, or word-length limit.

This private handshake requires the browser adapter. Running the launcher in
an ordinary terminal without the adapter will wait for an acknowledgement that
terminal does not implement. EOF, I/O failures, and caught termination signals
restore the saved terminal settings. SIGKILL cannot run process cleanup.

Run the stream and native PTY tests with:

```
cargo test --offline --manifest-path web/guest/console-session/Cargo.toml
```
