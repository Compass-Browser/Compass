// Run on chrome://compass-sync with --arg server=URL. Clicks "Start sync",
// returns the 12 recovery words, then clicks "I saved it".
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
$('start').click();
await until(() => !$('wordsPanel').hidden || $('offError').textContent,
            'the recovery code');
if ($('offError').textContent) {
  throw new Error($('offError').textContent);
}
const words = Array.from($('words').children).map(li => li.textContent);
$('saved').click();
await until(() => !$('restartPanel').hidden, 'the restart prompt');
return {words: words.join(' '), status: $('status').textContent};
