// Page side of the browser build: file picker, input, display, audio and save-game storage.  The engine itself runs in
// engine-worker.js; the two talk through a SharedArrayBuffer control block (layout documented in rr_present_web.cpp).
'use strict';
const $ = (id) => document.getElementById(id);
const CTL = {BUTTONS: 0, LX: 1, LY: 2, QUIT: 3, W: 4, H: 5, HOT: 6, AW: 7, AR: 8, TW: 9, TR: 10, POSTED: 11, CONSUMED: 12, TICK: 13, GPAD: 14, PAD: 15, TYPED: 16,
             MX: 48, MY: 49, CLICKS: 50, WHEEL: 51};
const AUDIO_FRAMES = 16384;

if (!self.crossOriginIsolated) $('isolation').hidden = false;
try { const v = localStorage.getItem('rr.prompts'); if (v && $('prompts')) $('prompts').value = v; } catch (e) {}
// The engine pauses itself once a frame through JavaScript Promise Integration (WebAssembly.Suspending): Chrome / Edge 137+.
const haveJspi = typeof WebAssembly === 'object' && 'Suspending' in WebAssembly;
if (!haveJspi) $('err').textContent = 'This browser lacks WebAssembly JSPI, which the game needs. Use a current Chrome or Edge (version 137 or newer).';
const isoInput = $('iso'), startBtn = $('start');
try { $('online').checked = localStorage.getItem('rr-online') === '1'; } catch (e) {}
$('online').addEventListener('change', () => { try { localStorage.setItem('rr-online', $('online').checked ? '1' : '0'); } catch (e) {} });
isoInput.addEventListener('change', () => { startBtn.disabled = !(isoInput.files && isoInput.files[0]) || !self.crossOriginIsolated || !haveJspi; });

// ---- save games (IndexedDB)
const db = () => new Promise((res, rej) => {
  const r = indexedDB.open('rr-web', 1);
  r.onupgradeneeded = () => r.result.createObjectStore('kv');
  r.onsuccess = () => res(r.result);
  r.onerror = () => rej(r.error);
});
async function loadSaves() {
  try { const d = await db(); return await new Promise((res) => { const q = d.transaction('kv').objectStore('kv').get('savedata'); q.onsuccess = () => res(q.result || []); q.onerror = () => res([]); }); }
  catch (e) { return []; }
}
async function storeSaves(files) {
  try { const d = await db(); d.transaction('kv', 'readwrite').objectStore('kv').put(files, 'savedata'); } catch (e) { console.warn('saving failed', e); }
}

let pendingBmp = null, lastRaf = 0, rxCount = 0;
// Shows the newest frame.  Normally called from requestAnimationFrame; when the browser stops running animation frames for
// a while (it can starve them while the engine's WebGL work keeps the GPU process busy) frames are shown as they arrive.
function showPending() { if (pendingBmp) { bmp.transferFromImageBitmap(pendingBmp); pendingBmp = null; ++fpsCount; ++self.rrStats.drawn; ack(); } }
function ack() { Atomics.add(ctl, CTL.CONSUMED, 1); Atomics.notify(ctl, CTL.CONSUMED); }
function drawLoop() {
  ++self.rrStats.raf; lastRaf = performance.now();
  showPending();
  if (ctl) { Atomics.add(ctl, CTL.TICK, 1); Atomics.notify(ctl, CTL.TICK); }   // display refresh: also a wake-up for the engine's pacing
  requestAnimationFrame(drawLoop);
}
requestAnimationFrame(drawLoop);
let memView = null, pcIndex = 0, crumbIndex = 0, stalledSeconds = 0;
const crumbNames = ["?", "draw", "vram_read", "vram_read_late_ok", "vram_write", "end_frame", "present_target", "readback", "readback_to", "draw_gl", "submit_gl", "queue_gl", "flush", "get_texture", "get_rt", "upload_from_vram", "post_pass", "taa_pass", "resolve_rt", "present_rgba", "sync_rows", "resolve_stencil", "purge_textures", "read_native_rgba", "read_scaled_rgba", "set_enhance", "capture_default_fb", "fallback_software", "verify_draw"];
const hex8 = v => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');
const crumbText = () => ' | GE list address ' + hex8(memView[crumbIndex]) + ', command ' + hex8(memView[crumbIndex + 1]) + ', last GL method ' + (crumbNames[memView[crumbIndex + 2]] || memView[crumbIndex + 2]) + ', marker ' + memView[crumbIndex + 3];
let fpsCount = 0, fpsT0 = performance.now();
setInterval(() => {
  const now = performance.now(), fps = fpsCount * 1000 / (now - fpsT0), rx = rxCount * 1000 / (now - fpsT0);
  fpsCount = 0; rxCount = 0; fpsT0 = now;
  if (running) {
    stalledSeconds = rx === 0 ? stalledSeconds + 1 : 0;
    if (stalledSeconds >= 3 && memView) {
      const pc = '0x' + (memView[pcIndex] >>> 0).toString(16).toUpperCase().padStart(8, '0');
      setStatus('The engine has stopped producing frames (stalled ' + stalledSeconds + ' s). Last game code address: ' + pc + crumbText());
      if (stalledSeconds === 3 || stalledSeconds % 30 === 0) log('STALL: no frames for ' + stalledSeconds + ' s, last game dispatch address ' + pc + crumbText());
    } else setStatus(rx.toFixed(1) + ' frames/s from the game, ' + fps.toFixed(1) + ' shown (the game targets 59.9)');
  }
}, 1000);
let worker = null, ctl = null, audioCtx = null, running = false;
const canvas = $('screen'), bmp = canvas.getContext('bitmaprenderer');
const logEl = $('log');
const logToConsole = /[?&]console/.test(location.search);
// (test automation) ?capture keeps every log line in window.rrCapture for the harness to drain; window.rrStats counts frames
const capture = /[?&]capture/.test(location.search);
const dropFrames = /[?&]dropframes/.test(location.search);
self.rrCapture = []; self.rrStats = {received: 0, drawn: 0, replaced: 0, raf: 0};
// The log is kept in memory (first 500 lines + last 20000) and drawn at most twice a second, only while the Log panel is
// open: rewriting a long <pre> on every line stalls the page (and with it the frame hand-over) for ~100 ms at a time.
const logHead = [], logTail = []; let logTailStart = 0, logDropped = 0, logDirty = false;
const logText = () => logHead.join('\n') + (logDropped ? '\n[... ' + logDropped + ' lines left out ...]' : '') + (logTail.length ? '\n' + logTail.slice(logTailStart).concat(logTail.slice(0, logTailStart)).join('\n') : '') + '\n';
const log = (t) => {
  if (logToConsole) console.log('[engine] ' + t);
  if (capture && self.rrCapture.length < 200000) self.rrCapture.push(t);
  if (logHead.length < 500) logHead.push(t);
  else if (logTail.length < 20000) logTail.push(t);
  else { logTail[logTailStart] = t; logTailStart = (logTailStart + 1) % logTail.length; ++logDropped; }
  logDirty = true;
};
setInterval(() => {
  if (!logDirty || !logEl.closest('details').open) return;
  logDirty = false;
  const all = logTail.length ? logTail.slice(logTailStart).concat(logTail.slice(0, logTailStart)) : logHead;
  logEl.textContent = (logTail.length ? '[showing the newest 300 lines - Copy log / Save log has everything]\n' : '') + all.slice(-300).join('\n') + '\n';
  logEl.scrollTop = logEl.scrollHeight;
}, 500);
logEl.closest('details').addEventListener('toggle', () => { logDirty = true; });
const setStatus = (t) => { $('status').textContent = t; };
const fail = (t) => { $('err').textContent = t; $('setup').style.display = ''; $('stage').style.display = 'none'; stopAll(); };

// ---- canvas size: render at the on-screen size (capped), the engine scales the 480x272 image to it
function fitSize() {
  if (!ctl) return;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  let w = Math.round(canvas.clientWidth * dpr);
  w = Math.max(480, Math.min(w, 1920));
  w &= ~1;
  const h = Math.round(w * 272 / 480);
  Atomics.store(ctl, CTL.W, w); Atomics.store(ctl, CTL.H, h);
}
window.addEventListener('resize', fitSize);

// ---- input
const kb = new Set();
const KEYBITS = {ArrowUp: 0x10, ArrowRight: 0x20, ArrowDown: 0x40, ArrowLeft: 0x80, Enter: 0x8, ShiftRight: 0x1, Backspace: 0x1,
  KeyQ: 0x100, KeyE: 0x200, KeyS: 0x1000, KeyX: 0x2000, KeyZ: 0x4000, KeyA: 0x8000};
const HOTKEYS = {F8: 1, F9: 2, F10: 4, F1: 8, End: 16};
const GAMEKEYS = new Set([...Object.keys(KEYBITS), 'KeyJ', 'KeyL', 'KeyI', 'KeyK', 'Escape', 'Space', ...Object.keys(HOTKEYS)]);
function pushTyped(unit) {
  const w = Atomics.load(ctl, CTL.TW);
  if (w - Atomics.load(ctl, CTL.TR) >= 32) return;
  Atomics.store(ctl, CTL.TYPED + (w & 31), unit);
  Atomics.store(ctl, CTL.TW, w + 1);
}
window.addEventListener('keydown', (e) => {
  if (!running) return;
  if (GAMEKEYS.has(e.code)) e.preventDefault();
  if (!e.repeat) {
    if (HOTKEYS[e.code]) Atomics.or(ctl, CTL.HOT, HOTKEYS[e.code]);
    if (e.code === 'Escape') pushTyped(27);
  }
  if (e.code === 'Enter') pushTyped(13);
  else if (e.code === 'Backspace') pushTyped(8);
  else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) pushTyped(e.key.charCodeAt(0));
  kb.add(e.code);
});
window.addEventListener('keyup', (e) => kb.delete(e.code));
// Mouse over the picture (server browser): position in 1/16 units of 480x272, presses and wheel steps as counters.
function pointerTo(e) {
  const r = canvas.getBoundingClientRect();
  if (r.width <= 0 || r.height <= 0) return;
  const x = (e.clientX - r.left) / r.width * 480, y = (e.clientY - r.top) / r.height * 272;
  const inside = x >= 0 && y >= 0 && x < 480 && y < 272;
  Atomics.store(ctl, CTL.MX, inside ? Math.floor(x * 16) : -1);
  Atomics.store(ctl, CTL.MY, inside ? Math.floor(y * 16) : -1);
}
canvas.addEventListener('pointermove', (e) => { if (ctl) pointerTo(e); });
canvas.addEventListener('pointerleave', () => { if (ctl) { Atomics.store(ctl, CTL.MX, -1); Atomics.store(ctl, CTL.MY, -1); } });
canvas.addEventListener('pointerdown', (e) => { if (!ctl || e.button !== 0) return; pointerTo(e); Atomics.add(ctl, CTL.CLICKS, 1); canvas.focus(); });
canvas.addEventListener('wheel', (e) => { if (!ctl) return; e.preventDefault(); Atomics.add(ctl, CTL.WHEEL, e.deltaY < 0 ? 1 : (e.deltaY > 0 ? -1 : 0)); }, {passive: false});
window.addEventListener('blur', () => kb.clear());

// Controllers.  Family (for the button pictures): 1 Xbox, 2 PlayStation, 3 Nintendo, 4 other (rr::pad::Family).
const padState = new Map();
let lastFamily = 0;
function padFamily(p) {
  const id = (p && p.id || '').toLowerCase();
  if (/xbox|xinput|x-box/.test(id)) return 1;
  if (/dualsense|dualshock|playstation|ps3|ps4|ps5/.test(id)) return 2;
  if (/switch|joy-con|joycon|pro controller|nintendo/.test(id)) return 3;
  const m = id.match(/vendor:\s*([0-9a-f]{4})/) || id.match(/^([0-9a-f]{1,4})-([0-9a-f]{1,4})-/);
  const v = m ? parseInt(m[1], 16) : -1;
  if (v === 0x045e) return 1;
  if (v === 0x054c) return 2;
  if (v === 0x057e) return 3;
  return 4;
}
const triggersOn = !/[?&]notriggers/.test(location.search);
// PSP bits of one controller.  "standard" mapping: buttons by position (0 south = CROSS, 1 east = CIRCLE, 2 west = SQUARE,
// 3 north = TRIANGLE), 4/5 shoulders, 6/7 triggers, 8 back, 9 start, 12-15 d-pad.  Other mappings (old browsers, unusual
// pads): the same first ten buttons, and the d-pad from a hat axis or axes 6/7 when present.
function padButtons(p) {
  const btn = (i) => !!(p.buttons[i] && (p.buttons[i].pressed || p.buttons[i].value > 0.5));
  let b = 0;
  if (btn(0)) b |= 0x4000; if (btn(1)) b |= 0x2000; if (btn(2)) b |= 0x8000; if (btn(3)) b |= 0x1000;
  if (btn(4)) b |= 0x100; if (btn(5)) b |= 0x200;
  if (btn(8)) b |= 0x1; if (btn(9)) b |= 0x8;
  if (p.mapping === 'standard') {
    if (btn(12)) b |= 0x10; if (btn(15)) b |= 0x20; if (btn(13)) b |= 0x40; if (btn(14)) b |= 0x80;
    if (btn(17)) b |= 0x1;   // PlayStation touchpad click
    if (triggersOn) { if (btn(7)) b |= 0x4000; if (btn(6)) b |= 0x8000; }
  } else {
    if (btn(12)) b |= 0x10; if (btn(15)) b |= 0x20; if (btn(13)) b |= 0x40; if (btn(14)) b |= 0x80;
    const ax = p.axes;
    if (ax.length >= 10 && Math.abs(ax[9]) <= 1.01 && ax[9] !== 0) {   // DirectInput hat as one axis: -1 up, then clockwise in steps of 2/7
      const h = Math.round((ax[9] + 1) * 3.5);
      if (h === 7 || h === 0 || h === 1) b |= 0x10; if (h >= 1 && h <= 3) b |= 0x20; if (h >= 3 && h <= 5) b |= 0x40; if (h >= 5 && h <= 7) b |= 0x80;
    } else if (ax.length >= 8) {
      if (ax[7] < -0.5) b |= 0x10; if (ax[7] > 0.5) b |= 0x40; if (ax[6] < -0.5) b |= 0x80; if (ax[6] > 0.5) b |= 0x20;
    }
  }
  return b;
}
window.addEventListener('gamepadconnected', (e) => { if (typeof log === 'function') log('[page] controller connected: ' + e.gamepad.id + ' (mapping ' + (e.gamepad.mapping || 'none') + ')'); });
window.addEventListener('gamepaddisconnected', (e) => { if (typeof log === 'function') log('[page] controller removed: ' + e.gamepad.id); });

function pollInput() {
  if (!running) return;
  let b = 0, kbits = 0;
  for (const k of kb) kbits |= KEYBITS[k] || 0;
  let lx = 128, ly = 128;
  if (kb.has('KeyJ')) lx = 0; if (kb.has('KeyL')) lx = 255; if (kb.has('KeyI')) ly = 0; if (kb.has('KeyK')) ly = 255;
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  let stickActive = false;
  for (const p of pads) {
    if (!p || !p.connected) continue;
    const pb = padButtons(p);
    const x = p.axes[0] || 0, y = p.axes[1] || 0, mag = Math.hypot(x, y);
    const st = padState.get(p.index) || {last: 0, stick: false};
    // the controller pressed or steered last decides the button pictures
    if ((pb && pb !== st.last) || (mag > 0.2 && !st.stick)) lastFamily = padFamily(p);
    st.last = pb; st.stick = mag > 0.2; padState.set(p.index, st);
    b |= pb;
    if (mag > 0.2 && !stickActive) {
      const k = Math.min(1, (mag - 0.2) / 0.8) / mag;
      lx = Math.max(0, Math.min(255, Math.round(128 + x * k * 127.5)));
      ly = Math.max(0, Math.min(255, Math.round(128 + y * k * 127.5)));
      stickActive = true;
    }
  }
  if (!pads || ![...pads].some(p => p && p.connected)) lastFamily = 0;
  else if (!lastFamily) lastFamily = padFamily([...pads].find(p => p && p.connected));
  Atomics.store(ctl, CTL.PAD, lastFamily | (stickActive ? 0x100 : 0));
  Atomics.store(ctl, CTL.BUTTONS, b | kbits); Atomics.store(ctl, CTL.GPAD, b); Atomics.store(ctl, CTL.LX, lx); Atomics.store(ctl, CTL.LY, ly);
  requestAnimationFrame(pollInput);
}

// ---- audio
const WORKLET = `
class RRPlayer extends AudioWorkletProcessor {
  constructor(o) { super(); this.ctl = new Int32Array(o.processorOptions.ctl); this.ring = new Int16Array(o.processorOptions.audio); }
  process(_in, outs) {
    const o = outs[0], L = o[0], R = o[1] || o[0], cap = this.ring.length >> 1;
    let r = Atomics.load(this.ctl, ${CTL.AR}); const w = Atomics.load(this.ctl, ${CTL.AW});
    for (let i = 0; i < L.length; ++i) {
      if (r < w) { const at = (r % cap) * 2; L[i] = this.ring[at] / 32768; R[i] = this.ring[at + 1] / 32768; ++r; }
      else { L[i] = 0; R[i] = 0; }
    }
    Atomics.store(this.ctl, ${CTL.AR}, r);
    Atomics.add(this.ctl, ${CTL.TICK}, 1); Atomics.notify(this.ctl, ${CTL.TICK});   // precise clock for the engine's frame pacing (every 128 samples)
    return true;
  }
}
registerProcessor('rr-player', RRPlayer);`;
async function startAudio(ctlSab, audioSab) {
  audioCtx = new AudioContext({sampleRate: 44100, latencyHint: 'interactive'});
  const url = URL.createObjectURL(new Blob([WORKLET], {type: 'text/javascript'}));
  await audioCtx.audioWorklet.addModule(url);
  const node = new AudioWorkletNode(audioCtx, 'rr-player', {outputChannelCount: [2], processorOptions: {ctl: ctlSab, audio: audioSab}});
  node.connect(audioCtx.destination);
  await audioCtx.resume();
}

// ---- start / stop
function stopAll() {
  running = false;
  if (ctl) Atomics.store(ctl, CTL.QUIT, 1);
  if (worker) { worker.terminate(); worker = null; }
  if (audioCtx) { audioCtx.close().catch(() => {}); audioCtx = null; }
}

startBtn.addEventListener('click', async () => {
  const file = isoInput.files[0];
  if (!file) return;
  $('err').textContent = '';
  startBtn.disabled = true;
  try {
    const ctlSab = new SharedArrayBuffer(4 * 64), audioSab = new SharedArrayBuffer(AUDIO_FRAMES * 2 * 2);
    ctl = new Int32Array(ctlSab);
    Atomics.store(ctl, CTL.MX, -1); Atomics.store(ctl, CTL.MY, -1);
    $('setup').style.display = 'none'; $('stage').style.display = 'flex';
    fitSize();
    await startAudio(ctlSab, audioSab);
    const saves = await loadSaves();
    worker = new Worker('engine-worker.js');
    worker.onmessage = (e) => {
      const m = e.data;
      if (m.t === 'frame') {
        // Keep only the newest frame and show it on the next display refresh (drawing the moment a frame arrives can put
        // two frames inside one refresh, which looks like stutter).  A frame that is replaced counts as consumed too.
        ++self.rrStats.received;
        if (dropFrames) { m.bm.close(); ack(); ++rxCount; return; }   // (test option ?dropframes: never show frames)
        if (pendingBmp) { ack(); ++self.rrStats.replaced; pendingBmp.close && pendingBmp.close(); }
        pendingBmp = m.bm; ++rxCount;
        if (performance.now() - lastRaf > 50) showPending();
      } else if (m.t === 'status') setStatus(m.text);
      else if (m.t === 'running') { running = true; setStatus(''); canvas.focus(); fitSize(); requestAnimationFrame(pollInput); }
      else if (m.t === 'mem') { memView = new Uint32Array(m.buf); pcIndex = m.pc >> 2; crumbIndex = m.crumb >> 2; }
      else if (m.t === 'log') log(m.text);
      else if (m.t === 'save') storeSaves(m.files);
      else if (m.t === 'title') document.title = m.text;
      else if (m.t === 'error') fail(m.text);
      else if (m.t === 'exit') { running = false; setStatus('The game has ended. Reload the page to play again.'); }
    };
    worker.onerror = (e) => fail('The engine stopped: ' + (e.message || 'unknown error') + '\nSee the log below, or the browser console.');
    // Test options (URL): ?args=--a b c (extra engine arguments, + for spaces), ?env=K=V,K2=V2 (environment for the engine; separate with | instead when a value holds commas), ?saves=DIR (start with the save folder DIR/ served next to the page, listed in DIR/list.txt),
    // ?replay=file.txt (a recorded input log served next to the page, played back by the engine)
    try { localStorage.setItem('rr.prompts', $('prompts').value); } catch (e) {}
    const q = new URLSearchParams(location.search);
    worker.postMessage({t: 'start', iso: file, ctl: ctlSab, audio: audioSab, saves, scale: +$('scale').value, debug: !q.has('quiet'),
                        args: (q.get('args') || '').split(' ').filter(Boolean).concat($('online').checked ? ['--online'] : [], ['--button-prompts', $('prompts').value], triggersOn ? [] : ['--no-triggers']), presentMode: +(q.get('presentmode') || 0), testSaves: q.get('saves') || '', env: q.get('env') || '', replay: q.get('replay') || ''});
    setStatus('Starting...');
  } catch (err) {
    fail(String((err && err.message) || err));
    startBtn.disabled = false;
  }
});
$('stop').addEventListener('click', () => { stopAll(); setStatus('Stopped. Reload the page to play again.'); });
$('full').addEventListener('click', () => { (canvas.requestFullscreen || (() => {})).call(canvas); });
$('mute').addEventListener('click', () => { if (ctl) Atomics.or(ctl, CTL.HOT, 1); canvas.focus(); });
canvas.addEventListener('dblclick', () => $('full').click());
document.addEventListener('fullscreenchange', () => setTimeout(fitSize, 100));
// A hidden or minimised tab can be slowed down by the browser; online races then catch up when it comes back, and the log
// shows when it happened.
let hiddenAt = 0;
document.addEventListener('visibilitychange', () => {
  if (!running) return;
  if (document.hidden) { hiddenAt = performance.now(); log('[page] tab hidden (the browser may slow the game down while it is not visible)'); }
  else log('[page] tab visible again' + (hiddenAt ? ' after ' + ((performance.now() - hiddenAt) / 1000).toFixed(1) + ' s' : ''));
});

$('copylog').addEventListener('click', async () => {
  try { await navigator.clipboard.writeText(logText()); $('copylog').textContent = 'Copied'; }
  catch (e) { logEl.textContent = logText(); const r = document.createRange(); r.selectNodeContents(logEl); const sel = getSelection(); sel.removeAllRanges(); sel.addRange(r); $('copylog').textContent = 'Selected - press Ctrl+C'; }
  setTimeout(() => { $('copylog').textContent = 'Copy log'; }, 2500);
});
$('savelog').addEventListener('click', () => {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([logText()], {type: 'text/plain'}));
  a.download = 'ridgeracer-web-log.txt';
  a.click();
});
