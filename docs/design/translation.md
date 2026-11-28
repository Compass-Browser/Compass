# Page translation

Status: designed 2026-11-28; T1 started (native spike done, see "Spike").
Based on Chromium 159.0.8154.34.

## Goal

Translate the page in place, on the device, as Safari, Orion and Firefox
do: a page in a language the user doesn't read offers a translation; one
click translates it, keeping its layout, links and forms; "Show original"
puts it back at once.

- **On the device.** The page's text never leaves the computer or phone.
  The only network traffic is downloading a language's model, once, from
  Mozilla's servers, and only after the user has agreed to that.
  Translation works in Incognito and offline once the model is downloaded.
- **One engine on every platform:** Mozilla's Bergamot (Marian), which
  Firefox Translations uses, with Firefox's open models. On iPhone and
  iPad, Apple's Translation framework comes first, with Bergamot for the
  pairs Apple lacks (see "iOS").
- **One page script everywhere.** The code that walks the page, sends text
  to the engine and writes the translation back is shared by every
  platform (`common/translate/`), as Reader's is (reader-mode.md), so a
  page translates the same way everywhere.
- **Chromium's UI stays.** On the desktop the translate bubble, "Always
  translate", "Never translate", the page-language detection and the
  Languages settings (settings-window.md, "Languages") are upstream's; only
  the engine underneath changes.

### Not Google's translate, not Chrome's TranslateKit

- Upstream's page translation loads `element.js` from
  `translate.googleapis.com` and sends the text to Google. Compass builds
  without Google API keys (privacy.md), so `TranslateManager` turns
  translation off entirely today (`translate_manager.cc`: `IsAvailable()`
  and `PreventAllTriggering` both check `google_apis::HasAPIKeyConfigured()`).
- Chrome's on-device translation (the web `Translator` API) runs Google's
  closed `libtranslatekit` from the component updater, which Compass can't
  ship. Its service, sandbox and file plumbing are open, though
  (`components/on_device_translation`), and they load the library through
  a small C ABI. **Compass builds its own library with that ABI, on
  Bergamot**, so the service, its sandbox and the `Translator` web API work
  unchanged.

## The engine: `libcompass_translatekit`

### Bergamot

- **Source:** `mozilla/translations`, `inference/` (Bergamot 0.6.0, the
  revision Firefox pins in `toolkit/components/translations/bergamot-translator/moz.yaml`),
  vendored in `third_party/bergamot/` (about 600 files, 8 MB) by
  `tools/translation/update_bergamot.py`, which fetches each project at its
  pinned revision, copies what inference needs and applies
  `third_party/bergamot/patches/`. Its README lists each part, revision and
  license: Bergamot (MPL-2.0), Marian (MIT), intgemm, simd_utils, onnxjs's
  GEMM, ssplit-cpp (Apache-2.0), PCRE2 10.49, and Marian's small
  third-party code (CLI11, yaml-cpp, spdlog, Pathie, phf, mio, cnpy,
  half_float).
- **Built with GN**, not CMake (`third_party/bergamot/BUILD.gn`), with
  Chromium's toolchain. `WASM_COMPATIBLE_SOURCE` is defined, as in
  Firefox's build: it compiles out Marian's training code, faiss, LSH and
  gzip input, so those aren't vendored. ruy, cpuinfo, zlib, SentencePiece
  (v0.2.2, which has the `Load` and `LoadFromSerializedProto` Marian uses),
  protobuf and eigen are Chromium's own rather than Marian's copies.
  Firefox only ships the WASM build, so no native platform is tested
  upstream; we test our own.
- **Matrix maths:**
  - x86-64 (Windows, Linux, Intel Macs): intgemm for the int8 model,
    dispatching at run time between SSSE3, AVX2 and AVX-512; the float
    GEMM through Marian's ONNX SGEMM (no BLAS to ship). If that proves too
    slow, OpenBLAS is the fallback (open question).
  - arm64 (Apple silicon, Windows on Arm, Android, iOS): ruy, with Apple's
    Accelerate for the float GEMM on Apple platforms.
- **Settings, as Firefox's** (`translations-engine.worker.js`): beam size
  1, `mini-batch-words` 1024, `max-length-break` 128, `workspace` 128,
  `skip-cost`, `gemm-precision int8shiftAlphaAll`, `alignment soft`; no
  quality scores; no lexical shortlist (Firefox's default; it skips the lex
  files and translates better).
- **Pivoting:** every model is to or from English. A pair without English
  (Italian to German) runs two models through `pivotMultiple()`, as Firefox
  and Chromium both do.

### Spike (2026-11-28, macOS arm64)

Bergamot 0.6.0 (`mozilla/translations` `eea6e5a8`) built natively with
its own CMake, Apple clang, ruy and Accelerate, and Firefox's German→English
model (`translations-models-v2`, version 3.0, `base-memory`, the hashes as
in the record) with Firefox's settings:

- **Builds after four fixes**, all from newer compilers, none in the
  engine: SentencePiece's trainer (`constexpr` of an out-of-range enum; the
  trainer isn't needed), Marian's `-Werror` (variable-length arrays in
  `ExceptionWithCallStack.cpp`), Marian's old zlib (`fdopen` defined away
  on macOS; Chromium's zlib replaces it), and Marian's platform defines
  (`ARM`, `SSE`, `FMA`, `COMPILE_CPU`, `BLAS_FOUND`, and `<fenv.h>`) not
  reaching the translator's own files. The GN build sets them itself.
- **Speed** on an M5 Max, 2,100 words of news text, every sentence unique
  (Bergamot caches sentences): 1.5 s on 1 thread (about 1,400 words a
  second), 0.55 s on 4, no gain past 4. A short paragraph, model load
  included: 0.4 s.
- **Memory:** about 210 MB resident for one model.
- **Quality:** good on news text; HTML mode keeps links (with their
  attributes), bold and `<br>` in place; an inline tag can take a neighbour's
  punctuation (`After that<em>, restart</em>`), as in Firefox.
- **Sources compiled:** 242 files, of which ruy, cpuinfo and zlib come
  from Chromium in the GN build, and SentencePiece's protobuf-lite from
  Chromium's protobuf if its version allows. Marian also compiles its
  training code and faiss when not built for WASM; the GN build should
  leave them out if nothing at inference reaches them.
- **The GN build** (Chromium's clang and libc++, official Release) is
  faster still: the same 2,100 words in 0.76 s on 1 worker, 0.48 s on 2,
  0.29 s on 4 (none faster on 6); the model loads in about 85 ms; German
  to Italian through English, 0.54 s. A Debug build (bounds-checked
  libc++) translates the same page without a check firing.
- **Windows x64** (win1, an i9-7980XE, so intgemm's AVX-512 path; the
  `fast` build): the same translation word for word, 1.67 s on 1 worker,
  0.69 s on 4, 0.68 s on 8; the model loads in 160 ms.
- **In the app** (Debug, bundled): startup finds the library in the
  framework's Libraries, the sandboxed service loads it, and a page's
  `Translator.create({sourceLanguage: 'de', targetLanguage: 'en'})`
  translates, with the package from `--translate-kit-packages`. Upstream
  calls a pair "available" only when both languages are among the
  profile's; otherwise it's "downloadable", and `create()` wants a user
  gesture even with the package installed. Page translation (T4) asks
  through Compass's own interface, not the web API, so the rule doesn't
  apply to it.
- **Bugs native use found**, both patched (third_party/bergamot/README.md,
  "Local changes"): Bergamot freed the model's memory after the first
  worker loaded it, so the others crashed on an empty model (Firefox runs
  one worker); and a model that failed to load left a logger behind that
  made every later service creation throw.

### The TranslateKit ABI

`components/translatekit/` builds `libcompass_translatekit` (a
`.dylib` on Mac, `.dll` on Windows, `.so` on Linux, about 4.5 MB). It
exports what `TranslateKitClient` loads (`translate_kit_client.cc`), with
the structs of `translate_kit_structs.h`, and uses no `//base`:

| Export | Does |
|---|---|
| `GetTranslateKitVersion` | `kVersion` (`version.h`), `2026.11.1.0` now: four numbers, at least upstream's `2025.1.10.0`, at most 14 characters. The third goes up when the ABI or the package layout changes |
| `InitializeStorageBackend` | Keeps the five file callbacks. The sandbox lets the library open nothing itself: every file comes mapped through them, and is copied into Bergamot's aligned memory (the model to 256 bytes, as Marian's int8 code wants) |
| `CreateTranslateKit` / `DeleteTranslateKit` | An `Engine` (`engine.{h,cc}`): one Bergamot `AsyncService`, shared by its translators, with one worker per core but one, at most 4 (more gave nothing) |
| `TranslateKitSetLanguagePackages` | Reads `TranslateKitLanguagePackageConfig` (language1, language2, virtual path `/N`) |
| `TranslateKitCreateTranslator` | Finds the direction's folder in a package of the pair, or two through English, and loads the models (each loaded once, shared). Null without them, or when Marian refuses a model |
| `TranslatorTranslate` | Text in, text out. Text that starts with the HTML marker is HTML (below) |
| `TranslateKitSplitSentences` | Bergamot's ssplit, by its rules alone (no per-language prefix lists) |

- **The package layout** (`engine.h`): a package holds a folder per
  direction it has, `<from>-<to>` in the package's language codes
  (compared without case, `_` as `-`), with `model.bin` and either
  `vocab.spm` or `srcvocab.spm` and `trgvocab.spm`. T2's installer writes
  it.
- **The model config** is Firefox's, given as YAML with the files in
  memory, plus two placeholder names under `vocabs`: Marian reads a name
  per side even when the vocabularies come from memory, and a bounds-
  checked libc++ stops at the empty list (Firefox's WASM build has no
  checks).
- **Errors:** Marian is set to throw instead of exiting
  (`setThrowExceptionOnAbort`), and every export catches, so a bad model
  fails one call, not the service.
- **HTML without patching the service.** Page translation needs
  Bergamot's HTML mode, but the service's interfaces (the mojom, the
  client, the browser-side translator) only carry a string. Rather than
  patch each of them on every rebase, text that starts with U+FDD0 U+FDD1
  (`kHtmlMarker`, `html_marker.h`; noncharacters, meant for a program's
  own use) is translated as HTML, with the marker dropped. A web page
  sending them through the `Translator` API only gets its own text
  translated as HTML.
- **Tests:** `TranslateKitEngineTest` (languages, directions, pivoting,
  broken and missing files) and `TranslateKitAbiTest` (the real library
  loaded as the service does: every export, a version upstream's own check
  accepts, packages, file callbacks, sentences, the marker). Both translate
  for real when `COMPASS_TRANSLATEKIT_TEST_PACKAGES` names a folder with a
  German→English package (`de-en/de-en/`), and skip that part otherwise.
- **`translatekit_cli`** (test-only) translates stdin with a folder of
  packages and prints the timing, to try models and machines.

### Where the library lives

Shipped with Compass, not downloaded. It's tied to the build's version, so
it updates with Compass:

- Mac: `Compass.app/Contents/Frameworks/Compass Framework.framework/Libraries/libcompass_translatekit.dylib`,
  signed by `compass sign` with the other libraries;
- Windows: next to `chrome.dll`, in the versioned folder;
- Linux: next to the `compass` binary.

Every consumer upstream has (the installer's `IsInit()`, `Init()` and
`GetLibraryPath()`, the service launcher, the macOS sandbox's library
path) reads `--translate-kit-binary-path` first, upstream's switch for
developers. So Compass sets it on the browser's command line early at
startup (`UseBundledTranslateKit()`, `browser/translate/
translatekit_library.cc`, from `compass::RegisterFeatureOverrides()`),
unless the user gave one: the service loads Compass's library, and
Google's TranslateKit component is never registered or downloaded, with
no change to upstream's files. Packaging: the framework's `bundle_deps`
and `parts.py`'s dylibs (Mac), `chrome.release` (Windows), the `chrome`
executable's `data_deps` (Windows, Linux) and `linux_package.py`'s
`LIBRARIES`.

## Language models

### Mozilla's models, from Mozilla's servers

- Mozilla publishes the models in Remote Settings, collection
  `translations-models-v2`: 115 directions (57 from English, 58 into it,
  Norwegian Nynorsk into English only), each a model (`*.intgemm.alphas.bin`,
  about 30 MB), a vocabulary (`vocab.*.spm`, or one per side) and a lexical
  shortlist we don't use. Files are zstd-compressed, with SHA-256 hashes
  before and after decompression.
- **Compass downloads them from Mozilla's CDN**, as Firefox does:
  `https://firefox-settings-attachments.cdn.mozilla.net/<attachment.location>`.
  Compass hosts nothing. Mozilla sees the download (below, "Asking
  first").
- **`tools/translation/update_models.py`** reads the collection
  (`https://firefox.settings.services.mozilla.com/v1/buckets/main/collections/translations-models-v2/records`),
  picks the newest record per direction and file type (records with a
  `filter_expression` only when it's Android's and nothing else exists),
  downloads and checks each file, and writes **`translate/models.json`**:
  per direction, the files with their name, type, attachment location,
  sizes and both hashes, and the model version. The manifest is checked in
  and built into Compass (`build/embed_text.py`), so a build downloads
  exactly the files it was tested with, and a file whose hash doesn't
  match is refused.
- **When Mozilla removes a file.** Attachment locations stay while their
  record does, but Mozilla replaces models with newer versions. A pinned
  file that's gone (404) makes Compass read the collection itself, once a
  day at most, and take that direction's current record instead. It's
  checked against the record's hashes, over TLS from Mozilla's own server,
  and kept until a Compass update's manifest names it. The tool runs before
  each Chromium rebase, so a release's manifest is rarely more than a
  month old.
- License: MPL-2.0 (the archived `mozilla/firefox-translations-models`
  says so; the v2 records have no license field). `about:credits` lists it.
  To confirm before shipping (open question).

### Packs on the device

- **A pack is one direction**, so Italian to English is one download and
  English to Italian another. Upstream's packs are both directions of an
  English pair; `LanguagePackKey` stays as it is and Compass's installer
  maps each key to two directions, downloaded as needed.
- **Where:** `<user data>/Compass Translation/<from>-<to>/` (shared by the
  profiles, so Incognito uses the same files).
- **When:** a pack downloads the first time the user translates a page
  that needs it, once they've agreed to downloads from Mozilla (below).
  Compass never downloads models on its own. "Always translate <language>" downloads
  ahead for the next page. The bubble shows "Translating…" during the
  download, and a page that waits more than 2 s gets the progress
  ("Downloading Italian, 12 of 31 MB"). A download that fails shows
  upstream's translation error with Try Again.
- **`TranslationPacks`** (`browser/translate/translation_packs.{h,cc}`,
  global, replaces upstream's component-updater installer through a
  `chromium_src` override of `OnDeviceTranslationInstallerImpl`): downloads
  with `SimpleURLLoader` to a temporary file, decompresses with Chromium's
  zstd, checks the SHA-256 against the manifest, then moves the folder into
  place. Upstream's prefs (`on_device_translation.translate_kit_packages.*`)
  record what's installed, so the service and `chrome://on-device-translation-internals`
  see the packs.
- **Updates:** a Compass update with a newer manifest downloads the new
  files of installed packs the next time they're used, then removes the old
  ones.
- **Settings**, General › Languages (settings-window.md): "Translation
  languages", the downloaded packs, each with its size and Remove, and
  "Remove all". A pack not used for 90 days is removed by itself.

### Asking first

Nothing is downloaded from Mozilla until the user agrees, once for the
browser (not per profile or per language):

- **The question** comes the first time a translation needs a pack, before
  any request: a dialog from the translate bubble (desktop) or a sheet
  (iOS): "Download translation languages from Mozilla? Compass translates
  pages on this device with Firefox's open translation models. Each
  language is downloaded once from Mozilla's servers (about 30 MB), which
  see your IP address and which language is downloaded, but never the
  page." Buttons: **Download** and **Not Now**. A link opens the privacy
  page's section on it.
- **Download** sets `compass.translate.model_downloads_allowed` (a
  local-state pref, so every profile shares it, as the packs do; doesn't
  sync) and goes on with the translation. **Not Now** cancels the
  translation (the bubble goes back to its first state) and asks again the
  next time the user chooses Translate. A page offered for translation
  never asks by itself: only the user's Translate does.
- **Settings**, General › Languages: "Download translation languages from
  Mozilla", a switch for the same pref. Turning it off stops downloads in
  progress; packs already downloaded keep working, and Remove all deletes
  them.
- **Incognito** asks the same question and the answer applies to the
  whole browser, as said in the dialog.
- **Policy:** an admin can set it with the pref, as with any other.
- **The web `Translator` API** follows the same pref: without it, a
  language that isn't downloaded is reported "unavailable", never
  "downloadable", so a website can't start a download.
- **iOS:** the same question and switch (UserDefaults
  `compass.translate.model_downloads_allowed`), only for Bergamot packs.
  Apple's own models download through iOS's system sheet, which asks for
  itself.

### Languages

Chromium's pack languages (`language_pack.h`: English and 38 others) are
all among Bergamot's, some under other codes (`iw` is `he`, `no` is `nb`,
`zh` is `zh-Hans`); a table in `translation_packs.cc` maps them. Phase 1
offers those. Bergamot's other 20 (af, az, be, bs, ca, et, eu, fa, gl, gu,
is, lv, ml, ms, sq, sr, tl, ug, ur, and nn into English only) need new
`LanguagePackKey`s, a patch of `language_pack.{h,cc}` (T7).

The translate bubble's language list comes from Google's server
(`TranslateLanguageList`), which Compass never asks. A `chromium_src`
override gives it the pack languages instead, so the bubble offers only
what Compass can translate.

## Desktop: the page

### Turning translation back on

A patch of `translate_manager.cc` replaces the three API-key checks and the
offline check with `compass::IsPageTranslationAvailable()`: the build has
the library. Offline is fine once the pack is there, so a missing pack
while offline gives the network error then.

### Page language

Upstream detects the page's language with a TFLite model from Optimization
Guide, which needs a Google key; without it, Chromium believes only the
page's `lang` attribute and `Content-Language`, which are often wrong or
missing. Compass turns the model path off (`translate::IsTFLiteLanguageDetectionEnabled()`
returns false, `chromium_src`), so the renderer uses CLD3, built into
Chromium and run on the page's text. Nothing is downloaded.

### The script

Upstream's renderer (`TranslateAgent`) injects one script into the
translate isolated world and polls `cr.googleTranslate` (`libReady`,
`finished`, `error`, `errorCode`, `sourceLang`, `translate()`, `revert()`).
That script is upstream's `translate.js` (`IDR_TRANSLATE_JS`), which
defines `cr.googleTranslate` around Google's `element.js`.

- **No fetch.** A `chromium_src` override of `TranslateScript::Request()`
  fills the script from Compass's resources at once, instead of fetching
  `element.js`: upstream's prelude and `translate.js` as before, then
  `common/translate/page_translator.js` and `desktop_service.js` in place
  of Google's.
- **`desktop_service.js`** provides what `translate.js` wants from Google's
  library: `google.translate.TranslateService(...)`, whose `isAvailable()`,
  `translatePage(src, tgt, progress)` and `restore()` call the shared
  page translator. So upstream's agent, its polling and error codes, and
  the bubble's states work unchanged.

### The engine from the page

The script runs in the translate isolated world. The web `Translator` API
is a poor fit from there: it runs as the page's origin, under the page's
permissions policy, and wants a user gesture to download. So Compass adds
its own channel:

- **`compass.mojom.PageTranslator`** (`browser/translate/page_translator.mojom`):
  `Translate(string source, string target, array<Segment> segments) =>
  array<string?>`, a segment being `{text, is_html}`. Bound per frame
  (`RegisterBrowserInterfaceBindersForFrame`), for http(s) and file pages.
- **Renderer:** `PageTranslatorBindings`
  (`renderer/translate/page_translator_bindings.{h,cc}`, a
  `RenderFrameObserver`) installs `compassTranslate.translate(source,
  target, segments)` (a gin object returning a promise) in the translate
  isolated world only (`DidCreateScriptContext` with
  `ISOLATED_WORLD_ID_TRANSLATE`), never in the page's world.
- **Browser:** `PageTranslatorHost` gets the profile's
  `OnDeviceTranslationServiceController` (upstream's, one service process
  per origin), asks `TranslationPacks` for the packs, creates or reuses
  the pair's `Translator`, and sends the segments with `Translate()`, HTML
  ones behind `kHtmlMarker` ("The TranslateKit ABI"). It caps a request at
  64 segments and 32 KB, and a frame at 4 requests in flight.
- **Incognito** uses the same packs and its own service process.

### What the user sees

Upstream's, with the anchors command-bar.md describes: the translate
bubble from the pill when Compass detects a language the user doesn't read
(the Languages settings' "Offer to translate" switch and each language's
own switch), Translate, the target language menu, "Always translate",
"Never translate <language>", "Never translate this site", and "Show
original". The command bar gets "Translate Page" and "Show Original"
(`CommandBarCommand::kTranslatePage`), and the page's context menu keeps
upstream's "Translate to <language>".

## The shared page translator: `common/translate/`

`page_translator.js` defines `__compassTranslator` in the world it runs in:
`start(source, target, engine)`, `stop()`, `restore()`, `progress()`.
`engine` is a function from segments to translations: `compassTranslate`
on the desktop, a script message on iOS. It's written for Compass; it
follows the approach of Firefox's `translations-document.sys.mjs`
(MPL-2.0) without copying its code.

- **Segments.** Block-level elements (`p`, `li`, `h1`–`h6`, `td`, …) are
  the units. A block whose content is only text goes as text. One with
  inline markup (links, emphasis, `<br>`) goes as HTML: its child elements
  are given `data-compass-t="n"` first, and the translation, parsed in an
  inert `<template>`, is matched back onto the live nodes by those numbers.
  The page's own elements stay in place, with their listeners and state;
  only text nodes change.
- **Skipped:** `translate="no"`, `.notranslate`, `contenteditable`,
  `code`, `pre`, `kbd`, `samp`, `script`, `style`, `textarea`, `svg`,
  `math`, form fields' values, and text that's only numbers, symbols or
  URLs.
- **Attributes translated:** `title`, `alt`, `placeholder`, `aria-label`,
  `aria-description`, and the `value` of button inputs.
- **Visible first.** IntersectionObservers queue the blocks in view first,
  then the rest of the page in order. Scrolling moves new blocks ahead.
- **Pages that change.** A MutationObserver translates added content and
  changed text, and ignores the translator's own writes.
- **Restore** without a reload: the original text of each changed text
  node and attribute is kept (in a `WeakMap`), and `restore()` puts it back.
  A node the page changed since keeps the page's version.
- **Cache:** per pair and per page, so switching between original and
  translation, or translating again, costs nothing.
- **Frames:** each frame runs its own copy; upstream's agent already
  starts translation in every frame.
- **Tests:** `common/translate/test/page_translator_test.html`, run with
  `--headless=new --dump-dom`, as `page_agent_test.html` is
  (`RESULT PASS`), with a fake engine that upper-cases text: markup
  survives, skipped elements stay, attributes are translated, restore
  brings back the original, added content is translated.

## iOS

### Engines

- **Apple's Translation framework first** (iOS 18 and later): on-device,
  with models that iOS manages and shares with Safari. A
  `TranslationSession` comes from SwiftUI's `.translationTask` on a hidden
  view in the browser window (iOS 18–25) or
  `TranslationSession(installedSource:target:)` (iOS 26). Availability is
  `LanguageAvailability.status(from:to:)`; batches go through
  `translations(from:)`. A language iOS hasn't downloaded yet asks the
  user through the system's own sheet (`prepareTranslation()`): iOS has no
  silent download.
- **Bergamot** when Apple has no model for the pair (Apple covers about 20
  languages, Bergamot 58), and on iOS 17, which has no Translation
  framework. It's the desktop's library, built as a static XCFramework
  (`ios/Frameworks/BergamotKit`) with a small Swift wrapper in `CompassKit`
  (`CompassTranslate`). The packs come from Mozilla with the same manifest,
  into Application Support/Compass/Translation/; there is no sandbox to
  work around.
- **Settings → General → Translation:** "Translate with" Apple (when
  available), or Compass's own models only; the downloaded Bergamot packs,
  with Remove. Apple's models are managed in iOS Settings.

### The page

- `page_translator.js` runs in Compass's own content world
  (`WKContentWorld` "CompassTranslate"), as Reader's scripts do, with an
  engine that posts segments to the app (`WKScriptMessageHandlerWithReply`)
  and awaits the reply. HTML segments go to Bergamot's HTML mode. Apple's
  framework translates plain text only, so with Apple the HTML segments go
  as their text nodes, one string each, and markup can't move with the
  words.
- **Language:** the page's `lang`, else `NLLanguageRecognizer` on its text
  (from the page translator's `sample()`).
- **UI:** a Translate tile in the page menu; a translate button in the
  address pill when the page's language isn't one of the user's preferred
  languages; ⌘⇧Y with a hardware keyboard (the Mac's keys). Translated, the
  button is filled, and its menu has the target language, Show Original,
  and "Always Translate <language>".

## Android

Upstream builds the on-device service only on desktop
(`enable_on_device_translation`). Android needs that flag turned on
(patch), the library built for arm64 and x86-64 into the APK, the service
process on Android, and upstream's Android translate UI (the message bar
and its menu), which already talks to `TranslateManager`. Later, after
desktop and iOS (T6).

## Privacy

- privacy.md gets a row: model downloads from Mozilla's CDN
  (`firefox-settings-attachments.cdn.mozilla.net`, and
  `firefox.settings.services.mozilla.com` when a pinned file is gone),
  which see the IP address and which language, nothing about the page.
  Only after the user agreed ("Asking first") and chose to translate.
  Mozilla's privacy notice covers those servers, not Compass's.
- No text, URL or language ever goes to a server. Detection and
  translation run on the device.
- In Incognito nothing is kept beyond the session except the downloaded
  packs, which hold nothing about the user.

## Strings

Every new string in English and Italian on each platform (desktop `grd` and
`compass_strings_it.xtb`, iOS `xcstrings`, Android `res_strings`), using the
shared glossary. Upstream's bubble strings already have Italian.

## Milestones

| | Work | Done when |
|---|---|---|
| T1 | Bergamot vendored and built with GN; `libcompass_translatekit` with the ABI and the HTML marker, shipped and signed; the service pointed at it | The library translates text and HTML through its ABI on Mac arm64, Windows x64 and Linux x64; the service loads it in a browser (the web `Translator` API with a pack from the command line) |
| T2 | `update_models.py` and `models.json`; `TranslationPacks` (download, check, install, remove, update, a removed file); the question and its switch | Unit tests with a fake server; nothing is requested before the user agrees; packs install from Mozilla's CDN |
| T3 | `common/translate/page_translator.js` and its test page | `RESULT PASS` in Chromium headless |
| T4 | Desktop: translation turned on, CLD3, the script without fetch, `PageTranslator` mojo and bindings, the language list, the command bar entries | Browser tests: the bubble translates a German test page into English with a fake library, Show Original restores it, Always Translate works, nothing goes to Google |
| T5 | Settings: translation languages; privacy.md; COMPARE.md | Settings browser test |
| T6 | iOS: Apple Translation, the UI, then Bergamot as fallback | `CompassTranslateTests`; UI test on a fixture page |
| T7 | The other 20 Bergamot languages (`LanguagePackKey`s) | Bubble lists them; one translates |
| T8 | Android | Translates on a device |

Tests use upstream's `mock_translate_kit_lib` where the real engine isn't
the point; a small set uses one real pack (Spanish↔English), fetched into
the test data by `tools/translation/fetch_test_pack.py` rather than checked in.

## Open questions

- **x86 float GEMM:** Marian's ONNX SGEMM (nothing to ship, untested
  natively, may be slow) or OpenBLAS (about 30 MB more per build). Measure
  in T1.
- **Windows on Arm:** ruy under MSVC/clang-cl is untried upstream.
- **The models' license** for the current v2 models: confirm MPL-2.0 with
  Mozilla.
- **Mozilla's terms:** Firefox's CDN isn't offered as a public service.
  Downloads are one-off, small in number and the same files Firefox
  fetches, but ask Mozilla before a stable release; if they object, the
  manifest's locations can point at a mirror without other changes.
- **Upstream's CPU check:** Chrome refuses on-device translation on some
  low-end machines; check what applies to our library.
