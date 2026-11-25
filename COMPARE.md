# Compass compared with other browsers

How Compass compares with Arc, its successor Dia, and the browsers people
move to from Arc. Written on 10 November 2026; the other columns checked on 14 November
2026, the Ardia and Cra columns added and the Compass column updated on 25
November 2026. Other browsers change
quickly, so check their sites before relying on a cell.

- The Compass column comes from this repository's code (`main`).
- The Crest column comes from its site, changelog and repository; the
  Ardia and Cra columns from their repositories (25 November 2026).
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
| **Compass** | Chromium | macOS 13+ (stable), Windows 10/11 (stable), Linux (stable, Flatpak with updates), iPhone and iPad (TestFlight), Android (Google Play testing) | BSD-3-Clause, free | Active |
| **Crest** | Chromium by default on Mac, WebKit for any site; WebKit only on iPhone/iPad | macOS 26.1+ (Apple silicon), iPhone, iPad | MPL-2.0, free | Early access (0.9.x) |
| **Ardia** | Chromium (Helium's patches); WebKit for streaming sites on the Mac | macOS 14+ (Apple silicon), Linux; iPhone (TestFlight), Android (APK); Windows not released | GPL-3.0 (its own code Apache-2.0), free | Active (0.2.48) |
| **Cra** | Chromium; WebKit for DRM pages on the Mac, and on iPhone/iPad | macOS 14+ (Apple silicon and Intel), iPhone and iPad (App Store, iOS 17+) | MIT, free | Early (0.1.56) |
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

| Feature | Compass | Crest | Ardia | Cra | Arc | Dia | Phi | Zen | Vivaldi | Edge | SigmaOS | Lilo | Orion |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **Sidebar and tabs** | | | | | | | | | | | | | |
| Spaces with themes | ✅ colors, gradients, grain, icons | ✅ Crest Studio emblems, gradients | ✅ gradients, grain, blur | ✅ emoji or symbol, accent colors, noise | ✅ | ~ | ✅ | ✅ | ✅ | ~ | ✅ | ✅ | ~ profiles and tab groups |
| Separate profile / cookies per space | ✅ switched in the same window | ✅ plus Sub-Spaces that share sign-ins | ✅ spaces are profiles; profiles can share data | ✅ spaces sit on profiles | ✅ | ? | ✅ | ✅ containers | ❌ | ❌ | ? | ✅ | ✅ profiles, containers |
| Locked private spaces | ~ incognito lock on iOS and Android | ✅ Face ID / Touch ID | ? | ❌ | ❌ | ? | ? | ❌ | ❌ | ❌ | ? | ? | ? |
| Favorites shared across spaces | ✅ | ~ a Sub-Space group's pinned tabs | ~ Top Apps, between profiles that share data | ✅ per profile | ✅ | ? | ? | ✅ Essentials | ~ | ❌ | ? | ? | ? |
| Pinned tabs in nested folders | ✅ in Pinned and Today | ✅ | ✅ nested groups; folders in Top Apps | ✅ in Pinned and Today | ✅ | ✅ nested groups, 8 levels | ✅ groups | ✅ | ~ stacks | ~ groups | ✅ | ✅ | ~ pinned tabs, tab groups |
| Auto-archive of unused tabs | ✅ Today, 12 h default | ✅ Space cleanup, 12 h–30 days | ✅ 12 h–30 days or custom | ✅ Today, 12 h–180 days | ✅ | ~ Clean Up Tabs on Windows | ? | ❌ | ❌ | ❌ | ~ | ✅ 12 h | ? |
| Rename tabs, mute entries, undo close | ✅ | ~ rename, mute | ✅ | ✅ | ✅ | ~ rename | ? | ✅ | ✅ | ~ | ? | ? | ? |
| Split view | ✅ up to 4 panes, side by side or stacked | ✅ 2–4 panes | ✅ up to 3, side by side or stacked | ✅ up to 4, side by side or stacked | ✅ up to 4 | ✅ | ✅ up to 4, side by side or stacked | ✅ up to 4 | ✅ tiling | ✅ | ✅ | ✅ up to 4 | ? |
| Tree-style tabs | ~ nested folders, made by hand | ~ | ~ ⌘-click groups a link with its opener | ~ nested folders, made by hand | ~ | ~ nested groups, made by hand | ❌ | ~ | ~ | ❌ | ~ | ~ | ✅ vertical tree tabs |
| Custom shortcuts | ✅ | ✅ | ✅ mouse buttons too | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ | ? | ✅ | ? |
| Ctrl+Tab in recently used order | ✅ with cards of each tab's preview; W closes one; can be turned off | ? | ✅ an option | ✅ with cards; across spaces with ⌃⌥Tab | ✅ tab switcher | ? | ? | ✅ an option | ✅ with previews | ? | ? | ? | ? |
| **Arc-style extras** | | | | | | | | | | | | | |
| Command bar, no toolbar | ✅ | ✅ | ✅ | ✅ | ✅ | ~ | ? | ✅ | ✅ Quick Commands | ❌ | ✅ | ✅ | ? |
| Peek (preview a link without a tab) | ✅ | ✅ | ✅ | ✅ | ✅ | ? | ✅ Peek View | ✅ Glance | ❌ | ❌ | ? | ✅ | ✅ link preview |
| Small windows for links from other apps | ✅ macOS, Windows, Linux | ✅ Quick Window | ✅ Little Ardia | ✅ Little Window | ✅ Little Arc | ? | ✅ Kiosk | ❌ | ❌ | ❌ | ? | ✅ Little window | ✅ link preview from any app |
| Link routing to spaces (Air Traffic Control) | ✅ links from other apps, by domain | ✅ | ✅ | ✅ | ✅ | ✅ Link Rules to profiles | ✅ URL Rules | ✅ Space Routing | ❌ | ❌ | ? | ✅ | ? |
| Auto picture-in-picture | ✅ including Teams, Meet and Zoom calls; iPhone and iPad on leaving the app | ✅ | ✅ Meet, YouTube | ✅ including Meet | ✅ | ✅ Meet | ? | ~ | ? | ~ | ? | ~ picture-in-picture | ~ picture-in-picture |
| Live folders | ✅ JavaScript plugins (RSS, GitHub, web list, calendar); plugins also show a tab's content in its hover card (Google Calendar's next events, GitHub's notifications) and unread counts on tabs; Claude can write them | ? | ✅ GitHub and Bitbucket pull requests, Notion, Confluence, Drive, meetings; plugins | ✅ JavaScript plugins (GitHub, GitLab, Jira, Azure DevOps), your own as a ZIP | ✅ | ❌ | ❌ | ✅ GitHub, RSS | ~ feed reader | ❌ | ❌ | ❌ | ? |
| Per-site CSS/JS (Boosts) | ✅ by hand or by Claude, synced; Zap elements from the command bar; desktop and iOS | ❌ | ~ TypeScript userscripts | ✅ Zap, share links, a gallery | ✅ | ❌ | ❌ | ✅ Boosts, Mods | ❌ | ❌ | ❌ | ❌ | ? |
| Share spaces and folders with other people | ✅ spaces, folders and easels; invite links or email; end-to-end encrypted | ? | ~ folders through a shared iCloud Drive or Dropbox folder | ~ a link to a copy of a folder | ✅ | ? | ? | ? | ? | ✅ Workspaces | ? | ? | ? |
| Development mode for a site | ✅ no cache on reload, full address bar, DevTools buttons; on for localhost by default; desktop and iOS | ? | ❌ removed in 0.2.47, to return as a plugin | ✅ | ✅ | ? | ? | ? | ? | ? | ? | ? | ? |
| Screenshots of a region or the full page | ✅ copied or saved; desktop | ? | ? only through its command line | ✅ | ✅ | ? | ? | ✅ | ✅ | ✅ | ? | ? | ? |
| Notes / canvas (Easels) | ✅ text, shapes, drawing, images, links, page captures (refreshed while a board is open, desktop); synced; imported from Arc; desktop, Android and iOS | ❌ | ❌ | ✅ Easels, Mac | ✅ | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ | ❌ | ? |
| Translucent sidebar | ❌ | ✅ Ultra Thin to Ultra Thick | ✅ | ✅ an option | ✅ | ? | ? | ✅ blur, transparency | ❌ | ❌ | ? | ? | ? |
| **Web content** | | | | | | | | | | | | | |
| Chrome extensions, desktop / phone | ✅ / ✅ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ✅ / ❌ | ❌ Firefox add-ons | ✅ / ❌ | ✅ / ❌ | ~ / ❌ | ✅ / ❌ | ✅ / ✅ also Firefox add-ons |
| Built-in ad blocker | ✅ EasyList, EasyPrivacy and cookie banners on desktop, Android and iPhone; your own lists on desktop and Android (docs/design/ad-blocker.md) | ✅ both engines, Mac, iPhone, iPad | ✅ uBlock Origin, cookie banners, your own lists | ✅ cookie banners, your own lists; Mac and iPhone | ❌ | ❌ | ✅ per profile | ❌ | ✅ | ~ | ✅ | ✅ | ✅ |
| Reader mode, on-device translation | ~ Reader mode; translation not yet | ✅ Reader on WebKit pages, translation in both engines | ✅ translation on macOS 26 | ~ Google translation on the Mac; Reader and on-device translation on iPhone | ❌ | ? | ~ Reader View | ✅ | ✅ | ✅ | ? | ✅ | ✅ on-device on macOS 26+ |
| Full-quality DRM video | ❌ no Widevine VMP | ✅ FairPlay through WebKit | ~ FairPlay through WebKit for streaming sites | ✅ FairPlay through WebKit when Widevine fails | ✅ | ✅ | ~ Widevine since 2.13 | ~ | ✅ | ✅ | ✅ | ~ FairPlay, no Widevine | ? |
| Mail, calendar, web panels | ❌ | ❌ | ❌ | ~ preview cards on favorites (Gmail, Outlook, Google Calendar…) | ❌ | ❌ | ❌ | ❌ | ✅ | ~ | ❌ | ❌ | ? |
| **AI** | | | | | | | | | | | | | |
| AI chat panel that reads and acts on the page | ✅ Claude, with developer tools (network, elements, storage, performance) when allowed | ❌ | ❌ on desktop, by choice; summaries on the phone | ~ Ask on Page reads it, Apple Intelligence | ~ Arc Max | ✅ | ✅ | ❌ | ❌ by choice | ✅ Copilot | ✅ Airis | ❌ agents through MCP | ? |
| Agent across tabs and windows | ✅ desktop, iPhone and iPad; off by default | ❌ | ~ outside agents through its command line | ❌ | ❌ | ✅ | ✅ | ❌ | ❌ | ✅ | ? | ❌ | ❌ by choice |
| MCP server for coding agents | ✅ macOS and Windows | ✅ Mac, per chosen space | ~ a command line and agent skill instead, macOS and Linux | ✅ Mac, added to each agent's settings | ❌ | ? | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ | ? |
| Own API key, Bedrock/Vertex, or Claude subscription via Claude Code | ✅ | — | ~ ChatGPT plan or Gemini Nano, phone only | ❌ Apple Intelligence only | ❌ | ❌ | ~ local models (Ollama, LM Studio) | — | — | ❌ | ❌ | — | ? |
| AI memory across sessions | ✅ desktop, iPhone and iPad, synced end-to-end encrypted, each save shown with Undo | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ kept on the Mac | ❌ | ❌ | ✅ | ? | ❌ | ? |
| **Sync and devices** | | | | | | | | | | | | | |
| Sync | ✅ no account, end-to-end encrypted, self-hostable (with company sign-in, OIDC); a space can stay on one device | ✅ iCloud; passwords through iCloud Keychain | ✅ no account, end-to-end encrypted, through a synced folder (iCloud Drive, Dropbox, NAS) | ✅ no account, through iCloud Drive or a synced folder; sidebar and settings only | Arc account | account | Phi account, experimental | Firefox account | Vivaldi account | Microsoft account | ? | ? | ✅ Mac and iPhone |
| Send tab to phone | ✅ | ? | ✅ | ~ Handoff | ❌ | ? | ? | ✅ | ✅ | ✅ | ❌ | ? | ? |
| Phone app with the same spaces | ✅ iPhone and iPad (beta), Android (beta, Google Play testing) | ✅ iPhone and iPad (App Store) | ~ iPhone (TestFlight) and Android (APK), synced tabs | ~ iPhone and iPad (App Store), Arc Search-style; the Mac's spaces read-only | ~ Arc Search is separate | in progress | ❌ | ❌ | ✅ | ✅ | ❌ | ❌ | ✅ iPhone and iPad |
| **Moving in** | | | | | | | | | | | | | |
| Import | Arc (macOS, Windows): profiles, cookies, sidebar, archive; Chrome, Edge, Brave, Vivaldi, Dia, Ardia, Cra, Crest, Yab: also as a whole new profile, cookies included (Yab's Chromium ones); Chrome, Edge, Brave, Vivaldi, Dia, Ardia, Firefox, Zen, Safari, Cra, Crest, Yab (the last four macOS): bookmarks, open tabs, history, passwords, Zen, Vivaldi, Ardia, Cra, Crest and Yab spaces, Safari tab groups | Arc, Zen, Chrome, Safari, Firefox, Brave, Edge, Vivaldi, Opera, Dia, Comet and more | Arc spaces, splits and rules; Dia's sidebar; Zen spaces; Chrome, Edge, Brave, Opera, Vivaldi, Helium, Firefox, Safari and more: bookmarks, history, tabs, passwords, cookies, extensions | Arc, Ardia and Yab spaces and sign-ins; Zen spaces; Dia, Chrome, Brave, Edge, Opera, Vivaldi, Helium: tabs, bookmarks, history, passwords, sign-ins; Firefox; Safari's export file | — | Arc, Chrome | Arc, Zen, Chrome | Arc and Dia spaces and profiles | ~ | ~ | ? | ~ bookmarks, history, sign-ins from many browsers | Arc, Safari, Firefox, Brave |
| Export spaces | ✅ a bookmarks file any browser imports (desktop); plain-text tab lists on iPhone and iPad | ? | ~ a bookmarks file | ✅ a bookmarks file (Mac); tab lists on iPhone | ? | ? | ? | ? | ? | ? | ? | ? | ? |
| Open source | ✅ | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ | ✅ | ~ | ❌ | ❌ | ✅ | ❌ Linux and Windows code to be released |

## What only Compass has

As far as the sources show:

- Sync without an account, end-to-end encrypted, on a server anyone can run.
- Claude built in, with the user's own key, a compatible gateway, or a
  Claude subscription through Claude Code; the same tools as an MCP server.
- Live folders as JavaScript plugins meant to run on desktop, Android and
  iPhone; the same plugins show a tab's content when you hover it (your
  next Google Calendar events, your GitHub notifications) and unread counts
  on tabs, reading the page apart from it, and Claude can write new ones.
- Picture-in-picture for Teams, Meet and Zoom calls, with their controls.

## Compass and Crest

Crest is the closest project to Compass: also open source, also an Arc
replacement, also with spaces, Peek, small windows, splits and an MCP server.

Crest has, Compass doesn't:

- A WebKit engine per site: full-quality DRM video on those pages.
- On-device page translation.
- A translucent sidebar.
- Import from a few more browsers (Opera, Comet and others).
- Private Spaces locked with Face ID or Touch ID, and Sub-Spaces.
- An App Store release for iPhone and iPad, and a Homebrew cask.

Compass has, Crest doesn't:

- Windows, Linux and Android.
- Account-free, end-to-end encrypted, self-hostable sync (Crest uses
  iCloud).
- A Claude chat panel in the browser (Crest has only the MCP server).
- Chrome extensions on the phone.
- Live folders and tab previews from plugins, call picture-in-picture, send
  tab to phone, favorites shared across all spaces.
- Ctrl+Tab in recently used order, sharing with other people, and a
  development mode for sites you build.

## Gaps

Features other browsers have that Compass doesn't, checked against the code:

| Missing | Who has it | State in Compass |
|---|---|---|
| On-device translation | Crest, Ardia, Orion, Zen, Vivaldi, Edge, Lilo | Not started. Reader mode is done, desktop and iOS (docs/design/reader-mode.md) |
| Full-quality DRM video | Chrome, Edge, Arc, Dia, Crest, Cra (Ardia for streaming sites) | Blocked: no Widevine VMP signature (docs/design/widevine.md) |
| Tree-style tabs | Orion, Vivaldi, Floorp | Partial: folders nest in Pinned and Today, but a new tab doesn't go under the tab that opened it |
| Translucent sidebar | Arc, Crest, Ardia, Cra, Zen | Arc's gradients import; translucency and vibrancy don't (docs/design/arc-import.md) |
| Android release | Vivaldi, Edge | In closed testing on Google Play (beta); production needs 12 testers for 14 days first |

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
- Ardia: [ardia.dev](https://ardia.dev),
  [release notes](https://ardia.dev/release-notes),
  [repository](https://github.com/ardia-browser/ardia),
  [plugins](https://github.com/ardia-browser/plugins),
  [Dia feature parity](https://github.com/ardia-browser/ardia/blob/main/docs/dia-feature-parity.md)
- Cra: [crabrowser.app](https://crabrowser.app),
  [release notes](https://crabrowser.app/release-notes),
  [repository](https://github.com/VictorKildahl/cra),
  [releases](https://github.com/VictorKildahl/cra/releases),
  [App Store](https://apps.apple.com/app/id6834101298)
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
