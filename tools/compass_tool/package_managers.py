# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Homebrew and winget (docs/design/updates.md, "Package managers").

After a release is published, `compass release publish` updates the
Homebrew cask in the tap (Compass-Browser/homebrew-tap, `brew install
compass-browser/tap/compass`) and `publish-win` opens a pull request to
microsoft/winget-pkgs for the new version of Compass.Browser, from a fork
of it under the account gh is signed in as. Both go through `gh api`, with
gh's own sign-in: no token is kept here.

Neither ever fails a release: what goes wrong is printed as a warning, and
can be redone by hand (`compass release packages <version>`).
COMPASS_SKIP_PACKAGE_MANAGERS=1 (or --no-package-managers) skips both.

The text generators are pure functions, tested in
tools/tests/test_package_managers.py.
"""

from __future__ import annotations

import base64
import json
import os
import re
import subprocess
import time
from typing import Callable, Dict, List, Optional

HOMEPAGE = 'https://compassbrowser.org/'
SOURCE_REPO = 'Compass-Browser/Compass'
SHORT_DESCRIPTION = 'Open-source Arc alternative built on Chromium'
SKIP_ENV = 'COMPASS_SKIP_PACKAGE_MANAGERS'

# Homebrew ------------------------------------------------------------------

TAP_REPO = 'Compass-Browser/homebrew-tap'
CASK_PATH = 'Casks/compass.rb'
CASK_TOKEN = 'compass'
# Sparkle's feed names the newest release of every channel; canary.json the
# newest canary one, which the cask follows (as winget does).
FEED_URL = 'https://updates.compassbrowser.org/canary.json'
# macOS releases' names in the cask's `depends_on macos:`, from the oldest
# Compass supports (LSMinimumSystemVersion, 13.0).
MACOS_NAMES = {
    '13': 'ventura',
    '14': 'sonoma',
    '15': 'sequoia',
    '26': 'tahoe',
}
DEFAULT_MINIMUM_MACOS = '13.0'
# Bundle ID and folders the app writes, for `brew uninstall --zap`.
MAC_BUNDLE_ID = 'it.cipi1965.compass'

# winget --------------------------------------------------------------------

WINGET_REPO = 'microsoft/winget-pkgs'
WINGET_ID = 'Compass.Browser'
WINGET_SCHEMA = '1.28.0'
WINGET_PUBLISHER = 'The Compass Authors'
# The key mini_installer writes under
# HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall.
WINGET_PRODUCT_CODE = 'Compass'
# What the Windows updater runs it with too: no window either way.
WINGET_SILENT = '--do-not-launch-chrome'


class PackageError(Exception):
  pass


def skipped(flag: bool = False) -> bool:
  return flag or os.environ.get(SKIP_ENV, '') not in ('', '0')


def version_key(version: str) -> tuple:
  return tuple(int(part) for part in version.split('.'))


# Cask ----------------------------------------------------------------------


def macos_requirement(minimum: str) -> str:
  """`depends_on macos:`'s value for LSMinimumSystemVersion |minimum|
  (that release or later, as `brew style` wants it written)."""
  major = (minimum or DEFAULT_MINIMUM_MACOS).split('.')[0]
  name = MACOS_NAMES.get(major)
  if not name:
    raise PackageError('no Homebrew name for macOS %s' % minimum)
  return ':%s' % name


def cask(version: str, sha256: str,
         minimum_macos: str = DEFAULT_MINIMUM_MACOS) -> str:
  """Casks/compass.rb for the macOS release |version| (its .dmg's SHA-256
  is |sha256|)."""
  if not re.fullmatch(r'\d+(\.\d+)*', version):
    raise PackageError('bad version %r' % version)
  if not re.fullmatch(r'[0-9a-f]{64}', sha256):
    raise PackageError('bad SHA-256 %r' % sha256)
  return '''\
cask "%(token)s" do
  version "%(version)s"
  sha256 "%(sha256)s"

  url "https://github.com/%(repo)s/releases/download/v#{version}/Compass-#{version}.dmg"
  name "Compass"
  name "Compass Browser"
  desc "%(desc)s"
  homepage "%(homepage)s"

  livecheck do
    url "%(feed)s"
    strategy :json do |json|
      json.dig("mac", "version")
    end
  end

  auto_updates true
  depends_on macos: %(macos)s

  app "Compass.app"

  zap trash: [
    "~/Library/Application Support/Compass",
    "~/Library/Caches/Compass",
    "~/Library/Caches/%(bundle)s",
    "~/Library/HTTPStorages/%(bundle)s",
    "~/Library/Preferences/%(bundle)s.plist",
    "~/Library/Saved Application State/%(bundle)s.savedState",
  ]
end
''' % {
      'token': CASK_TOKEN,
      'version': version,
      'sha256': sha256,
      'repo': SOURCE_REPO,
      'desc': SHORT_DESCRIPTION,
      'homepage': HOMEPAGE,
      'feed': FEED_URL,
      'macos': macos_requirement(minimum_macos),
      'bundle': MAC_BUNDLE_ID,
  }


def cask_version(text: str) -> Optional[str]:
  """The version a cask names, or None."""
  match = re.search(r'^\s*version "([^"]+)"', text or '', re.M)
  return match.group(1) if match else None


# winget manifests ----------------------------------------------------------


def win_display_version(chromium: str, win_build: int) -> str:
  """The DisplayVersion setup writes in the registry: Chromium's version
  with the Windows build number as MINOR (docs/design/updates.md,
  "Installer versions"), e.g. 157.31.8103.51."""
  parts = chromium.split('.')
  if len(parts) != 4 or not all(p.isdigit() for p in parts):
    raise PackageError('bad Chromium version %r' % chromium)
  if win_build < 1:
    raise PackageError('no Windows build number')
  parts[1] = str(win_build)
  return '.'.join(parts)


def winget_dir(version: str) -> str:
  """Where Compass.Browser |version|'s manifests go in winget-pkgs."""
  publisher, name = WINGET_ID.split('.')
  return 'manifests/%s/%s/%s/%s' % (publisher[0].lower(), publisher, name,
                                    version)


def _header(kind: str) -> str:
  return ('# Created by compass release (tools/compass_tool/'
          'package_managers.py in %s)\n'
          '# yaml-language-server: $schema=https://aka.ms/winget-manifest.'
          '%s.%s.schema.json\n\n' % (SOURCE_REPO, kind, WINGET_SCHEMA))


def winget_manifests(release: Dict) -> Dict[str, str]:
  """The three manifests (path in winget-pkgs -> YAML) for |release|'s
  Windows installer (a releases.json entry with "win")."""
  win = release.get('win')
  if not win:
    raise PackageError('release %s has no Windows installer' %
                       release.get('version'))
  version = release['version']
  if not re.fullmatch(r'\d+(\.\d+)*', version):
    raise PackageError('bad version %r' % version)
  url = win.get('url')
  if not url or not url.startswith('https://'):
    raise PackageError('release %s\'s installer has no https URL' % version)
  sha = win.get('sha256', '')
  if not re.fullmatch(r'[0-9a-fA-F]{64}', sha):
    raise PackageError('bad SHA-256 %r' % sha)
  display = win_display_version(release['chromium'], win.get('build', 0))
  date = release.get('date', '')[:10]
  folder = winget_dir(version)
  common = 'PackageIdentifier: %s\nPackageVersion: %s\n' % (WINGET_ID,
                                                            version)
  footer = 'ManifestVersion: %s\n' % WINGET_SCHEMA

  version_yaml = (_header('version') + common +
                  'DefaultLocale: en-US\nManifestType: version\n' + footer)

  installer_yaml = _header('installer') + common + '''\
InstallerType: exe
Scope: user
InstallModes:
- silent
- silentWithProgress
InstallerSwitches:
  Silent: %(silent)s
  SilentWithProgress: %(silent)s
UpgradeBehavior: install
Protocols:
- http
- https
FileExtensions:
- htm
- html
- pdf
- shtml
- svg
- webp
- xht
- xhtml
ProductCode: %(product)s
%(date)sAppsAndFeaturesEntries:
- DisplayName: Compass
  Publisher: %(publisher)s
  DisplayVersion: %(display)s
  ProductCode: %(product)s
Installers:
- Architecture: x64
  InstallerUrl: %(url)s
  InstallerSha256: %(sha)s
ManifestType: installer
''' % {
      'silent': WINGET_SILENT,
      'product': WINGET_PRODUCT_CODE,
      'date': 'ReleaseDate: %s\n' % date if date else '',
      'publisher': WINGET_PUBLISHER,
      'display': display,
      'url': url,
      'sha': sha.upper(),
  } + footer

  locale_yaml = _header('defaultLocale') + common + '''\
PackageLocale: en-US
Publisher: %(publisher)s
PublisherUrl: %(homepage)s
PublisherSupportUrl: https://github.com/%(repo)s/issues
PrivacyUrl: %(homepage)sprivacy
PackageName: Compass
PackageUrl: %(homepage)s
License: BSD-3-Clause
LicenseUrl: https://github.com/%(repo)s/blob/main/LICENSE
Copyright: Copyright 2026 The Compass Authors
ShortDescription: %(desc)s.
Description: |-
  Compass is an open-source browser built on Chromium, with a sidebar of
  spaces, pinned tabs and folders, and sync that never needs an account.
  It updates itself.
Tags:
- arc
- browser
- chromium
- web-browser
ReleaseNotesUrl: %(homepage)schangelog/windows
ManifestType: defaultLocale
''' % {
      'publisher': WINGET_PUBLISHER,
      'homepage': HOMEPAGE,
      'repo': SOURCE_REPO,
      'desc': SHORT_DESCRIPTION,
  } + footer

  return {
      '%s/%s.yaml' % (folder, WINGET_ID): version_yaml,
      '%s/%s.installer.yaml' % (folder, WINGET_ID): installer_yaml,
      '%s/%s.locale.en-US.yaml' % (folder, WINGET_ID): locale_yaml,
  }


def winget_branch(version: str) -> str:
  return '%s-%s' % (WINGET_ID, version)


def winget_pr_title(version: str, new_package: bool) -> str:
  if new_package:
    return 'New package: %s version %s' % (WINGET_ID, version)
  return 'Update: %s to %s' % (WINGET_ID, version)


def winget_pr_body(version: str, tested: bool = False) -> str:
  """winget-pkgs' pull request template, filled in. |tested|: validated
  and installed by hand with winget (the first submission was); otherwise
  the generator's tests and winget-pkgs' own validation cover it."""
  check = 'x' if tested else ' '
  return '''\
## Description

%(id)s %(version)s: Compass, an open-source browser built on Chromium \
(%(homepage)s, source at https://github.com/%(repo)s). Made by Compass's \
release tooling after the Windows release was published.

## Checklist

- [ ] Signed the [Contributor License Agreement](https://cla.opensource.microsoft.com)
- [ ] Linked to an issue (if applicable)

## Manifest Checklist

- [x] Checked that there aren't other open [pull requests](https://github.com/microsoft/winget-pkgs/pulls) for the same manifest update/change
- [x] This PR only modifies one (1) manifest
- [%(check)s] Validated manifest locally with `winget validate --manifest <path>`
- [%(check)s] Tested manifest locally with `winget install --manifest <path>`
- [x] Manifest conforms to the [%(schema)s schema](https://github.com/microsoft/winget-pkgs/tree/master/doc/manifest/schema/%(schema)s)

The installer is Chromium's per-user mini_installer, the same one \
Compass's own updater runs: installing it over an older version updates \
that in place (`UpgradeBehavior: install`).
''' % {'id': WINGET_ID, 'version': version, 'homepage': HOMEPAGE,
       'repo': SOURCE_REPO, 'schema': WINGET_SCHEMA, 'check': check}


# GitHub --------------------------------------------------------------------

Api = Callable[..., object]


def gh_api(method: str, path: str, payload: Optional[Dict] = None,
           missing_ok: bool = False):
  """`gh api`'s JSON reply (None for a 404 when |missing_ok|)."""
  args = ['gh', 'api', '-X', method, path]
  if payload is not None:
    args += ['--input', '-']
  try:
    done = subprocess.run(args, capture_output=True, text=True,
                          input=json.dumps(payload) if payload is not None
                          else None)
  except FileNotFoundError:
    raise PackageError('needs the GitHub CLI (gh), signed in')
  if done.returncode != 0:
    if missing_ok and 'HTTP 404' in done.stderr:
      return None
    raise PackageError('gh api %s %s failed: %s' %
                       (method, path, done.stderr.strip()[-300:]))
  return json.loads(done.stdout) if done.stdout.strip() else {}


def update_tap(release: Dict, api: Api = gh_api) -> Optional[str]:
  """Commits the cask for |release| (a releases.json entry with "mac") to
  the tap, unless it names that version or a newer one already. Returns
  the commit, or None when there was nothing to do."""
  mac = release.get('mac')
  if not mac:
    raise PackageError('release %s has no macOS build' % release['version'])
  version = release['version']
  text = cask(version, mac['sha256'],
              mac.get('minimum_system_version') or DEFAULT_MINIMUM_MACOS)
  path = 'repos/%s/contents/%s' % (TAP_REPO, CASK_PATH)
  current = api('GET', path, missing_ok=True)
  payload = {
      'message': '%s %s' % (CASK_TOKEN, version),
      'content': base64.b64encode(text.encode()).decode(),
  }
  if current:
    old = base64.b64decode(current.get('content', '')).decode()
    old_version = cask_version(old)
    if old_version and version_key(old_version) >= version_key(version):
      return None
    payload['sha'] = current['sha']
  reply = api('PUT', path, payload)
  return reply['commit']['sha']


def _fork(api: Api, login: str) -> str:
  """The signed-in account's fork of winget-pkgs, made if needed (GitHub
  makes forks in the background: this waits for it)."""
  name = '%s/%s' % (login, WINGET_REPO.split('/')[1])
  repo = api('GET', 'repos/' + name, missing_ok=True)
  if repo and (repo.get('parent') or {}).get('full_name') == WINGET_REPO:
    return name
  name = api('POST', 'repos/%s/forks' % WINGET_REPO,
             {'default_branch_only': True})['full_name']
  for _ in range(30):
    if api('GET', 'repos/%s/git/ref/heads/master' % name, missing_ok=True):
      return name
    time.sleep(10)
  raise PackageError('the fork %s isn\'t ready yet' % name)


def submit_winget(release: Dict, api: Api = gh_api,
                  tested: bool = False) -> Optional[str]:
  """Opens the winget-pkgs pull request for |release|'s Windows installer.
  Returns its URL, or None when winget-pkgs has that version already or a
  pull request for it is open."""
  version = release['version']
  files = winget_manifests(release)
  folder = winget_dir(version)
  if api('GET', 'repos/%s/contents/%s' % (WINGET_REPO, folder),
         missing_ok=True):
    return None
  login = api('GET', 'user')['login']
  branch = winget_branch(version)
  open_prs = api('GET', 'repos/%s/pulls?state=open&head=%s:%s' %
                 (WINGET_REPO, login, branch))
  if open_prs:
    return None
  package_dir = folder.rsplit('/', 1)[0]
  new_package = not api('GET', 'repos/%s/contents/%s' %
                        (WINGET_REPO, package_dir), missing_ok=True)
  fork = _fork(api, login)
  base = api('GET', 'repos/%s/git/ref/heads/master' % WINGET_REPO)
  base_sha = base['object']['sha']
  base_tree = api('GET', 'repos/%s/git/commits/%s' %
                  (WINGET_REPO, base_sha))['tree']['sha']
  tree = api('POST', 'repos/%s/git/trees' % fork, {
      'base_tree': base_tree,
      'tree': [{'path': path, 'mode': '100644', 'type': 'blob',
                'content': text} for path, text in sorted(files.items())],
  })['sha']
  title = winget_pr_title(version, new_package)
  commit = api('POST', 'repos/%s/git/commits' % fork, {
      'message': title, 'tree': tree, 'parents': [base_sha]})['sha']
  ref = 'repos/%s/git/refs/heads/%s' % (fork, branch)
  if api('GET', ref, missing_ok=True):
    api('PATCH', ref, {'sha': commit, 'force': True})
  else:
    api('POST', 'repos/%s/git/refs' % fork,
        {'ref': 'refs/heads/' + branch, 'sha': commit})
  pr = api('POST', 'repos/%s/pulls' % WINGET_REPO, {
      'title': title,
      'head': '%s:%s' % (login, branch),
      'base': 'master',
      'body': winget_pr_body(version, tested),
      'maintainer_can_modify': True,
  })
  return pr['html_url']


# After a release -------------------------------------------------------------


def _warn(what: str, error: Exception) -> None:
  print('warning: %s failed: %s (the release itself is done; redo it with '
        '`compass release packages <version>`)' % (what, error))


def after_mac_release(release: Dict, skip: bool = False,
                      api: Api = gh_api) -> None:
  """The tap's cask, after |release| was published. Never raises."""
  if skipped(skip):
    print('package managers skipped')
    return
  try:
    commit = update_tap(release, api)
    if commit:
      print('Homebrew: %s now installs %s (%s)' %
            (TAP_REPO, release['version'], commit[:10]))
    else:
      print('Homebrew: %s has %s or newer already' %
            (TAP_REPO, release['version']))
  except Exception as e:  # pylint: disable=broad-except
    _warn('updating the Homebrew cask', e)


def after_win_release(release: Dict, skip: bool = False,
                      api: Api = gh_api) -> None:
  """The winget-pkgs pull request, after |release| was published. Never
  raises."""
  if skipped(skip):
    print('package managers skipped')
    return
  try:
    url = submit_winget(release, api)
    if url:
      print('winget: opened %s' % url)
    else:
      print('winget: %s %s is in winget-pkgs or has an open pull request '
            'already' % (WINGET_ID, release['version']))
  except Exception as e:  # pylint: disable=broad-except
    _warn('the winget pull request', e)


def after_release(release: Dict, skip: bool = False,
                  api: Api = gh_api) -> List[str]:
  """Both, for whichever platforms |release| has. Returns what ran."""
  done = []
  if release.get('mac'):
    after_mac_release(release, skip, api)
    done.append('mac')
  if release.get('win'):
    after_win_release(release, skip, api)
    done.append('win')
  return done
