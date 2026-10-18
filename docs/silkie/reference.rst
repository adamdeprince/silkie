Silkie extension reference
==========================

Silkie extends kitty with tablet and touch input, gestures, terminal-side ink,
vector graphics, HDR, and display and presentation feedback. This reference
specifies Silkie's additions: their escape sequences, fields, units, replies,
configuration, and implementation limits.

Start with :doc:`capabilities-protocol` to discover what a terminal supports,
then :doc:`extension-framing` for the framing and timing rules shared by the
OSC 777 protocols. :doc:`show-pointer` provides an interactive input inspector.

For the project overview, see :doc:`index`. For installation, see
:doc:`install`. The :doc:`developer-guide` connects these extensions to
kitty's protocols, kitten tools, and the underlying terminal conventions.

.. toctree::
   :maxdepth: 1
   :caption: Discovery and framing

   capabilities-protocol
   extension-framing

.. toctree::
   :maxdepth: 1
   :caption: Text and typography

   font-protocol

.. toctree::
   :maxdepth: 1
   :caption: Input and interaction

   tablet-protocol
   scroll-protocol
   gesture-protocol
   haptic-protocol
   pointer-image-protocol
   ink-protocol

.. toctree::
   :maxdepth: 1
   :caption: Graphics and color

   vector-protocol
   gpu-surface-protocol
   hdr-protocol
   wide-gamut-protocol
   cell-alpha-protocol

.. toctree::
   :maxdepth: 1
   :caption: Display and timing

   display-protocol
   latency-protocol
   frame-protocol

.. toctree::
   :maxdepth: 1
   :caption: Using Silkie

   configuration
   show-pointer
   window-actions
