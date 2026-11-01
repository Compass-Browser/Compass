// Run on chrome://compass-sync. Returns what the page shows: the status
// line, which sections are visible, and the pairing panel. With
// --arg wait=REGEX, first waits up to 120 s for the status line or the
// pairing message to match.
const $ = id => document.getElementById(id);
const read = () => ({
  status: $('status').textContent,
  visible: ['off', 'restartPanel', 'on', 'pairing', 'confirm', 'mapping']
               .filter(id => !$(id).hidden),
  code: $('pairingCode').textContent,
  pairing: $('pairingMessage').textContent,
  device: $('deviceName').textContent,
  // The app menu's sync row (slice 4), e.g. "Sync: server full" (slice 7).
  menu: document.documentElement.dataset.menuLabel,
});
if (ARGS.wait) {
  const re = new RegExp(ARGS.wait);
  for (let i = 0; i < 480; ++i) {
    const now = read();
    if (re.test(now.status) || re.test(now.pairing) || re.test(now.menu) ||
        re.test(now.visible.join(' '))) {
      return now;
    }
    await new Promise(r => setTimeout(r, 250));
  }
  throw new Error('timed out waiting for /' + ARGS.wait + '/: ' +
                  JSON.stringify(read()));
}
return read();
