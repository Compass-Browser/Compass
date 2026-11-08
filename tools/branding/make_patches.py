#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Turns Compass's logo images into patches over upstream's Chromium logos.

  tools/branding/make_patches.py <chromium src>

Renders the images (tools/branding/render.swift), packs the Windows .ico,
writes the vector icons (make_icon.py) and the product SVG, then writes one
binary patch per replaced file to patches/, the same patch
`compass patches export` would write, without touching the checkout. Run it
after changing assets/compass_glyph.svg or assets/compass_card.svg, and
commit the patches. docs/design/branding.md "Logo" lists what's replaced.
"""

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))

from compass_tool.patches import patch_name  # noqa: E402

ICO = 'chrome/app/theme/chromium/win/chromium.ico'
VECTOR_ICONS = [
    'components/vector_icons/chromium/product.icon',
    'components/vector_icons/chromium/product_refresh.icon',
    # The same logo in one color, under names that aren't branded: toasts,
    # the task manager, a web app window's menu (browser_logo_old); infobars,
    # menu items, chrome:// suggestions (product_chrome_refresh_old); the
    # autofill popup (omnibox product_old); notification headers
    # (message_center product_old). chrome_product: the same places with
    # features::kRoundedIcons on.
    'chrome/app/vector_icons/browser_logo_old.icon',
    'chrome/app/vector_icons/chrome_product.icon',
    'components/omnibox/browser/vector_icons/chrome_product.icon',
    'components/omnibox/browser/vector_icons/product_chrome_refresh_old.icon',
    'components/omnibox/browser/vector_icons/product_old.icon',
    'ui/message_center/vector_icons/chrome_product.icon',
    'ui/message_center/vector_icons/product_old.icon',
]
# WebUI's searchbox and the contextual tasks toolbar (one color).
GLYPH_SVGS = [
    'ui/webui/resources/cr_components/searchbox/icons/chrome_product_cr23.svg',
    'ui/webui/resources/cr_components/searchbox/icons/'
    'chrome_product_cr23_old.svg',
]
DARK_LOGO = 'ui/webui/resources/images/chrome_logo_dark.svg'
SVGS = [
    'chrome/app/theme/chromium/product_logo.svg',
    'chrome/app/theme/chromium/product_logo_animation.svg',
]


def pack_ico(frames):
  """An .ico of PNG frames (Vista and later read PNG frames at any size)."""
  header = struct.pack('<HHH', 0, 1, len(frames))
  entries, data = b'', b''
  offset = len(header) + 16 * len(frames)
  for size, png in frames:
    side = 0 if size >= 256 else size
    entries += struct.pack('<BBBBHHII', side, side, 0, 0, 1, 32, len(png),
                           offset + len(data))
    data += png
  return header + entries + data


def build(src: Path, staging: Path) -> None:
  rendered = staging / 'rendered'
  subprocess.run(['swift', str(ROOT / 'tools/branding/render.swift'),
                  str(ROOT), str(src), str(rendered)], check=True,
                 stdout=subprocess.DEVNULL)
  new = staging / 'b'
  for image in rendered.rglob('*.png'):
    relative = image.relative_to(rendered)
    if relative.parts[0] == 'ico':
      continue
    (new / relative).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(image, new / relative)

  frames = sorted((int(png.stem), png.read_bytes())
                  for png in (rendered / 'ico/chromium').glob('*.png'))
  (new / ICO).parent.mkdir(parents=True, exist_ok=True)
  (new / ICO).write_bytes(pack_ico(frames))

  for path in SVGS:
    shutil.copy(ROOT / 'assets/compass_card.svg', new / path)
  build_glyphs(new)


def build_glyphs(new: Path) -> None:
  """The one-color glyphs: vector icons, WebUI SVGs, the dark mode logo."""
  sys.path.insert(0, str(ROOT / 'tools/branding'))
  import make_icon
  for path in VECTOR_ICONS:
    (new / path).parent.mkdir(parents=True, exist_ok=True)
    (new / path).write_text(make_icon.icon())
  for path in GLYPH_SVGS:
    (new / path).parent.mkdir(parents=True, exist_ok=True)
    (new / path).write_text(make_icon.svg())

  # WebUI toolbars show this in dark mode (light mode: product_logo_32).
  left, top, right, bottom = make_icon.bounds()
  side = max(right - left, bottom - top) * 1.08
  view_box = '%.1f %.1f %.1f %.1f' % ((left + right - side) / 2,
                                      (top + bottom - side) / 2, side, side)
  glyph = (ROOT / 'assets/compass_glyph.svg').read_text()
  glyph = glyph.replace('width="1024" height="1024" viewBox="70 70 884 884"',
                        'width="24" height="24" viewBox="%s"' % view_box)
  assert view_box in glyph
  (new / DARK_LOGO).parent.mkdir(parents=True, exist_ok=True)
  (new / DARK_LOGO).write_text(glyph)


def patch_for(src: Path, staging: Path, path: str) -> bytes:
  """`git diff --binary` of upstream's |path| against the staged one, with
  the a/ and b/ names `git diff HEAD` would give."""
  old = staging / 'a' / path
  old.parent.mkdir(parents=True, exist_ok=True)
  shutil.copy(src / path, old)
  result = subprocess.run(
      ['git', 'diff', '--no-index', '--full-index', '--binary',
       '--src-prefix=', '--dst-prefix=', 'a/' + path, 'b/' + path],
      cwd=str(staging), stdout=subprocess.PIPE)
  if result.returncode not in (0, 1):
    raise RuntimeError('git diff failed for ' + path)
  return result.stdout


def main():
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument('src', type=Path, help='the Chromium checkout\'s src/')
  args = parser.parse_args()
  src = args.src.resolve()
  patches_dir = ROOT / 'patches'

  with tempfile.TemporaryDirectory() as tmp:
    staging = Path(tmp)
    build(src, staging)
    new = staging / 'b'
    paths = sorted(p.relative_to(new).as_posix()
                   for p in new.rglob('*') if p.is_file())
    for path in paths:
      if not (src / path).is_file():
        raise SystemExit('upstream has no ' + path)
      content = patch_for(src, staging, path)
      target = patches_dir / patch_name(path)
      if not content:
        print('unchanged ' + path)
        continue
      if target.exists() and target.read_bytes() == content:
        continue
      target.write_bytes(content)
      print('wrote     ' + target.relative_to(ROOT).as_posix())
      check = subprocess.run(['git', 'apply', '--check', str(target)],
                             cwd=str(src), stderr=subprocess.PIPE, text=True)
      if check.returncode != 0:
        # Already applied in this checkout is fine.
        reverse = subprocess.run(['git', 'apply', '--check', '-R',
                                  str(target)], cwd=str(src),
                                 stderr=subprocess.DEVNULL)
        if reverse.returncode != 0:
          raise SystemExit('%s doesn\'t apply:\n%s' % (target, check.stderr))


if __name__ == '__main__':
  main()
