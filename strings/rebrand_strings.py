#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Copies an upstream strings .grd, its parts and translations, renamed.

Upstream's Chromium-branded strings name the product "Chromium" in their
text. This writes a copy where they say "Compass" instead, for grit to build
in place of the original (see strings/BUILD.gn and docs/design/branding.md).

Only message text changes, never names, order or conditions, so grit gives
every message the same resource ID and the generated header stays the same.
A message's translation ID is a fingerprint of its text, so the messages
whose text changed get new IDs: their translations are copied under the new
ID with the same renaming.

  rebrand_strings.py --grit-dir <dir> --resource-ids <file>
      --resource-ids-out <file> --src-root <dir> --copies-root <dir>
      --grd <src .grd> --out-dir <dir> ...
  rebrand_strings.py --list --grd <src .grd>   (files written, relative)
"""

import argparse
import os
import re
import sys

# Whole word only, with ASCII boundaries: "ChromiumOS" stays, and a name
# followed by a letter of another script is renamed ("Chromium을": Hangul
# isn't ASCII).
_NAME = re.compile(r'(?<![A-Za-z0-9_])Chromium(?![A-Za-z0-9_])')
_NEW_NAME = 'Compass'
# The open source project keeps its name: "made possible by the Chromium open
# source project", "The Chromium Projects".
_PROJECT = re.compile(
    r'(?<![A-Za-z0-9_])Chromium(?=\s+(?:open[ -]source|[Pp]rojects?)\b)')
# Spans left alone: placeholder examples, and the project's link in
# "made possible by the <a>Chromium</a> open source project" (in .grd files
# the placeholder holds the <a>, in .xtb files it is empty).
_KEEP = re.compile(
    r'<ex>.*?</ex>'
    r'|(?<=<ph name="BEGIN_LINK_CHROMIUM")[^>]*?(?:/>|>.*?</ph>).*?'
    r'(?=<ph name="END_LINK_CHROMIUM")', re.S)
_AUTHORS = ('The Chromium Authors', 'The Compass Authors')

_MESSAGE = re.compile(r'(<message\b[^>]*>)(.*?)(</message>)', re.S)
_PART = re.compile(r'<part\s+file="([^"]+)"')
_XTB_FILE = re.compile(r'<file\s+path="([^"]+\.xtb)"')
_TRANSLATION = re.compile(r'(<translation id=")(\d+)(">)(.*?)(</translation>)',
                          re.S)


def names_project(text):
  """Whether |text| names the project outside a link (see _PROJECT)."""
  return bool(_PROJECT.search(_KEEP.sub('', text)))


def rename(text):
  """Renames the product in a message's (or translation's) text."""
  text = text.replace(*_AUTHORS)
  out, last = [], 0
  for m in _KEEP.finditer(text):
    out.append(_rename_span(text[last:m.start()]))
    out.append(m.group(0))
    last = m.end()
  out.append(_rename_span(text[last:]))
  return ''.join(out)


def _rename_span(text):
  return _NAME.sub(
      lambda m: m.group(0) if _PROJECT.match(text, m.start()) else _NEW_NAME,
      text)


def rename_messages(source):
  """Renames the product in every <message> body of a .grd or .grdp."""
  return _MESSAGE.sub(lambda m: m.group(1) + rename(m.group(2)) + m.group(3),
                      source)


def read(path):
  with open(path, encoding='utf-8') as f:
    return f.read()


def write_if_changed(path, text):
  """Leaves an unchanged file alone, so its readers don't rebuild."""
  if os.path.exists(path) and read(path) == text:
    return
  os.makedirs(os.path.dirname(path), exist_ok=True)
  with open(path, 'w', encoding='utf-8', newline='') as f:
    f.write(text)


def source_files(grd):
  """The .grd's parts (recursively) and translations, relative to its dir."""
  base = os.path.dirname(grd)
  parts, xtbs, pending = [], [], [os.path.basename(grd)]
  while pending:
    rel = pending.pop(0)
    text = read(os.path.join(base, rel))
    for part in _PART.findall(text):
      part = os.path.normpath(os.path.join(os.path.dirname(rel), part))
      parts.append(part)
      pending.append(part)
    xtbs += _XTB_FILE.findall(text)
  return parts, xtbs


def message_ids(grd, grit_dir):
  """[(message name, translation ID)] in document order, conditions ignored."""
  sys.path.insert(0, grit_dir)
  from grit import grd_reader
  from grit.node import message
  # Every <if> is kept by Preorder(); the defines only need to exist.
  defines = {
      '_google_chrome': False,
      '_is_chrome_for_testing_branded': False,
      'reven': False,
      'toolkit_views': True,
      'use_aura': False,
      'use_ozone': False,
      'use_titlecase': True,
      'is_desktop_android': False,
      'scale_factors': '2x',
  }
  root = grd_reader.Parse(grd, defines=defines, skip_validation_checks=True)
  return [(n.attrs['name'], n.GetCliques()[0].GetId())
          for n in root.Preorder() if isinstance(n, message.MessageNode)]


def generate(grd, out_dir, grit_dir):
  base = os.path.dirname(grd)
  name = os.path.basename(grd)
  parts, xtbs = source_files(grd)

  sources_by_name = {}
  for rel in [name] + parts:
    source = read(os.path.join(base, rel))
    for m in _MESSAGE.finditer(source):
      key = re.search(r'name="(\w+)"', m.group(1))
      if key:
        sources_by_name[key.group(1)] = (
            sources_by_name.get(key.group(1), '') + m.group(2))
    write_if_changed(os.path.join(out_dir, rel), rename_messages(source))

  old = message_ids(grd, grit_dir)
  new = message_ids(os.path.join(out_dir, name), grit_dir)
  if [n for n, _ in old] != [n for n, _ in new]:
    raise SystemExit('rebrand_strings: messages differ after renaming')
  new_id = {o: n for (_, o), (_, n) in zip(old, new) if o != n}
  # A translation of a message that names both the product and the project
  # can't tell them apart (word order differs): it keeps its text.
  keep_text = {
      o for (name, o), (_, n) in zip(old, new)
      if o != n and names_project(sources_by_name.get(name, ''))
  }

  def translate(m):
    if m.group(2) not in new_id:
      return m.group(0)
    text = m.group(4) if m.group(2) in keep_text else rename(m.group(4))
    return m.group(1) + new_id[m.group(2)] + m.group(3) + text + m.group(5)

  for rel in xtbs:
    write_if_changed(os.path.join(out_dir, rel),
                     _TRANSLATION.sub(translate, read(os.path.join(base, rel))))


def write_resource_ids(grds, resource_ids, out, src_root, copies_root):
  """Gives each copy its original's start IDs, under the copy's path.

  grit looks a .grd's start IDs up by its path, and upstream's resource IDs
  only know the originals'.
  """
  ids = eval(read(resource_ids))  # A Python literal, as grit reads it.
  out_dir = os.path.dirname(out)
  entries = {
      'SRCDIR': os.path.relpath(src_root, out_dir).replace(os.sep, '/'),
  }
  for grd in grds:
    key = os.path.relpath(grd, src_root).replace(os.sep, '/')
    copy = os.path.relpath(os.path.join(copies_root, key), src_root)
    entries[copy.replace(os.sep, '/')] = {
        tag: value for tag, value in ids[key].items() if tag != 'META'
    }
  write_if_changed(out, '# Generated by rebrand_strings.py.\n' +
                   repr(entries) + '\n')


def main():
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument('--grd', action='append', required=True)
  parser.add_argument('--out-dir', action='append', default=[],
                      help='one per --grd, in the same order')
  parser.add_argument('--grit-dir')
  parser.add_argument('--resource-ids', help="upstream's resource IDs")
  parser.add_argument('--resource-ids-out')
  parser.add_argument('--src-root')
  parser.add_argument('--copies-root')
  parser.add_argument('--list', action='store_true',
                      help='print the files a --grd gives, relative to its '
                      '--out-dir')
  args = parser.parse_args()
  if args.list:
    for grd in args.grd:
      parts, xtbs = source_files(grd)
      print('\n'.join([os.path.basename(grd)] + parts + xtbs))
    return
  if (len(args.out_dir) != len(args.grd) or not args.grit_dir or
      not args.resource_ids or not args.resource_ids_out or
      not args.src_root or not args.copies_root):
    parser.error('every option and one --out-dir per --grd are required')
  for grd, out_dir in zip(args.grd, args.out_dir):
    generate(grd, out_dir, args.grit_dir)
  write_resource_ids(args.grd, args.resource_ids, args.resource_ids_out,
                     args.src_root, args.copies_root)


if __name__ == '__main__':
  main()
