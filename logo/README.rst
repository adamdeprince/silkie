Silkie application icon
=======================

``silkie.png`` is the canonical Goblin artwork: the user's black-and-white
Silkie chicken behind a terminal, rendered as a black-on-white woodcut. It was
created with the built-in image generation tool using ``~/Downloads/goblin.png``
as the character reference. The generation prompt is in ``silkie-prompt.txt``.

Export the application and terminal window resources with Pillow installed::

    python3 logo/make.py

On macOS, select a full Xcode installation to rebuild the asset catalog::

    DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 logo/make.py

On Linux, the script exports PNG, SVG and ICNS resources and keeps the committed
macOS asset catalog. ``--skip-macos-assets`` also skips Xcode on macOS.

``kitty.png``, ``kitty-128.png``, ``kitty.svg``, ``kitty.icns`` and ``Assets.car``
retain their inherited filenames because the launchers and packaging code use
them. Every one contains Silkie's chicken artwork. The SVG embeds the canonical
PNG so Linux icon loaders need no external image files.
