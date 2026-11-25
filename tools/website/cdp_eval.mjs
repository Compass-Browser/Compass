// Evaluates JavaScript in one of a Compass instance's pages over the
// DevTools protocol, for tools/website/capture_mac.sh: the sidebar and
// command bar are browser_ui targets on --remote-debugging-port.
//
//   node cdp_eval.mjs <port> <url prefix> <expression>
//
// Prints the result (JSON) and exits non-zero on an exception.
const [port, prefix, expression] = process.argv.slice(2);
const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const target = targets.find((t) => t.url.startsWith(prefix));
if (!target) {
  console.error(`No target at ${prefix}: ${targets.map((t) => t.url).join(', ')}`);
  process.exit(1);
}
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
ws.send(JSON.stringify({
  id: 1, method: 'Runtime.evaluate',
  params: { expression, awaitPromise: true, returnByValue: true, userGesture: true },
}));
const reply = await new Promise((resolve) => {
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id === 1) resolve(d); };
});
ws.close();
if (reply.result?.exceptionDetails) {
  console.error(JSON.stringify(reply.result.exceptionDetails));
  process.exit(1);
}
console.log(JSON.stringify(reply.result?.result?.value ?? null));
