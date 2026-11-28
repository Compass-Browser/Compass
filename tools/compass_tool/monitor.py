# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""`compass monitor`: a terminal view of a desktop release while it runs
(docs/building.md, "Watching a release").

It only watches: the Mac's `release publish` log and its processes, siso's
step counts, the deltas and uploads on disk and on the network, the win1 farm
jobs that build Windows and Linux, the files copied back from them, and the
GitHub release's assets. Nothing it does changes the release.

The layout follows CongressManager's build runner (tools/BuildRunner): the
jobs on the left with their state and time, the selected one's details and
log on the right.
"""

import base64
import collections
try:
  import curses
except ImportError:  # Windows' Python has none; only the live view needs it.
  curses = None
import datetime
import json
import os
import re
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

from . import release

WIN_HOST = 'win1'
BUILDS = Path.home() / 'Builds' / 'compass'
SYMBOLS = release.RELEASE_DIR / 'symbols'
WIN_LINUX_OUT = 'C:/src/compass-chromium/src/out/Portable-linux-x64'

SPINNER = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
GLYPH = {'waiting': '·', 'running': '▶', 'done': '✔', 'failed': '✖',
         'absent': '⊘'}

# The steps of a job, in order. A step is done once a later one has started
# (the release's own prints are buffered, so the log can lag behind its
# processes).
WAITING, RUNNING, DONE, FAILED, ABSENT = ('waiting', 'running', 'done',
                                          'failed', 'absent')


# Formatting -----------------------------------------------------------------


def duration(seconds: Optional[float]) -> str:
  if seconds is None or seconds < 0:
    return ''
  s = int(seconds)
  if s >= 3600:
    return '%d:%02d:%02d' % (s // 3600, s // 60 % 60, s % 60)
  return '%d:%02d' % (s // 60, s % 60)


def size(n: Optional[float]) -> str:
  if n is None:
    return '?'
  for unit in ('B', 'KB', 'MB', 'GB'):
    if abs(n) < 1024 or unit == 'GB':
      return ('%.0f %s' if unit == 'B' else '%.1f %s') % (n, unit)
    n /= 1024
  return ''


def percent(done: float, total: float) -> str:
  return '%d%%' % (100 * done / total) if total else ''


def bar(fraction: float, width: int) -> str:
  fraction = max(0.0, min(1.0, fraction))
  full = int(fraction * width)
  return '█' * full + '░' * (width - full)


# Parsing --------------------------------------------------------------------

NINJA_RE = re.compile(r'\[(\d+)/(\d+)\]\s+(?:(\d+)h)?(?:(\d+)m)?([\d.]+)s')
SISO_PS_RE = re.compile(r'\[(\d+)/(\d+) \(remaining: (\d+)\)\]')
ETIME_RE = re.compile(r'(?:(\d+)-)?(?:(\d+):)?(\d+):(\d+)$')
VERSION_RE = re.compile(r'(\d{8})\.(\d{4})')


def parse_ninja(text: str) -> Optional[Tuple[int, int, float]]:
  """(done, total, seconds) from the last ninja/siso status line in |text|."""
  last = None
  for last in NINJA_RE.finditer(text):
    pass
  if not last:
    return None
  h, m, s = last.group(3), last.group(4), last.group(5)
  seconds = int(h or 0) * 3600 + int(m or 0) * 60 + float(s)
  return int(last.group(1)), int(last.group(2)), seconds


def parse_siso_ps(text: str) -> Optional[Tuple[int, int]]:
  match = SISO_PS_RE.search(text)
  return (int(match.group(1)), int(match.group(2))) if match else None


def parse_etime(text: str) -> int:
  match = ETIME_RE.match(text.strip())
  if not match:
    return 0
  d, h, m, s = (int(g or 0) for g in match.groups())
  return ((d * 24 + h) * 60 + m) * 60 + s


def version_time(version: str) -> float:
  match = VERSION_RE.fullmatch(version)
  if not match:
    return 0.0
  return datetime.datetime.strptime(''.join(match.groups()),
                                    '%Y%m%d%H%M').timestamp()


def job_time(job_id: str) -> float:
  match = re.search(r'(\d{8})-(\d{6})$', job_id)
  if not match:
    return 0.0
  return datetime.datetime.strptime(''.join(match.groups()),
                                    '%Y%m%d%H%M%S').timestamp()


def parse_nettop(text: str) -> Dict[int, int]:
  """{pid: bytes sent} from `nettop -P -L 1 -J bytes_out`."""
  sent = {}
  for line in text.splitlines():
    parts = line.split(',')
    if len(parts) < 2 or '.' not in parts[0]:
      continue
    try:
      sent[int(parts[0].rsplit('.', 1)[1])] = int(parts[1])
    except ValueError:
      continue
  return sent


def parse_lsof_offsets(text: str) -> Dict[str, int]:
  """{path: read offset} from `lsof -o -F on -p <pid>`."""
  offsets, offset = {}, None
  for line in text.splitlines():
    if line.startswith('o'):
      value = line[1:]
      offset = int(value[2:]) if value.startswith('0t') else (
          int(value, 16) if value.startswith('0x') else None)
    elif line.startswith('n') and offset is not None:
      offsets[line[1:]] = offset
      offset = None
  return offsets


def upload_files(args: str) -> List[Path]:
  """The files a `gh release upload <tag> [--clobber] <file>...` sends.
  Paths can hold spaces (Application Support), so the arguments split only
  where an absolute path starts: / or, on Windows, a drive (C:\\)."""
  rest = re.sub(r'^.*?gh release upload \S+(?: --clobber)?', '', args)
  files = [Path(p.strip())
           for p in re.split(r' (?=/|[A-Za-z]:[\\/])', rest) if p.strip()]
  return [f for f in files if f.is_file()]


def latest_version(out: Path) -> Optional[str]:
  logs = sorted(out.glob('release-mac-*.log'),
                key=lambda p: p.stat().st_mtime)
  for log in reversed(logs):
    match = VERSION_RE.search(log.name)
    if match:
      return '%s.%s' % match.groups()
  return None


# Measuring ------------------------------------------------------------------


class Rate:
  """A counter's rate over the last |window| seconds, for an ETA."""

  def __init__(self, window: float = 120.0):
    self.window = window
    self.samples = collections.deque()

  def add(self, value: float, now: Optional[float] = None) -> None:
    now = time.time() if now is None else now
    if self.samples and value < self.samples[-1][1]:
      self.samples.clear()  # a new run, or a total that shrank
    self.samples.append((now, value))
    while len(self.samples) > 2 and now - self.samples[0][0] > self.window:
      self.samples.popleft()

  def per_second(self) -> Optional[float]:
    if len(self.samples) < 2:
      return None
    (t0, v0), (t1, v1) = self.samples[0], self.samples[-1]
    if t1 - t0 < 5:
      return None
    return (v1 - v0) / (t1 - t0)

  def eta(self, remaining: float) -> Optional[float]:
    rate = self.per_second()
    if not rate or rate <= 0:
      return None
    return remaining / rate


class Proc:

  def __init__(self, pid: int, ppid: int, etime: int, args: str):
    self.pid, self.ppid, self.etime, self.args = pid, ppid, etime, args


def processes() -> Dict[int, Proc]:
  out = run(['ps', '-axo', 'pid=,ppid=,etime=,args='])
  procs = {}
  for line in out.splitlines():
    parts = line.split(None, 3)
    if len(parts) < 4:
      continue
    try:
      procs[int(parts[0])] = Proc(int(parts[0]), int(parts[1]),
                                  parse_etime(parts[2]), parts[3])
    except ValueError:
      continue
  return procs


def descendants(procs: Dict[int, Proc], pid: int) -> List[Proc]:
  children = collections.defaultdict(list)
  for p in procs.values():
    children[p.ppid].append(p)
  found, todo = [], [pid]
  while todo:
    for child in children.get(todo.pop(), []):
      found.append(child)
      todo.append(child.pid)
  return found


def run(args: List[str], timeout: float = 20, **kwargs) -> str:
  try:
    return subprocess.run(args, capture_output=True, text=True,
                          timeout=timeout, **kwargs).stdout
  except (OSError, subprocess.SubprocessError):
    return ''


def tail(path: Path, limit: int = 64 * 1024) -> str:
  try:
    with open(path, 'rb') as f:
      f.seek(0, 2)
      f.seek(max(0, f.tell() - limit))
      return f.read().decode('utf-8', 'replace')
  except OSError:
    return ''


def log_lines(text: str, count: int = 400) -> List[str]:
  """The last |count| lines, with progress redraws (\\r) and blanks gone."""
  lines = []
  for line in text.replace('\r\n', '\n').split('\n'):
    line = line.rsplit('\r', 1)[-1].rstrip()
    if line:
      lines.append(line)
  return lines[-count:]


# win1 -----------------------------------------------------------------------

# One round trip a refresh: the compass jobs queued since the release began,
# each with its state, times, the end of its log and its artifacts, and the
# Linux build's files. Windows PowerShell 5 runs it (win1's SSH shell).
WIN_SCRIPT = r'''
$ErrorActionPreference = 'SilentlyContinue'
$since = __SINCE__
function Read1($p) { if (Test-Path $p) { (Get-Content $p -Raw).Trim() } else { '' } }
function Tail($p) {
  if (-not (Test-Path $p)) { return '' }
  $f = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite')
  try {
    $n = [Math]::Min($f.Length, 49152); [void]$f.Seek(-$n, 'End')
    $b = New-Object byte[] $n; [void]$f.Read($b, 0, $n)
    [Convert]::ToBase64String($b)
  } finally { $f.Close() }
}
$jobs = @()
foreach ($d in Get-ChildItem "$HOME\farm\jobs" -Directory) {
  if ($d.Name -notmatch '^compass-(win-release-build|linux)-') { continue }
  $meta = Read1 (Join-Path $d.FullName 'meta.json') | ConvertFrom-Json
  if ([double]$meta.submitted -lt $since) { continue }
  $art = @(Get-ChildItem (Join-Path $d.FullName 'artifacts') -File |
           ForEach-Object { @{ name = $_.Name; size = $_.Length } })
  $jobs += @{ id = $d.Name; recipe = $meta.recipe;
              submitted = $meta.submitted;
              status = Read1 (Join-Path $d.FullName 'status');
              started = Read1 (Join-Path $d.FullName 'started');
              finished = Read1 (Join-Path $d.FullName 'finished');
              log = Tail (Join-Path $d.FullName 'log');
              artifacts = $art }
}
$linux = @(Get-ChildItem '__LINUX_OUT__' -Filter 'Compass-__VERSION__-linux-x64.*' |
           ForEach-Object { @{ name = $_.Name; size = $(if ($_.PSIsContainer) { -1 } else { $_.Length }) } })
@{ jobs = $jobs; linux = $linux } | ConvertTo-Json -Depth 5 -Compress
'''


def query_win(version: str) -> Optional[Dict]:
  script = (WIN_SCRIPT.replace('__SINCE__', '%d' % (version_time(version) -
                                                     600))
            .replace('__LINUX_OUT__', WIN_LINUX_OUT)
            .replace('__VERSION__', version))
  encoded = base64.b64encode(script.encode('utf-16-le')).decode()
  out = run(['ssh', '-o', 'ConnectTimeout=10', '-o', 'BatchMode=yes',
             WIN_HOST, 'powershell -NoProfile -EncodedCommand ' + encoded],
            timeout=40)
  try:
    data = json.loads(out)
  except ValueError:
    return None
  jobs = data.get('jobs') or []
  if isinstance(jobs, dict):  # ConvertTo-Json unwraps a one-item array
    jobs = [jobs]
  for job in jobs:
    job['log'] = base64.b64decode(job.get('log') or '').decode('utf-8',
                                                               'replace')
    if isinstance(job.get('artifacts'), dict):
      job['artifacts'] = [job['artifacts']]
  linux = data.get('linux') or []
  if isinstance(linux, dict):
    linux = [linux]
  return {'jobs': jobs, 'linux': linux}


def pick_job(jobs: List[Dict], recipe: str, version: str,
             next_version: Optional[str] = None) -> Optional[Dict]:
  """The |recipe| job of the release: of those queued since it began (and
  before |next_version|'s began, if there's a later release), a running or
  queued one, else the newest. A version gets more than one (promoting
  Linux to stable repackages it), and the latest is the one to watch."""
  since = version_time(version) - 600
  until = version_time(next_version) - 600 if next_version else float('inf')
  found = [j for j in jobs if j.get('recipe') == recipe and
           since <= job_time(j['id']) < until]
  if not found:
    return None
  alive = [j for j in found if j.get('status') in ('running', 'queued')]
  return max(alive or found, key=lambda j: job_time(j['id']))


def next_version(out: Path, version: str) -> Optional[str]:
  """The first release after |version| with a log in |out|, if any."""
  later = []
  for log in out.glob('release-mac-*.log'):
    match = VERSION_RE.search(log.name)
    if match and version_time('%s.%s' % match.groups()) > version_time(version):
      later.append('%s.%s' % match.groups())
  return min(later, key=version_time) if later else None


# Jobs -----------------------------------------------------------------------


class Step:

  def __init__(self, name: str, state: str = WAITING, note: str = ''):
    self.name, self.state, self.note = name, state, note
    self.seconds: Optional[float] = None


class Job:

  def __init__(self, title: str):
    self.title = title
    self.state = WAITING
    self.note = ''
    self.seconds: Optional[float] = None
    self.steps: List[Step] = []
    self.details: List[Tuple[str, str]] = []  # (text, color role)
    self.log: List[str] = []
    self.log_name = ''


def settle(steps: List[Step], started: List[bool], alive: bool,
           finished_ok: bool) -> None:
  """States from which steps have started: done once a later one has (or
  the whole run succeeded), running while the run is alive, else failed."""
  last = max((i for i, s in enumerate(started) if s), default=-1)
  for i, step in enumerate(steps):
    if finished_ok or i < last:
      step.state = DONE
    elif i == last:
      step.state = RUNNING if alive else FAILED
    else:
      step.state = WAITING if alive or last < 0 else ABSENT


def overall(steps: List[Step]) -> str:
  states = [s.state for s in steps]
  if FAILED in states:
    return FAILED
  if states and all(s == DONE for s in states):
    return DONE
  if RUNNING in states or DONE in states:
    return RUNNING
  return WAITING


class Monitor:
  """Collects what the view shows. refresh() is cheap and runs every few
  seconds; the slow sources (win1, GitHub, siso) refresh on their own
  threads."""

  def __init__(self, version: str, out: Path, depot_tools: Path):
    self.version = version
    self.out = out
    self.depot_tools = depot_tools
    self.mac_log = out / ('release-mac-%s.log' % version)
    self.next_version = next_version(out, version)
    self.rates: Dict[str, Rate] = collections.defaultdict(Rate)
    self.first_seen: Dict[str, float] = {}
    self.done_seen: Dict[str, float] = {}
    self.win: Optional[Dict] = None
    self.win_checked = 0.0
    self.win_error = False
    self.assets: Dict[str, Tuple[int, str]] = {}
    self.assets_checked = 0.0
    self.siso: Dict[str, Tuple[int, int]] = {}
    self.lock = threading.Lock()
    self.jobs: List[Job] = []
    self.started = time.time()
    self._stop = threading.Event()

  # Background sources -------------------------------------------------------

  def start(self) -> None:
    for target, every in ((self._poll_win, 8), (self._poll_assets, 20),
                          (self._poll_siso, 4)):
      threading.Thread(target=self._loop, args=(target, every),
                       daemon=True).start()

  def stop(self) -> None:
    self._stop.set()

  def _loop(self, target: Callable[[], None], every: float) -> None:
    while not self._stop.is_set():
      try:
        target()
      except Exception:  # a source that fails leaves the last value
        pass
      self._stop.wait(every)

  def _poll_win(self) -> None:
    data = query_win(self.version)
    with self.lock:
      self.win_checked = time.time()
      self.win_error = data is None
      if data is not None:
        self.win = data

  def _poll_assets(self) -> None:
    out = run(['gh', 'release', 'view', release.github_tag(self.version),
               '--repo', release.GITHUB_REPO, '--json', 'assets', '--jq',
               '.assets[] | [.name, .size, .state] | @tsv'])
    assets = {}
    for line in out.splitlines():
      parts = line.split('\t')
      if len(parts) == 3:
        assets[parts[0]] = (int(parts[1]), parts[2])
    with self.lock:
      self.assets = assets
      self.assets_checked = time.time()

  def _poll_siso(self) -> None:
    found = {}
    for proc in processes().values():
      match = re.search(r'siso ninja .* -C (\S+)', proc.args)
      if match:
        out_dir = match.group(1)
        text = run([str(self.depot_tools / 'siso'), 'ps', '-C', out_dir],
                   timeout=15)
        counts = parse_siso_ps(text)
        if counts:
          found[Path(out_dir).name] = counts
    with self.lock:
      self.siso = found

  # Snapshot -----------------------------------------------------------------

  def refresh(self) -> List[Job]:
    procs = processes()
    with self.lock:
      win, assets, siso = self.win, dict(self.assets), dict(self.siso)
    jobs = [self._mac(procs, siso, assets)]
    jobs += self._farm(win)
    jobs += [self._publish_win(procs, win, assets),
             self._publish_linux(procs, win, assets)]
    now = time.time()
    for job in jobs:
      for step in job.steps:
        key = '%s/%s' % (job.title, step.name)
        if step.state == RUNNING:
          self.first_seen.setdefault(key, now)
          self.done_seen.pop(key, None)
        elif step.state == DONE and key in self.first_seen:
          self.done_seen.setdefault(key, now)
        if key in self.first_seen and step.seconds is None:
          step.seconds = self.done_seen.get(key, now) - self.first_seen[key]
    self.jobs = jobs
    return jobs

  def _track(self, key: str, value: float) -> Rate:
    rate = self.rates[key]
    rate.add(value)
    return rate

  def _eta_text(self, key: str, done: float, total: float,
                fmt: Callable[[float], str] = lambda v: '%.1f' % v,
                unit: str = ' steps') -> str:
    rate = self._track(key, done)
    per = rate.per_second()
    eta = rate.eta(total - done)
    parts = []
    if per:
      parts.append('%s%s/s' % (fmt(per), unit))
    if eta is not None:
      parts.append('ETA %s' % duration(eta))
    return ' · '.join(parts)

  def _upload(self, gh: Proc, key: str, details: List) -> str:
    """Details of a running `gh release upload`; returns a short note."""
    files = upload_files(gh.args)
    total = sum(f.stat().st_size for f in files)
    sent = parse_nettop(run(['nettop', '-P', '-L', '1', '-J', 'bytes_out'],
                            timeout=10)).get(gh.pid)
    offsets = {os.path.realpath(path): offset
               for path, offset in parse_lsof_offsets(run(
                   ['lsof', '-o', '-F', 'on', '-p', str(gh.pid)],
                   timeout=10)).items()}
    note = '%d files, %s' % (len(files), size(total))
    details.append(('Upload to GitHub: %d files, %s' % (len(files),
                                                       size(total)), 'head'))
    if sent is not None and total:
      eta = self._eta_text(key, sent, total, size, '')
      details.append(('  %s %s of %s sent (%s)' % (
          bar(sent / total, 20), size(sent), size(total),
          percent(sent, total)), 'run'))
      if eta:
        details.append(('  %s' % eta, 'dim'))
      note = '%s/%s' % (size(sent), size(total))
    for f in files:
      read = offsets.get(os.path.realpath(f))
      length = f.stat().st_size
      state = ('read %s' % percent(read, length) if read is not None
               else 'waiting or sent')
      details.append(('  %-44s %9s  %s' % (f.name[:44], size(length), state),
                      'dim'))
    return note

  # The Mac ------------------------------------------------------------------

  def _mac(self, procs: Dict[int, Proc], siso: Dict, assets: Dict) -> Job:
    job = Job('Mac (this Mac)')
    job.log_name = str(self.mac_log)
    v = self.version
    text = ''
    try:
      text = self.mac_log.read_text(errors='replace')
      stat = self.mac_log.stat()
      began = getattr(stat, 'st_birthtime', stat.st_ctime)
    except OSError:
      stat, began = None, None
    # `release publish [<version>]`; without one, the log names it.
    py = next((p for p in procs.values()
               if re.search(r'compass release publish(?:\s|$)', p.args) and
               (v in p.args or v in text[:200])), None)
    kids = descendants(procs, py.pid) if py else []
    alive = py is not None
    ok = ('released %s' % v) in text

    def kid(pattern: str) -> Optional[Proc]:
      return next((k for k in kids if re.search(pattern, k.args)), None)

    builds = re.findall(r'autoninja\S* -C (\S+)', text)
    universal = (py and '--universal' in py.args) or 'universalizer' in text \
        or len(builds) > 1
    outs = [Path(b).name for b in builds]
    if universal:
      for cpu in ('Release-arm64', 'Release-x64'):
        if cpu not in outs:
          outs.append(cpu)
    if not outs:
      outs = ['Release-arm64']
    deltas_done = sorted((release.SITE_DIR / 'mac').glob('Compass-*-%s.delta'
                                                         % v))
    tmp = sorted((release.SITE_DIR / 'mac').glob('.Compass-*-%s.delta.tmp'
                                                 % v))
    expected = self._expected_deltas()
    symbols = sorted((SYMBOLS / v).glob('*.tar.gz'))

    names = ['build %s' % o.split('-', 1)[-1] for o in outs]
    started = [any(Path(b).name == o for b in builds) for o in outs]
    if universal:
      names.append('universal app')
      started.append('universalizer' in text)
    names += ['sign + notarize app', 'notarize dmg', 'deltas',
              'upload to GitHub', 'feeds', 'cask + symbols']
    dmg_submit = re.search(r"notarytool', 'submit', '[^']*\.dmg", text)
    started += ['signing as' in text, bool(dmg_submit),
                bool(deltas_done or tmp or kid('BinaryDelta')),
                bool(kid(r'gh release (upload|create)') or
                     '/releases/tag/%s' % release.github_tag(v) in text),
                bool(kid(r'\brsync\b')),
                bool(ok or symbols or 'Homebrew:' in text)]
    job.steps = [Step(n) for n in names]
    settle(job.steps, started, alive, ok and not alive)

    # Notes and details for each step.
    d = job.details
    d.append(('Compass %s%s' % (v, ' — ' + re.search(
        r'\((\w+) channel\)', text).group(1) + ' channel'
        if re.search(r'\((\w+) channel\)', text) else ''), 'head'))
    if not text and not alive:
      job.state = ABSENT
      job.note = 'no release-mac-%s.log' % v
      return job
    for i, out_name in enumerate(outs):
      step = job.steps[i]
      counts = siso.get(out_name)
      if step.state == RUNNING and counts:
        done, total = counts
        step.note = '%d/%d' % (done, total)
        eta = self._eta_text('siso/' + out_name, done, total)
        d.append(('%s: %d of %d steps, %d left' % (step.name, done, total,
                                                    total - done), 'run'))
        d.append(('  %s %s  %s' % (bar(done / total, 20),
                                   percent(done, total), eta), 'run'))
      elif step.state == RUNNING:
        step.note = 'starting'
    notary = job.steps[len(outs) + (1 if universal else 0)]
    polls = text.count("'notarytool', 'info'")
    if notary.state == RUNNING:
      notary.note = 'notary: %d checks' % polls if polls else 'signing'
    delta_step = next(s for s in job.steps if s.name == 'deltas')
    delta_step.note = '%d/%d' % (len(deltas_done), expected)
    if delta_step.state in (RUNNING, DONE) or deltas_done:
      d.append(('Deltas: %d of %d made' % (len(deltas_done), expected),
                'run' if delta_step.state == RUNNING else 'ok'))
      for f in deltas_done:
        d.append(('  ✔ %-44s %9s' % (f.name[:44], size(f.stat().st_size)),
                  'dim'))
      maker = kid('BinaryDelta')
      for t in tmp:
        grown = t.stat().st_size
        rate = self._track('delta/' + t.name, grown).per_second()
        d.append(('  ▶ %-44s %9s%s' % (t.name[1:-4][:44], size(grown),
                                       '  %s/s' % size(rate) if rate else ''),
                  'run'))
      if maker and not tmp:
        target = re.search(r'(Compass-\S+\.delta)', maker.args)
        d.append(('  ▶ %s (diffing, %s)' % (
            target.group(1) if target else 'delta', duration(maker.etime)),
                  'run'))
    upload_step = next(s for s in job.steps if s.name == 'upload to GitHub')
    gh = kid(r'gh release upload')
    if gh:
      upload_step.note = self._upload(gh, 'upload/mac', d)
    mac_assets = ['Compass-%s.dmg' % v] + [f.name for f in deltas_done]
    on_github = [a for a in mac_assets if assets.get(a, (0, ''))[1] ==
                 'uploaded']
    if assets:
      d.append(('GitHub: %d of %d Mac files uploaded' % (len(on_github),
                                                         len(mac_assets)),
                'ok' if len(on_github) == len(mac_assets) else 'dim'))
    rsync = kid(r'\brsync\b')
    if rsync:
      d.append(('Feeds: rsync %s' % rsync.args.split()[-2]
                if len(rsync.args.split()) > 2 else 'Feeds: rsync', 'run'))
    if symbols:
      d.append(('Symbols: %s' % ', '.join(
          '%s %s' % (s.name, size(s.stat().st_size)) for s in symbols),
                'dim'))
    for line in text.splitlines():
      if line.startswith(('released ', 'Homebrew: ', 'warning:', 'error:')):
        d.append((line, 'fail' if line.startswith('error') else 'ok'))
    job.state = overall(job.steps)
    if not alive and not ok:
      job.state = FAILED
      job.note = 'stopped'
    elif job.state == DONE:
      job.note = ''
    else:
      job.note = next((s.name for s in job.steps if s.state == RUNNING), '')
    if began:
      end = time.time() if alive else (stat.st_mtime if stat else time.time())
      job.seconds = end - began
    job.log = log_lines(tail(self.mac_log))
    return job

  def _expected_deltas(self) -> int:
    try:
      manifest = release.load()
    except (OSError, ValueError):
      return release.DELTA_COUNT
    key = release.version_key(self.version)
    earlier = [r for r in manifest.get('releases', [])
               if r.get('mac') and release.version_key(r['version']) < key]
    return min(release.DELTA_COUNT, len(earlier))

  # win1 ---------------------------------------------------------------------

  FARM = {
      'win-release-build': ('Windows build (win1)', [
          ('checkout', r'== checking out'),
          ('build', r'autoninja'),
          ('sign', r'Artifact Signing|Submitting digest'),
          ('artifacts', r'== artifacts'),
      ]),
      'linux': ('Linux build (win1)', [
          ('checkout', r'== checking out'),
          ('gn gen', r'gn\.bat gen'),
          ('build', r'autoninja'),
          ('tarball', r'linux-x64\.tar\.xz'),
          ('flatpak', r'flatpak/build\.sh'),
          ('artifacts', r'== artifacts'),
      ]),
  }

  def _farm(self, win: Optional[Dict]) -> List[Job]:
    jobs = []
    for recipe, (title, phases) in self.FARM.items():
      job = Job(title)
      job.steps = [Step(name) for name, _ in phases]
      found = pick_job(win['jobs'], recipe, self.version,
                         self.next_version) if win else None
      if win is None:
        job.state = WAITING
        job.note = 'asking win1…' if not self.win_error else 'win1 unreachable'
        if self.win_error:
          job.details.append(('win1 did not answer (ssh %s)' % WIN_HOST,
                              'fail'))
        jobs.append(job)
        continue
      if not found:
        job.state = ABSENT
        job.note = 'not queued'
        job.details.append(('No %s job on win1 since %s' % (
            recipe, self.version), 'dim'))
        jobs.append(job)
        continue
      log = found['log']
      status = found.get('status') or ''
      alive = status in ('running', 'queued')
      ok = status == 'ok'
      started = [bool(re.search(pattern, log)) for _, pattern in phases]
      # The tail may have lost the early markers: a later one implies them.
      settle(job.steps, started, alive, ok)
      if status == 'queued':
        for s in job.steps:
          s.state = WAITING
      job.log_name = '%s:farm/jobs/%s/log' % (WIN_HOST, found['id'])
      job.log = log_lines(log)
      d = job.details
      d.append(('%s — %s' % (found['id'], status), 'head'))
      ninja = parse_ninja(log)
      build = next(s for s in job.steps if s.name == 'build')
      if ninja and build.state in (RUNNING, DONE):
        done, total, secs = ninja
        build.note = ('%d/%d' % (done, total) if build.state == RUNNING
                      else '%d steps' % total)
        if build.state == RUNNING:
          eta = self._eta_text('ninja/' + found['id'], done, total)
          if not eta and done:
            eta = 'ETA %s (average)' % duration(secs / done * (total - done))
          d.append(('Build: %d of %d steps, %d left, %s in' % (
              done, total, total - done, duration(secs)), 'run'))
          d.append(('  %s %s  %s' % (bar(done / total, 20),
                                     percent(done, total), eta), 'run'))
      for a in found.get('artifacts') or []:
        d.append(('  %-44s %9s' % (a['name'][:44], size(a['size'])), 'dim'))
      start = float(found['started']) if found.get('started') else None
      end = float(found['finished']) if found.get('finished') else time.time()
      job.seconds = end - start if start else None
      job.state = {'ok': DONE, 'queued': WAITING,
                   'running': RUNNING}.get(status, FAILED)
      if job.state == FAILED:
        for s in job.steps:
          if s.state == RUNNING:
            s.state = FAILED
      job.note = ('queued' if status == 'queued' else
                  next((s.name for s in job.steps if s.state == RUNNING), '')
                  if alive else ('' if ok else status))
      jobs.append(job)
    return jobs

  # Publishing from this Mac -------------------------------------------------

  def _publish(self, title: str, procs: Dict[int, Proc], action: str,
               copy_name: str, local: List[Path], remote: List[Dict],
               copier: str, assets_wanted: List[str], assets: Dict,
               done_line: str, extra: List[Tuple[str, str]]) -> Job:
    job = Job(title)
    v = self.version
    py = next((p for p in procs.values()
               if 'release %s' % action in p.args and
               (v in p.args or v in ''.join(str(x) for x in local))), None)
    kids = descendants(procs, py.pid) if py else []
    copying = next((p for p in procs.values() if re.search(copier, p.args)),
                   None)
    logs = sorted(self.out.glob('*%s*%s*.log' % (action.split('-')[1], v)))
    text = logs[-1].read_text(errors='replace') if logs else ''
    have = [p for p in local if p.exists()]
    uploaded = [a for a in assets_wanted
                if assets.get(a, (0, ''))[1] == 'uploaded']
    finished_ok = done_line in text or (
        not py and len(uploaded) == len(assets_wanted) and assets_wanted)
    job.steps = [Step(copy_name), Step('upload to GitHub')] + [
        Step(name) for name, _ in extra]
    started = [bool(have or copying), bool(py and (
        any('gh release' in k.args for k in kids) or uploaded))]
    for name, pattern in extra:
      started.append(bool(re.search(pattern, text)) or
                     any(re.search(pattern, k.args) for k in kids))
    alive = bool(py or copying)
    settle(job.steps, started, alive, bool(finished_ok))
    # Copied but not published yet: waiting on the publish, not failed.
    if not py and not finished_ok:
      if have and not copying:
        job.steps[0].state = DONE
        for s in job.steps[1:]:
          s.state = WAITING
    d = job.details
    d.append(('%s %s' % (title, v), 'head'))
    total_remote = sum(r['size'] for r in remote if r.get('size', -1) > 0)
    total_local = sum(p.stat().st_size for p in have if p.is_file())
    if remote and job.steps[0].state != DONE and total_remote:
      job.steps[0].note = '%s/%s' % (size(total_local), size(total_remote))
      eta = self._eta_text('copy/' + title, total_local, total_remote, size,
                           '')
      d.append(('Copy to this Mac: %s of %s %s' % (
          size(total_local), size(total_remote), eta), 'run'))
      d.append(('  %s %s' % (bar(total_local / total_remote, 20),
                             percent(total_local, total_remote)), 'run'))
    for p in local:
      d.append(('  %s %-44s %9s' % ('✔' if p.exists() else '·', p.name[:44],
                                   size(p.stat().st_size) if p.is_file()
                                   else ('dir' if p.exists() else '')),
                'dim'))
    gh = next((k for k in kids if 'gh release upload' in k.args), None)
    if gh:
      job.steps[1].note = self._upload(gh, 'upload/' + title, d)
    if assets_wanted:
      job.steps[1].note = job.steps[1].note or '%d/%d' % (
          len(uploaded), len(assets_wanted))
      d.append(('GitHub: %d of %d files uploaded' % (len(uploaded),
                                                     len(assets_wanted)),
                'ok' if len(uploaded) == len(assets_wanted) else 'dim'))
    for line in text.splitlines():
      if line.startswith(('released ', 'published ', 'the Flatpak',
                          'warning:', 'error:')):
        d.append((line, 'fail' if line.startswith('error') else 'ok'))
    job.state = overall(job.steps)
    if job.state == RUNNING and not alive and not finished_ok:
      job.state = WAITING
    job.note = ('' if job.state == DONE else
                next((s.name for s in job.steps if s.state == RUNNING),
                     'to publish' if have else ''))
    if py:
      job.seconds = py.etime
    if logs:
      job.log_name = str(logs[-1])
      job.log = log_lines(tail(logs[-1]))
    return job

  def _publish_win(self, procs: Dict[int, Proc], win: Optional[Dict],
                   assets: Dict) -> Job:
    v = self.version
    found = pick_job(win['jobs'], 'win-release-build', v,
                     self.next_version) if win else None
    folder = BUILDS / (found['id'] if found else 'compass-win-release-build')
    names = ['CompassSetup-%s.exe' % v, 'CompassSetup-%s.json' % v,
             release.win_installer_name(v)]
    return self._publish(
        'Windows publish', procs, 'publish-win', 'fetch',
        [folder / n for n in names],
        (found or {}).get('artifacts') or [],
        r'tar -xf - -C \S*%s' % (found['id'] if found else 'no-job'),
        ['CompassSetup-%s.exe' % v, release.win_installer_name(v)], assets,
        'released %s for Windows' % v, [('feeds', r'\brsync\b')])

  def _publish_linux(self, procs: Dict[int, Proc], win: Optional[Dict],
                     assets: Dict) -> Job:
    v = self.version
    folder = BUILDS / ('linux-%s' % v)
    base = 'Compass-%s-linux-x64' % v
    names = [base + ext for ext in ('.tar.xz', '.json', '.flatpak', '.oci')]
    return self._publish(
        'Linux publish', procs, 'publish-linux', 'copy',
        [folder / n for n in names], (win or {}).get('linux') or [],
        r'scp .*%s' % re.escape(base), [base + '.tar.xz', base + '.flatpak'],
        assets, 'published %s for Linux' % v,
        [('Flatpak remote', r'the Flatpak remote')])


# The view -------------------------------------------------------------------

COLORS = {'run': 1, 'ok': 2, 'fail': 3, 'dim': 4, 'head': 5, 'sel': 6,
          'warn': 7}
STATE_COLOR = {RUNNING: 'run', DONE: 'ok', FAILED: 'fail', WAITING: None,
               ABSENT: 'dim'}


def _init_colors() -> None:
  curses.start_color()
  try:
    curses.use_default_colors()
    bg = -1
  except curses.error:
    bg = curses.COLOR_BLACK
  bright = 8 if curses.COLORS >= 16 else 0
  curses.init_pair(1, curses.COLOR_CYAN + bright, bg)
  curses.init_pair(2, curses.COLOR_GREEN + bright, bg)
  curses.init_pair(3, curses.COLOR_RED + bright, bg)
  curses.init_pair(4, 8 if curses.COLORS >= 16 else curses.COLOR_WHITE, bg)
  curses.init_pair(5, curses.COLOR_CYAN + bright, bg)
  curses.init_pair(6, curses.COLOR_BLACK, curses.COLOR_CYAN + bright)
  curses.init_pair(7, curses.COLOR_YELLOW + bright, bg)


def _attr(role: Optional[str]) -> int:
  if not role or not curses.has_colors():
    return curses.A_NORMAL
  attr = curses.color_pair(COLORS[role])
  if role == 'head':
    attr |= curses.A_BOLD
  return attr


def fit(left: str, right: str, width: int) -> str:
  """|left| cut to leave room for |right|, which ends at the edge."""
  if width <= 0:
    return ''
  if len(right) >= width:
    return right[-width:]
  room = width - len(right)
  if len(left) > room:
    left = left[:max(0, room - 1)] + '…'
  return left.ljust(room) + right


def rows(jobs: List[Job]) -> List[Tuple[int, Optional[Step]]]:
  out = []
  for i, job in enumerate(jobs):
    out.append((i, None))
    out.extend((i, s) for s in job.steps)
  return out


def header(monitor: 'Monitor', jobs: List[Job]) -> str:
  count = collections.Counter(j.state for j in jobs)
  total = max((j.seconds or 0) for j in jobs) if jobs else 0
  text = ' Compass %s   %s   %d running · %d done · %d waiting · %d failed' % (
      monitor.version, duration(total), count[RUNNING], count[DONE],
      count[WAITING] + count[ABSENT], count[FAILED])
  if all(j.state == DONE for j in jobs):
    text = ' Compass %s released on every platform   %s' % (monitor.version,
                                                           duration(total))
  return text


def plain(jobs: List[Job], monitor: 'Monitor', details: bool = True) -> str:
  """The view as text, for --once and for tests."""
  lines = [header(monitor, jobs).strip(), '']
  for job in jobs:
    lines.append('%s %-28s %-24s %8s' % (GLYPH[job.state], job.title,
                                          job.note[:24],
                                          duration(job.seconds)))
    for step in job.steps:
      lines.append('    %s %-24s %-24s %8s' % (
          '›' if step.state == RUNNING else GLYPH[step.state], step.name,
          step.note[:24], duration(step.seconds)))
    if details:
      for text, _ in job.details:
        lines.append('      ' + text)
  return '\n'.join(lines)


class View:

  def __init__(self, monitor: Monitor):
    self.monitor = monitor
    self.selected = 0
    self.log_top: Optional[int] = None  # None follows the end
    self.spin = 0

  def run(self, screen) -> None:
    curses.curs_set(0)
    # Plain redraws: no scroll regions or line insertion, which some
    # terminals (and terminal emulators in tests) get wrong.
    screen.idlok(False)
    screen.scrollok(False)
    if curses.has_colors():
      _init_colors()
    screen.timeout(250)
    screen.keypad(True)
    jobs = self.monitor.refresh()
    last = time.time()
    while True:
      if time.time() - last >= 3:
        jobs = self.monitor.refresh()
        last = time.time()
      self.spin += 1
      # Moving the selection shifts whole lines, which ncurses would scroll
      # into place; a full repaint draws the same thing everywhere.
      view = (self.selected, self.log_top)
      if view != getattr(self, '_last_view', view):
        screen.clear()
      self._last_view = view
      self.draw(screen, jobs)
      key = self._key(screen)
      if key in (ord('q'), ord('Q')):
        return
      if key in (curses.KEY_UP, ord('k')):
        self.selected = max(0, self.selected - 1)
        self.log_top = None
      elif key in (curses.KEY_DOWN, ord('j')):
        self.selected = min(len(jobs) - 1, self.selected + 1)
        self.log_top = None
      elif key == curses.KEY_PPAGE:
        self.log_top = max(0, (self.log_top if self.log_top is not None
                               else 10 ** 6) - 10)
      elif key == curses.KEY_NPAGE and self.log_top is not None:
        self.log_top += 10
      elif key in (curses.KEY_END, ord('f')):
        self.log_top = None
      elif key == ord('r'):
        jobs = self.monitor.refresh()
        last = time.time()

  ESCAPES = {'[A': curses.KEY_UP, 'OA': curses.KEY_UP,
             '[B': curses.KEY_DOWN, 'OB': curses.KEY_DOWN,
             '[5~': curses.KEY_PPAGE, '[6~': curses.KEY_NPAGE,
             '[F': curses.KEY_END, 'OF': curses.KEY_END,
             '[4~': curses.KEY_END} if curses else {}

  def _key(self, screen) -> int:
    """A key, with the arrow and page keys curses' terminfo didn't decode
    (a terminal in the other cursor-key mode) decoded here. Esc alone does
    nothing."""
    key = screen.getch()
    if key != 27:
      return key
    screen.nodelay(True)
    sequence = ''
    try:
      while len(sequence) < 4:
        ch = screen.getch()
        if ch < 0:
          break
        sequence += chr(ch)
        if sequence in self.ESCAPES:
          return self.ESCAPES[sequence]
    finally:
      screen.nodelay(False)
      screen.timeout(250)
    return -1

  def draw(self, screen, jobs: List[Job]) -> None:
    height, width = screen.getmaxyx()
    screen.erase()
    if height < 8 or width < 40:
      self._put(screen, 0, 0, 'Terminal too small', width, 'fail')
      screen.refresh()
      return
    self._put(screen, 0, 0, header(self.monitor, jobs), width, 'head')
    left = max(34, min(60, width * 2 // 5))
    body = height - 2
    self._jobs(screen, jobs, 1, 0, body, left)
    for y in range(1, height - 1):
      self._put(screen, y, left, '│', 1, 'dim')
    job = jobs[min(self.selected, len(jobs) - 1)]
    self._details(screen, job, 1, left + 2, body, width - left - 2)
    win_age = (time.time() - self.monitor.win_checked
               if self.monitor.win_checked else None)
    footer = ' ↑↓ a job · PgUp/PgDn its log · End follows · r refresh · q quit'
    footer = fit(footer, 'win1 %s ago ' % duration(win_age)
                 if win_age is not None else 'win1 … ', width)
    self._put(screen, height - 1, 0, footer, width, 'dim')
    screen.refresh()

  def _put(self, screen, y: int, x: int, text: str, width: int,
           role: Optional[str] = None, attr: Optional[int] = None) -> None:
    height, cols = screen.getmaxyx()
    width = min(width, cols - x - (1 if y == height - 1 else 0))
    if width <= 0 or y >= height:
      return
    try:
      screen.addstr(y, x, text[:width], _attr(role) if attr is None else attr)
    except curses.error:
      pass

  def _jobs(self, screen, jobs: List[Job], y0: int, x0: int, height: int,
            width: int) -> None:
    all_rows = rows(jobs)
    # Scrolled to keep the selected job, and as many of its steps as fit,
    # in view.
    first = next(i for i, (j, s) in enumerate(all_rows)
                 if j == self.selected and s is None)
    last = first + len(jobs[self.selected].steps)
    top = 0
    if last >= height:
      top = min(first, last - height + 1)
    for y, (index, step) in enumerate(all_rows[top:top + height]):
      job = jobs[index]
      if step is None:
        glyph = (SPINNER[self.spin % len(SPINNER)] if job.state == RUNNING
                 else GLYPH[job.state])
        left = ' %s %s' % (glyph, job.title)
        right = '%s  %7s ' % (job.note[:18], duration(job.seconds))
        role = STATE_COLOR[job.state]
        if index == self.selected:
          self._put(screen, y0 + y, x0, fit(left, right, width), width,
                    attr=_attr('sel'))
          continue
      else:
        mark = {RUNNING: '›', DONE: '✔', FAILED: '✖'}.get(step.state, ' ')
        left = '    %s %s' % (mark, step.name)
        right = '%s  %7s ' % (step.note[:18], duration(step.seconds))
        role = {RUNNING: 'run', FAILED: 'fail', DONE: 'dim'}.get(step.state,
                                                                  'dim')
      self._put(screen, y0 + y, x0, fit(left, right, width), width, role)

  def _details(self, screen, job: Job, y0: int, x0: int, height: int,
               width: int) -> None:
    lines = list(job.details)
    y = 0
    for text, role in lines[:max(0, height - 4)]:
      self._put(screen, y0 + y, x0, text, width, role)
      y += 1
    y += 1
    title = '── Log: %s ' % job.log_name if job.log_name else '── No log '
    self._put(screen, y0 + y, x0, title.ljust(width, '─'), width, 'dim')
    y += 1
    room = height - y
    if room <= 0:
      return
    log = job.log
    top = (max(0, len(log) - room) if self.log_top is None
           else min(self.log_top, max(0, len(log) - room)))
    for i, line in enumerate(log[top:top + room]):
      role = ('fail' if re.search(r'\berror\b|FAILED|Traceback', line, re.I)
              else 'warn' if 'warning' in line.lower()
              else 'head' if line.startswith(('== ', '$ ')) else None)
      self._put(screen, y0 + y + i, x0, line, width, role)
    if self.log_top is not None and top + room < len(log):
      mark = ' ↓ %d more · End follows ' % (len(log) - top - room)
      self._put(screen, y0 + height - 1, x0 + max(0, width - len(mark)), mark,
                len(mark), 'warn')


def main(version: Optional[str], out: Path, depot_tools: Path,
         once: bool = False, wait: float = 0) -> str:
  version = version or latest_version(out)
  if not version:
    raise ValueError('no release-mac-*.log in %s: which version? '
                     '(compass monitor <version>)' % out)
  if not once and not curses:
    raise ValueError('the live view needs curses, which this Python lacks '
                     '(Windows): use --once')
  monitor = Monitor(version, out, depot_tools)
  if once:
    # The slow sources once, in the foreground.
    for source in (monitor._poll_win, monitor._poll_assets,
                   monitor._poll_siso):
      try:
        source()
      except Exception:
        pass
    monitor.refresh()
    if wait:
      time.sleep(wait)
    return plain(monitor.refresh(), monitor)
  monitor.start()
  try:
    curses.wrapper(View(monitor).run)
  finally:
    monitor.stop()
  return ''
