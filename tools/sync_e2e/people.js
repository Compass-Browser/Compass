// Run on chrome://compass-sync. People on your server: whether it shows,
// and its rows ("name | details"), after [--arg wait=REGEX] matches one.
const $ = id => document.getElementById(id);
const rows = () => Array.from($('peopleList').children).map(
    li => li.querySelector('.name').textContent + ' | ' +
          li.querySelector('.details').textContent);
const wait = ARGS.wait ? new RegExp(ARGS.wait) : null;
for (let i = 0; i < 120; ++i) {
  if (!wait || rows().some(r => wait.test(r))) break;
  await new Promise(r => setTimeout(r, 250));
}
return {shown: !$('people').hidden, rows: rows(),
        error: $('peopleError').textContent};
