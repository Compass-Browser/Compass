// Run on chrome://compass-sync of a device joining a chain that has other
// profiles. Waits (up to 120 s) for the mapping step, then
// confirms it: the proposed mapping, or with --arg map="NAME=CHAIN NAME;..."
// the given choices ("new" for a new profile of the chain). Returns what was
// proposed and chosen, and the outcome.
const $ = id => document.getElementById(id);
for (let i = 0; i < 480 && $('mapping').hidden; ++i) {
  await new Promise(r => setTimeout(r, 250));
}
if ($('mapping').hidden) {
  throw new Error('no mapping asked: ' + $('status').textContent + ' / ' +
                  $('pairingMessage').textContent);
}
// The rows: "<local name> syncs with <select>".
const rows = Array.from($('mappingRows').querySelectorAll('label'));
const choice = row => {
  const select = row.querySelector('select');
  return select.options[select.selectedIndex].textContent;
};
const local = row => row.firstChild.textContent.replace(/ syncs with $/, '');
const proposed = Object.fromEntries(rows.map(row => [local(row), choice(row)]));
if (ARGS.map) {
  for (const pair of ARGS.map.split(';')) {
    const [name, target] = pair.split('=');
    const row = rows.find(r => local(r) === name.trim());
    if (!row) {
      throw new Error('no local profile ' + name + ' in ' +
                      JSON.stringify(proposed));
    }
    const select = row.querySelector('select');
    const option = Array.from(select.options).find(
        o => target.trim() === 'new' ? o.value === '' :
                                       o.textContent === target.trim());
    if (!option) {
      throw new Error('no chain profile ' + target);
    }
    select.value = option.value;
  }
}
const chosen = Object.fromEntries(rows.map(row => [local(row), choice(row)]));
$('mapProfiles').click();
for (let i = 0; i < 120; ++i) {
  await new Promise(r => setTimeout(r, 250));
  if ($('mappingError').textContent) {
    throw new Error($('mappingError').textContent);
  }
  if ($('mapping').hidden) {
    return {proposed, chosen, status: $('status').textContent,
            pairing: $('pairingMessage').textContent};
  }
}
throw new Error('the mapping stayed: ' + $('status').textContent);
