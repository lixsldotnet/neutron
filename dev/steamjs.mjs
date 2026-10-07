// Evaluate a JS expression in Steam's SharedJSContext over CEF remote debugging.
// Usage: node steamjs.mjs '<expression>'   (awaits promises, prints JSON)
const port = process.env.STEAM_CEF_PORT || 8080;
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
