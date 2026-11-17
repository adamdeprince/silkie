Install Silkie
================

Silkie is built from the `silkie branch
<https://github.com/adamdeprince/silkie/tree/silkie>`_ of this repository.
The installed executable is ``silkie``; its companion tools use
``silkie-kitten``. Upstream kitty packages
provide upstream kitty rather than Silkie's extensions.

Binary packages
----------------

Install Silkie and Iosevka on a supported macOS, Ubuntu, Debian, or Kylin system::

   curl -fsSL https://silkie.goblinreactor.com/install.sh | bash

The installer explains which Goblin Reactor package source it needs and asks
for permission before adding or changing it. It reuses an existing matching
source, verifies the APT signing key, and installs ``silkie`` with Homebrew or
APT. It also installs the unmodified Iosevka font: the official Homebrew
``font-iosevka`` cask on macOS, or a checksum-verified upstream release on Linux.
Source approval is read from your terminal, so it also works when the
script is piped to Bash. macOS needs Apple Silicon and macOS 26 or newer;
Homebrew is installed with your permission if it is missing.

The macOS app is not signed with an Apple Developer ID or notarized, so the
installer and the Homebrew cask clear its quarantine flag after installing it;
otherwise macOS would refuse to open it.

The manual commands are below.

On Apple Silicon with macOS 26 Tahoe, install the Homebrew cask::

   brew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git
   brew install --cask goblinreactor/tap/silkie
   brew install --cask font-iosevka

The cask clears the app's quarantine flag after installing it. If macOS
still blocks first launch, approve Silkie in System Settings > Privacy &
Security, or run ``xattr -dr com.apple.quarantine /Applications/silkie.app``. The
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

To check the installed commands::

   silkie --version
   silkie-kitten --version
   silkie-kitten show-pointer --help

The Linux repository signatures, package checksums, and executable links were
checked for all five targets listed above on October 28, 2026. APT installation and native-module loading were also tested on Ubuntu 26.04
amd64 (``naamah``) and Kylin V10 SP1 LoongArch64 (``loongson``).

The :doc:`Silkie Sprint and Silkie Scramble games <games>` are distributed
as separate Homebrew and APT packages. Their game page includes installation
commands, supported platforms, controls, and gameplay videos.

Fonts
-------

The installer includes Iosevka for Silkie's default text, using the wider
Extended faces for regular, bold and italic text. For a manual font installation,
use ``brew install --cask font-iosevka`` on macOS or download the unmodified
fonts from `Iosevka's releases <https://github.com/be5invis/Iosevka/releases>`_
on Linux. The scripted Linux installation currently pins Iosevka 34.9.1.

Iosevka is copyright Renzhi Li and is distributed under the
`SIL Open Font License 1.1
<https://github.com/be5invis/Iosevka/blob/main/LICENSE.md>`_. This license permits
use, installation and bundling with software, provided the font retains its
license and copyright notices; the font cannot be sold on its own. The
installer downloads the original fonts without modifying or renaming them,
and saves the matching ``Iosevka-LICENSE.md`` alongside the font files.
The fonts remain licensed separately from Silkie.

To use Iosevka's narrower original width instead, configure::

   font_family Iosevka
   font_role sans Iosevka

Install Iosevka Slab separately for the serif role.
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

The macOS dependency setup uses loader-relative library references so checkout
paths do not overflow the downloaded libraries' Mach-O load commands. Setup
is retried automatically if an earlier download or relocation was incomplete.
A fresh checkout with this fix completed the build and native-module loading
checks on October 28, 2026.

.. include:: _includes/source-build.rst

Keep the checkout in place when running this development build. Re-run
``./dev.sh build`` after changing the source. Development builds use the internal
``kitty/launcher/kitty`` and ``kitty/launcher/kitten`` paths rather than the
installed ``silkie`` and ``silkie-kitten`` commands. On macOS, the build also supplies
``kitty/launcher/kitty.app``; see the `upstream build notes
<https://sw.kovidgoyal.net/kitty/build/>`_ for details about unsigned applications
and relocating a checkout.

Try the input inspector
------------------------

Inside the installed Silkie terminal, run::

   silkie-kitten show-pointer

For a source build, run the tool from the repository directory. On macOS::

   ./kitty/launcher/kitty.app/Contents/MacOS/kitten show-pointer

On Linux::

   ./kitty/launcher/kitten show-pointer

The :doc:`show_pointer kitten
<show-pointer>` queries the input protocols and displays the events supported
by the terminal and connected devices.

Next, read :doc:`configuration` for Silkie-specific options and the
`kitty configuration manual <https://sw.kovidgoyal.net/kitty/conf/>`_ for the
inherited settings. Applications should query :doc:`capabilities-protocol`
before enabling extensions.
