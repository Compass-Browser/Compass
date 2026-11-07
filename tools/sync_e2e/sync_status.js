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
  // "Expires in m:ss" while the code shows, else ''.
  expiry: $('pairingExpiry') && !$('pairingExpiry').hidden ?
      $('pairingExpiry').textContent :
      '',
  // The "Add a device" QR code's invite while the code shows, else null.
  qr: $('pairingQr') && !$('pairingQrPanel').hidden ?
      ($('pairingQr').dataset.invite || '') :
      null,
  // The app menu's sync row, e.g. "Synced" or "Sync: server full".
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
