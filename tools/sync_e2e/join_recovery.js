// Run on chrome://compass-sync with --arg server=URL --arg words="w1 ... w12".
// Joins the chain of a recovery code through "Use a recovery code".
const $ = id => document.getElementById(id);
const until = async (test, what, seconds = 30) => {
  for (let i = 0; i < seconds * 4; ++i) {
    if (test()) return;
    await new Promise(r => setTimeout(r, 250));
  }
  throw new Error('timed out: ' + what + ' / status: ' +
                  $('status').textContent + ' / ' + $('offError').textContent);
};
await until(() => !$('off').hidden, 'the "off" state');
$('server').value = ARGS.server;
$('showJoin').click();
$('showRecovery').click();
$('recovery').value = ARGS.words;
$('recover').click();
await until(() => !$('restartPanel').hidden || $('offError').textContent,
            'the restart prompt');
if ($('offError').textContent) {
  throw new Error($('offError').textContent);
}
return {status: $('status').textContent};
