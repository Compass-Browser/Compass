#!/usr/bin/env python3
# Copyright 2026 The Compass Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Runs JavaScript in a page of a browser started with --remote-debugging-port.

Sync end-to-end helper (docs/design/sync-slice-2-steps.md; started for the
slice 0 spike). Standard library only, so it runs on a bare macOS Python 3.

  cdp.py PORT wait                    wait until the browser answers (90 s)
  cdp.py PORT run URL FILE.js [--settle S] [--arg NAME=VALUE]...
      opens URL in a new background tab, waits for it to load (+S seconds,
      default 3), runs the file's JavaScript as the body of an async function
      (with the --arg values in a constant ARGS), prints the returned value
      as JSON, closes the tab. Exit status 1 on a JS exception.
  cdp.py PORT in MATCH FILE.js [--arg NAME=VALUE]...
      runs the file's JavaScript (as for run) in a page that is already open
      and whose URL contains MATCH, such as a window's sidebar
      (compass-sidebar), found in the browser's target list; leaves it open.
  cdp.py PORT shot URL OUT.png [--settle S] [--size WxH]
      opens URL in a new background tab at WxH (default 900x1400), waits
      for it (+S seconds, default 3), saves a PNG of the whole page with
      Page.captureScreenshot, closes the tab. Never focuses anything. The
      page's colors follow the browser's theme (chrome://theme/colors.css),
      so a dark screenshot needs an instance started with --force-dark-mode.
  cdp.py - waitlog FILE REGEX [TIMEOUT]
      waits (default 180 s) until a line of FILE matches REGEX, prints it.
      Exit status 1 on timeout. (No browser needed; PORT is ignored.)
"""

import base64
import json
import os
import re
import socket
import struct
import sys
import time
import urllib.parse
import urllib.request


def http(port, path, method='GET'):
  req = urllib.request.Request('http://127.0.0.1:%d%s' % (port, path),
                               method=method)
  with urllib.request.urlopen(req, timeout=10) as r:
    body = r.read().decode()
  return json.loads(body) if body.strip().startswith(('{', '[')) else body


def wait(port, timeout=90):
  deadline = time.time() + timeout
  while True:
    try:
      return http(port, '/json/version')
    except OSError:
      if time.time() > deadline:
        raise
      time.sleep(1)


class WebSocket:
  """Just enough of RFC 6455 for CDP: unfragmented text frames."""

  def __init__(self, url):
    u = urllib.parse.urlparse(url)
    self.sock = socket.create_connection((u.hostname, u.port), timeout=120)
    key = base64.b64encode(os.urandom(16)).decode()
    self.sock.sendall(('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n'
                       'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
                       'Sec-WebSocket-Version: 13\r\n\r\n' %
                       (u.path, u.hostname, u.port, key)).encode())
    head = b''
    while b'\r\n\r\n' not in head:
      chunk = self.sock.recv(1)
      if not chunk:
        raise ConnectionError('websocket handshake failed')
      head += chunk
    if b' 101 ' not in head.split(b'\r\n', 1)[0]:
      raise ConnectionError('websocket handshake: ' + head.decode(errors='replace'))

  def _recv_exact(self, n):
    data = b''
    while len(data) < n:
      chunk = self.sock.recv(n - len(data))
      if not chunk:
        raise ConnectionError('websocket closed')
      data += chunk
    return data

  def send(self, text):
    payload = text.encode()
    header = bytes([0x81])
    n = len(payload)
    if n < 126:
      header += bytes([0x80 | n])
    elif n < 65536:
      header += bytes([0x80 | 126]) + struct.pack('>H', n)
    else:
      header += bytes([0x80 | 127]) + struct.pack('>Q', n)
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    self.sock.sendall(header + mask + masked)

  def recv(self):
    message = b''
    while True:
      b0, b1 = self._recv_exact(2)
      n = b1 & 0x7f
      if n == 126:
        n = struct.unpack('>H', self._recv_exact(2))[0]
      elif n == 127:
        n = struct.unpack('>Q', self._recv_exact(8))[0]
      if b1 & 0x80:
        self._recv_exact(4)  # Servers don't mask; skip if they do.
      data = self._recv_exact(n)
      opcode = b0 & 0x0f
      if opcode == 0x8:
        raise ConnectionError('websocket closed by browser')
      if opcode in (0x9, 0xa):
        continue  # ping/pong
      message += data
      if b0 & 0x80:
        return message.decode()


class Session:

  def __init__(self, ws_url):
    self.ws = WebSocket(ws_url)
    self.next_id = 0

  def call(self, method, session_id=None, **params):
    self.next_id += 1
    message = {'id': self.next_id, 'method': method, 'params': params}
    if session_id:
      message['sessionId'] = session_id
    self.ws.send(json.dumps(message))
    while True:
      msg = json.loads(self.ws.recv())
      if (msg.get('id') == self.next_id and
          msg.get('sessionId') == session_id):
        if 'error' in msg:
          raise RuntimeError('%s: %s' % (method, msg['error']))
        return msg['result']


def evaluate(port, url, js, settle):
  target = http(port, '/json/new?' + url, method='PUT')
  try:
    s = Session(target['webSocketDebuggerUrl'])
    deadline = time.time() + 60
    while True:
      r = s.call('Runtime.evaluate', expression='document.readyState',
                 returnByValue=True)
      if r['result'].get('value') == 'complete':
        break
      if time.time() > deadline:
        raise TimeoutError('page did not load: ' + url)
      time.sleep(0.5)
    time.sleep(settle)
    r = s.call('Runtime.evaluate',
               expression='(async () => {\n%s\n})()' % js,
               awaitPromise=True, returnByValue=True)
    if 'exceptionDetails' in r:
      print(json.dumps(r['exceptionDetails'], indent=1), file=sys.stderr)
      return 1
    print(json.dumps(r['result'].get('value'), indent=1))
    return 0
  finally:
    try:
      http(port, '/json/close/' + target['id'])
    except OSError:
      pass


def screenshot(port, url, out, settle, width, height):
  target = http(port, '/json/new?' + url, method='PUT')
  try:
    s = Session(target['webSocketDebuggerUrl'])
    s.call('Emulation.setDeviceMetricsOverride', width=width, height=height,
           deviceScaleFactor=2, mobile=False)
    deadline = time.time() + 60
    while True:
      r = s.call('Runtime.evaluate', expression='document.readyState',
                 returnByValue=True)
      if r['result'].get('value') == 'complete':
        break
      if time.time() > deadline:
        raise TimeoutError('page did not load: ' + url)
      time.sleep(0.5)
    time.sleep(settle)
    r = s.call('Page.captureScreenshot', format='png',
               captureBeyondViewport=True)
    with open(out, 'wb') as f:
      f.write(base64.b64decode(r['data']))
    print(out)
    return 0
  finally:
    try:
      http(port, '/json/close/' + target['id'])
    except OSError:
      pass


def evaluate_in(port, match, js):
  browser = http(port, '/json/version')
  s = Session(browser['webSocketDebuggerUrl'])
  deadline = time.time() + 60
  while True:
    targets = s.call('Target.getTargets')['targetInfos']
    found = [t for t in targets if match in t.get('url', '')]
    if found:
      break
    if time.time() > deadline:
      raise TimeoutError('no open page matches ' + match + ': ' +
                         ', '.join(t.get('url', '') for t in targets))
    time.sleep(1)
  session_id = s.call('Target.attachToTarget', targetId=found[0]['targetId'],
                      flatten=True)['sessionId']
  r = s.call('Runtime.evaluate', session_id=session_id,
             expression='(async () => {\n%s\n})()' % js,
             awaitPromise=True, returnByValue=True)
  if 'exceptionDetails' in r:
    print(json.dumps(r['exceptionDetails'], indent=1), file=sys.stderr)
    return 1
  print(json.dumps(r['result'].get('value'), indent=1))
  return 0


def waitlog(path, pattern, timeout):
  regex = re.compile(pattern)
  deadline = time.time() + timeout
  while True:
    try:
      with open(path, errors='replace') as f:
        for line in f:
          if regex.search(line):
            print(line.rstrip())
            return 0
    except FileNotFoundError:
      pass
    if time.time() > deadline:
      print('timed out waiting for /%s/ in %s' % (pattern, path),
            file=sys.stderr)
      return 1
    time.sleep(2)


def main(argv):
  if len(argv) >= 3 and argv[1] == 'waitlog':
    timeout = float(argv[4]) if len(argv) >= 5 else 180
    return waitlog(argv[2], argv[3], timeout)
  if len(argv) >= 2 and argv[1] == 'wait':
    print(json.dumps(wait(int(argv[0])), indent=1))
    return 0
  if len(argv) >= 4 and argv[1] == 'shot':
    settle, width, height = 3.0, 900, 1400
    rest = argv[4:]
    while rest:
      flag = rest.pop(0)
      if flag == '--settle' and rest:
        settle = float(rest.pop(0))
      elif flag == '--size' and rest:
        width, _, height = rest.pop(0).partition('x')
        width, height = int(width), int(height)
      else:
        print('unknown option: ' + flag, file=sys.stderr)
        return 2
    return screenshot(int(argv[0]), argv[2], argv[3], settle, width, height)
  if len(argv) >= 4 and argv[1] in ('run', 'in'):
    settle = 3.0
    args = {}
    rest = argv[4:]
    while rest:
      flag = rest.pop(0)
      if flag == '--settle' and rest:
        settle = float(rest.pop(0))
      elif flag == '--arg' and rest:
        name, _, value = rest.pop(0).partition('=')
        args[name] = value
      else:
        print('unknown option: ' + flag, file=sys.stderr)
        return 2
    with open(argv[3]) as f:
      js = 'const ARGS = %s;\n%s' % (json.dumps(args), f.read())
    if argv[1] == 'in':
      return evaluate_in(int(argv[0]), argv[2], js)
    return evaluate(int(argv[0]), argv[2], js, settle)
  print(__doc__, file=sys.stderr)
  return 2


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
