#!/usr/bin/env python3
"""Writes website/compare.html from COMPARE.md.

The page is COMPARE.md as HTML, in the changelog pages' style, with the wide
tables in scrolling boxes (first column fixed, Compass's column marked).
Run it after editing COMPARE.md:

  python3 tools/make_compare_page.py
"""

import html
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Compass compared with Arc, Dia and other browsers</title>
<meta name="description" content="How Compass Browser compares with Arc, Dia, Crest, Zen, Vivaldi, Edge, Orion and the other browsers people move to from Arc.">
<link rel="icon" href="/favicon.ico" sizes="16x16 32x32 48x48">
<link rel="icon" href="/compass.svg" type="image/svg+xml">
<link rel="stylesheet" href="/changelog/changelog.css">
<style>
main { max-width: 1100px; }
main > p, main > ul, main > h2, main > h3 { max-width: 720px; }
h2 { margin: 36px 0 12px; }
h3 { margin: 24px 0 8px; font-size: 17px; }
.table {
  overflow-x: auto; margin: 0 0 16px; border: 1px solid var(--line);
  border-radius: 14px; background: var(--surface);
}
table { border-collapse: separate; border-spacing: 0; font-size: 14px; line-height: 1.4; }
th, td {
  padding: 8px 10px; text-align: left; vertical-align: top;
  border-bottom: 1px solid var(--line); min-width: 110px;
}
tr:last-child td { border-bottom: 0; }
thead th { position: sticky; top: 0; background: var(--chip); font-weight: 600; white-space: nowrap; }
th:first-child, td:first-child {
  position: sticky; left: 0; z-index: 1; background: var(--surface);
  min-width: 170px; max-width: 240px; border-right: 1px solid var(--line);
}
thead th:first-child { background: var(--chip); z-index: 2; }
td.compass, th.compass { background: color-mix(in srgb, var(--accent) 7%, var(--surface)); }
thead th.compass { background: color-mix(in srgb, var(--accent) 16%, var(--chip)); }
tr.group td { position: static; max-width: none; border-right: 0; background: var(--bg); font-weight: 600; }
/* The group title spans the table, so stick the text, not the cell. */
tr.group span { display: inline-block; position: sticky; left: 10px; }
.legend { color: var(--muted); font-size: 14px; }
</style>
</head>
<body>
<main>
<p class="back"><a href="/">Compass Browser</a></p>
"""

TAIL = """<p class="legend">From <a href="https://github.com/Compass-Browser/Compass/blob/main/COMPARE.md">COMPARE.md</a> in the source code.</p>
</main>
</body>
</html>
"""


def inline(text: str) -> str:
  out = html.escape(text, quote=False)
  out = re.sub(r'`([^`]+)`', r'<code>\1</code>', out)
  out = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', out)
  out = re.sub(r'\[([^\]]+)\]\(([^)\s]+)\)',
               lambda m: '<a href="%s">%s</a>' % (link(m.group(2)), m.group(1)),
               out)
  return out


def link(target: str) -> str:
  # Repository-relative docs point at GitHub.
  if re.match(r'^[a-z]+:', target) or target.startswith('#'):
    return target
  return 'https://github.com/Compass-Browser/Compass/blob/main/' + target


def cells(line: str) -> list:
  return [c.strip() for c in line.strip().strip('|').split('|')]


def table(lines: list) -> str:
  head = cells(lines[0])
  compass = [i for i, h in enumerate(head) if 'Compass' in h]
  out = ['<div class="table"><table>', '<thead><tr>']
  for i, h in enumerate(head):
    out.append('<th%s>%s</th>' % (' class="compass"' if i in compass else '',
                                  inline(h)))
  out.append('</tr></thead><tbody>')
  for line in lines[2:]:
    row = cells(line)
    # A group row: bold first cell, the rest empty.
    if row[0].startswith('**') and not any(row[1:]):
      out.append('<tr class="group"><td colspan="%d"><span>%s</span></td></tr>' %
                 (len(head), inline(row[0])))
      continue
    out.append('<tr>')
    for i, c in enumerate(row):
      out.append('<td%s>%s</td>' % (' class="compass"' if i in compass else '',
                                    inline(c)))
    out.append('</tr>')
  out.append('</tbody></table></div>')
  return '\n'.join(out)


def convert(md: str) -> str:
  out = []
  lines = md.splitlines()
  i = 0
  para = []

  def flush():
    if para:
      out.append('<p>%s</p>' % inline(' '.join(para)))
      para.clear()

  while i < len(lines):
    line = lines[i]
    if line.startswith('|'):
      flush()
      block = []
      while i < len(lines) and lines[i].startswith('|'):
        block.append(lines[i])
        i += 1
      out.append(table(block))
      continue
    m = re.match(r'^(#{1,3}) (.*)', line)
    if m:
      flush()
      level = len(m.group(1))
      out.append('<h%d>%s</h%d>' % (level, inline(m.group(2)), level))
    elif line.startswith('- '):
      flush()
      items = []
      while i < len(lines) and (lines[i].startswith('- ') or
                                lines[i].startswith('  ')):
        if lines[i].startswith('- '):
          items.append(lines[i][2:].strip())
        else:
          items[-1] += ' ' + lines[i].strip()
        i += 1
      out.append('<ul>%s</ul>' % ''.join('<li>%s</li>' % inline(t)
                                         for t in items))
      continue
    elif not line.strip():
      flush()
    else:
      para.append(line.strip())
    i += 1
  flush()
  return '\n'.join(out)


def main() -> None:
  md = (ROOT / 'COMPARE.md').read_text()
  body = convert(md)
  # The legend paragraph reads better smaller.
  body = body.replace('<p>✅ yes', '<p class="legend">✅ yes', 1)
  (ROOT / 'website' / 'compare.html').write_text(HEAD + body + '\n' + TAIL)


if __name__ == '__main__':
  main()
