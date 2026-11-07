// Run in an open page: cdp.py PORT in MATCH navigate.js --arg url=URL.
// Navigates that tab as following a link would, so the browser sees a real
// navigation (tools/sync_e2e/README.md). Returns at once; the page
// loads afterwards.
setTimeout(() => {
  location.href = ARGS.url;
}, 100);
return 'navigating to ' + ARGS.url;
