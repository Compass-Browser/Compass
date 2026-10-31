// Run on chrome://compass-sync of A with --arg answer=allow|deny. Waits (up
// to 120 s) for "Add <device>?", answers, and returns the device name and
// the outcome.
const $ = id => document.getElementById(id);
for (let i = 0; i < 480 && $('confirm').hidden; ++i) {
  await new Promise(r => setTimeout(r, 250));
}
if ($('confirm').hidden) {
  throw new Error('nobody asked to join: ' + $('pairingMessage').textContent);
}
const device = $('deviceName').textContent;
$(ARGS.answer === 'deny' ? 'deny' : 'allow').click();
for (let i = 0; i < 120 && !$('confirm').hidden; ++i) {
  await new Promise(r => setTimeout(r, 250));
}
await new Promise(r => setTimeout(r, 2000));
return {device, message: $('pairingMessage').textContent};
