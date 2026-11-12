# Settings

Compass's own settings are sections of upstream's `chrome://settings`
(compass://settings), listed in its menu right after "You and Google":

| Menu item | Path | Element | Standalone page |
|---|---|---|---|
| Sync | `/sync` | `<compass-sync-page>` | chrome://compass-sync |
| Keyboard shortcuts | `/shortcuts` | `<compass-shortcuts-page>` | chrome://compass-settings |
| Import from Arc | `/importArc` | `<compass-arc-import-page>` | chrome://compass-import |

The command bar ("Keyboard shortcuts", "Import from Arc") and the app menu's
sync row open the sections (`chrome::ShowSettingsSubPage()`, which reuses an
open settings tab). Sync is hidden where upstream hides the people page
(guest). Import from Arc is listed everywhere; off macOS it says Arc wasn't
found.

Upstream's **On startup** section is gone (no menu item, view or route;
upstream does the same for a guest): Compass's sidebar brings back its
spaces and their tabs itself (docs/design/sidebar.md, "Today persistence"),
so choosing what opens at launch has no place in Compass. The pref behind it (`RestoreOnStartup`) keeps whatever value it
had and still decides whether the first window shows the switcher's first
space (docs/design/spaces.md, "Startup with several profiles"); policy can
still set it.

The standalone pages stay: they show the same element alone, and the
end-to-end sync steps (`tools/sync_e2e/`) and browser tests drive them with
`document.getElementById()`. Nothing in the browser links to them any more.

## How

- **The elements** (`resources/sidebar/settings/`): plain custom elements
  (`CompassSettingsPage` in `page_base.ts`), not Polymer or Lit. Each
  renders a heading and a card like upstream's `settings-section`, with its
  markup (`*.html`, wrapped by `html_to_wrapper` as a native template) in
  its light DOM, so ids stay reachable from the standalone page's document.
  Inside chrome://settings the element sits in `<settings-main>`'s shadow
  tree, so every style rule is scoped under `.compass-page`.
- **No cr_elements.** The same modules also make the standalone pages, so
  they stay plain HTML controls with the colors.css tokens.
- **Search:** each element has `searchContents(query)`, which
  `<settings-main>` calls for every section; a section whose text lacks the
  query is hidden. Nothing is highlighted.
- **Loading:** settings_main.ts imports `/compass/settings/*_page.js`, as
  it imports `/shared/settings/...`: settings' BUILD.gn maps `/compass/` to
  `//compass/resources/sidebar`'s TypeScript output (`ts_composite`), and
  optimized builds bundle the sections into settings' bundle. (Upstream's
  lint wants every custom tag in settings_main.html imported there.)
  Unbundled builds load them from the settings data source, where
  `compass::SettingsSections::Create()` (called from `settings::SettingsUI`'s
  constructor) adds the whole Compass resource pak under `compass/`. The
  route, menu item and view for each section are in route.ts / router.ts,
  settings_menu.html and settings_main.html (patches).
- **Mojo:** the same interfaces as the standalone pages
  (`sidebar.mojom` `SettingsHandler`, `ArcImportHandler`, `compass_sync.mojom`
  `PageHandlerFactory`). Their implementations are classes of their own
  (`ShortcutsPageHandler`, `ArcImportPageHandler`, `SyncPageHandlerFactory`),
  owned by the standalone controllers and by `SettingsSections`.
  `settings::SettingsUI` gets a templated `BindInterface()` that hands them
  to `SettingsSections`, and its broker registration lists them (patches).
  `//compass/browser/ui/webui/settings:sections` holds only the header, so
  `//chrome/browser/ui/webui/settings` can include it without depending on
  `//compass/browser/ui`.

## Not done

- Strings are English only, as on the standalone pages.
- The menu icons are upstream's closest (`cr:sync`, `settings20:keyboard-lock`,
  `settings20:archive`); adding Compass icons means patching settings'
  icons.html.ts (a late `<cr-iconset>` would trip `cr-icon`'s assert).
- Search doesn't highlight matches inside the sections.
