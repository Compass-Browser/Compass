# Material Symbols

Compass's icons on Android (`res_symbols/drawable`): [Material Symbols](https://fonts.google.com/icons)
Rounded, weight 500 (filled: `_fill`), as vector drawables from
[google/material-design-icons](https://github.com/google/material-design-icons)
(`symbols/android/<name>/materialsymbolsrounded/<name>_wght500_24px.xml`),
under the Apache License 2.0. Each is renamed `compass_sym_<name>` and has its
`android:tint` removed (Compose's `Icon` tints it). They stand in for the SF
Symbols the iOS app uses (docs/design/android.md, "Look").

To add one, fetch it the same way and add it to `symbols_resources` in
BUILD.gn.
