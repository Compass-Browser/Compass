// Run on chrome://compass-sync of a device that syncs (A). Clicks "Add a
// device" and returns the pairing code. Pairing goes on in the browser after
// the tab closes; answer with answer_device.js.
const $ = id => document.getElementById(id);
// Clear what an earlier pairing left on the page.
if (!$('pairing').hidden) {
  $('cancelPairing').click();
  for (let i = 0; i < 40 && !$('pairing').hidden; ++i) {
    await new Promise(r => setTimeout(r, 250));
  }
}
for (let i = 0; i < 120 && $('addDevice').disabled; ++i) {
  await new Promise(r => setTimeout(r, 250));
}
if ($('addDevice').disabled) {
  throw new Error('not syncing yet: ' + $('status').textContent);
}
$('addDevice').click();
for (let i = 0; i < 120; ++i) {
  if ($('pairingCode').textContent) {
    return {code: $('pairingCode').textContent,
            message: $('pairingMessage').textContent};
  }
  if ($('pairingMessage').classList.contains('error')) {
    throw new Error($('pairingMessage').textContent);
  }
  await new Promise(r => setTimeout(r, 250));
}
throw new Error('no code: ' + $('pairingMessage').textContent);
