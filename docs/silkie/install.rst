Install Silkie
================

Silkie is built from the `silkie branch
<https://github.com/adamdeprince/silkie/tree/silkie>`_ of this repository.
Source builds retain the executable names ``kitty`` and ``kitten``; binary
packages provide ``silkie`` and ``silkie-kitten``. Upstream kitty packages
provide upstream kitty rather than Silkie's extensions.

Binary packages
----------------

On Apple Silicon with macOS 26 Tahoe, install the Homebrew cask::

   brew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git
   brew install --cask goblinreactor/tap/silkie

The application requires macOS approval before first launch. The
`Homebrew repository's Silkie instructions <https://brew.goblinreactor.com/#silkie>`_
explain app approval and the available release.

For Linux, the `signed APT repository <https://apt.goblinreactor.com/#silkie>`_
provides installation instructions, the signing key, and the repository entry
for your distribution. Current targets include Ubuntu 26.04 on amd64 and arm64,
and Kylin V10 SP1 on old-world LoongArch64 (``loongarch64``). Packages for
Ubuntu 24.04 and Debian 13 on amd64 are also listed there. Use the suite matching
your system; old-world ``loongarch64`` and new-world ``loong64`` are different
binary targets.

After configuring the repository as described there::

   sudo apt-get update
   sudo apt-get install silkie

Launch ``silkie`` and use ``silkie-kitten`` for its companion tools.

Fonts
-------

Install Iosevka for Silkie's default text and Iosevka Slab for the serif role.
For CJK text, install a regional Sarasa Term or Noto Sans CJK family. Font
files are separate from the application; when a preferred font is absent,
the operating system substitutes an available face. The :doc:`font-protocol`
reference links the font sources and explains regional glyph selection.

Source builds
---------------

Prerequisites
---------------

Install Git, a C compiler, and `Go <https://go.dev/doc/install>`_. The minimum
Go version is declared in `go.mod on the silkie branch
<https://github.com/adamdeprince/silkie/blob/silkie/go.mod>`_.

On macOS, install Apple's command line developer tools with ``xcode-select --install``
if they are not already available. On Linux, install the X11 and D-Bus development
libraries listed in `kitty's build prerequisites
<https://sw.kovidgoyal.net/kitty/build/#x11-dev-libs>`_.

The ``dev.sh`` build uses prebuilt dependencies for the supported platform,
including its Python environment. The first build needs network access and
downloads those dependencies. The `build implementation
<https://github.com/adamdeprince/silkie/blob/silkie/bypy/devenv.go>`_ is in this
repository. For a build against system libraries, consult the
`upstream dependency list <https://sw.kovidgoyal.net/kitty/build/#dependencies>`_
and Silkie's `Brewfile
<https://github.com/adamdeprince/silkie/blob/silkie/Brewfile>`_ on macOS.

Build and launch
-----------------

.. include:: _includes/source-build.rst

Keep the checkout in place when running this development build. Re-run
``./dev.sh build`` after changing the source. On macOS, the build also supplies
``kitty/launcher/kitty.app``; see the `upstream build notes
<https://sw.kovidgoyal.net/kitty/build/>`_ for details about unsigned applications
and relocating a checkout.

Try the input inspector
------------------------

Inside the running Silkie terminal, use the executable from the same checkout::

   ./kitty/launcher/kitten show-pointer

Run that command from the repository directory. The :doc:`show_pointer kitten
<show-pointer>` queries the input protocols and displays the events supported
by the terminal and connected devices.

Next, read :doc:`configuration` for Silkie-specific options and the
`kitty configuration manual <https://sw.kovidgoyal.net/kitty/conf/>`_ for the
inherited settings. Applications should query :doc:`capabilities-protocol`
before enabling extensions.
