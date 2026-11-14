# Claude in the sidebar

Small, background uses of Claude that keep the sidebar tidy, plus quick
answers in the command bar: names for link folders, Tidy Today, shorter tab
titles, and Ask in the command bar. The chat panel and its tools are
claude.md's; this doc reuses its endpoint, credentials and request code.

Status: A1 and A2 done on the desktop and on iOS (14 November 2026);
A3 to A5 and A7 planned, and A6's A3–A5. Desktop first, then iOS and Android.

## Goal

- Link folders get a short name that says what's in them ("Rust async
  runtimes"), instead of their opener's title or host.
- One click turns a messy Today into a few named folders.
- Long tab titles read as short ones in the sidebar.
- A question typed in the command bar gets a short answer there.

Not goals: anything that reads page content in the background, or that
acts without a visible result the user can undo.

## Settings and privacy

- **A switch per feature, each off by default.** Settings → Claude →
  "Claude in the sidebar" lists the features, each a row of its own switch
  and its own model picker (per profile; iOS UserDefaults to match):
  "Name Link Folders" (`compass.claude.name_link_folders`), "Tidy Today"
  (`compass.claude.tidy_today`), "Shorter Tab Titles"
  (`compass.claude.short_titles`) and "Answer Questions in the Command
  Bar" (`compass.claude.command_bar_ask`). There's no master switch. Every
  switch needs Claude set up, and is disabled with a line saying so while
  it isn't.
- **What's sent:** for folder names, Tidy Today and short titles, only
  tab titles and hosts (never paths, query strings or page text). For Ask,
  the question, and the current tab's title and host when the user leaves
  "about this page" ticked.
- **Never:** incognito tabs or windows, or a feature whose switch is off.
  Spaces that don't sync are included: nothing goes anywhere but the
  user's own Claude endpoint, which the chat already uses.
- **Model:** each feature has its own, next to its switch
  (`compass.claude.<feature>_model`, e.g.
  `compass.claude.name_link_folders_model`, default `claude-haiku-5-5`),
  picked from the chat's model list (a gateway's too); the chat keeps its
  own. `ClaudeQuick` takes the model, and the switch it checks, from the
  calling feature.
- **Cost guard:** at most 30 background requests an hour per profile;
  past that, folders keep their names until the next hour. Errors back
  off (1 min, then 10).

## Shared core

- **`ClaudeQuick`** (`browser/claude/claude_quick.{h,cc}`): one short,
  non-streaming request — system prompt, one user message, `max_tokens`
  ≤ 200, no tools, 15 s timeout — through the chat's endpoint, auth styles
  and cache levels (`ClaudeRequest`, `ClaudeApi`). Returns text or an
  error; never shows UI itself. Per profile, keyed service.
- **Prompts** in `common/claude/sidebar_prompts.md` (one section per
  feature), shared by desktop, iOS (bundle resource) and Android, each
  asking for JSON (`{"name": "…"}`, `{"folders": [...]}`,
  `{"titles": {...}}`) so parsing is strict; anything else is dropped.
- **Names:** 1–3 words, title case per the user's language (the prompt
  passes the browser locale), no emoji, at most 32 characters; trimmed and
  checked in C++ (`SanitizeAiName()`), else dropped.

## 1. Names for link folders

Link folders (sidebar.md, "Link folders") are named after their opener.
With "Name Link Folders" on:

- **When:** a link folder in Today reaches 3 entries, then 6, then 12;
  each time 5 s after its last change (debounced per folder), Claude names
  it from its entries' titles and hosts. A new name replaces the old only
  if it differs.
- **The user's name wins.** A folder records where its name came from:
  `SidebarNode::NameSource` = `kOpener`, `kClaude` or `kUser`. Renaming by
  hand sets `kUser`, and Claude never names that folder again. Synced
  (a new optional field in the spaces specifics; older clients ignore it
  and treat every name as the user's), so another device doesn't rename
  it either. Only the device whose window made the folder asks.
- **Shown:** the new name fades in with a short sparkle on the folder's
  icon. The folder's menu gets "Undo Name" (back to the previous name,
  and `kUser` so it stays) while the name is Claude's.
- Folders made by hand ("+" in Today), Pinned folders and tab-group
  folders are never named automatically; their menu gets "Suggest a Name"
  (one request, the name shown with Undo), also under "Name Link
  Folders". "Undo Name" stays whenever a name is Claude's, switch or not.

## 2. Tidy Today

- Its own switch, "Tidy Today" (`compass.claude.tidy_today`), and model.
- "Tidy" in Today's header, beside "Clear" (shown on hover, as Clear is),
  and in the command bar. Disabled with fewer than 5 loose entries.
- One request with Today's top-level loose entries (titles, hosts, ids;
  splits as one entry; folders and their contents left alone). Claude
  returns up to 6 groups of 2+ entries each, with names; entries in no
  group stay loose.
- Applied as one change: each group becomes a Today folder (`kClaude`
  names) at the place of its first entry; the toast "Tidied into N
  folders" has Undo, which puts every entry back where it was and removes
  the folders (one `SpaceModel` batch, so sync sees one change).
- Never closes, archives or moves tabs to other spaces.

## 3. Shorter tab titles

- With its own switch on, "Shorter Tab Titles"
  (`compass.claude.short_titles`, with its model), a tab whose title is
  longer than the row shows
  (about 40 characters, or with a site suffix like " · GitHub", " - YouTube",
  " | The Verge") gets a short title: "Pull request #123 · org/repo ·
  GitHub" → "PR #123: fix login".
- Display only: `SidebarNode::ai_title_` (not synced, not saved; made
  again after a restart when needed). The hover card, the window title and
  the command bar keep the page's title; a user's own name
  (`custom_title_`) always wins.
- Batched: titles that changed in the last 10 s go in one request (up to
  20), cached per (host, title) for the session, so navigating back costs
  nothing.

## 4. Ask in the command bar

- With its own switch on, "Answer Questions in the Command Bar"
  (`compass.claude.command_bar_ask`, with its model; the answer streams on
  that model), a typed line that reads as a question (ends with
  "?", or starts with who/what/why/how/…, in the browser's language) shows
  an "Ask Claude" row at the top; Tab or Return on it asks. Searching stays
  the default for everything else.
- The answer streams into the command bar under the field (the chat's
  streaming client, the feature's model, no tools, a short-answer
  prompt),
  at most ~6 lines; "Continue in Chat" opens the panel with the question
  and answer as the conversation's start. Esc closes it as usual.
- "About this page" (on when a page is shown) adds the tab's title and
  host only; reading the page itself stays the chat's job.

## Platforms

- **Desktop:** all four (`browser/claude/claude_quick.*`,
  `browser/sidebar/` for NameSource and the folder triggers,
  `browser/ui/webui/sidebar/` for Tidy, Undo Name and the sparkle,
  `browser/ui/views/command_bar/` for Ask).
- **iOS:** `ClaudeQuick` in `CompassClaude`, the same prompts, the
  same switches in Settings → Claude; link folders, Tidy and short titles
  in the sidebar; Ask in the command bar.
- **Android:** the desktop's C++ through `ClaudeBridge`; Compose for Tidy
  and the menus.
- The sync field (`NameSource`) lands with the desktop, so iOS and Android
  keep it even before they ask Claude themselves.

## As built (A1, A2)

- `ClaudeQuick` streams the reply through `ClaudeClient` and collects
  it (the client only streams); the 15 s timeout covers reading the key
  too. Requests run side by side. The rate limit and back-off apply to
  background requests; Suggest a Name skips the hourly limit and the
  back-off (the user asked), though its errors count towards the back-off.
  Refusals and replies that aren't one JSON object don't back off.
- **Settings:** "Claude in the sidebar" is built from `SIDEBAR_FEATURES`
  in `claude_page.ts`, one row (switch and model picker) per feature;
  only "Name Link Folders" exists so far. `QuickRequest` carries the
  feature's switch (`feature_pref`) and model; `ClaudeQuick` refuses
  (`kOff`) while that switch is off.
- **Set up means a saved key** (the Claude API's or a gateway's). Claude
  Code mode alone doesn't answer these requests yet (it would start a
  `claude` process per request); the switches say so and stay off.
- A subscription token gets Claude Code's line first in the system prompt,
  as the chat's requests do; the cache levels are the chat's
  (`CacheLevelFor()`), with no step-down retry (two breakpoints at most).
- Names: 1–3 words, at most 32 characters (`SanitizeAiName()`: quotes
  around and a full stop after are trimmed; emoji, control and format
  characters, line breaks and markup drop the name).
- The triggers live in `LinkFolderNamer` (`browser/ui/claude/`), a
  per-profile service that observes the spaces, not in `browser/sidebar/`
  (which can't depend on Claude). "The device that made the folder" is
  `SidebarNode::link_folder_here()`, set by `TabStripBridge` and not
  saved: after a restart a link folder isn't named again, though its name
  and NameSource stay. Entries are the folder's children (a split or a
  nested folder counts once); the request lists up to 20 tabs of the whole
  folder.
- Undo Name goes back to the name before Claude's first, saved with the
  folder (`name_before_claude`) but not synced: another device shows
  Claude's name without Undo Name, and offers Suggest a Name instead.
  Renaming by hand only counts when the name changes.
- "Suggest a Name" shows for any folder with entries (not live folders)
  while "Name Link Folders" is on and a key is saved, in windows that aren't
  incognito; its name is Claude's (kClaude), with Undo Name. A link folder
  this device made can then be named again at its next threshold.
- The sparkle is a CSS animation of the folder's icon and name in the
  sidebar page (`Page.FolderNamed()`, sent only for Claude's automatic
  names and suggestions).

## As built on iOS (A6, first part: A1, A2)

Done on iOS (14 November 2026): `ClaudeQuick`, the prompts, the switch and
model of "Name Link Folders", link folders themselves, and their names.
Tidy Today, short titles and Ask (A3–A5) aren't on iOS yet.

- **`ClaudeQuick`** (`CompassKit/Sources/CompassClaude/ClaudeQuick.swift`):
  the desktop's rules (one system prompt and one user message, no tools,
  `max_tokens` ≤ 200, 15 s, one JSON object back, 30 background requests an
  hour, back-off of 1 then 10 minutes, Suggest a Name exempt), with a plain
  non-streamed request: the streaming client (`ClaudeClient`) is in the
  app target, not CompassKit, and a reply this short doesn't need one. `sanitizeAiName()` is
  `SanitizeAiName()` (Swift's Unicode properties stand in for ICU's:
  emoji presentation and modifiers, any non-ASCII emoji, regional
  indicators). `common/claude/sidebar_prompts.md` is a bundle resource
  (project.yml).
- **Settings → Claude → Claude in the Sidebar:** a `ClaudeFeatureRow` per
  feature, its own switch and model picker (the chat's list, a gateway's
  too), disabled with a line saying why until a key is saved. UserDefaults
  keys are the desktop's pref names.
- **Link folders** (sidebar.md's rules, iOS's ways in): a link's
  long-press "Open in Background", or ⌘-click on iPad
  (`LinkFolders` in CompassModel, per window). Today's "top" is the end of
  its children there (the sidebar shows Today newest first).
- **Names:** `LinkFolderNamer` (CompassClaude), the desktop's triggers (3, 6
  and 12 entries, 5 s), `linkFolderHere` (not saved), Undo Name and Suggest a
  Name in the folder's long-press menu, sparkles on the folder's icon while
  the new name fades in. `NameSource` is saved in the sidebar snapshot and
  synced as `CompassNode.name_source`; a rename in the Rename alert (or by
  Claude's rename_tab) makes a folder the user's.
- **Never:** incognito (its changes never reach the namer, and its folders
  get no Suggest a Name), or with the switch off (`ClaudeQuick` refuses
  before anything is sent).
- **Tests:** `ClaudeQuickTests`, `LinkFolderNamerTests` (fake endpoint),
  `LinkFolderTests`, `NameSourceSyncTests`.

## Milestones

| # | Scope |
|---|---|
| A1 | `ClaudeQuick`, prompts file, the settings' per-feature rows (switch and model), cost guard; unit tests with a fake endpoint |
| A2 | Names for link folders: `NameSource` (model, store, sync field), triggers, Undo Name, Suggest a Name |
| A3 | Tidy Today with Undo |
| A4 | Shorter tab titles |
| A5 | Ask in the command bar |
| A6 | iOS: A1–A5 (A1, A2 done) |
| A7 | Android: A1–A5 |

## Tests

- `ClaudeQuickTest`: request shape (no tools, max_tokens, model), JSON
  parsing and refusals, timeout, rate limit, back-off.
- `SpaceModelTest`: `NameSource` set by each path; a user rename stops
  naming; Undo Name; Tidy's batch and its undo.
- `CompassSpacesSyncBridgeTest`: `NameSource` round-trips; missing field
  reads as `kUser`.
- Browser tests with a fake Claude server (as `ClaudeApiTest` does):
  a link folder renamed at 3 entries and not after a manual rename; Tidy
  makes folders and Undo restores; short titles shown and the hover card
  keeps the full one; the command bar's Ask row and Continue in Chat.
- Off by default: no request is made with a feature's switch off (counted by the
  fake server).

## Open questions

- Whether short titles should also apply to favorites and Pinned entries
  (which often have the user's own name already).
- Whether Tidy should also offer to archive tabs it can't group.
