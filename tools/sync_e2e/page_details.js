// Run on chrome://compass-sync (slice 4). Returns what the full page shows
// beside the status: the server field (value and placeholder), the app
// menu's sync row for this state (<html data-menu-label>), the devices, the
// switches and the server line. With --arg wait=REGEX, first waits up to
// 120 s for the menu label, the status or a device line to match; with
// --arg devices=N, for at least N devices.
const $ = id => document.getElementById(id);
const read = () => ({
  status: $('status').textContent,
  menuLabel: document.documentElement.dataset.menuLabel || '',
  serverValue: $('server').value,
  serverPlaceholder: $('server').placeholder,
  visible: ['off', 'restartPanel', 'on', 'pairing', 'wordsPanel']
               .filter(id => !$(id).hidden),
  devices: Array.from($('devices').children).map(li => ({
    name: li.querySelector('.name').textContent,
    details: li.querySelector('.details').textContent,
    thisDevice: li.dataset.thisDevice === 'true',
  })),
  types: Object.fromEntries(
      Array.from($('types').querySelectorAll('li'))
          .map(li => [li.querySelector('.name').textContent,
                      li.querySelector('input').checked])),
  serverUrl: $('serverUrl').textContent,
});
const done = now => {
  if (ARGS.devices && now.devices.length < Number(ARGS.devices)) {
    return false;
  }
  if (!ARGS.wait) {
    return true;
  }
  const re = new RegExp(ARGS.wait);
  return re.test(now.menuLabel) || re.test(now.status) ||
      now.devices.some(d => re.test(d.name + ' ' + d.details));
};
for (let i = 0; i < 480; ++i) {
  const now = read();
  if (done(now)) {
    return now;
  }
  if (!ARGS.wait && !ARGS.devices) {
    return now;
  }
  await new Promise(r => setTimeout(r, 250));
}
throw new Error('timed out: ' + JSON.stringify(read()));
