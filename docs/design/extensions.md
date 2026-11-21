# Extensions: Manifest V2

Compass keeps Manifest V2 extensions working: they install (unpacked, or a
`.crx` dropped on chrome://extensions in developer mode) and run, and the
extensions page shows no "no longer supported" notices. uBlock Origin, the
full one, is the usual reason.

## How

Chromium 157 still loads and runs MV2: `extension.cc` accepts manifest
versions 2 to 3, and MV2's APIs are all there (blocking `webRequest`,
persistent background pages, `browser_action`, `page_action`,
`tabs.executeScript`). What stops them is `ManifestV2Handler`
(`extensions/browser/manifest_v2_handler.cc`), which blocks installing and
enabling them and disables those it finds at startup. Every decision it makes
goes through `manifest_v2_util::IsExtensionAffected()`: the install and enable
checks, the startup pass, re-enabling, the extensions page's banners. Compass
overrides that function (`chromium_src/extensions/browser/manifest_v2_util.cc`)
to say nothing is affected, so:

- MV2 extensions install and enable;
- those upstream disabled with `DISABLE_UNSUPPORTED_MANIFEST_VERSION` come
  back at the next start (`ManifestV2Handler::CheckDisabledExtensions()`);
- no deprecation banners.

## Limits

- The Chrome Web Store no longer lists MV2 extensions: they come from their
  authors (uBlock Origin: the `.chromium.crx` or `.chromium.zip` of a GitHub
  release). See "Installing a .crx".
- They don't update on their own: an unpacked extension never does, and the
  store doesn't serve MV2 updates.
- Loading one unpacked still shows upstream's "Manifest version 2 is
  deprecated" warning (`extension.cc`).

## Installing a .crx

A `.crx` signed by its author rather than the Web Store has no Web Store
"publisher proof". Upstream installs it without one only when it's dropped
on chrome://extensions in developer mode
(`developer_private_functions.cc`: `OffStoreInstallAllowedFromSettingsPage`)
or when the `ExtensionInstallSources` policy allows the site; a `.crx`
downloaded from a link otherwise fails with `CRX_REQUIRED_PROOF_MISSING`.

- **Developer mode allows downloads too**
  (`patches/chrome-browser-download-download_crx_util.cc.patch`):
  `OffStoreInstallAllowedByPrefs()` is also true while
  `extensions.ui.developer_mode` is on, so a `.crx` from a link installs (with
  the usual install prompt) like a dropped one. It's the same trust decision,
  without the trip through Finder.
- **The refusal says what to do**
  (`patches/extensions-browser-sandboxed_unpacker.cc.patch`): instead of
  "Package is invalid: 'CRX_REQUIRED_PROOF_MISSING'", "This extension isn't
  from the Chrome Web Store, so Compass installs it only with Developer mode
  on. Turn on Developer mode at the top right of chrome://extensions, then
  open the file again (or drag it onto that page)."
- A `.crx` whose manifest has an `update_url` (uBlock Origin's GitHub builds
  point at gorhill's feed) updates from there: updates of extensions not from
  the store are checked with `GetPolicyVerifierFormat()`, which needs no
  publisher proof.
- Tests: `CompassCrxDownloadTest` (`browser/extensions/crx_download_unittest.cc`).

## Tests

`CompassManifestV2Test` (`browser/extensions/manifest_v2_unittest.cc`),
with upstream's testing bypass off: nothing is affected, install and enable
aren't blocked, startup leaves an MV2 extension enabled. Upstream's
`manifest_v2_util_unittest.cc` and `manifest_v2_handler_*test.cc` test the
deprecation and fail against the override; Compass doesn't build upstream's
test suites.
