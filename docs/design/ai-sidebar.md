# Claude in the sidebar

Small, background uses of Claude that keep the sidebar tidy, plus quick
answers in the command bar: names for link folders, Tidy Today, shorter tab
titles, and Ask in the command bar. The chat panel and its tools are
claude.md's; this doc reuses its endpoint, credentials and request code.

Status: planned (14 November 2026). Desktop first, then iOS and Android.

## Goal

- Link folders get a short name that says what's in them ("Rust async
  runtimes"), instead of their opener's title or host.
- One click turns a messy Today into a few named folders.
- Long tab titles read as short ones in the sidebar.
- A question typed in the command bar gets a short answer there.

Not goals: anything that reads page content in the background, or that
acts without a visible result the user can undo.

## Settings and privacy

- **Off by default.** Settings → Claude → "Name Folders and Tidy Tabs"
  (`compass.claude.sidebar_ai`, per profile; iOS UserDefaults
  `ClaudeSidebarAI`). Ask in the command bar has its own switch,
  "Answer Questions in the Command Bar" (`compass.claude.command_bar_ask`),
  also off. Both need Claude set up (a key, a gateway or Claude Code); the
  switches say so while it isn't.
- **What's sent:** for folder names, Tidy Today and short titles, only
  tab titles and hosts (never paths, query strings or page text). For Ask,
  the question, and the current tab's title and host when the user leaves
  "about this page" ticked.
- **Never:** incognito tabs or windows, or a profile with the switch off.
  Spaces that don't sync are included: nothing goes anywhere but the
  user's own Claude endpoint, which the chat already uses.
- **Model:** a "Fast model" setting (`compass.claude.fast_model`, default
  `claude-haiku-5-5`), listed with the chat's models; the chat keeps its
  own. A gateway's model list applies to both.
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
With the switch on:

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
  (one request, the name shown with Undo).

## 2. Tidy Today

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

- With the switch on, a tab whose title is longer than the row shows
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
- Settings has its own tick under the switch, "Shorter Tab Titles", for
  those who want folder names but not this.

## 4. Ask in the command bar

- With its switch on, a typed line that reads as a question (ends with
  "?", or starts with who/what/why/how/…, in the browser's language) shows
  an "Ask Claude" row at the top; Tab or Return on it asks. Searching stays
  the default for everything else.
- The answer streams into the command bar under the field (the chat's
  streaming client, the chat model, no tools, a short-answer prompt),
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

## Milestones

| # | Scope |
|---|---|
| A1 | `ClaudeQuick`, prompts file, settings switches and Fast model, cost guard; unit tests with a fake endpoint |
| A2 | Names for link folders: `NameSource` (model, store, sync field), triggers, Undo Name, Suggest a Name |
| A3 | Tidy Today with Undo |
| A4 | Shorter tab titles |
| A5 | Ask in the command bar |
| A6 | iOS: A1–A5 |
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
- Off by default: no request is made with the switch off (counted by the
  fake server).

## Open questions

- Whether short titles should also apply to favorites and Pinned entries
  (which often have the user's own name already).
- Whether Tidy should also offer to archive tabs it can't group.
