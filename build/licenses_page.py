#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Writes Compass's third-party licenses page from license files.

  licenses_page.py --out licenses.html \
      "Name|What it's for|https://source|License name|path/to/LICENSE" ...

Each argument is one entry: its name, a line on what Compass uses it for,
its source, the license's name and the license file, whose text the page
shows as it is. Served by chrome://compass-settings/licenses.html, which
the About page links to (docs/design/shortcuts.md "Clean links").
"""

import argparse
import html
import os

PAGE = '''<!doctype html>
<html dir="$i18n{{textdirection}}" lang="$i18n{{language}}">
  <head>
    <meta charset="utf-8">
    <title>Third-party licenses</title>
    <link rel="stylesheet" href="chrome://resources/css/text_defaults_md.css">
    <link rel="stylesheet" href="chrome://theme/colors.css?sets=ui,chrome">
    <style>
      html {{
        color-scheme: light dark;
      }}

      body {{
        background: var(--color-sys-base, #fff);
        color: var(--color-sys-on-surface, #1f1f1f);
        margin: 0;
      }}

      main {{
        margin: 0 auto;
        max-width: 760px;
        padding: 40px 24px 72px;
      }}

      h1 {{
        font-size: 22px;
        font-weight: 500;
        margin: 0 0 8px;
      }}

      .lead,
      .about {{
        color: var(--color-sys-on-surface-subtle, #5f6368);
      }}

      section {{
        border-top: 1px solid var(--color-sys-divider, rgba(0, 0, 0, 0.12));
        margin-top: 24px;
        padding-top: 16px;
      }}

      h2 {{
        font-size: 15px;
        font-weight: 500;
        margin: 0 0 4px;
      }}

      .about {{
        margin: 0 0 12px;
      }}

      a {{
        color: var(--color-sys-primary, #0b57d0);
      }}

      pre {{
        background: var(--color-sys-surface-container, rgba(0, 0, 0, 0.04));
        border-radius: 8px;
        font-size: 12px;
        max-height: 360px;
        overflow: auto;
        padding: 12px;
        white-space: pre-wrap;
      }}
    </style>
  </head>
  <body>
    <main>
      <h1>Third-party licenses</h1>
      <p class="lead">Compass is built on Chromium, whose licenses are on
        <a href="chrome://credits">chrome://credits</a>. Compass also
        includes:</p>
{sections}
    </main>
  </body>
</html>
'''

SECTION = '''      <section>
        <h2>{name} ({license})</h2>
        <p class="about">{about} Source:
          <a href="{url}" target="_blank">{url_text}</a></p>
        <pre>{text}</pre>
      </section>'''


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--out', required=True)
  parser.add_argument('entries', nargs='+')
  args = parser.parse_args()

  sections = []
  for entry in args.entries:
    name, about, url, license_name, path = entry.split('|')
    with open(path, encoding='utf-8') as f:
      text = f.read()
    sections.append(
        SECTION.format(name=html.escape(name),
                       license=html.escape(license_name),
                       about=html.escape(about),
                       url=html.escape(url, quote=True),
                       url_text=html.escape(url),
                       text=html.escape(text).replace('$', '&#36;')))

  os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
  with open(args.out, 'w', encoding='utf-8') as f:
    f.write(PAGE.format(sections='\n'.join(sections)))


if __name__ == '__main__':
  main()
