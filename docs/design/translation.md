# Page translation

Status: T1 to T5 done (2026-11-28): the engine on macOS arm64, Windows
x64 and Linux x64; language packs from the mirror; the shared page
translator; page translation on the desktop (verified on macOS); its
settings and privacy notes. T6 (2026-11-28): iOS with Apple's
Translation framework first and Bergamot after it, the page, the UI and
Settings; Apple's engine still to try on a device. T7 (2026-11-28):
Bergamot's other 19 languages on the desktop. Android (T8) next.
Based on Chromium 159.0.8154.34.

## Goal

Translate the page in place, on the device, as Safari, Orion and Firefox
do: a page in a language the user doesn't read offers a translation; one
click translates it, keeping its layout, links and forms; "Show original"
puts it back at once.

- **On the device.** The page's text never leaves the computer or phone.
  The only network traffic is downloading a language's model, once, from
  Compass's mirror of Firefox's models, and only after the user has agreed
  to that.
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
- **Windows x64** (win1, an i9-7980XE, so intgemm's AVX-512 path): the
  same translation word for word; 1.76 s on 1 worker and 0.79 s on 4 in the
  official Release build (0.68 s on 8 in `fast`: nothing past 4); the model
  loads in 160 ms. All `TranslateKit*` tests pass, real models included.
- **Linux x64** (the same machine, in WSL): 1.84 s on 1 worker, 1.04 s on
  4; all `TranslateKit*` tests pass. The model takes 1.5 s to load there,
  read across WSL's `/mnt/c`.
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

### Firefox's models, from Compass's mirror

- Mozilla publishes the models in Remote Settings, collection
  `translations-models-v2`: 114 directions (57 from English, 57 into it,
  on 2026-11-28), each a model (`*.intgemm.alphas.bin`, about 30 MB), a
  vocabulary (`vocab.*.spm`, or one per side) and a lexical shortlist we
  don't use. Files are zstd-compressed, with SHA-256 hashes before and
  after decompression.
- **Not from Mozilla's CDN.** It answers 406 to any User-Agent containing
  "Chrome" (Firefox's, Safari's, curl's and a bare `Mozilla/5.0` get the
  file), which reads as Mozilla turning Chromium browsers away on purpose;
  Compass doesn't work around it.
- **Compass's mirror:** the `translation/` folder of the `compass-assets`
  R2 bucket, served as `https://assets.compassbrowser.org/translation/`
  (the bucket and domain are for other features' files too). Each file
  keeps its path on Mozilla's CDN
  (`main-workspace/translations-models-v2/<uuid>.zst`), is uploaded once
  and never replaced or removed, so every Compass ever built finds its
  files. About 2.5 GB in all (232 files, uploaded and checked 2026-11-28),
  within R2's free tier, with free egress. In the Debug app, a page's
  `Translator.create()` for German installed `de_en` from it and translated.
- **`tools/translation/update_models.py`** reads the collection, picks one
  set per direction (the newest version, then the largest architecture:
  base, base-memory, tiny; records with no `filter_expression`, or one
  that only leaves Android out; a model and one vocabulary or one per
  side), and writes **`browser/translate/models.json`**: the mirror's URL
  and, per direction, the files with their type, path, sizes and both
  hashes. `--upload` copies to the mirror every file it lacks, after
  checking both hashes (`--verify` only checks). The manifest is checked in
  and built into Compass (`build/embed_text.py`), so a build downloads
  exactly the files it was made with, and a file that doesn't match is
  refused. Run it before each Chromium rebase.
- License: MPL-2.0 (the archived `mozilla/firefox-translations-models`
  says so; the v2 records have no license field). `about:credits` lists it.
  To confirm before shipping (open question).

### Packs on the device

- **A pack is upstream's: both directions of a pair with English**
  (`LanguagePackKey`, German is `de_en`), so installing one downloads two
  models (about 45 MB). Upstream asks for a pack, not a direction.
- **Where:** `<user data>/Compass Translation/<pack>/<from>-<to>/`
  (`de_en/de-en/model.bin`, `vocab.spm`), in upstream's language codes;
  the manifest uses Mozilla's (`iw` is `he`, `no` is `nb`, `zh` is
  `zh-Hans`; `ToModelLanguage()`). Shared by the profiles, so Incognito
  uses the same files.
- **When:** a pack downloads the first time something needs it (the user
  translating a page, or a website's `Translator.create()` with a user
  gesture), once the user has agreed to downloads (below). Compass never
  downloads models on its own. T4 adds the bubble's progress and errors.
- **`PackInstaller`** (`browser/translate/translation_pack_installer.
  {h,cc}`, global): downloads each file with `SimpleURLLoader` to a
  temporary file (models are past the 5 MB a string download takes),
  checks size and SHA-256, decompresses with Chromium's zstd, checks the
  result's size and SHA-256, and writes it into a staging folder; once all
  are in, the staging folder replaces the pack's. A file that's missing or
  doesn't match fails the install and leaves nothing behind.
- **Upstream unchanged around it.** Upstream's installer would register a
  Google component per pack; a `chromium_src` override of
  `installer_impl.cc` renames its two calls
  (`RegisterTranslateKitLanguagePackComponent`,
  `UninstallTranslateKitLanguagePackComponent`) to Compass's stand-ins
  (`language_pack_installer_shim.{h,cc}`), which call `PackInstaller`. It
  then records the pack in upstream's own prefs
  (`on_device_translation.translate_kit_packages.*`: the path and the
  registered flag), so upstream's installer, service and
  `chrome://on-device-translation-internals` see it as installed. The
  startup registration of Google's TranslateKit components is gone
  (`registration.cc` patch): it would otherwise ask Google to update every
  pack marked registered.
- **Updates:** a Compass update with a newer manifest downloads the new
  files of installed packs the next time they're used, then removes the
  old ones (not yet).
- **Settings**, General › Languages › Translation (settings-window.md):
  the switch below ("Asking first"), the downloaded packs, each with its
  size and Remove, and Remove All, which go through upstream's
  `ComponentManager` (and so `PackInstaller::Uninstall()`), so the service
  forgets them. `SettingsWindowBrowserTest.TranslationLanguages`.
- **Tests:** `TranslationManifestTest` (the built-in manifest has every
  pack upstream knows, both ways; parsing refuses http, bad hashes and
  paths that leave the folder) and `PackInstallerTest` (both directions
  installed and recorded, nothing requested without consent, a file that
  doesn't match or is missing, a 6 MB model, uninstall), with a test URL
  loader factory.

### Asking first

No model is downloaded until the user agrees, once for the browser (not
per profile or per language):

- **The question** comes the first time a translation needs a pack, before
  any request: a dialog from the translate bubble (desktop) or a sheet
  (iOS): "Download translation languages? Compass translates pages on
  this device with Firefox's open translation models. Each language is
  downloaded once (about 45 MB) from Compass's server, which sees your IP
  address and which language is downloaded, but never the page." Buttons:
  **Download** and **Not Now**. A link opens the privacy page's section on
  it.
- **Download** sets `compass.translate.model_downloads_allowed` (a
  local-state pref, so every profile shares it, as the packs do; doesn't
  sync) and goes on with the translation. **Not Now** cancels the
  translation (the bubble goes back to its first state) and asks again the
  next time the user chooses Translate. A page offered for translation
  never asks by itself: only the user's Translate does.
- **Settings**, General › Languages › Translation: "Download translation
  languages", a switch for the same pref. Turning it off stops new
  downloads (one already running finishes); packs already downloaded keep
  working, and Remove All deletes them.
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
`zh` is `zh-Hans`); a table in `translation_packs.cc` maps them. Bergamot's
other 19 (af, az, be, bs, ca, et, eu, fa, gl, gu, is, lv, ml, ms, sq, sr,
tl, ug, ur) are new `SupportedLanguage`s and `LanguagePackKey`s (patches of
`supported_languages.{h,cc}` and `language_pack.h`, T7), after upstream's,
so upstream's values don't move; their component keys are placeholders,
since Compass installs every pack from its mirror. Chrome's list calls
Filipino `fil`, mapped to the packs' `tl`. Nynorsk (`nn`, which Firefox
has into English only and the pinned manifest doesn't) goes to the Bokmål
pack, as before. iOS offers every language in the manifest. Checked: the
manifest test finds both directions for every key, the bubble's list has
Afrikaans, Catalan, Filipino and Urdu, and Catalan translates
(`translatekit_cli`, "Bon dia…" → "Good morning…").

The translate bubble's language list comes from Google's server
(`TranslateLanguageList`), which Compass never asks. A `chromium_src`
override gives it the pack languages instead, so the bubble offers only
what Compass can translate.

## Desktop: the page

### Turning translation back on

Upstream turns page translation off without Google's API keys: three
checks in `translate_manager.cc` (offering it, the menu, `IsAvailable()`).
All three honor upstream's own switch for tests,
`TranslateManager::SetIgnoreMissingKeyForTesting()`, so Compass sets it at
startup (`compass::translate::EnablePageTranslation()`, from
`RegisterFeatureOverrides()`, desktop only): no patch. Upstream's two
offline checks stay for now, so a device offline isn't offered
translation even with the pack installed (not yet).

### Page language

Upstream detects the page's language with a TFLite model from Optimization
Guide, which needs a Google key; without it, Chromium believed only the
page's `lang` attribute and `Content-Language`, which are often wrong or
missing. A `chromium_src` override of `translate_util.cc` makes
`IsTFLiteLanguageDetectionEnabled()` false, so the renderer uses CLD3,
built into Chromium and run on the page's text. Nothing is downloaded.

### The script

Upstream's renderer (`TranslateAgent`) injects one script into the
translate isolated world and polls `cr.googleTranslate` (`libReady`,
`finished`, `error`, `errorCode`, `sourceLang`, `translate()`, `revert()`).
That script is upstream's prelude and `translate.js` (`IDR_TRANSLATE_JS`),
which defines `cr.googleTranslate`, followed by Google's `element.js`,
fetched.

- **No fetch.** `TranslateScript` asks a `TranslateUrlFetcher` for
  `element.js`; a `chromium_src` override of `translate_script.cc` swaps it
  for one that hands back Compass's script at once (posted, as the script
  deletes its fetcher in the callback): `common/translate/
  page_translator.js` and `desktop_service.js`, embedded
  (`PageTranslateScript()`, `browser/translate/page_translation.cc`).
  Upstream's prelude and `translate.js` stay; nothing goes to the network.
- **`desktop_service.js`** provides what `translate.js` wants from Google's
  library: `google.translate.TranslateService()`, whose `isAvailable()`,
  `translatePage(source, target, progress)`, `restore()` and
  `getDetectedLanguage()` drive the shared page translator, with
  `compassTranslate.send()` as its engine (wrapped in a promise), then
  calls `cr.googleTranslate.onTranslateElementLoad()` as Google's script
  does. It reports finished once the translator is idle, and an
  unsupported language or a failed engine with `translate.js`'s codes. So
  upstream's agent, its polling, its errors and the bubble's states work
  unchanged.

### The engine from the page

The script runs in the translate isolated world. The web `Translator` API
is a poor fit from there: it runs as the page's origin, under the page's
permissions policy, and wants a user gesture to download. So Compass adds
its own channel:

- **`compass.mojom.PageTranslator`** (`common/translate/
  page_translator.mojom`): `Translate(source, target, array<
  TranslationSegment>) => (PageTranslationResult, array<string?>)`, a
  segment being `{text, html}`, the result `kOk`,
  `kUnsupportedLanguage`, `kDeclined` or `kFailed`. Registered with
  upstream's document binders by a `chromium_src` override of
  `chrome_browser_interface_binders.cc` (wrapping
  `PopulateChromeFrameBinders()`); http(s) and file pages only.
- **Renderer:** `PageTranslatorBindings` (`renderer/translate/
  page_translator_bindings.{h,cc}`, a `RenderFrameObserver` created by
  `ChromeContentRendererClient`, patch) installs
  `compassTranslate.send(source, target, segments, callback)` in the
  translate isolated world only (`DidCreateScriptContext` with
  `ISOLATED_WORLD_ID_TRANSLATE`), never in the page's world: at most 64
  segments and 64 KB a call. The answer calls `callback(result,
  translations)` through `WebLocalFrame::CallFunctionEvenIfScriptDisabled()`.
- **Browser:** `PageTranslatorHost` (`browser/translate/
  page_translator_host.{h,cc}`, a `DocumentService`) maps the languages to
  upstream's codes (`ToOnDeviceLanguage()`: `he` is `iw`, `nb` is `no`,
  `zh-TW` is `zh-Hant`), works out the packs the pair needs (one per
  language that isn't English), and for a missing one: asks the user
  first if they haven't agreed yet ("Asking first"), then installs it
  through upstream's `ComponentManager` (and so Compass's installer), so
  upstream's service hears of it, waiting for `PackInstaller`'s result.
  Then it creates the pair's translator from upstream's
  `ServiceControllerManager`, under one origin for all page translation
  (`chrome://compass-translate`), so a language's models load once for
  every page, and translates each segment, HTML behind `kHtmlMarker`. A
  service that goes away mid-batch answers null rather than leaving the
  page waiting.
- **Incognito** uses the same packs and its own service process.

### Asking, on the desktop

The question is a tab-modal dialog on the page being translated
(`browser/ui/translate/model_download_dialog.cc`, `TabModalConfirmDialog`):
"Download translation languages?", with the text in "Asking first",
**Download** and **Not Now**. Download sets the pref and goes on; Not Now
(or Esc, or closing it) makes the translation fail with upstream's
translation error, and asks again next time.

### Languages

Upstream's bubble lists the languages `TranslateLanguageList` has, a
built-in list that it refreshes from Google's server. A `chromium_src`
override of `translate_language_list.cc` filters the built-in list to the
languages Compass has models for (`CanTranslateLanguage()`) and gives it a
fetcher that never fetches.

### What the user sees

Not upstream's translate bubble: a translate button in the address pill,
before Reader's (`SidebarHeader`, from the tab's `TranslationTabState`).
`ChromeTranslateClient::ShowTranslateUI()` (patch) hands each translate
step to `TranslationTabState::OnTranslateStep()`, which keeps it for the
tab and tells the window's header; upstream's bubble shows only where the
window has no pill (the sidebar laid out collapsed, or horizontal tabs).

- On a page in a language the user doesn't read (the Languages settings'
  "Offer to translate" switch and each language's own switch), the button
  shows; pressing it translates into the user's language.
- While the language downloads the first time, the button is accent-colored
  with the download's progress beside it ("12/45 MB", from
  `PackInstaller::OnProgress()`, every file of the pack counted), so a slow
  connection shows it moving; then accent-colored while it translates.
- The page counts as translated once the first batch (the first screen)
  is back (`started()` in `page_translator.js`, not `idle()`): the rest
  goes on as it comes, and the menu's Show Original works from then on.
- Translated, or still translating, pressing it (or right-clicking it)
  opens a menu
  (`TranslatePillMenu`): "Translated from <language>", Translate To (the
  on-device languages), Show Original, Always Translate <language>, Never
  Translate <language>, Never Translate This Site. A failed translation
  shows the button in the error color, and its menu has Try Again.

The command bar has "Translate Page" (translating into the user's
language, as the context menu's "Translate to …" does) and "Show Original"
(`CommandBarCommand::kTranslatePage`, `kShowOriginal`), each when it
applies. Translating into another language restores the original first
(`start()` calls `restore()`).

### Tests

`PageTranslationBrowserTest`: a German page is detected as German and
offered, without Google's keys; the language list has German, Italian and
English, not Javanese or Latin; translating it into English (with a real
German pack, `COMPASS_TRANSLATEKIT_TEST_PACKAGES`) gives English, with the
link the same node and its address, and Show Original gives the German
back; asking for Italian, whose pack isn't there, shows the dialog, and
closing it leaves the page, the consent and the disk as they were; the
pill's button shows on the German page (no bubble), with a download's
progress, and goes on a page that needs nothing.

## The shared page translator: `common/translate/`

`page_translator.js` defines `__compassTranslator` in the world it runs in:
`start({source, target, engine})`, `stop()`, `restore()`, `progress()`
(`{done, pending, failed}`), `idle()` (a promise for "nothing left") and
`started()` (for "the first batch is back").
`engine` is an async function from segments (`{text, html}`) to their
translations, `null` for one it couldn't do: `compassTranslate` on the
desktop, a script message on iOS. It's written for Compass; it follows the
approach of Firefox's `translations-document.sys.mjs` (MPL-2.0) without its
code.

- **Units.** An element whose content is text and inline elements (`a`,
  `b`, `em`, `span`, `br`, `img`, `code`, …) is one unit; inside a block
  that also holds blocks, each loose run of text is its own. A unit with
  no elements inside goes as plain text. One with elements goes as HTML: a
  copy whose elements carry `data-compass-t="n"` and no other attribute,
  with comments dropped and `code`, `kbd`, `samp`, `var` and
  `translate="no"` elements emptied, so their words stay as they are.
- **Writing back.** The translated HTML is parsed in an inert
  `<template>`; the page's own elements are found by their numbers and put
  where the translation moved them, and its text goes into the page's own
  text nodes (new ones only when the translation has more pieces). Links,
  buttons and their listeners are the same nodes; nothing else about the
  page's elements changes. An element the engine dropped stays, at the end
  of its parent.
- **Skipped:** `translate="no"`, `.notranslate`, `contenteditable`,
  `script`, `style`, `noscript`, `template`, `code`, `pre`, `kbd`, `samp`,
  `var`, `textarea`, `input`, `svg`, `math`, media and frames, form
  fields' values, and text without letters, or that's only a web or email
  address.
- **Also translated:** the title; `title`, `alt`, `placeholder`,
  `aria-label` and `aria-description`; button inputs' `value`; options'
  text, where an option without a `value` gets its original text as one
  first, so the form still submits it.
- **Order.** Units go in batches (16, or about 6,000 characters), two at a
  time. An IntersectionObserver puts what's in view (and half a screen
  around it) first; the rest follow in the page's order.
- **Pages that change.** A MutationObserver queues added content, text the
  page writes over a translation (sent on its own), and translatable
  attributes it sets. The translator's own writes are dropped from the
  observer's records as it makes them.
- **Restore** without a reload: the original text of every text node it
  wrote, every attribute, and the child list of every element it
  rearranged are kept, and `restore()` puts them back. A text node the page
  changed since keeps the page's version.
- **Cache:** per pair, for the page's life, so translating again, or after
  restoring, costs nothing.
- **Frames:** each frame runs its own copy; upstream's agent already
  starts translation in every frame.
- **Tests:** `common/translate/test/page_translator_test.html`, run with
  `--headless=new --virtual-time-budget=10000 --dump-dom`, as
  `page_agent_test.html` is (`RESULT PASS`), with a fake engine that
  upper-cases text and, given "swap", swaps two elements: plain, inline
  and mixed text; a link that's the same node, with its href, class and
  listener; reordered elements; code kept in a sentence; every skipped
  kind; attributes, button values, options and their values; text far
  below the fold; added and changed content; restore (structure, text,
  attributes, a bare option's value, the page's own change kept); the
  cache; and a failing engine.

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
  (`CompassTranslate`). The packs come from the mirror with the same manifest,
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

### Where it is (T6)

- `CompassKit/Sources/CompassTranslate` (tested on the Mac,
  `CompassTranslateTests`): `PageLanguage` (codes, detection from `lang`
  and `NLLanguageRecognizer`, the text winning when it's sure, which pages
  to offer, the target), `HTMLSegment`/`PlainTextBatch` (an HTML segment's
  text runs one string each for Apple, the markup kept, entities
  unescaped and escaped back), `TranslationPrefs` (UserDefaults
  `TranslateAlways`, `TranslateNever`, `TranslateNeverSites`,
  `TranslateOffer`).
- `App/Browser/Translation.swift`: `PageTranslation` (per tab: offered,
  translating, translated, failed; checked when a page loads, Incognito
  too), `TranslateScripts` (`page_translator.js` in the `CompassTranslate`
  world; `sample()` for detection, `start()` waiting for `started()`),
  the engine's batches through `WKScriptMessageHandlerWithReply`
  (`compassTranslate`), `AppleTranslator` and `TranslationHost` (a hidden
  view per window running `.translationTask`; the session stays in the
  host's task and serves the pair's queued batches until 30 s without
  one; a new pair sets a new configuration).
- UI: `TranslateButton` at the address pill's start (tap: translate, or
  Show Original; its menu: Translate To, Show Original or Try Again,
  Always Translate, Never Translate the language or the site); the
  address's long-press menu; a Translate tile in the page menu; ⌘⇧Y.
- **The simulator has no Apple Translation** ("Translation isn't supported
  on the current device", `TranslationErrorDomain` 11): the Apple engine
  needs a device. Debug flags: `-CompassTranslate YES` translates every
  offered page, `-CompassTranslateFake YES` upper-cases instead of
  translating, which checks the page script, the bridge and the button in
  the simulator (done 2026-11-28 on German Wikipedia: text upper-cased in
  place, links kept, the button filled).
- **Bergamot on iOS (T6b).** `tools/translation/build_ios.py` builds
  `ios/Frameworks/BergamotKit.xcframework` (not checked in; `compass ios
  build`, `sim` and `archive` run it when it's missing): it asks the Mac
  arm64 build's GN for the engine's targets and zstd's (about 270, 481
  files: Bergamot, Marian, ssplit, PCRE2, SentencePiece, protobuf-lite,
  ruy, cpuinfo, Abseil, zstd), their sources, defines and include folders,
  and compiles them with Xcode's clang and Apple's libc++ (Chromium's
  libc++ flags and `_LIBCPP_`/`CR_` defines dropped) for `arm64-apple-ios17`
  and its simulator, 36 MB each, static. No Chromium iOS checkout needed;
  the generated files come from the Mac build's gen/. Simulators are arm64
  only (`EXCLUDED_ARCHS` x86_64). Its C API is
  `components/translatekit/ios/bergamot_kit.h` (engine, packages,
  translator, translate, and `bk_zstd_decompress_file()`, since Apple's
  Compression framework has no zstd).
- `BergamotPacks` (CompassTranslate, tested with a fake server): the
  desktop's `models.json` (bundled), a pack per language other than
  English with both directions, each file's size and SHA-256 checked
  compressed and decompressed, staged and moved into place, progress across
  the packs. `CompassTranslator` (App): the packs in Application
  Support/Compass/Translation/ (excluded from backups), downloads with
  `URLSession` and a progress delegate, a `BKEngine` with two workers,
  translators per pair, `bk_translate()` off the main thread with HTML
  segments in Bergamot's HTML mode.
- **Which engine** (`PageTranslation.engine()`): Apple's when "Translate
  With" is Apple and `LanguageAvailability` doesn't say unsupported (on a
  device, iOS 18+), else Compass's when the manifest has the pair, else the
  page isn't offered. The first Compass download asks (an alert with the
  size, `TranslateModelDownloadsAllowed`), and the button shows a
  determinate ring while it downloads, its label "Downloading German for
  translation: 12 of 45 MB".
- **Settings › Translation:** Offer to Translate Pages; Translate With
  (Apple, then Compass / Compass Only; iOS 18+); Download Languages When
  Needed; Compass's languages with Remove; the Never Translate languages
  and sites, swipe to delete.
- **Checked** in the simulator (2026-11-28): German Wikipedia, Apple
  unsupported there, so Bergamot: the German pack downloaded from the
  mirror and the page translated into English with its links, about 14 s
  from launch. Apple's engine needs a device.

## Android

Upstream builds the on-device service only on desktop
(`enable_on_device_translation`). Android needs that flag turned on
(patch), the library built for arm64 and x86-64 into the APK, the service
process on Android, and upstream's Android translate UI (the message bar
and its menu), which already talks to `TranslateManager`. Later, after
desktop and iOS (T6).

## Privacy

- privacy.md has a row (Kept on purpose): model downloads from
  `assets.compassbrowser.org` (Cloudflare R2), which sees the IP address
  and which language, nothing about the page. Only after the user agreed
  ("Asking first") and something asked for the language. Nothing goes to
  Mozilla or Google.
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
| T2 | `update_models.py` and `models.json`; the mirror; `PackInstaller` (download, check, install, remove) behind upstream's installer; consent pref; no Google components | Unit tests with a fake server; nothing is requested before the user agrees; packs install from the mirror in the app. The question's UI is T4's, its switch T5's |
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
- **Upstream's CPU check:** Chrome refuses on-device translation on some
  low-end machines; check what applies to our library.
