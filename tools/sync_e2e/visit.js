// Run on any page: `cdp.py PORT run URL visit.js` opens URL in a tab, which
// is the visit (it lands in history); this only reports what loaded.
return {url: location.href, title: document.title};
