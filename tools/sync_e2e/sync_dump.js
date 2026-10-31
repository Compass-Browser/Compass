// Run on chrome://sync-internals (with --settle 5). Returns the "Dump status"
// JSON: transport state, types, Nigori/passphrase state, last errors.
document.querySelector('#dump-status').click();
await new Promise(r => setTimeout(r, 1000));
const text = document.querySelector('#status-text').value;
try {
  return JSON.parse(text.slice(text.indexOf('{')));
} catch (e) {
  return text;
}
