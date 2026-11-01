// Run on chrome://compass-sync of the new device (B) with --arg server=URL
// --arg code=7-482913. Starts joining and returns once B waits for A's
// answer (or failed). Check the outcome later with sync_status.js.
const $ = id => document.getElementById(id);
for (let i = 0; i < 120 && $('off').hidden; ++i) {
  await new Promise(r => setTimeout(r, 250));
}
// Clear what an earlier attempt left on the page.
if (!$('pairing').hidden) {
  $('cancelPairing').click();
  for (let i = 0; i < 40 && !$('pairing').hidden; ++i) {
    await new Promise(r => setTimeout(r, 250));
  }
}
$('server').value = ARGS.server;
$('showJoin').click();
$('code').value = ARGS.code;
$('join').click();
for (let i = 0; i < 240; ++i) {
  const message = $('pairingMessage').textContent;
  if (message && !$('pairing').hidden) {
    return {message, failed: $('pairingMessage').classList.contains('error')};
  }
  await new Promise(r => setTimeout(r, 250));
}
throw new Error('no progress: ' + $('pairingMessage').textContent);
