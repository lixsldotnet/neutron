// Evaluate a JS expression in Steam's SharedJSContext over CEF remote debugging.
// Usage: node steamjs.mjs '<expression>'   (awaits promises, prints JSON)
//        node steamjs.mjs                  (lists the CEF targets)
// Needs Node 22 or newer (global WebSocket). The port comes from STEAM_CEF_PORT, which
// the Steam.app start script from install.sh sets, or from that start script itself.
import { readFileSync } from 'node:fs';
const START_SCRIPT = '/Applications/Steam.app/Contents/MacOS/steam_neutron';
// Start scripts from older installs only have the -devtools-port argument.
const port = process.env.STEAM_CEF_PORT || (() => {
  try {
    const m = readFileSync(START_SCRIPT, 'utf8').match(/^export STEAM_CEF_PORT=(\d+)$|-devtools-port (\d+)/m);
    return m && (m[1] || m[2]);
  } catch { return undefined; }
})();
if (!port) { console.error(`no CEF port: set STEAM_CEF_PORT or run ./install.sh (${START_SCRIPT})`); process.exit(1); }
const expr = process.argv[2];
const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
if (!expr) { for (const t of targets) console.log(t.title, '|', t.url); process.exit(0); }
const t = targets.find(t => t.title === (process.env.STEAM_CEF_TARGET || 'SharedJSContext')) ?? targets.find(t => /steamloopback/.test(t.url));
if (!t) { console.error('no SharedJSContext'); process.exit(1); }
const ws = new WebSocket(t.webSocketDebuggerUrl.replace(/127\.0\.0\.1:\d+|localhost:\d+/, `127.0.0.1:${port}`));
ws.onopen = () => ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate',
  params: { expression: `(async()=>JSON.stringify(await (${expr})))()`, awaitPromise: true, returnByValue: true } }));
ws.onmessage = m => { const r = JSON.parse(m.data); if (r.id !== 1) return;
  const v = r.result?.result; console.log(r.result?.exceptionDetails ? 'EXC ' + JSON.stringify(r.result.exceptionDetails.exception?.description) : v?.value);
  ws.close(); process.exit(0); };
