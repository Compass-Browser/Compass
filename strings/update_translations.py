#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Adds translations of compass_strings.grd's messages to an .xtb by name.

An .xtb file names each translation by its message's ID, a fingerprint grit
computes from the English text (and meaning), so translations can't be
written by hand. This reads tab-separated lines,

  IDS_COMPASS_SAVE<TAB>Salva
  IDS_COMPASS_DELETE_SPACE<TAB>Eliminare <ph name="SPACE_NAME"/>?

writes them under their IDs, and drops translations no message has any more
(a message whose English text changed gets a new ID, so its old translation
goes and it shows in English until it's translated again). Text is as the
.xtb has it: XML-escaped, with each placeholder as <ph name="..."/>.

Run from the Chromium checkout's src/, which has grit:

  python3 compass/strings/update_translations.py \\
      --xtb compass/strings/translations/compass_strings_it.xtb it.tsv
  python3 compass/strings/update_translations.py \\
      --xtb compass/strings/translations/compass_strings_it.xtb --missing
"""

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ElementTree

_HERE = os.path.dirname(os.path.abspath(__file__))
_TRANSLATION = re.compile(r'<translation id="(\d+)">(.*?)</translation>', re.S)
_PLACEHOLDER = re.compile(r'<ph name="([A-Z0-9_]+)"\s*/>')


def read_messages(grd, grit_dir):
  """{name: (translation ID, placeholder names)}, conditions ignored."""
  sys.path.insert(0, grit_dir)
  from grit import grd_reader
  from grit.node import message
  defines = {'_google_chrome': False}
  root = grd_reader.Parse(grd, defines=defines, skip_validation_checks=True)
  messages = {}
  for node in root.Preorder():
    if isinstance(node, message.MessageNode):
      clique = node.GetCliques()[0]
      placeholders = sorted(
          p.GetPresentation() for p in clique.GetMessage().GetPlaceholders())
      messages[node.attrs['name']] = (clique.GetId(), placeholders)
  return messages


def read_xtb(path):
  if not os.path.exists(path):
    return {}
  with open(path, encoding='utf-8') as f:
    return {m.group(1): m.group(2) for m in _TRANSLATION.finditer(f.read())}


def write_xtb(path, lang, translations):
  lines = [
      '<?xml version="1.0" ?>',
      '<!DOCTYPE translationbundle>',
      '<translationbundle lang="%s">' % lang,
  ]
  for id_ in sorted(translations):
    lines.append('<translation id="%s">%s</translation>' %
                 (id_, translations[id_]))
  lines.append('</translationbundle>')
  with open(path, 'w', encoding='utf-8', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')


def check_text(name, text, placeholders):
  """An error for |text|, or None."""
  try:
    ElementTree.fromstring('<t>%s</t>' % text)
  except ElementTree.ParseError as e:
    return '%s: not valid XML (%s)' % (name, e)
  found = sorted(_PLACEHOLDER.findall(text))
  if sorted(set(found)) != sorted(set(placeholders)):
    return '%s: placeholders %s, the message has %s' % (name, found,
                                                       placeholders)
  return None


def main():
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument('--grd', default=os.path.join(_HERE,
                                                    'compass_strings.grd'))
  parser.add_argument('--grit-dir',
                      default=os.path.normpath(
                          os.path.join(_HERE, '..', '..', 'tools', 'grit')))
  parser.add_argument('--xtb', required=True)
  parser.add_argument('--lang',
                      help='the .xtb\'s language, if it doesn\'t exist yet')
  parser.add_argument('--missing', action='store_true',
                      help='list the messages without a translation')
  parser.add_argument('tsv', nargs='*', help='NAME<TAB>translation lines')
  args = parser.parse_args()

  messages = read_messages(args.grd, args.grit_dir)
  ids = {id_ for id_, _ in messages.values()}
  translations = {
      id_: text
      for id_, text in read_xtb(args.xtb).items() if id_ in ids
  }

  errors = []
  given = {}
  for tsv in args.tsv:
    with open(tsv, encoding='utf-8') as f:
      for number, line in enumerate(f, 1):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'):
          continue
        name, sep, text = line.partition('\t')
        if not sep or name not in messages:
          errors.append('%s:%d: no message %r' % (tsv, number, name))
          continue
        id_, placeholders = messages[name]
        error = check_text(name, text.strip(), placeholders)
        if error:
          errors.append('%s:%d: %s' % (tsv, number, error))
          continue
        # Messages with the same English text share an ID, and so one
        # translation: give one a meaning="" if they need different ones.
        if id_ in given and given[id_][1] != text.strip():
          errors.append('%s:%d: %s has the text of %s, which is translated '
                        'differently' % (tsv, number, name, given[id_][0]))
          continue
        given[id_] = (name, text.strip())
  if errors:
    sys.exit('\n'.join(errors))
  for id_, (_, text) in given.items():
    translations[id_] = text

  lang = args.lang
  if not lang:
    with open(args.xtb, encoding='utf-8') as f:
      lang = re.search(r'<translationbundle lang="([^"]+)"', f.read()).group(1)
  write_xtb(args.xtb, lang, translations)

  if args.missing:
    for name, (id_, _) in sorted(messages.items()):
      if id_ not in translations:
        print(name)


if __name__ == '__main__':
  main()
