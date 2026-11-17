Environment variables
=====================

Variables that you set to configure Silkie use ``SILKIE_`` names. Variables
that Silkie sets for the programs running in it keep kitty's names, because
editors, image viewers, libraries, kittens and the shell integration scripts
look for those names to detect the kitty protocols and to talk to the terminal.

Variables that configure Silkie
-------------------------------

Silkie still reads the old ``KITTY_`` name of each renamed variable, for
compatibility with existing scripts. When both names are set, the ``SILKIE_``
name wins, even when it is set to an empty value, so it can override a
``KITTY_`` value inherited from the environment.

.. list-table::
   :header-rows: 1
   :widths: 30 50 20

   * - Variable
     - Effect
     - Old name, still read
   * - ``SILKIE_CONFIG_DIRECTORY``
     - The directory for ``silkie.conf`` and other configuration files. See
       :doc:`configuration` for the default search order.
     - None, see below
   * - ``SILKIE_CACHE_DIRECTORY``
     - The directory for cache files. Defaults to ``~/Library/Caches/kitty``
       on macOS and ``$XDG_CACHE_HOME/kitty`` or ``~/.cache/kitty``
       elsewhere, the same directory kitty uses.
     - ``KITTY_CACHE_DIRECTORY``
   * - ``SILKIE_RUNTIME_DIRECTORY``
     - The directory for runtime files such as sockets. Defaults to the
       per-user cache directory of macOS, to ``XDG_RUNTIME_DIR``, to
       ``/run/user/<uid>``, or to ``run`` in the cache directory.
     - ``KITTY_RUNTIME_DIRECTORY``
   * - ``SILKIE_DISABLE_WAYLAND``
     - When set to a non-empty value, Silkie uses X11 even when a Wayland
       compositor is available.
     - ``KITTY_DISABLE_WAYLAND``
   * - ``SILKIE_WAYLAND_DETECT_MODIFIERS``
     - When set, to any value, Silkie tries to discover the XKB
       :kbd:`hyper` and :kbd:`meta` modifiers under Wayland. If discovery
       fails, the default Wayland XKB mappings are used.
     - ``KITTY_WAYLAND_DETECT_MODIFIERS``
   * - ``SILKIE_SIMD``
     - ``128``, ``256`` or, on x86-64, ``512`` selects the width of the SIMD
       vector instructions Silkie uses; any other value disables them. This
       overrides CPU detection, so a width the CPU lacks crashes Silkie
       with ``SIGILL``.
     - ``KITTY_SIMD``
   * - ``SILKIE_RC_PASSWORD``
     - The password for ``kitten @`` when ``remote_control_password`` is
       configured. It is the default of ``kitten @ --password-env``. A
       variable named explicitly with ``--password-env`` is read alone.
     - ``KITTY_RC_PASSWORD``
   * - ``SILKIE_CLONE_SOURCE_CODE``
     - Shell code that the shell integration runs with ``eval`` in a window
       created by ``clone-in-kitty``, with the ``env_var`` strategy of
       ``clone_source_strategies``.
     - ``KITTY_CLONE_SOURCE_CODE``
   * - ``SILKIE_CLONE_SOURCE_PATH``
     - A file that the shell integration sources in a window created by
       ``clone-in-kitty``, with the ``path`` strategy of
       ``clone_source_strategies``.
     - ``KITTY_CLONE_SOURCE_PATH``
   * - ``SILKIE_DEVELOP_FROM``
     - The directory of a Silkie source tree to load the Python code from.
       Only frozen builds made with ``bypy`` read it.
     - ``KITTY_DEVELOP_FROM``

``KITTY_CONFIG_DIRECTORY`` is not read. Silkie keeps its configuration apart
from kitty's, and many kitty users set ``KITTY_CONFIG_DIRECTORY`` for kitty
alone. Reading it would move Silkie's configuration, its Settings and its
profiles into kitty's directory.

Silkie also reads ``VISUAL`` and ``EDITOR`` for the editor that opens
configuration files, ``SHELL`` for the default shell, ``GLFW_IM_MODULE=ibus``
to enable IME input under X11, ``SSH_ASKPASS`` for the ssh kitten, and the
usual ``DISPLAY``, ``WAYLAND_DISPLAY``, ``XDG_*``, ``XCURSOR_THEME``,
``XCURSOR_SIZE``, ``LANG`` and ``LC_*`` variables. Those names belong to other
software and are not renamed.

Variables for building Silkie
-----------------------------

``setup.py`` reads these when building Silkie. The same precedence applies.

.. list-table::
   :header-rows: 1
   :widths: 40 40 20

   * - Variable
     - Effect
     - Old name, still read
   * - ``SILKIE_NO_LTO``
     - When set, to any value, builds without link time optimization.
     - ``KITTY_NO_LTO``
   * - ``SILKIE_EGL_LIBRARY``
     - The path of the EGL library to load at runtime.
     - ``KITTY_EGL_LIBRARY``
   * - ``SILKIE_STARTUP_NOTIFICATION_LIBRARY``
     - The path of the libstartup-notification library to load at runtime.
     - ``KITTY_STARTUP_NOTIFICATION_LIBRARY``
   * - ``SILKIE_CANBERRA_LIBRARY``
     - The path of the libcanberra library to load at runtime.
     - ``KITTY_CANBERRA_LIBRARY``
   * - ``SILKIE_SYSTEMD_LIBRARY``
     - The path of the libsystemd library to load at runtime.
     - ``KITTY_SYSTEMD_LIBRARY``
   * - ``SILKIE_FONTCONFIG_LIBRARY``
     - The path of the fontconfig library to load at runtime.
     - ``KITTY_FONTCONFIG_LIBRARY``

Variables in configuration files
--------------------------------

The paths in ``include``, ``globinclude`` and ``geninclude`` directives can use
``${SILKIE_OS}``, which is ``linux``, ``macos`` or ``bsd``::

    include ${SILKIE_OS}.conf

``${KITTY_OS}`` has the same value, for configuration files written for kitty.
Programs run by ``geninclude`` get both variables in their environment.

Variables that Silkie sets for programs
---------------------------------------

These keep their kitty names. Programs and libraries use them to detect the
kitty protocols, and kittens, the shell integration scripts and the remote side
of the ssh kitten depend on them.

``TERM``
    The terminal name, ``xterm-kitty`` by default. See ``term``.
``TERMINFO``
    The directory of the terminfo database, or the database itself in
    base64. See ``terminfo_type``.
``COLORTERM``
    ``truecolor``.
``WINDOWID``
    The id of the OS window, on platforms that have one.
``KITTY_WINDOW_ID``
    The id of the window the program runs in, for remote control.
``KITTY_PID``
    The process id of Silkie.
``KITTY_PUBLIC_KEY``
    The public key for encrypted remote control commands, as
    ``protocol:key data``.
``KITTY_LISTEN_ON``
    The remote control socket address, or ``fd:<number>``, when remote
    control is listening. ``kitten @`` uses it as the default for ``--to``.
``KITTY_INSTALLATION_DIR``
    The installation directory.
``KITTY_SHELL_INTEGRATION``
    The shell integration options. The integration scripts remove it.
``ZDOTDIR``, ``XDG_DATA_DIRS``, ``ENV``
    Changed for zsh, fish and bash to load the shell integration.
``KITTY_SI_RUN_COMMAND_AT_STARTUP``
    A command that the shell integration runs once the shell has started.
    It can also be passed with ``launch --env``. A value inherited by Silkie
    itself is ignored.
``KITTY_HOLD``
    ``1`` when the shell runs because of ``--hold``.
``KITTY_PIPE_DATA``
    The layout of the screen, for programs that receive its contents with
    ``launch --stdin-source``.
``KITTY_CHILD_CMDLINE``
    The command line of the program in the window, for
    ``command_on_bell``.
``KITTY_COMMON_OPTS``, ``KITTY_CHILD_PID``, ``OVERLAID_WINDOW_LINES``, ``OVERLAID_WINDOW_COLS``
    Set for kittens that run in an overlay. ``SILKIE_CONFIG_DIRECTORY`` is
    set for kittens too.
``SSH_AUTH_SOCK``
    The forwarded agent, with ``ssh_agent_forwarding yes``.
``PATH``
    Has Silkie's own directory added, so that ``kitty`` and ``kitten``
    can be found.
``LANG``
    On macOS only, set when the macOS language and region do not form a
    valid locale.

Silkie does not set ``SILKIE_`` copies of these variables. A kitty or Silkie
running inside Silkie replaces ``KITTY_WINDOW_ID`` and the other values with
its own, but it would leave a ``SILKIE_`` copy behind, pointing programs in the
inner terminal at the outer one. To find out whether a program runs in Silkie
and what it supports, query the terminal as described in
:doc:`capabilities-protocol`.

Variables used internally
-------------------------

Silkie, its kittens and its shell integration pass these between their own
processes. Do not set them. They keep kitty's names, may change without
notice, and are listed only so that they can be recognized:
``KITTY_IS_CLONE_LAUNCH``, ``KITTY_CLONE_SOURCE_STRATEGIES``,
``KITTY_SI_DATA``, ``KITTY_STDIO_FORWARDED``, ``KITTY_KITTEN_RUN_MODULE``,
``KITTEN_RUNNING_AS_UI``, ``KITTY_BASIC_COLORS``,
``KITTY_PATH_TO_KITTY_EXE``, ``KITTY_EXEC_FOR_DETACH``,
``KITTY_LAUNCHED_BY_LAUNCH_SERVICES``, ``KITTY_RESTORE_LOGOUT_SNAPSHOT``,
``KITTY_SHELL_ACTIVE_WINDOW_ID``, ``KITTY_SHELL_ACTIVE_TAB_ID``, the
``KITTY_BASH_*`` and ``KITTY_SH_*`` variables, ``KITTY_ORIG_ZDOTDIR``,
``KITTY_FISH_XDG_DATA_DIR``, and, for the ssh kitten,
``KITTY_SSH_KITTEN_DATA_DIR``, ``KITTY_SSH_ASKPASS_*``,
``KITTY_LOGIN_SHELL``, ``KITTY_LOGIN_CWD``, ``KITTY_REMOTE`` and
``SSH_KITTEN_KITTY_DIR``.
