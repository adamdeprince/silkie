Native windows, Settings and profiles
========================================

The terminal uses the native user interface of the system it runs on: Cocoa on
macOS and GTK 3 on Linux, including GNOME and UKUI (Kylin). Menus are pull down
menus of that toolkit and tabs are its tabs, as long as :opt:`native_tabs` is on.
Every tab is an OS window, so tabs of one window can use different profiles. On
Linux without GTK 3 the tab bar drawn by the terminal is used instead, and
settings are changed by editing the configuration file.

The Settings window
---------------------

:guilabel:`Settings…` shows every option of :file:`kitty.conf` as a control, generated from the
same definitions that document them. What is changed there goes to
:file:`settings.conf` in the configuration directory, which is read after
:file:`kitty.conf`, so what is set in the window wins and :file:`kitty.conf`
itself is never rewritten. The arrow next to a control removes the setting again.

Find
--------

:guilabel:`Edit › Find › Find…` (:kbd:`⌘+F` on macOS, :sc:`find` everywhere)
searches the screen and the scrollback of the active window, as Find does in
iTerm2. Every match is highlighted as you type, in :opt:`find_match_background`,
and the current match in :opt:`find_current_background`. :guilabel:`Find Next`
(:kbd:`⌘+G`, :sc:`find_next`, or :kbd:`Enter` in the field) moves up to older
output and :guilabel:`Find Previous` (:kbd:`⇧+⌘+G`, :sc:`find_previous`,
:kbd:`⇧+Enter`) down, scrolling the match into view and wrapping around at the
ends. :guilabel:`Use Selection for Find` (:kbd:`⌘+E`, :sc:`find_selection`)
finds the selected text. Matches are found across soft line wraps, in wide,
combining and scaled text, with full Unicode case folding unless
:guilabel:`Aa` is on. :guilabel:`.*` searches for a Python regular expression,
in which ``^`` and ``$`` are the start and end of a line. :kbd:`Esc` closes it.

The bar is native: on macOS a panel over the top right of the terminal, on Linux
at the end of the menu bar, so that opening it never resizes the terminal. Without
native tabs on Linux a line in place of the window title is used, which takes the
keys while it is open: :kbd:`Enter` and :kbd:`F3`, with :kbd:`Shift` to go back,
:kbd:`Alt+C` for case sensitivity and :kbd:`Alt+R` for regular expressions.

The scrollback, however long and whether compressed or not, is searched in the
background, newest lines first, a few milliseconds at a time, so that the terminal
stays responsive; the counter shows ``…`` until every line was searched. Output
that arrives while Find is open is searched as it comes. A program that uses the
keys of Find itself can have them with :opt:`find_keys_to_terminal`, for all windows,
a profile, a tab or a single window; the menu items keep working.

.. _profiles:

Profiles
-----------

A profile is a named set of settings for one kind of work, for example with its own
colors, font and shell for a production machine. It is a file in the
:file:`profiles` folder of the configuration directory, in the syntax of
:file:`kitty.conf`, with only what differs from the global settings::

    profile_name Production
    profile_command ssh prod
    profile_directory ~/ops
    profile_title PROD
    profile_env DEPLOY_ENV=production
    background #4a0000
    font_size 18
    minimum_contrast 40

A profile holds what can differ between OS windows: colors, :opt:`minimum_contrast`, fonts and font size,
cursor shape, opacity, initial size, padding, layouts and scrollback, plus the
command, directory, title and environment. Everything else is process wide and is set in the
other pages of the Settings window. Open a profile from the :guilabel:`Profiles` menu or with
the actions :ac:`new_tab_with_profile`, :ac:`new_os_window_with_profile` and :ac:`set_window_profile`.
The profile for new windows is :opt:`default_profile`.
