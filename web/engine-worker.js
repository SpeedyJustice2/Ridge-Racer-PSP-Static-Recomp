// Runs the recompiled game.  Mounts the player's disc image as a read-only file system (the game's files are read straight
// out of the .iso on demand, nothing is copied), then starts RidgeRacer.js (the Emscripten module).  main() blocks this
// worker until the game ends, so everything the page needs to tell the engine goes through the shared control block.
'use strict';

function readIso(file) {
  const rd = new FileReaderSync();
  const bytes = (off, len) => new Uint8Array(rd.readAsArrayBuffer(file.slice(off, off + len)));
  const pvd = bytes(16 * 2048, 2048);
  const magic = String.fromCharCode(pvd[1], pvd[2], pvd[3], pvd[4], pvd[5]);
  if (pvd[0] !== 1 || magic !== 'CD001') throw new Error('That file is not a readable disc image (ISO 9660).');
  const u32 = (a, o) => (a[o] | (a[o + 1] << 8) | (a[o + 2] << 16) | (a[o + 3] << 24)) >>> 0;
  const files = [];
  const walk = (lba, size, prefix, depth) => {
    if (depth > 12) return;
    const dir = bytes(lba * 2048, size);
    let p = 0;
    while (p < size) {
      const len = dir[p];
      if (len === 0) { p = (Math.floor(p / 2048) + 1) * 2048; continue; }   // records never straddle a sector
      const nameLen = dir[p + 32];
      if (!(nameLen === 1 && (dir[p + 33] === 0 || dir[p + 33] === 1))) {
        let name = '';
        for (let i = 0; i < nameLen; ++i) name += String.fromCharCode(dir[p + 33 + i]);
        const semi = name.indexOf(';');
        if (semi >= 0) name = name.slice(0, semi);
        const eLba = u32(dir, p + 2), eSize = u32(dir, p + 10), isDir = (dir[p + 25] & 2) !== 0;
        if (isDir) walk(eLba, eSize, prefix + name + '/', depth + 1);
        else files.push({name: prefix + name, data: file.slice(eLba * 2048, eLba * 2048 + eSize), lba: eLba, size: eSize});
      }
      p += len;
    }
  };
  walk(u32(pvd, 156 + 2), u32(pvd, 156 + 10), '', 0);
  return {files, bytes};
}

onmessage = (e) => {
  const m = e.data;
  if (m.t !== 'start') return;
  try {
    postMessage({t: 'status', text: 'Reading the disc image...'});
    const iso = readIso(m.iso);
    const boot = iso.files.find(f => f.name.toUpperCase() === 'PSP_GAME/SYSDIR/BOOT.BIN');
    if (!boot) throw new Error('PSP_GAME/SYSDIR/BOOT.BIN was not found on that disc image.');
    const head = iso.bytes(boot.lba * 2048, 4);
    if (head[0] === 0x7E && head[1] === 0x50 && head[2] === 0x53 && head[3] === 0x50)
      throw new Error("This disc image is still encrypted ('~PSP'). The game needs a decrypted copy of your own game's executable.");
    if (!(head[0] === 0x7F && head[1] === 0x45 && head[2] === 0x4C && head[3] === 0x46))
      throw new Error('BOOT.BIN is not an ELF executable (expected a decrypted copy).');

    postMessage({t: 'status', text: 'Loading the game code (this is a large file)...'});
    importScripts('RidgeRacer.js');
    const args = ['/game/PSP_GAME/SYSDIR/BOOT.BIN', '/game', '--gpu', '--internal-scale', String(m.scale || 2)];
    if (!m.debug) args.push('--quiet');
    let replayText = null;
    if (m.replay) {   // (test option) an input log to play back
      const x = new XMLHttpRequest(); x.open('GET', m.replay, false); x.send();
      if (x.status === 200) { replayText = x.responseText; args.push('--replay-input', '/replay.txt'); }
      else postMessage({t: 'log', text: 'replay file not found: ' + m.replay});
    }
    for (const a of (m.args || [])) args.push(a);
    let saves = m.saves || [];
    if (m.testSaves) {   // (test option ?saves=DIR) start with the save folder DIR/ next to the page (DIR/list.txt names its files)
      saves = [];
      const get = (u, bin) => { const x = new XMLHttpRequest(); x.open('GET', u, false); if (bin) x.responseType = 'arraybuffer'; x.send(); return x.status === 200 ? x.response : null; };
      const list = get(m.testSaves + '/list.txt', false);
      if (list === null) postMessage({t: 'log', text: 'test saves not found: ' + m.testSaves});
      for (const f of String(list || '').split('\n').map(l => l.trim()).filter(Boolean)) {
        const data = get(m.testSaves + '/' + f, true);
        if (data) saves.push({path: '/savedata/' + f, data: new Uint8Array(data)});
      }
      postMessage({t: 'log', text: 'test saves: ' + saves.length + ' file(s) from ' + m.testSaves});
    }
    RRModule({
      // threads (the online helper) start from the engine's own script, not from this worker's
      mainScriptUrlOrBlob: new URL('RidgeRacer.js', self.location.href).href,
      rrCtl: new Int32Array(m.ctl),
      rrPresentMode: m.presentMode | 0,
      rrAudio: new Int16Array(m.audio),
      arguments: args,
      print: (t) => postMessage({t: 'log', text: String(t)}),
      printErr: (t) => postMessage({t: 'log', text: String(t)}),
      preRun: [(M) => {
        const FS = M.FS;
        if (replayText !== null) FS.writeFile('/replay.txt', replayText);
        for (const kv of String(m.env || '').split(String(m.env || '').includes('|') ? '|' : ',').filter(Boolean)) { const i = kv.indexOf('='); if (i > 0) M.ENV[kv.slice(0, i)] = kv.slice(i + 1); }
        FS.mkdir('/game');
        FS.mount(FS.filesystems.WORKERFS, {blobs: iso.files.map(f => ({name: f.name, data: f.data}))}, '/game');
        for (const s of saves) {
          const dir = s.path.slice(0, s.path.lastIndexOf('/'));
          FS.mkdirTree(dir);
          FS.writeFile(s.path, s.data);
        }
        postMessage({t: 'status', text: ''});
        postMessage({t: 'running'});
      }],
      onExit: (code) => postMessage({t: 'exit', code}),
    }).catch((err) => postMessage({t: 'error', text: String((err && err.message) || err)}));
  } catch (err) {
    postMessage({t: 'error', text: String((err && err.message) || err)});
  }
};
