# Live folder plugins

JavaScript plugins that fill live folders, the SDK they use and the runtime
scripts every platform loads. The contract and how to test a plugin are in
[docs/design/live-folders.md](../docs/design/live-folders.md).

```
sdk/          text.js, xml.js, html.js, ics.js (+ tests/)
runtime/      prelude.js (in the plugin's context), manifest.js, result.js (host side)
builtin/      rss, github, web-list, calendar (each with tests/)
conformance/  runtime rule cases that every runtime must pass
```

```
node --test plugins/sdk/tests/
node tools/live_folders/run.mjs test
```
