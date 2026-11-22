# Settings

Compass's settings are its settings window ([settings-window.md](settings-window.md)):
chrome://compass-settings in a window of its own, with the panes as toolbar
tabs. Upstream's chrome://settings page is never shown: every chrome://settings
URL, and every way of opening settings, opens the window at the pane for it.

Compass's own sections are elements the window shows in its panes:

| Section | Pane (path) | Element | Standalone page |
|---|---|---|---|
| Sync | Profiles (`sync`) | `<compass-sync-page>` | chrome://compass-sync |
| Import from Another Browser | Profiles (`importArc`) | `<compass-arc-import-page>` | chrome://compass-import |
| Links from Other Apps | Links (`links`) | `<compass-links-page>` | none |
| Keyboard shortcuts | Shortcuts (`shortcuts`) | `<compass-shortcuts-page>` | chrome://compass-settings/shortcuts.html |
| Boosts | Boosts (`boosts`) | `<compass-boosts-page>` | chrome://compass-settings/boosts.html |
| Plugins | Plugins (`plugins`) | `<compass-plugins-page>` | none |
| Claude | AI (`ai`; `claude` too) | `<compass-claude-page>` | none |
| Development Mode | Advanced (`devMode`) | `<compass-dev-mode-page>` | none |

The command bar ("Keyboard shortcuts", "Import from Another Browser",
"Route this site to space…", which opens `links?add=<domain>`; "Boost this
site…", which opens `boosts?add=<host>`) and the app menu's sync row open
them with `chrome::ShowSettingsSubPage()`, which opens or brings forward the
window there. Import from Another Browser lists the other browsers'
profiles found (browser-import.md), and Arc's when Arc is there
(arc-import.md). Its path is still `importArc`, the Arc import's.

There's no "On startup" setting: Compass's sidebar brings back its spaces
and their tabs itself (docs/design/sidebar.md, "Today persistence"). The
pref behind upstream's (`RestoreOnStartup`) keeps whatever value it had and
still decides whether the first window shows the switcher's first space
(docs/design/spaces.md, "Startup with several profiles"); policy can still
set it.

The standalone pages stay: they show the same element alone, and the
end-to-end sync steps (`tools/sync_e2e/`) and browser tests drive them with
`document.getElementById()`. Nothing in the browser links to them.

## How

- **The elements** (`resources/sidebar/settings/`): plain custom elements
  (`CompassSettingsPage` in `page_base.ts`), not Polymer or Lit. Each
  renders a heading and a card, with its markup (`*.html`, wrapped by
  `html_to_wrapper` as a native template) in its light DOM, so ids stay
  reachable from the standalone page's document and the window's stylesheet
  reaches it. Styles are scoped under `.compass-page`; the window re-skins
  them (settings-window.md, "Visual design").
- **No cr_elements.** The same modules also make the standalone pages, so
  they stay plain HTML controls with the colors.css tokens.
- **Mojo:** `sidebar.mojom` `SettingsHandler` (shortcuts, links, Boosts,
  plugins, development mode), `ArcImportHandler`, `compass_sync.mojom`
  `PageHandlerFactory` and `claude.mojom` `SettingsHandler`, whose
  implementations (`ShortcutsPageHandler`, `ArcImportPageHandler`,
  `SyncPageHandlerFactory`, `ClaudeSettingsHandler`) chrome://compass-settings
  binds through `SettingsSections::CreateHandlers()`; the standalone
  controllers bind their own. Each section binds its own `SettingsHandler`,
  and they all stay. Links from Other Apps uses `SettingsHandler` too
  (`GetLinkRules()`, `SetLinkRule()`, `RemoveLinkRule()`, for the page's
  profile; [link-routing.md](link-routing.md)).

## Plugins

`plugins` (`plugins_page.{ts,html}`, `browser/ui/webui/settings/plugin_settings.cc`)
lists every live folder and tab preview plugin (docs/design/live-folders.md):
built-in, installed, local or made by Claude, each with its kinds ("Live
folder", "Tab preview", "Badge"). Expanding one shows, for a preview, the sites it
reads, its switch, its settings form and the "Advanced: use your session on
<site>" switch (off by default, naming the related hosts it reaches); for a
folder, how many folders use it and what they reach; its source, read-only,
and what its last preview logged; and Remove for plugins that aren't
built-in. At the top: "Show tab previews in hover cards"
(`compass.tab_previews.enabled`), Install plugin… (review, then install),
Open plugins folder, and, with "Let Claude make plugins" on, Make one with
Claude. `plugins?id=<id>` opens one. Through `SettingsHandler`
(`GetPlugins()`, `SetPluginPreviewEnabled()`, `SetPluginSessionGranted()`,
`SetPluginPreviewConfig()`, …). Test: `SettingsSectionsBrowserTest.PluginsSection`.

## Not done

- Strings are English only, as on the standalone pages.
