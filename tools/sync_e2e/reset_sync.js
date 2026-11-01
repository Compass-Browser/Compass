// Run on chrome://compass-sync of a device that syncs. Clicks "Reset sync",
// confirms the dialog, returns the new chain's 12 recovery words, then
// clicks "I saved it" and waits for the restart prompt.
const $ = id => document.getElementById(id);
const until = async (test, what, seconds = 60) => {
  for (let i = 0; i < seconds * 4; ++i) {
    if (test()) return;
    await new Promise(r => setTimeout(r, 250));
  }
  throw new Error('timed out: ' + what + ' / status: ' +
                  $('status').textContent + ' / ' + $('onError').textContent);
};
await until(() => !$('on').hidden && !$('reset').disabled, 'sync on');
$('reset').click();
await until(() => $('confirmDialog').open, 'the confirm dialog', 10);
$('confirmOk').click();
await until(() => !$('wordsPanel').hidden || $('onError').textContent,
            'the new recovery code');
if ($('onError').textContent) {
  throw new Error($('onError').textContent);
}
const words = Array.from($('words').children).map(li => li.textContent);
$('saved').click();
await until(() => !$('restartPanel').hidden, 'the restart prompt');
return {words: words.join(' '), status: $('status').textContent};
