// Run on chrome://sync-internals. Same as its "Trigger GetUpdates" button:
// asks the engine to fetch from the server now instead of at the next poll.
chrome.send('triggerRefresh');
return 'refresh requested';
