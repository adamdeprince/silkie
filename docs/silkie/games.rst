Games for Silkie and kitty
==========================

.. Media provenance: Scramble's video and poster come from that game's
   docs/video/gameplay.mp4 and docs/images/brood.png. Sprint was recorded
   in a dedicated Silkie window with --seed 42 --autoplay, using the
   ScreenCaptureKit helper in silkie-scramble/docs/tools/capture.swift.
   Both videos are silent H.264 MP4 window recordings, without added art.

Play all four games in the website's Linux terminal, or run them in native
Silkie and kitty terminals. Each browser link opens a terminal filling its
own window and starts the game automatically. Use **Fullscreen** to hide
the browser controls. Each game keeps its own files and saves in this
browser's local storage; reopening its link returns to the running window.

.. raw:: html

   <ul>
     <li><a href="browser-demo.html?game=presidential" target="silkie-presidential">Play Presidential full screen</a></li>
     <li><a href="browser-demo.html?game=silkie-loophole" target="silkie-silkie-loophole">Play Silkie Loophole full screen</a></li>
     <li><a href="browser-demo.html?game=silkie-sprint" target="silkie-silkie-sprint">Play Silkie Sprint full screen</a></li>
     <li><a href="browser-demo.html?game=silkie-scramble" target="silkie-silkie-scramble">Play Silkie Scramble full screen</a></li>
   </ul>

Presidential
--------------

You are President of the United States. Read the daily briefing, draft
orders, and press GO to advance a day in a simulation of the world economy.
Explore trade, finance, government, diplomacy, and the effects of your policies.
Use F1–F9 to switch panels, B for the briefing, and ? for help.
Campaigns save after each day and when you quit.

Build from the ``presidential`` checkout with
``cargo install --path crates/terminal-ui``, then run ``presidential``.
The economic data and world map are embedded, so the game works offline.

Silkie Loophole
----------------

A raccoon in a bedsheet goes trick-or-treating around Montpelier, Vermont.
Collect candy, discover the neighbours' routines, and keep the disguise
together. Walk with arrows or WASD, press Space or Enter to interact,
M for the map, J for the journal, and ? for help. Ctrl+C quits.

Install it from the repositories described under Install Sprint and Scramble below:
``brew install goblinreactor/tap/silkie-loophole`` on Apple Silicon with macOS 26 Tahoe,
or ``sudo apt-get install silkie-loophole`` on Ubuntu 26.04 amd64 or Kylin V10 SP1
old-world LoongArch64. Then run ``silkie-loophole``. See the
`Loophole source and manual <https://github.com/adamdeprince/loophole>`_.

Sprint and Scramble
---------------------

Silkie Sprint and Silkie Scramble are arcade games set in the same backyard
world, with a Silkie hen at the centre of each adventure. They run inside
the terminal and require its graphics protocol. The videos below show the
games being played in Silkie.

Both games support `kitty <https://sw.kovidgoyal.net/kitty/>`_ and are
optimized for Silkie. The rules, scoring, and difficulty are the same in
both terminals; Silkie's extensions improve graphics, input, and timing.
See :doc:`install` to get Silkie.

Install Sprint and Scramble
----------------------------

On Apple Silicon with macOS 26 Tahoe, use the
`Homebrew repository <https://brew.goblinreactor.com/>`_::

   brew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git
   brew install goblinreactor/tap/silkie-sprint goblinreactor/tap/silkie-scramble

On Ubuntu 26.04 amd64 or Kylin V10 SP1 old-world LoongArch64, configure the
`signed APT repository <https://apt.goblinreactor.com/>`_ for your system:
``resolute`` for Ubuntu, or ``kylin`` for Kylin. Then install::

   sudo apt-get update
   sudo apt-get install silkie-sprint silkie-scramble

Open Silkie or kitty and run ``silkie-sprint`` or ``silkie-scramble``.
The games are separate packages from the terminal. Check the repository's
package listings for the platforms currently available.
Kylin packages use the old-world ``loongarch64`` ABI, which differs from
new-world ``loong64``.

Silkie Sprint
---------------

*Hop the traffic. Ride the river. Fly when you must.*

Guide a Silkie hen across roads, a railway, and a river to the henhouse.
Dodge traffic, ride floating logs, and hop into empty nest boxes to lay
eggs. Fill every nest and the eggs hatch. Your hen can fly briefly, but
her wings need time on the ground to recover, and the sky has hazards too.

Use the arrow keys or WASD to hop and Space to fly or land. Press P to pause.

.. raw:: html

   <figure class="gameplay">
     <video controls playsinline preload="metadata" poster="_static/games/sprint.png" width="1000" height="680" aria-label="Silkie Sprint gameplay" aria-describedby="sprint-caption">
       <source src="_static/games/sprint.mp4" type="video/mp4">
       <p><a href="_static/games/sprint.mp4">Download the Silkie Sprint gameplay video.</a></p>
     </video>
     <figcaption id="sprint-caption">Silkie Sprint running in Silkie, with the built-in autopilot crossing roads and rivers. Approximately 30 seconds; silent.</figcaption>
   </figure>
   <p><a href="_static/games/sprint.mp4">Download Sprint gameplay (MP4)</a></p>

Silkie Scramble
-----------------

*Gather the chicks. Mind the fox. Bring them home.*

Explore a garden maze, gather wandering chicks, and lead them back to the
coop. A fox, a weasel, and a hawk threaten your brood. Bringing more chicks
home together earns more points, but a longer procession is harder to keep
safe. Draw temporary fences to divert predators and use garden gates to
control their routes.

Use the arrow keys or WASD to move. Draw fences with the mouse, or with a
pen or touchscreen in Silkie. Space places a hurdle or operates a nearby
latched gate. Press P to pause.

.. raw:: html

   <figure class="gameplay">
     <video controls playsinline preload="metadata" poster="_static/games/scramble.png" width="1200" height="792" aria-label="Silkie Scramble gameplay" aria-describedby="scramble-caption">
       <source src="_static/games/scramble.mp4" type="video/mp4">
       <p><a href="_static/games/scramble.mp4">Download the Silkie Scramble gameplay video.</a></p>
     </video>
     <figcaption id="scramble-caption">Silkie Scramble running in Silkie, with the built-in autopilot gathering chicks and bringing them home. Approximately 40 seconds; silent.</figcaption>
   </figure>
   <p><a href="_static/games/scramble.mp4">Download Scramble gameplay (MP4)</a></p>

What Silkie adds
-----------------

Both games discover terminal capabilities at startup and use kitty's
`graphics protocol <https://sw.kovidgoyal.net/kitty/graphics-protocol/>`_
for sprites. Silkie's additional protocols provide:

* :doc:`Retained vector scenery <vector-protocol>`, with small scene patches
  for changes such as nest contents, gates, and fences.
* :doc:`Frame presentation feedback <frame-protocol>` and
  :doc:`low-latency mode <latency-protocol>` while playing.
* :doc:`Wide-gamut colour <wide-gamut-protocol>` and
  :doc:`HDR highlights <hdr-protocol>` on supported displays, plus
  :doc:`translucent menus <cell-alpha-protocol>`.
* :doc:`Pen and touchscreen input <tablet-protocol>`,
  :doc:`haptic feedback <haptic-protocol>`, and
  :doc:`custom pointer images <pointer-image-protocol>` where supported.
* :doc:`Terminal-side ink <ink-protocol>` for pen-drawn fences in Scramble.

Run either game with ``--kitty-only`` to compare its baseline presentation
with Silkie's enhancements, or ``--report`` to inspect detected capabilities.
Run directly in the terminal; the games do not configure graphics passthrough
for tmux or screen.
