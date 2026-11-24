// Run on chrome://compass-sync of an instance that syncs on a server it
// owns, with [--arg name=NAME]. "Make an invite link" under People on your
// server; returns the link (docs/design/sync.md, "Inviting friends").
const $ = id => document.getElementById(id);
const until = async (test, what, seconds = 30) => {
  for (let i = 0; i < seconds * 4; ++i) {
    if (test()) return;
    await new Promise(r => setTimeout(r, 250));
  }
  throw new Error('timed out: ' + what + ' / ' + $('peopleError').textContent);
};
await until(() => !$('people').hidden, 'People on your server');
$('inviteName').value = ARGS.name || '';
$('invite').click();
await until(() => $('inviteLink').value || $('peopleError').textContent,
            'the invite link');
if ($('peopleError').textContent) {
  throw new Error($('peopleError').textContent);
}
return {link: $('inviteLink').value};
