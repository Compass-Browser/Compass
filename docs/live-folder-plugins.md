# Writing a live folder plugin

A live folder is a pinned sidebar folder whose entries come from a plugin and
refresh on their own: the latest posts of a feed, the pull requests waiting
for you, the next meetings. A plugin is a small JavaScript file plus a
manifest. The same plugin runs on desktop, Android and iOS.

This guide is for plugin authors. The design behind it (sync, storage,
the native runtimes) is in [design/live-folders.md](design/live-folders.md).
The code is in `plugins/`; the built-in plugins in `plugins/builtin/` are
the best examples to copy from.

- [Quick start](#quick-start)
- [How a refresh works](#how-a-refresh-works)
- [The manifest](#the-manifest)
- [The entry script](#the-entry-script)
- [The `compass` object](#the-compass-object)
- [The result](#the-result)
- [Errors](#errors)
- [Secrets and cookies](#secrets-and-cookies)
- [Limits and the sandbox](#limits-and-the-sandbox)
- [Developing and testing](#developing-and-testing)
- [Installing and sharing](#installing-and-sharing)
- [Checklist](#checklist)

## Quick start

A plugin is a folder:

```
hacker-news/
  manifest.json
  main.js
  icon.svg        optional
```

`manifest.json` says what the plugin is, which hosts it may reach and what
the user fills in when they make a folder with it:

```json
{
  "id": "com.example.hacker-news",
  "name": "Hacker News",
  "description": "Front page stories, or stories matching a search.",
  "version": "1.0.0",
  "api": 1,
  "entry": "main.js",
  "hosts": ["hn.algolia.com"],
  "config": [
    {"key": "query", "type": "string", "label": "Only stories about", "optional": true,
     "placeholder": "rust"},
    {"key": "minPoints", "type": "number", "label": "At least this many points",
     "default": 50, "min": 0, "max": 5000},
    {"key": "limit", "type": "number", "label": "Stories to show", "default": 20, "min": 1, "max": 50}
  ],
  "refresh": {"default": 900, "min": 300}
}
```

`main.js` defines one global function, `fetchItems`, that returns the
folder's entries:

```js
// Hacker News stories from the Algolia search API.

function searchUrl(config) {
  const params = new URLSearchParams({
    tags: config.query ? 'story' : 'front_page',
    numericFilters: 'points>=' + config.minPoints,
    hitsPerPage: String(config.limit),
  });
  if (config.query) params.set('query', config.query);
  const path = config.query ? 'search_by_date' : 'search';
  return 'https://hn.algolia.com/api/v1/' + path + '?' + params;
}

async function fetchItems(config, compass) {
  const res = await compass.fetch(searchUrl(config));
  if (!res.ok) throw new Error('Hacker News search answered HTTP ' + res.status);
  const data = await res.json();
  compass.log('hits', data.hits.length);

  const items = data.hits.map((hit) => {
    const discussion = 'https://news.ycombinator.com/item?id=' + hit.objectID;
    return {
      id: hit.objectID,
      title: hit.title,
      url: hit.url || discussion,
      subtitle: hit.points + ' points · ' + (hit.num_comments || 0) + ' comments',
      time: hit.created_at_i * 1000,
    };
  });
  return {title: config.query ? 'HN: ' + config.query : 'Hacker News', items};
}
```

Try it from a checkout of this repo, against the real network:

```
node tools/live_folders/run.mjs check path/to/hacker-news
node tools/live_folders/run.mjs run path/to/hacker-news --config query=rust
```

Then try it in Compass: in the sidebar, click **New Live Folder** (the folder
with a bolt, beside New Folder), then **Open plugins folder**. Copy the
`hacker-news` folder into the folder that opens and switch back to Compass.
The plugin shows in the list marked **Local**. See
[Local plugins](#local-plugins).

## How a refresh works

Each refresh runs in a fresh, empty JavaScript context: a dedicated Worker
on desktop and Android, a `JSContext` on iOS and a Node `vm` context in the
reference runtime. The context loads, in order:

1. the SDK: `plugins/sdk/text.js`, `xml.js`, `html.js`, `ics.js`,
2. `plugins/runtime/prelude.js`, which defines the `compass` object,
3. your entry script.

Then the runtime calls `fetchItems(config, compass)`, waits for the value it
returns, checks it and turns it into the folder's entries. Nothing survives
from one refresh to the next: no globals, no storage, no cache. Everything
the plugin knows comes from `config`, its secrets and what it fetches.

A folder refreshes when it's created or edited, when it's expanded, when its
space becomes active and on a timer (`refresh.default`, or the result's
`nextRefresh`). After an error it backs off and tries again later.

Each device runs the plugin itself. What syncs between devices is the
folder's definition: the plugin ID, the config, and, for an installed
plugin, where it came from and its hash. The entries, secrets and cookie
grants stay on the device.

## The manifest

`manifest.json`, UTF-8, at most 256 KB.

| Key | Required | Rules |
|---|---|---|
| `id` | yes | Reverse-DNS style, such as `com.example.feed`; at most 100 characters. `compass.*` is reserved for the built-ins, and Compass refuses to install or load a plugin that claims it. |
| `name` | yes | Shown in the plugin list and as the folder's default name. |
| `description` | no | One line, shown in the plugin list. |
| `version` | yes | `1`, `1.2`, `1.2.3` or `1.2.3.4`. |
| `api` | yes | `1`. A runtime refuses API versions it doesn't know. |
| `entry` | no | The entry script, a `.js` file in the plugin folder. Defaults to `main.js`. At most 2 MB. |
| `icon` | no | An `.svg` or `.png` file in the plugin folder, at most 256 KB. Draw SVGs 16×16 with `currentColor`, so they follow the theme. Without one the folder keeps the folder glyph. |
| `hosts` | no | The hosts `compass.fetch()` may reach. See below. Without it, the plugin can't fetch anything. |
| `cookies` | no | The hosts that may get the profile's cookies, once the user allows it for the folder. Each must also be allowed by `hosts`. Same syntax. |
| `secrets` | no | Values the user enters once per device, kept encrypted and never synced. See [Secrets and cookies](#secrets-and-cookies). |
| `config` | no | The fields of the folder's settings form. They sync with the folder. See below. |
| `refresh` | no | Seconds between refreshes: `{"default": 600, "min": 300}`. Neither may be below 300 (5 minutes). |

`node tools/live_folders/run.mjs check <dir>` validates a manifest and lists
every problem at once. The rules are in `plugins/runtime/manifest.js`; the
native runtimes port them and are tested against the same cases.

### Hosts

Each entry in `hosts` (and `cookies`) is one of:

| Pattern | Allows |
|---|---|
| `example.com` | Exactly that host. |
| `*.example.com` | `example.com` and every subdomain of it. |
| `{config:key}` | The host of the URL in config field `key`, such as the feed URL the user typed. |
| `{secret:key}` | The host of the URL in secret `key`, such as a private calendar address. |

Placeholders are resolved before every refresh, so a plugin like the RSS
reader can reach whichever feed the user picks and nothing else. Only
`http` and `https` URLs are allowed, never with `user:password@`. Ports are
not part of the match.

Declare as few hosts as you can: the user sees the list before they install
your plugin.

### Config fields

```json
{"key": "show", "type": "select", "label": "Show", "default": "review-requested",
 "options": [{"value": "review-requested", "label": "Review requests"}, "authored"]}
```

| Key | |
|---|---|
| `key` | The name the value has in `config`: a letter, then letters, digits and `_`. |
| `type` | `string`, `url`, `number`, `bool` or `select`. |
| `label` | The form label. |
| `default` | Used when the user leaves the field empty. |
| `optional` | `true` if the field may stay empty. A required field without a default must be filled in. |
| `placeholder` | Hint text in an empty field. |
| `min`, `max` | For `number`. |
| `options` | For `select`: strings, or `{value, label}` objects. |

`fetchItems` gets the config already checked against these rules, with
defaults filled in: numbers are numbers within `min`/`max`, `bool`s are
booleans, `url`s are absolute `http(s)` URLs and `select`s are one of the
options. An optional field the user left empty is missing from `config`, so
test for it (`if (config.query)`); an empty `bool` is `false`. A folder whose config breaks the rules
fails with `bad_config` without running the plugin.

Config syncs to the user's other devices. Never put a token or a private
URL in config: declare a secret.

## The entry script

The entry is a **classic script**, not an ES module: no `import` or
`export`. It must define a global function:

```js
async function fetchItems(config, compass) {
  // ...
  return {items: [...]};
}
```

- It can be `async` or return a promise. The runtime waits for it, up to the
  refresh's time limit.
- `config` is the frozen, validated config.
- `compass` is also a global, so helper functions can use it without passing
  it around.
- Throwing (or rejecting) fails the refresh, and the error's message is
  shown on the folder. Write it for the user: "GitHub rejected the token.
  Check it in the folder's settings." beats "401".

A plugin that needs several files concatenates them into the entry: the
runtime loads one script and has no module loader. Keep helpers as plain
functions in the same file, as the built-ins do.

### What's available

The context has ECMAScript's globals (`JSON`, `Promise`, `Map`, `Intl`,
`RegExp` and so on) plus:

- `URL`, `URLSearchParams`,
- `TextEncoder`, `TextDecoder`,
- `setTimeout`, `clearTimeout`,
- `compass`.

There is **no** DOM, `document` or `DOMParser` (Workers and JavaScriptCore
have none: use `compass.xml` and `compass.html`), no global `fetch` or
`XMLHttpRequest` (use `compass.fetch`), no `localStorage` or `indexedDB`,
no `eval` or `new Function`, no `importScripts` and nothing from Node.
Something that happens to work in one runtime but isn't in this list may be
missing in another: stick to the list.

## The `compass` object

### `compass.fetch(url, options)`

```js
const res = await compass.fetch('https://api.example.com/items?page=1', {
  method: 'POST',                         // GET (default), HEAD or POST
  headers: {'content-type': 'application/json', authorization: 'Bearer …'},
  body: JSON.stringify({query: 'is:open'}),
  credentials: 'include',                 // send cookies; see "Secrets and cookies"
});
```

It resolves to a response:

| Member | |
|---|---|
| `status` | The HTTP status. |
| `ok` | `true` for 2xx. |
| `url` | The final URL, after redirects. Use it as the base for relative links. |
| `headers` | A plain object; header names are lower case. |
| `text()` | A promise of the body as a string, decoded with the `charset` of `Content-Type`, UTF-8 otherwise. |
| `json()` | A promise of the parsed body; rejects if it isn't JSON. |

It is not the web's `fetch`: there are no `Request`, `Response` or
`Headers` classes, the body is always text (no binary, no streaming) and
only `GET`, `HEAD` and `POST` exist. An HTTP error status is not an
exception; check `res.ok`. `fetch` rejects (with an error whose `code` says
why) when the URL isn't allowed, the plugin has fetched too often, the
response is too large or the network fails. Let those errors propagate
unless you have something better to do: the runtime reports them with the
right code.

The runtime drops request headers a plugin must not set: `cookie`, `host`,
`origin`, `referer`, `content-length`, any `sec-*` and the hop-by-hop
headers. Redirects are followed, up to 5, and each hop must be an allowed
host too.

### `compass.secret(key)`

The value of a declared secret, or `null` if the user left an optional one
empty. See [Secrets and cookies](#secrets-and-cookies).

### `compass.now()`, `compass.locale`, `compass.timeZone`

`now()` is the time of this refresh, in epoch milliseconds. Use it instead
of `Date.now()`: tests fix it, so results with times in them stay
repeatable. `locale` (such as `en-GB`) and `timeZone` (such as
`Europe/Rome`) are the user's, for `Intl` formatting and for deciding what
"today" means.

### `compass.log(...args)`

Writes a line to the refresh's log. Strings are logged as they are, other
values as JSON. The last refresh keeps up to 100 lines of 1000 characters.
`run.mjs run` prints them as `[log]` lines; Compass keeps them with the
folder's state but doesn't show them in the sidebar yet.

### `compass.text`

| Function | |
|---|---|
| `decodeEntities(s)` | `&amp;`, `&#39;`, `&eacute;`… → characters. |
| `stripTags(html)` | Text of an HTML fragment: tags removed, entities decoded, whitespace collapsed. |
| `collapse(s)` | Runs of whitespace → one space, trimmed. |
| `parseAttributes(s, lowerNames)` | `'href="/a" rel=next'` → `{href: '/a', rel: 'next'}`. |
| `parseDate(s)` | ISO 8601 and RFC 822 (feed) dates → epoch ms, or `null`. |

### `compass.xml`

A small, lenient XML parser for feeds and APIs. It never throws: unclosed
elements close at the end and stray end tags are ignored.

```js
const doc = compass.xml.parse(await res.text());
for (const item of doc.findAll('item')) {
  const title = item.childText('title');
  const link = item.child('link');
  const href = link && (link.attr('href') || link.text());
}
```

Elements have `name`, `localName`, `attrs` and `children` (elements and
strings), and:

| Method | |
|---|---|
| `child(name)` | First child element with that name, or `null`. `'*'` matches any. |
| `childText(name)` | Its text, or `''`. |
| `elements(name?)` | Child elements, optionally only those with that name. |
| `find(name)` | First descendant with that name, depth first. |
| `findAll(name)` | Every descendant with that name. |
| `attr(name)` | An attribute's value, or `null`. |
| `text()` | Text content, whitespace collapsed. `rawText()` keeps it as is. |

Names keep their prefix (`content:encoded`). A name without a prefix also
matches prefixed elements with that local name, but an exact match wins:
`child('link')` in an RSS item finds `<link>`, not `<atom:link>`.

### `compass.html`

| Function | |
|---|---|
| `links(page, pageUrl)` | Every `<a href>` as `{url, text, title}`, with URLs made absolute (honouring `<base>`). |
| `title(page)` | The `<title>`, as text. |

It is pattern based, not a DOM: enough for "the links on this page", not for
scraping deep structure. Prefer a JSON API or a feed when the site has one.

### `compass.ics`

iCalendar (RFC 5545) for calendar plugins:

```js
const calendar = compass.ics.parse(await res.text());
const events = compass.ics.occurrences(calendar, {
  from: compass.now(),
  to: compass.now() + 24 * 3600 * 1000,
  timeZone: compass.timeZone,
});
// → [{uid, title, start, end, allDay, location, description, url, status, props}],
//   sorted by start (epoch ms)
```

It expands `RRULE` (daily, weekly, monthly and yearly, with `INTERVAL`,
`COUNT`, `UNTIL`, `BYDAY`, `BYMONTHDAY`, `BYMONTH`, `BYSETPOS`), `RDATE`,
`EXDATE` and `RECURRENCE-ID` overrides, and reads `TZID`s through `Intl`
(including Windows zone names). It doesn't read `VTIMEZONE` blocks, and
ignores `BYWEEKNO`, `BYYEARDAY`, `BYHOUR` and sub-daily rules.
`plugins/builtin/calendar/main.js` shows it in use.

## The result

```js
return {
  items: [
    {
      id: '1234',                            // required: stable across refreshes
      title: 'Fix the login page',           // required
      url: 'https://example.com/issues/1234',// required: absolute http(s)
      subtitle: 'example/web#1234',          // optional
      time: '2026-11-10T09:30:00Z',          // optional: epoch ms or a date string
      badge: 'Draft',                        // optional: at most 12 characters
      icon: 'https://example.com/a.png',     // optional: http(s) image URL
    },
  ],
  title: 'Issues',      // optional: the folder's name if the user didn't give one
  nextRefresh: 300,     // optional: seconds until the next refresh
};
```

- **`id`** is how Compass recognises an entry from one refresh to the next.
  An entry whose `id` is still there is updated in place (and keeps its
  tab, if it's open). One that is new shows as unseen. One that has gone is
  removed, unless its tab is open: then it stays until the tab is closed.
  Use the source's own ID or the entry's permanent URL; never an index or
  anything that changes when the title does. Numbers become strings;
  duplicates are dropped.
- **`title`**: whitespace collapsed, cut to 300 characters.
- **Order** is kept: return entries in the order the folder should show them.
- **At most 50 items**; the rest are dropped. Respect a `limit` config field
  if you have one, so you don't fetch more than you show.
- **`nextRefresh`** is clamped to between `refresh.min` and one day. Use it
  when the source says when to come back (a meeting about to start, a rate
  limit's reset time).

Invalid entries (no title, a relative URL, not an object) are dropped with
a warning; the refresh still works. Only a result that isn't
`{items: [...]}`, or that isn't plain data (functions, class instances),
fails with `bad_result`. The rules are in `plugins/runtime/result.js`.

Returning `{items: []}` is fine and shows an empty folder. Throw instead
when an empty folder would hide a problem, such as a page that no longer has
the list you're reading.

## Errors

When a refresh fails, the folder keeps its previous entries and shows the
error. Each failure has a code; the codes are the contract between
runtimes, the messages may differ.

| Code | Cause |
|---|---|
| `plugin_error` | The plugin threw or rejected, or the entry failed to load (a syntax error). Its message is shown. |
| `bad_plugin` | The entry doesn't define a `fetchItems` function. |
| `bad_manifest` | The manifest breaks the rules. |
| `bad_config` | The folder's config breaks the manifest's rules. |
| `missing_secret` | A required secret isn't set on this device. |
| `bad_result` | The result isn't `{items: [...]}` of plain data. |
| `bad_request` | A fetch had a bad URL, method or option. |
| `host_not_allowed` | A fetch, or a redirect, went to a host `hosts` doesn't allow. |
| `too_many_fetches` | More than 10 fetches in one refresh. |
| `response_too_large` | A response over 2 MB. |
| `network` | The request failed: DNS, TLS, connection. |
| `timeout` | The refresh took more than 30 seconds. |

## Secrets and cookies

### Secrets

Use a secret for anything that grants access: an API token, a private
calendar address, a personal feed URL with a key in it.

```json
"secrets": [
  {"key": "token", "label": "GitHub token",
   "help": "A fine-grained token with read access to issues and pull requests."}
]
```

`{key, label, help?, optional?}` (`key`: a letter, then letters, digits and
`_`). The user enters each secret once per device, in the folder's settings
form, and every folder of the plugin on that device shares it; Compass stores it encrypted (OSCrypt
on desktop and Android, the Keychain on iOS) and never syncs it. On a new
device a synced folder asks for its secrets again. A refresh whose required
secret is missing fails with `missing_secret` before the plugin runs.

Read it with `compass.secret('token')`. To fetch from a host that is only
known from a secret, declare `{secret:key}` in `hosts`, as the calendar
plugin does with the user's private iCal address.

Don't log secrets, put them in results or send them anywhere but the
service they belong to.

### Cookies

A plugin can ask for the profile's cookies, so it sees a site as the
signed-in user, without a token:

```json
"hosts": ["{config:url}"],
"cookies": ["{config:url}"]
```

```js
const res = await compass.fetch(config.url, {credentials: 'include'});
```

Cookies are sent only when all of these are true:

1. the request asks for them with `credentials: 'include'`,
2. the host matches `cookies` (and `hosts`),
3. the user allowed it for this folder. The settings form shows "Send your
   cookies for …" whenever the plugin declares `cookies`. The grant is per
   folder and per device, and doesn't sync.

Otherwise the request goes out without cookies; it doesn't fail. A plugin
can't add a `cookie` header of its own. `plugins/builtin/web-list` shows the
pattern, with a "Use my login on this site" switch in its config.

Prefer a token when the service has an API that takes one; its API host
often won't accept the website's session anyway (api.github.com doesn't).

## Limits and the sandbox

| Limit | Value |
|---|---|
| Time per refresh | 30 seconds, fetches included |
| Fetches per refresh | 10 |
| Response size | 2 MB |
| Redirects per fetch | 5, each hop checked against `hosts` |
| Items per result | 50 |
| Log | 100 lines of 1000 characters |
| Refresh interval | at least 300 seconds |
| Manifest / entry / icon size | 256 KB / 2 MB / 256 KB |

Nothing inside the plugin's context is trusted. The plugin shares its
global with the SDK and the prelude and can change them, so none of them is
a security boundary: the browser checks every fetch, applies every limit and
validates the result outside the plugin's context. The context has no
network of its own (on desktop the Worker runs on a `chrome-untrusted://`
page with a strict CSP), so `compass.fetch` is the only way out.

## Developing and testing

### The reference runtime

`tools/live_folders/` is a Node implementation of the runtime with the same
scripts, rules and limits. It needs only Node (no build) and a checkout of
this repo.

```
node tools/live_folders/run.mjs check <plugin-dir>
node tools/live_folders/run.mjs run <plugin-dir> \
    [--config key=value]... [--secret key=value]... \
    [--locale en-GB] [--tz Europe/Rome]
```

`check` validates the manifest and prints it with defaults filled in. `run`
does one refresh against the real network and prints the log (`[log]`),
every fetch (`[fetch]`) and the result, or the error, as JSON. It exits 1 on
failure. Config values are given as text and checked against the manifest
as in Compass.

### Local plugins

A folder with a `manifest.json` placed directly in the plugins folder is a
**local plugin**. It's the quickest way to try a plugin in Compass while you
write it:

It's `Compass Plugins` in the browser's user data directory: on macOS
`~/Library/Application Support/Compass/Compass Plugins/`. A build started
with `--user-data-dir=<dir>` uses `<dir>/Compass Plugins/`. The **Open
plugins folder** button in the New Live Folder panel opens the right one
(and creates it if needed).

- Compass reads local plugins again whenever the plugin list shows and when
  you switch back to the window, and runs them from where they are. Edit
  `main.js`, then refresh the folder (folder menu › Refresh) to run the new
  code.
- They're marked **Local** in the list. Compass never deletes or changes
  them; remove the folder yourself.
- They have no source URL, so another device that syncs a folder made with
  one can't install it. Install from a URL to share a plugin (below).
- A local plugin wins over an installed one with the same ID.
- The same rules apply as to installed plugins: hosts, cookie grants,
  limits.

The folder name doesn't matter, but it must not be the plugin's ID: Compass
keeps installed plugins in `Compass Plugins/<id>/<version>/`.

### Fixture tests

Tests don't touch the network. A case is a folder with `case.json` (the
inputs and recorded responses) and `expected.json` (the outcome):

```json
{
  "config": {"url": "https://example.org/feeds/releases.atom", "limit": 10},
  "secrets": {},
  "grantCookies": false,
  "env": {"now": 1794300000000, "locale": "en-US", "timeZone": "Europe/Rome"},
  "responses": {
    "https://example.org/feeds/releases.atom": {"file": "feed.xml"},
    "POST https://api.example.com/graphql": {"status": 200, "json": {"data": {}}}
  }
}
```

Each response is `{status, headers, file | body | json, rejectHeaders}`,
keyed by URL (or `"POST <url>"`). A fetch with no recorded response, or
carrying a header listed in its `rejectHeaders`, fails with `network`, so
the outcome no longer matches. `expected.json` is
`{ok, result | error: {code}, fetches: [{method, url, cookies}]}`, so a test
also pins down what the plugin fetched and whether cookies went with it.

```
node tools/live_folders/run.mjs test               # every case
node tools/live_folders/run.mjs test rss/          # cases whose name contains rss/
node tools/live_folders/run.mjs test --update      # rewrite expected.json; review the diff
```

The runner finds cases in `plugins/builtin/<plugin>/tests/<case>/` and
`plugins/conformance/<case>/` (whose `plugin/` subfolder is the plugin
under test), so a plugin's tests run when it lives in this repo. Desktop
runs the same cases in a browser test
(`browser/ui/live_folders/live_folder_runtime_browsertest.cc`), so a
built-in behaves the same in Node and in Compass.

Record real responses (save the feed or the API's JSON next to the case),
cut them down to a few entries and replace anything private. Cover the
error paths too: an HTTP error, a malformed response, an empty list.

## Installing and sharing

To share a plugin, host its files on a web server, side by side:

```
https://example.com/plugins/hacker-news/manifest.json
https://example.com/plugins/hacker-news/main.js
https://example.com/plugins/hacker-news/icon.svg
```

In Compass, **New Live Folder › Install a plugin…** takes the manifest's
URL (`https`, or `http` for `localhost`). Compass fetches the manifest and
the files it names from beside it, and shows the plugin's hosts, the hosts
it may ask cookies for and the secrets it asks for. If the user confirms,
it fetches the files again, checks they're the same ones (by hash) and
installs them into `Compass Plugins/<id>/<version>/`. Installed plugins can
be removed from the same list; their folders stay, and show that the plugin
is missing.

The URL and the hash sync with each folder made with the plugin, so the
user's other devices offer to install the same files. To update a plugin,
publish the new files at the same URL with a higher `version`; installing
it again replaces the old version on that device. Compass doesn't check
for updates on its own yet.

Installing from a `.zip` and a public catalog don't exist yet.

## Checklist

- [ ] `run.mjs check` passes; `id` is yours, not `compass.*`.
- [ ] `hosts` lists only what the plugin fetches; `cookies` only if it
      needs the user's login.
- [ ] Tokens and private URLs are secrets, not config.
- [ ] `fetchItems` is a global function in a classic script; no `import`,
      DOM, `fetch`, `eval` or storage.
- [ ] Every `id` is stable across refreshes.
- [ ] Errors are thrown with a message the user can act on; `res.ok` is
      checked.
- [ ] Times come from `compass.now()`; dates are formatted with
      `compass.locale` and `compass.timeZone`.
- [ ] At most 10 fetches and well under 30 seconds per refresh;
      `refresh.default` no more often than the data changes.
- [ ] Fixture tests cover the normal case and the error paths.
