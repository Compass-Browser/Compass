// Run on chrome://compass-sync (slice 4) with --arg type=Bookmarks (a
// switch's name as the page shows it) --arg on=true|false. Flips the switch
// if needed, waits for the browser to confirm, and returns every switch.
const $ = id => document.getElementById(id);
const want = ARGS.on !== 'false';
const find = () => Array.from($('types').querySelectorAll('li'))
                       .find(li => li.querySelector('.name').textContent ===
                                   ARGS.type);
for (let i = 0; i < 120 && (!find() || $('on').hidden); ++i) {
  await new Promise(r => setTimeout(r, 250));
}
const item = find();
if (!item || $('on').hidden) {
  throw new Error('no switch ' + ARGS.type + ' (is sync on?): ' +
                  $('status').textContent);
}
const toggle = item.querySelector('input');
if (toggle.checked !== want) {
  toggle.click();
}
// The page re-renders from the browser's state: wait until it agrees.
await new Promise(r => setTimeout(r, 1500));
for (let i = 0; i < 40 && find().querySelector('input').checked !== want;
     ++i) {
  await new Promise(r => setTimeout(r, 250));
}
return Object.fromEntries(
    Array.from($('types').querySelectorAll('li'))
        .map(li => [li.querySelector('.name').textContent,
                    li.querySelector('input').checked]));
