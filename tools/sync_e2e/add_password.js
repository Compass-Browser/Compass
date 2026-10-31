// Run on chrome://password-manager with --arg tag=NAME. Saves a password for
// https://example.com/compass-sync-e2e/NAME, user "e2e-NAME", in the profile
// (device) store, which local sync uses.
await chrome.passwordsPrivate.addPassword({
  url: 'https://example.com/compass-sync-e2e/' + ARGS.tag,
  username: 'e2e-' + ARGS.tag,
  password: 'e2e-Passw0rd!-' + ARGS.tag,
  note: 'compass sync e2e',
  useAccountStore: false,
});
return 'added';
