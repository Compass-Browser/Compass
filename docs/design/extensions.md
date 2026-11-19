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
  authors (uBlock Origin: the `.chromium.zip` of a GitHub release, loaded
  unpacked). A `.crx` downloaded from a site is still refused unless the
  install-sources policy allows it.
- They don't update on their own: an unpacked extension never does, and the
  store doesn't serve MV2 updates.
- Loading one unpacked still shows upstream's "Manifest version 2 is
  deprecated" warning (`extension.cc`).

## Tests

`CompassManifestV2Test` (`browser/extensions/manifest_v2_unittest.cc`),
with upstream's testing bypass off: nothing is affected, install and enable
aren't blocked, startup leaves an MV2 extension enabled. Upstream's
`manifest_v2_util_unittest.cc` and `manifest_v2_handler_*test.cc` test the
deprecation and fail against the override; Compass doesn't build upstream's
test suites.
