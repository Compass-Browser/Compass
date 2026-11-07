// Run on chrome://compass-command-bar opened in a tab (cdp.py run) with
// --arg query=TEXT. Queries the command bar through the page's own browser
// proxy and returns the "Tabs on other devices" rows: title,
// subtitle ("On <device> · <url>") and URL. With --arg want=REGEX, retries
// for up to 120 s until a row's title or subtitle matches (the other
// device's tabs arrive with the next sync).
const {CommandBarBrowserProxy} =
    await import('chrome://compass-command-bar/command_bar/browser_proxy.js');
const {ResultKind} =
    await import('chrome://compass-command-bar/command_bar.mojom-webui.js');
const proxy = CommandBarBrowserProxy.getInstance();
const query = () => new Promise(resolve => {
  const id = proxy.callbackRouter.setResults.addListener((text, results) => {
    if (text !== ARGS.query) {
      return;
    }
    proxy.callbackRouter.removeListener(id);
    resolve(results.filter(r => r.kind === ResultKind.kOtherDeviceTab)
                .map(r => ({title: r.title, subtitle: r.subtitle,
                            url: r.iconUrl})));
  });
  proxy.handler.query(ARGS.query, false);
});
const re = ARGS.want ? new RegExp(ARGS.want) : null;
for (let i = 0; i < 40; ++i) {
  const rows = await query();
  if (!re || rows.some(r => re.test(r.title) || re.test(r.subtitle))) {
    return rows;
  }
  await new Promise(r => setTimeout(r, 3000));
}
throw new Error('no row matches /' + ARGS.want + '/');
