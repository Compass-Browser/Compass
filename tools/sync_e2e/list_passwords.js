// Run on chrome://password-manager. Lists saved passwords (no values).
const list = await chrome.passwordsPrivate.getSavedPasswordList();
return list.map(e => ({username: e.username, urls: e.urls,
                       storedIn: e.storedIn, note: e.note}));
