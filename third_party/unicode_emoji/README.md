# Unicode's emoji, with CLDR's English names and keywords

The emoji the space emoji picker offers
(`compass/resources/sidebar/emoji_data.ts`, docs/design/spaces.md, "Space
icons"), derived from:

- Unicode's `emoji-test.txt` (Emoji 18.0, 2026-04-30): the emoji, their
  order and groups, https://unicode.org/Public/emoji/latest/emoji-test.txt;
- CLDR's `annotations/en.xml` and `annotationsDerived/en.xml`: names and
  search keywords, https://github.com/unicode-org/cldr (main, 2026-11-25).

Both under the Unicode License V3 (`LICENSE`). `emoji_data.ts` keeps the
fully-qualified emoji up to Emoji 15.0, without skin tone variants.

To update: `tools/update_emoji_data.py`, then the dates and versions here.
