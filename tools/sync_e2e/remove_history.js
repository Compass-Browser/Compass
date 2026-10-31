// Run on chrome://history with --arg match=TEXT. Removes every visit whose
// URL contains TEXT, as the page's "Remove from history" does (one sync
// delete directive per entry), and returns what it removed.
const m = await import('chrome://history/history.js');
const handler = m.BrowserProxyImpl.getInstance().handler;
const {results} = await handler.queryHistory(
    '', 150, null, /*includeUserVisits=*/ true, /*includeActorVisits=*/ false);
const entries = results.value.filter(e => e.url.includes(ARGS.match));
const removalList = entries.flatMap(
    e => Object.entries(e.allTimestamps)
             .map(([url, timestamps]) => ({url, timestamps})));
if (removalList.length) {
  await handler.removeVisits(removalList);
}
return removalList;
