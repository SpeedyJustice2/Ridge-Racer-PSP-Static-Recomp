// run.js - drives the browser build in headless Chromium for automated testing.
// node run.js --out DIR [--seconds 120] [--shot 5] [--query "debug&console"] [--saves DIR] [--keys "t:Key:dur,..."] [--iso rr.iso] [--port 8080]
//   Serves web-dist with the COOP/COEP headers (serve.py), loads the page, picks the disc image, presses Start, then every
//   --shot seconds saves a screenshot of the canvas and the status line; all engine log lines, page and worker console
//   messages and errors go to DIR/console.txt.  --keys presses keys at given seconds after start (e.g. "20:Enter:0.2,25:KeyZ:5").
const { chromium } = require('playwright-core');
const { spawn } = require('child_process');
const fs = require('fs'), path = require('path');
const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const out = arg('out', 'out'), seconds = +arg('seconds', '120'), shotEvery = +arg('shot', '5');
const keys = arg('keys', ''), iso = path.resolve(arg('iso', path.join(__dirname, 'rr.iso')));
const port = +arg('port', '8080'), dist = arg('dist', '/home/claude/rig/web-dist');
fs.mkdirSync(out, { recursive: true });
// --saves DIR: copy that save folder (e.g. /home/claude/rig/save) next to the page and start the game with it (?saves=testsave)
const savesDir = arg('saves', '');
let query = arg('query', 'capture');
if (savesDir) {
  const dst = path.join(dist, 'testsave'); fs.rmSync(dst, { recursive: true, force: true });
  const files = [];
  const walk = (d, rel) => { for (const n of fs.readdirSync(d)) { const p = path.join(d, n), r = rel ? rel + '/' + n : n; if (fs.statSync(p).isDirectory()) walk(p, r); else { fs.mkdirSync(path.dirname(path.join(dst, r)), { recursive: true }); fs.copyFileSync(p, path.join(dst, r)); files.push(r); } } };
  walk(savesDir, ''); fs.writeFileSync(path.join(dst, 'list.txt'), files.join('\n') + '\n');
  query += '&saves=testsave';
}
const logf = fs.createWriteStream(path.join(out, 'console.txt'));
const t0 = Date.now();
const L = (s) => { const line = `[${((Date.now() - t0) / 1000).toFixed(1)}s] ${s}`; logf.write(line + '\n'); if (/error|fail|stall|exception|abort|warn/i.test(s)) process.stdout.write(line.slice(0, 300) + '\n'); };

(async () => {
  const srv = spawn('python3', [path.join(dist, 'serve.py'), dist], { env: { ...process.env, PORT: String(port) }, stdio: 'ignore' });
  await new Promise(r => setTimeout(r, 800));
  const memlog = fs.createWriteStream(path.join(out, 'mem.txt'));
  const memTimer = setInterval(() => {   // resident memory of every Chromium process, every 10 s
    try {
      const ps = require('child_process').execSync("ps -eo rss,args | grep -E 'chrome-linux/chrome' | grep -v grep").toString().trim().split('\n');
      const parts = ps.map(l => { const m = l.trim().match(/^(\d+)\s+.*?--type=([\w-]+)/); return m ? m[2] + ':' + Math.round(+m[1] / 1024) : 'browser:' + Math.round(+l.trim().split(' ')[0] / 1024); });
      memlog.write(`[${((Date.now() - t0) / 1000).toFixed(0)}s] ${parts.join(' ')} MB\n`);
    } catch (e) {}
  }, 10000);
  const browser = await chromium.launch({
    logger: process.env.CHROMELOG ? { isEnabled: () => true, log: (n, sev, msg) => L('pw ' + n + ' ' + msg) } : undefined,
    env: { ...process.env },
    executablePath: process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome', headless: true,
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required',
           '--js-flags=--max-old-space-size=8192 ' + (process.env.JSFLAGS || ''), '--disable-dev-shm-usage', '--enable-logging=stderr', '--v=0'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  page.on('console', m => L(`console.${m.type()}: ${m.text()}`));
  page.on('pageerror', e => L(`PAGEERROR: ${e.message}`));
  page.on('crash', () => L('PAGE CRASHED'));
  page.on('worker', w => { L('worker started: ' + w.url()); w.on('close', () => L('worker closed')); });
  await page.goto(`http://127.0.0.1:${port}/index.html?${query}`);
  L('isolated=' + await page.evaluate(() => self.crossOriginIsolated) + ' webgl2=' + await page.evaluate(() => !!document.createElement('canvas').getContext('webgl2')));
  await page.selectOption('#scale', arg('scale', '1'));
  await page.setInputFiles('#iso', iso);
  await page.click('#start');
  L('started');
  const plan = keys.split(',').filter(Boolean).map(s => { const [t, k, d] = s.split(':'); return { t: +t, k, d: +(d || 0.15), done: false }; });
  let n = 0;
  const end = Date.now() + seconds * 1000;
  while (Date.now() < end) {
    const el = (Date.now() - t0) / 1000;
    for (const p of plan) if (!p.done && el >= p.t) { p.done = true; L(`key ${p.k} ${p.d}s`); page.keyboard.down(p.k).then(() => setTimeout(() => page.keyboard.up(p.k), p.d * 1000)); }
    await new Promise(r => setTimeout(r, shotEvery * 1000));
    try {
      const lines = await page.evaluate(() => self.rrCapture.splice(0));
      if (lines.length) fs.appendFileSync(path.join(out, 'engine.txt'), lines.join('\n') + '\n');
      const st = await page.evaluate(() => JSON.stringify(self.rrStats));
      L('STATS ' + st);
      const status = await page.textContent('#status'), err = await page.textContent('#err'), title = await page.title();
      L(`STATUS ${status} | ${err} | ${title}`);
      await page.locator('#screen').screenshot({ path: path.join(out, `shot_${String(++n).padStart(3, '0')}.png`) });
    } catch (e) { L('poll failed: ' + e.message); }
  }
  fs.writeFileSync(path.join(out, 'pagelog.txt'), await page.textContent('#log').catch(() => ''));
  clearInterval(memTimer); await browser.close(); srv.kill(); logf.end();
})().catch(e => { L('HARNESS ERROR ' + e.stack); process.exit(1); });
