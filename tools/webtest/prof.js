// prof.js: like run.js (no screenshots) but takes a CPU profile of the engine worker from --at to --at+--len seconds.
const { chromium } = require('playwright-core');
const { spawn } = require('child_process');
const WebSocket = require('ws');
const fs = require('fs'), path = require('path');
const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const at = +arg('at', '40'), len = +arg('len', '20'), out = arg('out', 'prof'), query = arg('query', 'quiet');
const dist = '/home/claude/rig/web-dist';
fs.mkdirSync(out, { recursive: true });
(async () => {
  const srv = spawn('python3', [path.join(dist, 'serve.py'), dist], { env: { ...process.env, PORT: '8081' }, stdio: 'ignore' });
  await new Promise(r => setTimeout(r, 800));
  const browser = await chromium.launch({ executablePath: process.env.CHROME || '/opt/pw-browsers/chromium-1194/chrome-linux/chrome', headless: true,
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--remote-debugging-port=9333', '--disable-dev-shm-usage'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  await page.goto('http://127.0.0.1:8081/index.html?' + query);
  await page.selectOption('#scale', '1');
  await page.setInputFiles('#iso', path.join(__dirname, 'rr.iso'));
  await page.click('#start');
  await new Promise(r => setTimeout(r, at * 1000));
  const ver = await (await fetch('http://127.0.0.1:9333/json/version')).json();
  const ws = new WebSocket(ver.webSocketDebuggerUrl);
  await new Promise(r => ws.on('open', r));
  let id = 0; const pending = new Map(); let workerSession = null;
  ws.on('message', (d) => { const m = JSON.parse(d); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
  const send = (method, params = {}, sessionId) => new Promise(res => { const i = ++id; pending.set(i, res); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
  const targets = (await send('Target.getTargets')).result.targetInfos;
  const w = targets.find(t => t.type === 'worker');
  console.log('worker target', w && w.url);
  const att = await send('Target.attachToTarget', { targetId: w.targetId, flatten: true });
  workerSession = att.result.sessionId;
  await send('Profiler.enable', {}, workerSession);
  await send('Profiler.setSamplingInterval', { interval: 500 }, workerSession);
  await send('Profiler.start', {}, workerSession);
  await new Promise(r => setTimeout(r, len * 1000));
  const prof = (await send('Profiler.stop', {}, workerSession)).result.profile;
  fs.writeFileSync(path.join(out, 'worker.cpuprofile'), JSON.stringify(prof));
  // self time per function
  const self = new Map(), byId = new Map(prof.nodes.map(n => [n.id, n]));
  const dt = {}; prof.samples.forEach((s, i) => { dt[s] = (dt[s] || 0) + (prof.timeDeltas[i] || 0); });
  for (const n of prof.nodes) { const k = n.callFrame.functionName || '(anon)'; self.set(k, (self.get(k) || 0) + (dt[n.id] || 0)); }
  const total = [...self.values()].reduce((a, b) => a + b, 0);
  const top = [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 40);
  const txt = top.map(([k, v]) => (100 * v / total).toFixed(1).padStart(5) + '%  ' + k).join('\n');
  fs.writeFileSync(path.join(out, 'top.txt'), txt); console.log(txt);
  await browser.close(); srv.kill(); process.exit(0);
})().catch(e => { console.error(e); process.exit(1); });
