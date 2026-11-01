# Privacy: Google services, first run, startup

Compass is a non-branded Chromium build without Google API keys
(`use_official_google_api_keys = false`, `config/args/common.gn`). That
already turns most of Google's services off. This page lists what Compass
turns off on top, what it keeps on purpose, and where each switch lives.

## No first run, no profile picker

- **First run:** `ShouldOpenFirstRun()` is false
  (`patches/chrome-browser-ui-startup-first_run_service.cc.patch`), so the
  first-run profile picker never opens. `first_run::IsChromeFirstRun()` still
  works, so the sentinel, `initial_preferences` and auto-import behave as
  upstream. The macOS first-run dialog is Chrome-branded only.
- **Profile picker at startup:** off by default (Local State
  `profile.picker_availability_on_startup` = 1, disabled). The
  picker's "Ask on startup" checkbox goes with it. An enterprise policy can
  still turn it on. With spaces of several profiles, startup shows the
  switcher's first space (docs/design/spaces.md, "Startup with several
  profiles"). The profile menu's "Manage profiles" still opens the picker.

## Turned off by Compass

**Field trial testing config.** A non-branded Chromium build applies
`testing/variations/fieldtrial_testing_config.json` at every start: Chromium's
experiments, Gemini among them, override the browser's defaults and
Compass's. `disable_fieldtrial_testing_config = true` (`config/args/common.gn`)
leaves features at their defaults plus Compass's overrides.

| What | Endpoint | Switch |
|---|---|---|
| Network time | `clients2.google.com/time` (plain HTTP) | feature `NetworkTimeServiceQuerying` off (`browser/feature_overrides.cc`) |
| AI Mode eligibility | `www.google.com/async/folae` | feature `AimServerEligibilityEnabled` off |
| Autofill field type predictions | `content-autofill.googleapis.com` | feature `AutofillServerCommunication` off; local heuristics stay |
| Gemini in the browser (Glic) | `gemini.google.com` | feature `Glic` off |
| Google account list at launch | `accounts.google.com/ListAccounts` | feature `AvoidAutoTriggerListAccountsOnStale` on: asked only when a Google cookie changes |
| Password leak check | `passwordsleakcheck-pa.googleapis.com` | profile pref default false (`browser/prefs/pref_defaults.cc`) |
| Error page probes | Google Public DNS, `gstatic.com/generate_204` | profile pref `alternate_error_pages.enabled` default false. Captive portal ("sign in to this Wi-Fi") hints go with it |
| URL-keyed data collection | page URLs to the search engine | never turned on automatically (`patches/components-unified_consent-unified_consent_service.cc.patch`) |
| Browser sign-in (Dice) | `accounts.google.com` | `CanEnableDiceForBuild()` is false (`patches/chrome-browser-signin-account_consistency_mode_manager.cc.patch`), even with an OAuth client from the environment: `signin.allowed` is false at every start and there's no account consistency |
| "Google services settings" in the profile menu | — | removed (`patches/chrome-browser-ui-views-profiles-profile_menu_view.cc.patch`) |

The pref defaults only apply while the user hasn't changed the setting, and
Settings still has them.

## Already off in this build

Without Chrome branding or API keys these never run: UMA and UKM metrics,
crash report upload (dumps stay local), Domain Reliability, the variations
seed, feedback upload, HaTS surveys, the server side of Translate, the
spelling service, Optimization Guide hints and models, Privacy Sandbox APIs.
**Browser sign-in** to a Google account is off too: Dice needs Google's OAuth
client, so `signin.allowed` is forced false at every start. Signing in to
Google websites in a tab works as usual. Compass sync doesn't use a Google
account (docs/design/sync.md).

## Kept on purpose

| What | Why |
|---|---|
| Chrome Web Store and extension updates (`clients2.google.com/service/update2/crx`) | Extensions, and their security fixes |
| Web push (GCM/FCM, `mtalk.google.com`) | Sites' notifications while closed. Starts only once a site or extension registers |
| Safe Browsing (`safebrowsing.googleapis.com`) | Phishing and malware protection. Without an API key it sends `key=dummytoken`, so Google likely rejects the list updates: it needs a key or a proxy to work (open item) |
| Component updater (`update.googleapis.com`, `edgedl.me.gvt1.com`) | CRLSet revocations, Certificate Transparency logs, file type policies, origin trials |
| Google as the default search engine, with suggestions | The user's choice; any engine can be set in Settings |

## Checking it

Launch a fresh profile with `--log-net-log=<file>` and list the hosts. Only
the kept services' hosts should show up, plus the search engine's suggest
requests while typing in the address bar.
