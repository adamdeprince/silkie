.. raw:: html

   <link rel="stylesheet" href="_static/demo/demo.css">
   <div class="browser-demo-window">
     <section class="demo-panel" id="demo-panel" aria-label="Interactive Silkie terminal">
       <div class="demo-toolbar" role="group" aria-label="Terminal controls">
         <span class="demo-window-title">silkie<span aria-hidden="true">_</span></span>
         <button type="button" id="demo-start" hidden>Restart shell</button>
         <button type="button" id="demo-fullscreen" aria-pressed="false">Fullscreen</button>
       </div>
       <p class="demo-status" id="demo-status" role="status" aria-live="polite">Starting the shell…</p>
       <div id="silkie-terminal" aria-label="Silkie Linux terminal"></div>
     </section>
     <p class="demo-caption">Play in a full-window terminal:
     <a href="browser-demo.html?game=presidential" data-demo-game="presidential" target="silkie-presidential">Presidential</a> ·
     <a href="browser-demo.html?game=silkie-loophole" data-demo-game="silkie-loophole" target="silkie-silkie-loophole">Silkie Loophole</a> ·
     <a href="browser-demo.html?game=ground-stop" data-demo-game="ground-stop" target="silkie-ground-stop">Ground Stop</a> ·
     <a href="browser-demo.html?game=silkie-sprint" data-demo-game="silkie-sprint" target="silkie-silkie-sprint">Silkie Sprint</a> ·
     <a href="browser-demo.html?game=silkie-scramble" data-demo-game="silkie-scramble" target="silkie-silkie-scramble">Silkie Scramble</a>.
     Use <strong>Fullscreen</strong> to fill the display.
     <a href="browser-ssh.html">SSH setup</a> ·
     <a href="https://cheerpx.io/">Powered by CheerpX</a></p>
   </div>
   <noscript><p>Enable JavaScript to run the WebAssembly terminal.</p></noscript>
   <script type="module" src="_static/demo/app.mjs"></script>
