# chromium_src overrides

A file at `chromium_src/<path>` replaces `src/<path>` at compile time, without
editing the upstream file. Prefer an override to a patch whenever the change
can be expressed this way: overrides don't conflict when Chromium changes
nearby lines.

## How it works

`build/redirect_cc.py` wraps every C/C++/Objective-C compile (GN's
`cc_wrapper`, set by `strata gen`):

- **Source files** (`.cc`, `.mm`, …): if `chromium_src/<path>` exists, it is
  compiled instead of `src/<path>`.
- **Headers:** `chromium_src/` comes first on the include path, so
  `chromium_src/<path>.h` wins over `src/<path>.h` in every file that
  includes it.

An override almost always includes the original via the `src/` prefix, which
the wrapper puts on the include path:

```cpp
// chromium_src/chrome/browser/ui/browser.cc
#include "strata/browser/window_sync/live_tab_registry.h"

// Rename the upstream function so we can wrap it.
#define CreateBrowser CreateBrowser_ChromiumImpl
#include "src/chrome/browser/ui/browser.cc"
#undef CreateBrowser

Browser* Browser::CreateBrowser(const CreateParams& params) {
  Browser* browser = CreateBrowser_ChromiumImpl(params);
  strata::LiveTabRegistry::OnBrowserCreated(browser);
  return browser;
}
```

The `#define` trick also has to cover the declaration, so the matching header
usually needs a one-line override too. Keep overrides small and put real logic
in `//strata`.

## Rules

- Mirror the upstream path exactly. `strata build` warns about overrides whose
  upstream file doesn't exist. That usually means the file moved in a rebase,
  and the override is being silently ignored.
- Adding or deleting an override makes `strata build` touch the upstream file,
  so ninja rebuilds what depends on it. For a widely included header, that
  can be a large rebuild.
- Remote execution (`use_remoteexec`) isn't compatible with the wrapper, and
  is disabled in `config/args/common.gn`.
