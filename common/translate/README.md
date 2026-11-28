# common/translate

Compass's page translator, shared by desktop (Mac, Windows, Linux) and iOS.
See [docs/design/translation.md](../../docs/design/translation.md), "The
shared page translator".

| File | What |
|---|---|
| `page_translator.js` | `__compassTranslator.start({source, target, engine})`, `stop()`, `restore()`, `progress()`, `idle()`: finds the page's text, sends it to the platform's engine, writes the translation into the page's own nodes and puts the original back |
| `test/page_translator_test.html` | Checks it with a fake engine; run it as its header says (`RESULT PASS`) |

A platform runs `page_translator.js` in each frame, in Compass's own world
(the desktop's translate isolated world, a `WKContentWorld` on iOS), and
gives `start()` its engine: an async function from `[{text, html}]` to the
translations, `null` for one it couldn't do. HTML segments are a block's
markup with each element numbered (`data-compass-t`) and nothing else; the
engine must keep the numbers on the elements it returns.
