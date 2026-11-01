// Run on chrome://compass-sync. Cancels (or closes) the pairing panel and
// waits for it to go; returns whether it went.
const $ = id => document.getElementById(id);
if (!$('pairing').hidden) {
  $('cancelPairing').click();
}
for (let i = 0; i < 40 && !$('pairing').hidden; ++i) {
  await new Promise(r => setTimeout(r, 250));
}
return {closed: $('pairing').hidden};
