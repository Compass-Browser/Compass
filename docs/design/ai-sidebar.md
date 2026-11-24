# Claude in the sidebar

Small, background uses of Claude that keep the sidebar tidy, plus quick
answers in the command bar: names for link folders, Tidy Today, shorter tab
titles, and Ask in the command bar. The chat panel and its tools are
claude.md's; this doc reuses its endpoint, credentials and request code.

Status: A1, A2 and A3 (Tidy Today) done on the desktop and on iOS (14
November 2026); A4 (Shorter tab titles) and A5 (Ask in the command bar)
done on the desktop and on iOS (14 November 2026), so A6 is done; A7
(Android) done on 18 November 2026. Desktop first, then iOS and Android.

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
  (`compass.claude.short_tab_titles`) and "Answer Questions in the Command
  Bar" (`compass.claude.command_bar_ask`). There's no master switch. Every
  switch needs Claude set up, and is disabled with a line saying so while
  it isn't; with "Answer with" Apple Intelligence, the first three need
  the on-device model instead ("On this device").
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
  ≤ 400 (Tidy's reply lists its entries; a name asks 60), no tools, 15 s
  timeout — through the chat's endpoint, auth styles
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
  (`compass.claude.short_tab_titles`, with its model), a tab whose title is
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

## As built (A3, Tidy Today)

- **`TodayTidier`** (`browser/ui/claude/today_tidier.*`), a per-profile
  service like `LinkFolderNamer` (none off the record): `Tidy(space)` sends
  one `ClaudeQuick` request (the "tidy_today" prompt section, the feature's
  model, `max_tokens` 400, not background: no hourly limit or back-off, as
  Suggest a Name), listing up to 40 loose entries (Today's top-level tabs
  and splits, from the top) as "<n>. <title> (<host>)", a split's titles
  joined by " + ". The ids are those numbers, mapped back to the entries.
  One tidy per space at a time (`IsBusy()`).
- **The reply** (`ParseTidyReply()`, `claude_quick.*`): `{"folders":
  [{"name", "entries"}]}`; unknown ids and ids already taken (in that folder
  or an earlier one) are dropped, then a folder whose name
  `SanitizeAiName()` drops, or with fewer than 2 entries left, goes; the
  first 6 that pass are used. A reply of another shape, or with nothing
  left, is "Claude found nothing to group."
- **Applied** by `SpaceModel::TidyToday()`, synchronously, so sync sends one
  change: each folder goes where its topmost entry was, its entries in
  Today's order, named `kClaude` with "New Folder" as the name before (so
  Undo Name works). Entries closed or moved meanwhile are skipped.
  `SpaceModel::UndoTidy()`, also one go, empties the folders back into
  Today and removes them, then puts Today's entries from before in their
  order with what came since on top (where new entries go); something
  dragged into a tidied folder since is left loose. The service keeps the
  last 10 tidies for Undo (not saved).
- **Header and toast:** the sidebar page gets `TidyChanged(available,
  busy)` (the switch and the key, watched with a `PrefChangeRegistrar`;
  never in incognito) and counts the loose entries itself. The toast
  (`ShowToast(text, undo_tidy_id)`) shows for 8 s in each window showing the
  space: "Tidied into N folders" with Undo, or the error ("Couldn't tidy
  Today: Claude took too long.", "…couldn't be reached.", "…isn't set
  up."). A switch turned off while Claude answers drops the answer.
- **Command bar:** "Tidy Today" (`CommandBarCommand::kTidyToday`), listed
  only when the window's space can be tidied (switch on, set up, 5 loose
  entries, none on its way).
- **Tests:** `ClaudeQuickTest.ParseTidyReply` (unknown, repeated and taken
  ids, singletons, bad names, more than 6) and `SidebarPrompts`;
  `SpaceModelTest.TidyTodayAndUndo` and `UndoTidyAfterChanges` (exact
  restore, splits whole, folders and Pinned untouched);
  `SidebarPageHandlerTest.TidyToday` (header state per switch and key, too
  few entries, toast and Undo, nothing to group);
  `ClaudeSettingsHandlerBrowserTest.SidebarAiSettings`;
  `TodayTidierBrowserTest` with a fake Claude server (`TidiesAndUndoes`,
  `TidyButtonAndUndoToast` through the page, `NothingWithTheSwitchOff`: no
  button, no request).

## As built (A4, Shorter tab titles)

- **The switch and model:** "Shorter Tab Titles"
  (`compass.claude.short_tab_titles`, off) and
  `compass.claude.short_tab_titles_model` (default `claude-haiku-5-5`), a
  row of `SIDEBAR_FEATURES`. (This section's first draft named the pref
  `compass.claude.short_titles`; the built name is `short_tab_titles`, which
  iOS's UserDefaults key matches.)
- **Which tabs** (`ShortTitler::IsEligible()`): tab nodes of a space, in
  Today (folders and split panes included) and in Pinned, without a custom
  title, whose title `NeedsShortTitle()`: longer than 40 characters, or
  ending with a site suffix (" · ", " - ", " | ", " — ", " – " or " :: "
  and at most 30 characters after it). Not favorites: their tiles show no
  title, so a request for them would buy nothing (the open question's
  answer: Today and unnamed Pinned entries; a custom title always wins).
- **`SidebarNode::short_title()`**, set by `SpaceModel::SetShortTitle()`:
  not saved, not synced, not in `display_title()`. Observers hear
  `OnNodeShortTitleChanged()`, not `OnNodeChanged()`, so the store and the
  sync bridge never see it. `SetTitle()` with a new title drops it first.
  The sidebar's row shows `customTitle || shortTitle || title`
  (`Node.short_title`, `Page.NodeShortTitleChanged()`); its tooltip, Rename,
  the hover card, the window title, the command bar and Claude's
  descriptions keep the title.
- **`ShortTitler`** (`browser/ui/claude/short_titler.*`), a per-profile
  service like `LinkFolderNamer` (none off the record, so incognito's spaces
  are never followed), made by the sidebar's page handler. It follows the
  profile's spaces; an eligible tab whose (host, title) isn't cached waits
  for the batch: one background `ClaudeQuick` request 10 s after the first
  title waiting (`kBatchDelay`), or at once when 20 wait (`kMaxBatch`),
  listing them as "<n>. <title> (<host>)". The "short_titles" prompt
  section asks for `{"titles": {"<id>": "…"}}`; `ParseShortTitlesReply()`
  keeps known ids whose title `SanitizeShortTitle()` keeps: the name rules
  without the word limit, up to 40 characters (`kMaxShortTitleLength`). A
  title that isn't shorter than the page's is none.
- **Cache:** per (host, title), for the session (up to 2,000, then it
  starts over), also "none", so a title seen again costs nothing; tabs with
  the same title share one request and one answer. Requests count against
  the hourly limit; one refused by it, the back-off or an error leaves those
  titles long until they change.
- **The switch off** (or the key removed) clears every short title and what
  waits; on again, cached ones come back without a request, the rest are
  asked for.
- **Tests:** `ClaudeQuickTest.NeedsShortTitle`, `SanitizeShortTitle`,
  `ParseShortTitlesReply`, `SidebarPrompts`; `ShortTitlerTest` (a fake
  endpoint: nothing while off or not set up, the 10 s batch with hosts and
  no paths, custom titles and short titles left out, the cache with
  "none", a new title asking again and a user's name winning meanwhile, 20
  at once, the switch clearing and restoring, Pinned but not favorites);
  `SidebarPageHandlerTest.ShortTitles`;
  `ClaudeSettingsHandlerBrowserTest.SidebarAiSettings`;
  `ShortTitlerBrowserTest` with a fake Claude server (`ShowsShortTitles`:
  the row shows it while its tooltip and the hover card keep the title, the
  same title again makes no request, a name wins; `NothingWithTheSwitchOff`).

## As built (A5, Ask in the command bar)

- **The switch and model:** "Answer Questions in the Command Bar"
  (`compass.claude.command_bar_ask`, off) and
  `compass.claude.command_bar_ask_model`, whose default is
  `claude-sonnet-5-5` (`prefs::kDefaultAskModel`): an answer the user waits
  for and reads, so better than Haiku, and quicker than the chat's Opus.
  The chat's model isn't used: each feature has its own.
- **Which lines** (`LooksLikeQuestion(text, locale)`, `claude_quick.*`):
  two words or more, no "://", and ending with "?" (also "？" or "؟"),
  starting with "¿", or starting with a question word: English's (who,
  what, why, how, when, where, which, whose, whom; "what's" counts) always,
  and the browser language's for Italian, French, German, Spanish,
  Portuguese and Dutch, including two-word starts ("por qué", "est-ce
  que", "o que"). Verbs like "is" or "can" don't start one without a "?":
  "can opener" is a search.
- **The row:** with the switch on and Claude set up (never off the record),
  such a line gets `ResultKind::kAskClaude`, "Ask Claude" with the question
  as its subtitle, first, above the default suggestion; the field then
  completes nothing inline, and the chat's row at the end reads "Ask in
  Chat". Tab or Enter on it, or a click, asks (`PageHandler.Ask(question,
  about_page)`); elsewhere they do what they did.
- **"About this page":** a small switch on the row, on by default, shown
  when the active tab is a web page (http or https; `Result.about_page`
  carries its title). On, the request starts with "I'm looking at a page
  titled "<title>" on <host>." (`DescribeAskPage()`); the page's text never
  goes.
- **The request** (`MakeAskRequest()`): `ClaudeQuick::Stream()`, the chat's
  streaming client (`ClaudeClient`) on the chat's endpoint, key, auth style
  and cache level, the feature's model, the "command_bar_ask" prompt
  section (plain text, at most about 6 short lines, the answer first),
  `max_tokens` 400, no tools, no thinking, not background (no hourly limit
  or back-off), 60 s at most (`kStreamTimeout`). `Page.OnAnswer(text)`
  brings the text so far as it grows, `OnAnswerDone(error)` the end.
  Another Ask, a new opening of the bar, or Close drops the answer on its
  way (`ClaudeQuick::Cancel()`).
- **The answer** replaces the results under the field: plain text, line
  breaks kept, about 6 lines high, then it scrolls; "Claude is answering…"
  until the first words; an error line ("Claude took too long.") if it
  fails. Enter, or the "Continue in Chat" button, continues; typing goes
  back to the results; Esc closes the bar, as always.
- **Continue in Chat** (`PageHandler.ContinueInChat()`,
  `ClaudePanel::Continue()`, `chat.mojom` `Page.OnContinue()` and
  `TakeQuestion()`'s answer for a panel that wasn't loaded): the panel opens
  with a new chat whose first exchange is the question (with the page line
  before it in the message, if it was sent) and the answer, as plain user
  and assistant messages; nothing is sent until the user writes, and the
  next message goes on from them. The handler starts a new chat as a first
  message would (approvals, memory and developer tools frozen anew, Claude
  Code's session forgotten). Claude Code keeps its own conversation, so in
  Claude Code mode the next message carries the exchange as a text block.
  Without an answer yet, Continue asks the question in the chat instead.
- **Tests:** `ClaudeQuickTest.LooksLikeQuestion`, `DescribeAskPage`,
  `StreamsAnswers` (request shape: model, no tools, the page line,
  `max_tokens`, the prompt; the text as it grows; no hourly limit),
  `StreamOffAndCancel`; `CommandBarPageHandlerTest.AskRowForQuestions`
  (off, not set up, on; the row first, no inline completion, "Ask in Chat";
  other text; another feature's switch) and `NoClaudeInIncognito`;
  `ClaudeSettingsHandlerBrowserTest.SidebarAiSettings`;
  `CommandBarAskBrowserTest` with a fake Claude server
  (`AsksAndContinuesInChat`: the row with "About this page", Enter, the
  streamed answer, the request's title and host without the path, Continue
  in Chat's two turns and the next message's three messages;
  `NothingWithTheSwitchOff`).

## As built on iOS (A6: A1–A5)

Done on iOS (14 November 2026): `ClaudeQuick`, the prompts, the switch and
model of "Name Link Folders", link folders themselves, and their names;
then Tidy Today (A3), shorter tab titles (A4) and Ask in the command bar
(A5).

- **`ClaudeQuick`** (`CompassKit/Sources/CompassClaude/ClaudeQuick.swift`):
  the desktop's rules (one system prompt and one user message, no tools,
  `max_tokens` ≤ 400, 15 s, one JSON object back, 30 background requests an
  hour, back-off of 1 then 10 minutes, Suggest a Name and Tidy exempt), with a plain
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
- **Tidy Today** (`TodayTidier`, CompassClaude; `AppServices.tidier`): the
  desktop's request, reply and limits (the "tidy_today" section, the
  feature's model, `max_tokens` 400, not background; up to 40 loose
  entries from Today's top as "<n>. <title> (<host>)", a split's titles
  joined by " + "; `parseTidyReply()` is `ParseTidyReply()`: up to 6
  folders of 2+ known, untaken ids). Loose entries are the ones the phone
  shows (`isPhoneVisible`: the desktop's own pages stay out). "Tidy" sits
  before "Clear" in Today's header, always visible (no hover on iOS) while
  the switch is on and a key saved, never in incognito; dimmed below 5
  loose entries, "Tidying…" with pulsing sparkles while Claude answers.
  The result is the toast over the bar the closes use (`ClosedToast`):
  "Tidied into N folders" with Undo for 8 s, or the desktop's error lines.
  iOS's command bar lists no commands, so there's no "Tidy Today" there.
- **Applied** by `SpaceStore.tidyToday(in:groups:)` and undone by
  `undoTidy(_:)`, each one `batch`, so sync sends one change. Today's top
  is the end of its children: a folder goes where its topmost (highest
  index) entry was, its entries topmost first; Undo puts what came since
  above the entries from before. Folders are `.claude` with "New Folder"
  as the name before, so Undo Name works. The last 10 tidies can be
  undone (not saved).
- **Tests:** `ClaudeQuickTests`, `LinkFolderNamerTests` (fake endpoint),
  `LinkFolderTests`, `NameSourceSyncTests`, `TodayTidyTests` (apply and
  exact undo, splits whole, folders and Pinned untouched, undo after
  changes, incognito) and `TodayTidierTests` (reply parsing: bad ids,
  duplicates, singletons, bad names, more than 6; the request; apply and
  undo through a fake endpoint; nothing sent with the switch off or in
  incognito; the switch turned off mid-request; errors; no hourly limit).
- **Shorter tab titles** (`TabTitleShortener`, CompassClaude;
  `AppServices.shortener`): the desktop's switch and model
  (`compass.claude.short_tab_titles`, `compass.claude.short_tab_titles_model`,
  default `claude-haiku-5-5`), tabs (`isEligible()`: Today and Pinned, not
  favorites, no custom title, `isLong()` = `NeedsShortTitle()`), request
  ("short_titles", "<n>. <title> (<host>)" under "The tabs:", background:
  the hourly limit and back-off apply), batch (10 s after the first title
  waiting, at once when 20 wait, 20 a request), reply
  (`parseShortTitlesReply()`, `sanitizeShortTitle()` = `SanitizeShortTitle()`,
  a title that isn't shorter is none) and cache (per (host, title) while
  the app runs, "none" too, up to 2,000; a failed request caches nothing).
  Only the sidebar's rows show them (`NodeRow`: split panes, the tab picker,
  the command bar, the page menu, the chat and sync keep the page's title);
  nothing on the node, so nothing is saved or synced; a custom title wins;
  never in incognito. The switch off (or no key) hides them and drops what
  waits; on again, cached ones come back without a request. iOS asks
  for what the sidebar shows rather than following every tab: a row asks
  (`want()`) when it shows, when its title changes and when the switch goes
  on, so a title that failed is asked for again when its row shows next.
- **Ask in the command bar** (`CommandBarAsker`, CompassClaude, one per
  command bar): the desktop's switch and model
  (`compass.claude.command_bar_ask`, `compass.claude.command_bar_ask_model`,
  default `claude-sonnet-5-5`, `ClaudeQuick.defaultAskModel`).
  `isQuestion()` is `LooksLikeQuestion()` (the same words and languages).
  An "Ask Claude" card then tops the results and Return asks (otherwise
  Return searches); "About This Page", on by default, shows while the shown
  tab is an http(s) page outside incognito and puts `describePage()`'s line
  ("I'm looking at a page titled "<title>" on <host>.") before the
  question. **Streamed:** the app's chat client (`ClaudeClient.stream`) is
  reused with the body `CommandBarAsker.body()` builds (the
  "command_bar_ask" section, the feature's model, `max_tokens` 400,
  `stream`, no tools, no thinking, the chat's cache level and
  subscription-token line), so the answer grows in a card under the field;
  60 s at most ("Claude took too long."). Another line in the field, or
  closing the bar, drops it. "Continue in Chat" (or Return once answered)
  opens the Claude sheet on a new chat whose first exchange is the question
  (with the page line, if it went) and the answer (`ClaudeChat.seed`,
  `Conversation.addAssistant`); nothing is sent until the user writes. The
  user's action: no hourly limit or back-off; never in incognito; the
  switch turned off mid-answer stops it.
- **Tests:** `TabTitleShortenerTests` (parsing, sanitizing, long titles,
  the cache per host and title, batching after the window and at once at
  20, a longer answer dropped, the user's name wins, Pinned but not
  favorites, nothing with the switch off or in incognito, the hourly limit,
  errors not cached) and `CommandBarAskerTests` (`LooksLikeQuestion`'s
  cases, the page line, the request shape, streaming, nothing sent with the
  switch off or in incognito, errors, the timeout, cancel, the switch
  turned off mid-answer, Continue in Chat's conversation).

## On this device (A8: Apple Intelligence)

Name Link Folders, Tidy Today and Shorter Tab Titles can be answered by
Apple Intelligence's on-device model instead of the chat's provider, on a
Mac and on iOS (issue #1): no key, Claude Code mode too, and titles and
hosts never leave the device. Ask in the command bar stays on the chat's
provider (a ~3B model answers knowledge questions poorly).

- **The setting.** "Answer with": "Same as chat" (the default) or "Apple
  Intelligence (on this Mac)" (`compass.claude.sidebar_on_device`, a bool;
  iOS UserDefaults of the same name, "Answer With" in Settings → AI in the
  Sidebar, offered where the device can run Apple Intelligence). With it,
  the three features hide their model pickers, and their switches can be on
  only while the model is available. Unavailable, the settings say why
  (an older system or a device that can't run it, Apple Intelligence off,
  the model still downloading) and the switches stay off: there's no
  silent fallback to the cloud.
- **One library, two platforms.** `CompassOnDevice` in CompassKit
  (`ios/CompassKit/Sources/CompassOnDevice/`, no dependencies):
  `OnDeviceModel.availability`, `OnDeviceModel.answer(feature:system:user:)`
  and `OnDeviceLines`, the Mac helper's protocol. Each feature's reply is
  generated as a fixed `@Generable` type (guided generation: free-form
  replies broke the JSON), greedily, with the guardrails for transforming
  given text, and handed back as the JSON object Claude's prompt asks for
  (short titles come as `[{id, title}]` and become `{"titles": {...}}`), so
  `ParseTidyReply()`, `ParseShortTitlesReply()` and the callers don't
  change. iOS's `ClaudeQuick` calls it directly (`OnDeviceBackend.system`).
- **The Mac's helper.** Foundation Models has only a Swift API, and
  Chromium's macOS toolchain has no Swift tool, so the browser asks
  `compass-on-device` (`tools/on_device/`): that library and a two-line
  `main.swift`, built by a GN action that runs `xcrun swiftc`
  (`build_helper.py`; FoundationModels linked weakly, so it starts on any
  macOS and says "unsupported"; the SDK path resolved, since swiftc finds
  `@Generable`'s macro plugin next to the real SDK, not Chromium's link to
  it). It ships as `Contents/MacOS/compass-on-device` (`bundle_data`, the
  `chrome/BUILD.gn` patch) and is signed like `compass-mcp` (the
  `parts.py` patch). `compass_on_device_model` (GN arg, default `is_mac`;
  needs Xcode with the macOS 26 SDK or newer) builds it, and its buildflag
  `COMPASS_ON_DEVICE_MODEL` lets `OnDeviceModel::Get()` offer it.
- **The protocol** (JSON, a line each way): `{"id", "status": true}` →
  `{"id", "availability": "available" | "unsupported" | "disabled" |
  "not_ready"}`; `{"id", "feature", "system", "user"}` → `{"id", "text"}`
  or `{"id", "error": "unavailable" | "refused" | "failed" |
  "bad_request"}`.
- **`OnDeviceModel`** (`browser/claude/on_device_model.{h,cc}`, Mac): one
  helper process per request (it ends when its stdin closes), one request
  at a time on a background sequence, each within `ClaudeQuick::kTimeout`
  of being asked, waiting included; a helper past its time is killed. It
  remembers the last availability; unknown counts as available until a
  request or the settings' check (`CheckOnDevice()`) says otherwise.
- **`ClaudeQuick`** (both platforms): a `QuickRequest` carries an
  on-device feature (the prompt's id) and system prompt
  (`SetOnDevicePrompt()`, iOS `withOnDevicePrompt`). With "Answer with" on
  and a model, `UsesOnDevice()` sends it there; `IsEnabled()` then needs
  the switch and the model, not a key. Errors map onto `QuickError`s
  (unavailable is `kNotSetUp`). Background requests: at most 120 an hour
  on the device (for the battery, not cost); the back-off is the same.
- **Prompts.** The small model gets its own sections where Claude's
  didn't work for it: `folder_name_on_device`, `tidy_today_on_device` and
  `short_titles_on_device` (`<id>_on_device`, else `<id>`'s). They spell
  out the language (`{language}`, the English name of the browser's
  language: with "it-IT" alone, names came back in English), say what an
  id is (it took numbers from titles), and are shorter, with one example.
- **Quality**, tried on a MacBook Pro (macOS 27.2) with 15 mixed Italian
  and English tabs: names in Italian in 0.4–0.8 s ("Viaggio a Lisbona",
  "Ricette"; empty for tabs with nothing in common, though not always);
  Tidy in 1–2 s, five folders, one trip split in two; short titles for 10
  of 15 in under 3 s ("Modello 730/2026", "Fix login redirect loop").
  Weaker than Claude, as the settings say; Undo Name and Tidy's Undo cover
  its mistakes, and the browser's checks (`SanitizeAiName()`, at least 2
  entries a folder) still drop what's off.
- **Tests:** `OnDeviceModelTest`, `OnDeviceHelperTest` (fake helpers as
  shell scripts: the request line, no reply, the timeout),
  `ClaudeQuickOnDeviceTest` (no key, the on-device prompt, errors, the
  switch off, the chat's provider, unavailable), `ClaudeQuickTest.OnDevicePrompts`;
  iOS `OnDeviceLinesTests` and `ClaudeQuickOnDeviceTests`. The real model
  only by hand.

## As built on Android (A7)

The desktop's C++ (android.md, "Claude in the sidebar (P12)"):
`LinkFolderNamer`, `TodayTidier` and `ShortTitler` build for Android over
`SpacesAndroid` (`browser/ui/claude/sidebar_spaces.h`), and
`ClaudeSidebarBridge` (`browser/android/`) brings Suggest a Name, the
sparkle, Tidy with its toast and Undo, and Ask to the Compose shell. The
switches and models are in the Claude sheet's ⋯ menu, Claude in the
Sidebar. Folders got a long-press menu: Rename, Undo Name or Suggest a
Name. Android's own link folders (android.md, P11) are named
automatically, as on the desktop. Tried in the emulator against a fake Messages API; no unit tests of
the Android side yet.

## Milestones

| # | Scope |
|---|---|
| A1 | `ClaudeQuick`, prompts file, the settings' per-feature rows (switch and model), cost guard; unit tests with a fake endpoint |
| A2 | Names for link folders: `NameSource` (model, store, sync field), triggers, Undo Name, Suggest a Name |
| A3 | Tidy Today with Undo |
| A4 | Shorter tab titles |
| A5 | Ask in the command bar |
| A6 | iOS: A1–A5 (done) |
| A7 | Android: A1–A5 (done) |
| A8 | Apple Intelligence for A2–A4 on the Mac and iOS (done) |

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

- Whether Tidy should also offer to archive tabs it can't group.
