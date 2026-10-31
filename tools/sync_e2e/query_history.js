// Run on chrome://history with --arg match=TEXT. Lists the history entries
// whose URL contains TEXT, as the page gets them (local and synced visits):
// URL, the device a synced visit came from, and each visit's timestamps.
const m = await import('chrome://history/history.js');
const handler = m.BrowserProxyImpl.getInstance().handler;
const {results} = await handler.queryHistory(
    '', 150, null, /*includeUserVisits=*/ true, /*includeActorVisits=*/ false);
return results.value.filter(e => e.url.includes(ARGS.match)).map(e => ({
  url: e.url,
  title: e.title,
  device: e.deviceName,
  deviceType: e.deviceType,
  timestamps: e.allTimestamps,
}));
