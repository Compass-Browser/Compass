# Compass compared with other browsers

How Compass compares with Arc, its successor Dia, and the browsers people
move to from Arc. Written on 10 November 2026; the Compass column updated
and the other columns checked on 14 November 2026. Other browsers change
quickly, so check their sites before relying on a cell.

- The Compass column comes from this repository's code (`main`).
- The Crest column comes from its site, changelog and repository.
- The other columns come from public reviews and the vendors' sites.

✅ yes · ~ partial or different · ❌ no · ? not confirmed by any source found ·
— not applicable

## Arc's status

The Browser Company announced on 22 October 2026 that Arc **stops working
on 22 December 2026**: no updates, and all accounts deactivated. New Arc
accounts are already blocked, Arc Search loses its AI features, sign-in and
tab sync on 23 November, account data requests close on 15 December, and Arc
Search for Android leaves Google Play on 22 December. Personal data is
deleted within 30 days of the shutdown. The team's successor is Dia, on Mac
and, since October 2026, Windows (The Browser Company is part of Atlassian
since October 2025).

## Platform and basics

| Browser | Engine | Platforms | License / price | Status |
|---|---|---|---|---|
| **Compass** | Chromium | macOS 13+ (stable), Windows 10/11 (beta), iPhone and iPad (TestFlight), Linux (canary, Flatpak with updates), Android (in progress) | BSD-3-Clause, free | Active |
| **Crest** | Chromium by default on Mac, WebKit for any site; WebKit only on iPhone/iPad | macOS 26.1+ (Apple silicon), iPhone, iPad | MPL-2.0, free | Early access (0.9.x) |
| **Arc** | Chromium | macOS, Windows; Arc Search on iOS/Android | Free, closed | Shuts down 22 Dec 2026 |
| **Dia** | Chromium | macOS (Apple silicon), Windows | Free core features, paid Pro tier, closed | Active, adding Arc features |
| **Phi** | Chromium, native AppKit/SwiftUI shell | macOS 14+ (Apple silicon) | Apache-2.0, free | Active |
| **Zen** | Firefox | macOS, Windows, Linux | MPL-2.0, free | Active |
| **Vivaldi** | Chromium | Desktop, Android, iOS | Free, UI closed | Active |
| **Edge** | Chromium | All | Free, closed | Active |
| **SigmaOS** | WebKit | macOS | Free + paid | Active |
| **Lilo** | WebKit (runs Chrome extensions) | macOS 26+ | MIT, free | Beta (0.21) |
| **Orion** | WebKit (runs Chrome and Firefox extensions) | macOS, iPhone, iPad; Linux beta, no updates since 2 Oct 2026 | Free, paid Orion+, closed | Active on Mac and iPhone; Kagi stopped Linux and Windows in October 2026 |

## Features

| Feature | Compass | Crest | Arc | Dia | Phi | Zen | Vivaldi | Edge | SigmaOS | Lilo | Orion |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **Sidebar and tabs** | | | | | | | | | | | |
| Spaces with themes | ✅ colors, gradients, grain, icons | ✅ Crest Studio emblems, gradients | ✅ | ~ | ✅ | ✅ | ✅ | ~ | ✅ | ✅ | ~ profiles and tab groups |
| Separate profile / cookies per space | ✅ switched in the same window | ✅ plus Sub-Spaces that share sign-ins | ✅ | ? | ✅ | ✅ containers | ❌ | ❌ | ? | ✅ | ✅ profiles, containers |
| Locked private spaces | ~ incognito lock on iOS and Android | ✅ Face ID / Touch ID | ❌ | ? | ? | ❌ | ❌ | ❌ | ? | ? | ? |
| Favorites shared across spaces | ✅ | ~ a Sub-Space group's pinned tabs | ✅ | ? | ? | ✅ Essentials | ~ | ❌ | ? | ? | ? |
| Pinned tabs in nested folders | ✅ in Pinned and Today | ✅ | ✅ | ✅ nested groups, 8 levels | ✅ groups | ✅ | ~ stacks | ~ groups | ✅ | ✅ | ~ pinned tabs, tab groups |
| Auto-archive of unused tabs | ✅ Today, 12 h default | ✅ Space cleanup, 12 h–30 days | ✅ | ~ Clean Up Tabs on Windows | ? | ❌ | ❌ | ❌ | ~ | ✅ 12 h | ? |
| Rename tabs, mute entries, undo close | ✅ | ~ rename, mute | ✅ | ~ rename | ? | ✅ | ✅ | ~ | ? | ? | ? |
| Split view | ✅ up to 4 panes, side by side or stacked | ✅ 2–4 panes | ✅ up to 4 | ✅ | ✅ up to 4, side by side or stacked | ✅ up to 4 | ✅ tiling | ✅ | ✅ | ✅ up to 4 | ? |
| Tree-style tabs | ~ nested folders, made by hand | ~ | ~ | ~ nested groups, made by hand | ❌ | ~ | ~ | ❌ | ~ | ~ | ✅ vertical tree tabs |
| Custom shortcuts | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ | ? | ✅ | ? |
| **Arc-style extras** | | | | | | | | | | | |
| Command bar, no toolbar | ✅ | ✅ | ✅ | ~ | ? | ✅ | ✅ Quick Commands | ❌ | ✅ | ✅ | ? |
| Peek (preview a link without a tab) | ✅ | ✅ | ✅ | ? | ✅ Peek View | ✅ Glance | ❌ | ❌ | ? | ✅ | ✅ link preview |
| Small windows for links from other apps | ✅ macOS, Windows, Linux | ✅ Quick Window | ✅ Little Arc | ? | ✅ Kiosk | ❌ | ❌ | ❌ | ? | ✅ Little window | ✅ link preview from any app |
| Link routing to spaces (Air Traffic Control) | ❌ | ✅ | ✅ | ✅ Link Rules to profiles | ✅ URL Rules | ✅ Space Routing | ❌ | ❌ | ? | ✅ | ? |
| Auto picture-in-picture | ✅ including Teams, Meet and Zoom calls; iPhone and iPad on leaving the app | ✅ | ✅ | ✅ Meet | ? | ~ | ? | ~ | ? | ~ picture-in-picture | ~ picture-in-picture |
| Live folders | ✅ JavaScript plugins (RSS, GitHub, web list, calendar) | ? | ✅ | ❌ | ❌ | ✅ GitHub, RSS | ~ feed reader | ❌ | ❌ | ❌ | ? |
| Per-site CSS/JS (Boosts) | ❌ | ❌ | ✅ | ❌ | ❌ | ✅ Boosts, Mods | ❌ | ❌ | ❌ | ❌ | ? |
| Notes / canvas (Easels) | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ? |
| Translucent sidebar | ❌ | ✅ Ultra Thin to Ultra Thick | ✅ | ? | ? | ✅ blur, transparency | ❌ | ❌ | ? | ? | ? |
| **Web content** | | | | | | | | | | | |
| Chrome extensions, desktop / phone | ✅ / ✅ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ❌ Firefox add-ons | ✅ / ❌ | ✅ / ❌ | ~ / ❌ | ✅ / ❌ | ✅ / ✅ also Firefox add-ons |
| Built-in ad blocker | ❌ (uBlock Origin Lite as an extension) | ✅ both engines, Mac, iPhone, iPad | ❌ | ❌ | ✅ per profile | ❌ | ✅ | ~ | ✅ | ✅ | ✅ |
| Reader mode, on-device translation | ❌ | ✅ Reader on WebKit pages, translation in both engines | ❌ | ? | ~ Reader View | ✅ | ✅ | ✅ | ? | ✅ | ✅ on-device on macOS 26+ |
| Full-quality DRM video | ❌ no Widevine VMP | ✅ FairPlay through WebKit | ✅ | ✅ | ~ Widevine since 2.13 | ~ | ✅ | ✅ | ✅ | ~ FairPlay, no Widevine | ? |
| Mail, calendar, web panels | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ~ | ❌ | ❌ | ? |
| **AI** | | | | | | | | | | | |
| AI chat panel that reads and acts on the page | ✅ Claude, with developer tools (network, elements, storage, performance) when allowed | ❌ | ~ Arc Max | ✅ | ✅ | ❌ | ❌ by choice | ✅ Copilot | ✅ Airis | ❌ agents through MCP | ? |
| Agent across tabs and windows | ✅ desktop, iPhone and iPad; off by default | ❌ | ❌ | ✅ | ✅ | ❌ | ❌ | ✅ | ? | ❌ | ❌ by choice |
| MCP server for coding agents | ✅ macOS and Windows | ✅ Mac, per chosen space | ❌ | ? | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ | ? |
| Own API key, Bedrock/Vertex, or Claude subscription via Claude Code | ✅ | — | ❌ | ❌ | ~ local models (Ollama, LM Studio) | — | — | ❌ | ❌ | — | ? |
| AI memory across sessions | ✅ desktop, iPhone and iPad, synced end-to-end encrypted, each save shown with Undo | ❌ | ❌ | ✅ | ✅ kept on the Mac | ❌ | ❌ | ✅ | ? | ❌ | ? |
| **Sync and devices** | | | | | | | | | | | |
| Sync | ✅ no account, end-to-end encrypted, self-hostable; a space can stay on one device | ✅ iCloud; passwords through iCloud Keychain | Arc account | account | Phi account, experimental | Firefox account | Vivaldi account | Microsoft account | ? | ? | ✅ Mac and iPhone |
| Send tab to phone | ✅ | ? | ❌ | ? | ? | ✅ | ✅ | ✅ | ❌ | ? | ? |
| Phone app with the same spaces | ✅ iPhone and iPad (beta), Android (in progress) | ✅ iPhone and iPad (App Store) | ~ Arc Search is separate | in progress | ❌ | ❌ | ✅ | ✅ | ❌ | ❌ | ✅ iPhone and iPad |
| **Moving in** | | | | | | | | | | | |
| Import | Arc (macOS): profiles, cookies, sidebar, archive | Arc, Zen, Chrome, Safari, Firefox, Brave, Edge, Vivaldi, Opera, Dia, Comet and more | — | Arc, Chrome | Arc, Zen, Chrome | Arc and Dia spaces and profiles | ~ | ~ | ? | ~ bookmarks, history, sign-ins from many browsers | Arc, Safari, Firefox, Brave |
| Open source | ✅ | ✅ | ❌ | ❌ | ✅ | ✅ | ~ | ❌ | ❌ | ✅ | ❌ Linux and Windows code to be released |

## What only Compass has

As far as the sources show:

- Sync without an account, end-to-end encrypted, on a server anyone can run.
- Claude built in, with the user's own key, a compatible gateway, or a
  Claude subscription through Claude Code; the same tools as an MCP server.
- Live folders as JavaScript plugins meant to run on desktop, Android and
  iPhone.
- Picture-in-picture for Teams, Meet and Zoom calls, with their controls.

## Compass and Crest

Crest is the closest project to Compass: also open source, also an Arc
replacement, also with spaces, Peek, small windows, splits and an MCP server.

Crest has, Compass doesn't:

- Link routing to spaces (Air Traffic Control).
- A WebKit engine per site: full-quality DRM video and Reader mode on those
  pages.
- Built-in ad blocking and on-device page translation.
- A translucent sidebar.
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
  shared across all spaces.

## Gaps

Features other browsers have that Compass doesn't, checked against the code:

| Missing | Who has it | State in Compass |
|---|---|---|
| Link routing to spaces | Arc, Crest, Dia, Zen, Phi, Lilo | Next after little windows (docs/design/shortcuts.md, little-window.md "Not done") |
| Per-site CSS/JS | Arc (Boosts), Zen (Boosts, Mods) | Not started |
| Built-in ad blocker | Vivaldi, SigmaOS, Orion, Crest, Phi, Lilo, Brave | Not started; extensions instead |
| Reader mode, on-device translation | Crest, Orion, Zen, Vivaldi, Edge, Lilo | Not started |
| Full-quality DRM video | Chrome, Edge, Arc, Dia, Crest | Blocked: no Widevine VMP signature (docs/design/widevine.md) |
| Import from browsers other than Arc | Crest, Orion, Zen, Phi, Lilo, Dia | Not started |
| Notes / canvas | Arc, Vivaldi | Not started |
| Tree-style tabs | Orion, Vivaldi, Floorp | Partial: folders nest in Pinned and Today, but a new tab doesn't go under the tab that opened it |
| Translucent sidebar | Arc, Crest, Zen | Arc's gradients import; translucency and vibrancy don't (docs/design/arc-import.md) |
| Arc import on Windows and Linux | — | macOS only |
| Android release | Vivaldi, Edge | The app is built; Google Play testing being set up |

## Sources

- Arc: [farewell page](https://arc.net/),
  [TechCrunch](https://techcrunch.com/2026/10/22/arc-browser-sunsets-december-22-as-the-browser-company-doubles-down-on-dia/),
  [Android Authority](https://www.androidauthority.com/arc-browser-shutting-down-december-2026-3726900/),
  [CyberInsider](https://cyberinsider.com/arc-browser-to-stop-working-on-december-22-as-team-shifts-to-dia/),
  [Wikipedia](https://en.wikipedia.org/wiki/Arc_(web_browser))
- Dia: [release log](https://www.diabrowser.com/updates),
  [Wikipedia](https://en.wikipedia.org/wiki/Dia_(web_browser)),
  [TechCrunch](https://techcrunch.com/2025/11/03/dias-ai-browser-starts-adding-arcs-greatest-hits-to-its-feature-set)
- Crest: [crestbrowser.com](https://crestbrowser.com/),
  [engines](https://crestbrowser.com/guides/engines/),
  [sync](https://crestbrowser.com/guides/sync-across-devices/),
  [repository](https://github.com/pauljoda/Crest),
  [changelog](https://github.com/pauljoda/Crest/blob/main/CHANGELOG.md)
- Phi: [phibrowser.com](https://phibrowser.com/),
  [layouts](https://phibrowser.com/help/layouts/),
  [2.8.0](https://phibrowser.com/release-note/2.8.0),
  [2.10.0](https://phibrowser.com/release-note/2.10.0),
  [2.12.0](https://phibrowser.com/release-note/2.12.0),
  [2.13.0](https://phibrowser.com/release-note/2.13.0),
  [2.14.0](https://phibrowser.com/release-note/2.14.0),
  [2.17.0](https://phibrowser.com/release-note/2.17.0)
- Zen: [release notes](https://zen-browser.app/release-notes),
  [Space Routing in 1.21b](https://alternativeto.net/news/2026/6/zen-browser-1-21b-adds-space-routing-and-updates-to-firefox-151-0-4/)
- Lilo: [getlilo.app](https://getlilo.app/)
- Orion: [orionbrowser.com](https://orionbrowser.com/),
  [macOS features](https://orionbrowser.com/platforms/macos),
  [iOS features](https://orionbrowser.com/platforms/ios),
  [Orion+](https://orionbrowser.com/orionplus),
  [Linux and Windows update](https://blog.kagi.com/update-orion-linux-windows),
  [1.1 release](https://alternativeto.net/news/2026/6/orion-1-1-brings-multi-account-containers-page-translation-redesigned-settings-and-more/),
  [FOSS Force](https://fossforce.com/2026/08/no-ads-no-telemetry-no-ai-agent-orion-browser-does-linux/)
- Vivaldi, SigmaOS and others:
  [Open Source Alternatives](https://www.opensourcealternatives.to/alternative-to/arc),
  [Tabbit migration guide](https://go.tabbit.com/browsers-with-workspaces-like-arc)
