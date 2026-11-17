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
