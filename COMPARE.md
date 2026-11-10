# Compass compared with other browsers

How Compass compares with Arc, its successor Dia, and the browsers people
move to from Arc. Written on 10 November 2026: other browsers change
quickly, so check their sites before relying on a cell.

- The Compass column comes from this repository's code (`main`).
- The Crest column comes from its site, changelog and repository.
- The other columns come from public reviews and the vendors' sites.

✅ yes · ~ partial or different · ❌ no · ? not confirmed by any source found ·
— not applicable

## Arc's status

The Browser Company announced on 22 October 2026 that Arc **stops working
on 22 December 2026**: no updates, and all accounts deactivated. New Arc
accounts are already blocked, Arc Search loses its AI features on 23
November, and account data requests close on 15 December. The team's
successor is Dia (The Browser Company is part of Atlassian since October
2025).

## Platform and basics

| Browser | Engine | Platforms | License / price | Status |
|---|---|---|---|---|
| **Compass** | Chromium | macOS 13+ (stable), Windows 10/11 (beta), iPhone and iPad (TestFlight), Android (in progress), Linux (packages build, not published) | BSD-3-Clause, free | Active |
| **Crest** | Chromium by default on Mac, WebKit for any site; WebKit only on iPhone/iPad | macOS 26.1+ (Apple silicon), iPhone, iPad | MPL-2.0, free | Early access (0.8.x) |
| **Arc** | Chromium | macOS, Windows; Arc Search on iOS/Android | Free, closed | Shuts down 22 Dec 2026 |
| **Dia** | Chromium | macOS (Apple silicon) | Free core features, paid Pro tier, closed | Active, adding Arc features |
| **Phi** | Chromium, native AppKit/SwiftUI shell | macOS | Apache-2.0, free | Active |
| **Zen** | Firefox | macOS, Windows, Linux | MPL-2.0, free | Active |
| **Vivaldi** | Chromium | Desktop, Android, iOS | Free, UI closed | Active |
| **Edge** | Chromium | All | Free, closed | Active |
| **SigmaOS** | WebKit | macOS | Free + paid | Active |
| **Lilo** | WebKit (runs Chrome extensions) | macOS | Open source, free | Active |

## Features

| Feature | Compass | Crest | Arc | Dia | Phi | Zen | Vivaldi | Edge | SigmaOS | Lilo |
|---|---|---|---|---|---|---|---|---|---|---|
| **Sidebar and tabs** | | | | | | | | | | |
| Spaces with themes | ✅ colors, gradients, grain, icons | ✅ Crest Studio emblems, gradients | ✅ | ~ | ✅ | ✅ | ✅ | ~ | ✅ | ✅ |
| Separate profile / cookies per space | ✅ switched in the same window | ✅ plus Sub-Spaces that share sign-ins | ✅ | ? | ✅ | ✅ containers | ❌ | ❌ | ? | ✅ |
| Locked private spaces | ~ incognito lock on iOS and Android | ✅ Face ID / Touch ID | ❌ | ? | ? | ❌ | ❌ | ❌ | ? | ? |
| Favorites shared across spaces | ✅ | ? | ✅ | ? | ? | ✅ Essentials | ~ | ❌ | ? | ? |
| Pinned tabs in nested folders | ✅ in Pinned and Today | ✅ | ✅ | ✅ pinned | ✅ groups | ✅ | ~ stacks | ~ groups | ✅ | ✅ |
| Auto-archive of unused tabs | ✅ Today, 12 h default | ✅ Space cleanup, 12 h–30 days | ✅ | ? | ? | ❌ | ❌ | ❌ | ~ | ? |
| Rename tabs, mute entries, undo close | ✅ | ~ | ✅ | ? | ? | ✅ | ✅ | ~ | ? | ? |
| Split view | ✅ up to 4 panes, side by side or stacked | ✅ 2–4 panes | ✅ up to 4 | ? | ✅ side by side or stacked | ✅ up to 4 | ✅ tiling | ✅ | ✅ | ? |
| Tree-style tabs | ~ nested folders, made by hand | ~ | ~ | ❌ | ❌ | ~ | ~ | ❌ | ~ | ~ |
| Custom shortcuts | ✅ | ✅ | ✅ | ✅ | ? | ✅ | ✅ | ❌ | ? | ? |
| **Arc-style extras** | | | | | | | | | | |
| Command bar, no toolbar | ✅ | ✅ | ✅ | ~ | ? | ✅ | ✅ Quick Commands | ❌ | ✅ | ? |
| Peek (preview a link without a tab) | ✅ | ✅ | ✅ | ? | ? | ✅ Glance | ❌ | ❌ | ? | ? |
| Small windows for links from other apps | ✅ macOS | ✅ Quick Window | ✅ Little Arc | ? | ? | ❌ | ❌ | ❌ | ? | ? |
| Link routing to spaces (Air Traffic Control) | ❌ | ✅ | ✅ | ? | ? | ❌ | ❌ | ❌ | ? | ? |
| Auto picture-in-picture | ✅ including Teams, Meet and Zoom calls | ✅ | ✅ | ✅ Meet | ? | ~ | ? | ~ | ? | ? |
| Live folders | ✅ JavaScript plugins (RSS, GitHub, web list, calendar) | ? | ✅ | ❌ | ❌ | ? | ~ feed reader | ❌ | ❌ | ❌ |
| Per-site CSS/JS (Boosts) | ❌ | ❌ | ✅ | ❌ | ❌ | ✅ Mods | ❌ | ❌ | ❌ | ❌ |
| Notes / canvas (Easels) | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ |
| Translucent sidebar | ❌ | ? | ✅ | ? | ? | ? | ❌ | ❌ | ? | ? |
| **Web content** | | | | | | | | | | |
| Chrome extensions, desktop / phone | ✅ / ✅ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ❌ Firefox add-ons | ✅ / ❌ | ✅ / ❌ | ~ / ❌ | ✅ / ❌ |
| Built-in ad blocker | ❌ (uBlock Origin Lite as an extension) | ~ WebKit pages and mobile | ❌ | ❌ | ? | ❌ | ✅ | ~ | ✅ | ? |
| Reader mode, on-device translation | ❌ | ✅ WebKit pages | ❌ | ? | ? | ✅ | ✅ | ✅ | ? | ? |
| Full-quality DRM video | ❌ no Widevine VMP | ✅ FairPlay through WebKit | ✅ | ✅ | ? | ~ | ✅ | ✅ | ✅ | ✅ |
| Mail, calendar, web panels | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ~ | ❌ | ❌ |
| **AI** | | | | | | | | | | |
| AI chat panel that reads and acts on the page | ✅ Claude | ❌ | ~ Arc Max | ✅ | ✅ | ❌ | ❌ by choice | ✅ Copilot | ✅ Airis | ? |
| Agent across tabs and windows | ✅ off by default | ❌ | ❌ | ✅ | ✅ | ❌ | ❌ | ✅ | ? | ? |
| MCP server for coding agents | ✅ macOS and Windows | ✅ Mac, per chosen space | ❌ | ? | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ |
| Own API key, Bedrock/Vertex, or Claude subscription via Claude Code | ✅ | — | ❌ | ❌ | ❌ | — | — | ❌ | ❌ | ? |
| AI memory across sessions | ✅ desktop and iPhone, synced end-to-end encrypted, each save shown with Undo | ❌ | ❌ | ✅ | ✅ kept on the Mac | ❌ | ❌ | ✅ | ? | ? |
| **Sync and devices** | | | | | | | | | | |
| Sync | ✅ no account, end-to-end encrypted, self-hostable | ✅ iCloud; passwords through iCloud Keychain | Arc account | account | data stays local | Firefox account | Vivaldi account | Microsoft account | ? | ? |
| Send tab to phone | ✅ | ? | ❌ | ? | ? | ✅ | ✅ | ✅ | ❌ | ? |
| Phone app with the same spaces | ✅ iPhone (beta), Android (in progress) | ✅ iPhone and iPad (App Store) | ~ Arc Search is separate | in progress | ❌ | ❌ | ✅ | ✅ | ❌ | ? |
| **Moving in** | | | | | | | | | | |
| Import | Arc (macOS): profiles, cookies, sidebar, archive | Arc, Zen, Chrome, Safari, Firefox, Brave, Edge, Vivaldi, Opera, Dia, Comet and more | — | Arc | ? | ? | ~ | ~ | ? | ? |
| Open source | ✅ | ✅ | ❌ | ❌ | ✅ | ✅ | ~ | ❌ | ❌ | ✅ |

## What only Compass has

As far as the sources show:

- Sync without an account, end-to-end encrypted, on a server anyone can run.
- Claude built in, with the user's own key, a compatible gateway, or a
  Claude subscription through Claude Code; the same tools as an MCP server.
- Live folders as JavaScript plugins meant to run on desktop, Android and
  iPhone.
- Chrome Web Store extensions on the phone (uBlock Origin Lite and
  1Password tested; on iPhone, 1Password unlocks with Face ID).
- Picture-in-picture for Teams, Meet and Zoom calls, with their controls.

## Compass and Crest

Crest is the closest project to Compass: also open source, also an Arc
replacement, also with spaces, Peek, small windows, splits and an MCP server.

Crest has, Compass doesn't:

- Link routing to spaces (Air Traffic Control).
- A WebKit engine per site: full-quality DRM video, Reader mode, on-device
  translation and built-in blocking on those pages.
- Import from almost every browser, not only Arc.
- Private Spaces locked with Face ID or Touch ID, and Sub-Spaces.
- An App Store release for iPhone and iPad, and a Homebrew cask.

Compass has, Crest doesn't:

- Windows, Linux and Android.
- Account-free, end-to-end encrypted, self-hostable sync (Crest uses
  iCloud).
- A Claude chat panel in the browser (Crest has only the MCP server).
- Chrome extensions on the phone.
- Live folders, call picture-in-picture, send tab to phone, favorites
  shared across spaces, renaming tabs.

## Gaps

Features other browsers have that Compass doesn't, checked against the code:

| Missing | Who has it | State in Compass |
|---|---|---|
| Link routing to spaces | Arc, Crest | Next after little windows (docs/design/shortcuts.md, little-window.md "Not done") |
| Per-site CSS/JS | Arc (Boosts), Zen (Mods) | Not started |
| Built-in ad blocker | Vivaldi, SigmaOS, Crest (WebKit pages), Brave | Not started; extensions instead |
| Reader mode, on-device translation | Crest, Zen, Vivaldi, Edge | Not started |
| Full-quality DRM video | Chrome, Edge, Arc, Dia, Crest | Blocked: no Widevine VMP signature (docs/design/widevine.md) |
| Import from browsers other than Arc | Crest | Not started |
| Notes / canvas | Arc, Vivaldi | Not started |
| Tree-style tabs | Vivaldi, Floorp | Partial: folders nest in Pinned and Today, but a new tab doesn't go under the tab that opened it |
| Translucent sidebar | Arc | Arc's gradients import; translucency and vibrancy don't (docs/design/arc-import.md) |
| Linux download | Zen, Vivaldi, Edge | Packages build (`.tar.xz`, Flatpak); not on the website |
| Little windows and Arc import on Windows | Arc | Little windows written, not tried; Arc import macOS only |

## Sources

- Arc: [Android Authority](https://www.androidauthority.com/arc-browser-shutting-down-december-2026-3726900/),
  [CyberInsider](https://cyberinsider.com/arc-browser-to-stop-working-on-december-22-as-team-shifts-to-dia/),
  [Wikipedia](https://en.wikipedia.org/wiki/Arc_(web_browser))
- Dia: [Wikipedia](https://en.wikipedia.org/wiki/Dia_(web_browser)),
  [TechCrunch](https://techcrunch.com/2025/11/03/dias-ai-browser-starts-adding-arcs-greatest-hits-to-its-feature-set)
- Crest: [crestbrowser.com](https://crestbrowser.com/),
  [engines](https://crestbrowser.com/guides/engines/),
  [sync](https://crestbrowser.com/guides/sync-across-devices/),
  [repository](https://github.com/pauljoda/Crest),
  [changelog](https://github.com/pauljoda/Crest/blob/main/CHANGELOG.md)
- Phi: [phibrowser.com](https://phibrowser.com/),
  [layouts](https://phibrowser.com/help/layouts/),
  [2.8.0](https://phibrowser.com/release-note/2.8.0)
- Lilo: [getlilo.app](https://getlilo.app/)
- Zen, Vivaldi, SigmaOS and others:
  [Open Source Alternatives](https://www.opensourcealternatives.to/alternative-to/arc),
  [Tabbit migration guide](https://go.tabbit.com/browsers-with-workspaces-like-arc)
