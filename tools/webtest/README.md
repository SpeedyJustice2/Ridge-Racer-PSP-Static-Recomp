# webtest - automated tests of the browser build (headless Chromium)

Used by Claude in its Linux workspace; works anywhere with Node 18+ and a Chromium.

    cd tools/webtest && npm install
    node run.js --out OUTDIR --seconds 300 --shot 30 --saves PATH/TO/SAVEDATA --dist PATH/TO/web-dist --query "capture&replay=smoke60.txt"
    node prof.js --at 75 --len 20 --out PROFDIR --query "quiet&replay=smoke60.txt"     (CPU profile of the engine worker)

CHROME=path picks the browser (default: the Playwright Chromium in Claude's workspace).  Outputs: console.txt (page and
worker messages, STATS/STATUS lines), engine.txt (engine log), mem.txt (memory of every browser process every 10 s),
shot_NNN.png (the game screen).

Page test options (URL): ?capture ?console ?quiet ?dropframes ?args=... ?env=K=V,K2=V2 (or K=V|K2=V2)
?replay=FILE (input log next to the page) ?saves=DIR (save folder next to the page, DIR/list.txt) ?presentmode=1|2|3
(1/2 = frame hand-over tests, 3 = never yield to the event loop, which brings back the GPU-process leak).
