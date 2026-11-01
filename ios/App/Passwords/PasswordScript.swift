// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import WebKit

/// The page side of password filling and saving, in an isolated content
/// world so pages can't see or call it.
@MainActor
enum PasswordScript {
  static let world = WKContentWorld.world(name: "CompassPasswords")
  static let handler = "compassPasswords"

  static var userScript: WKUserScript {
    WKUserScript(source: source, injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: world)
  }

  /// Fills the first login form on the page.
  static func fill(username: String, password: String) -> String {
    let data = try! JSONSerialization.data(withJSONObject: [username, password])
    let args = String(decoding: data, as: UTF8.self)
    return "window.__compassFill && window.__compassFill(...\(args));"
  }

  static let source = #"""
  (() => {
    const post = (m) => window.webkit.messageHandlers.compassPasswords.postMessage(m);
    const visible = (e) => !!(e.offsetWidth || e.offsetHeight || e.getClientRects().length);
    const passwordFields = () => [...document.querySelectorAll('input[type=password]')].filter(visible);
    // The text field before a password field is its username.
    const usernameFor = (pw) => {
      const inputs = [...(pw.form || document).querySelectorAll('input')].filter(visible);
      const i = inputs.indexOf(pw);
      for (let j = i - 1; j >= 0; j--) {
        const t = (inputs[j].type || 'text').toLowerCase();
        if (['text', 'email', 'tel', ''].includes(t)) return inputs[j];
      }
      return null;
    };
    const name = (e) => (e && (e.name || e.id)) || '';
    const setValue = (e, v) => {
      const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
      setter.call(e, v);
      e.dispatchEvent(new Event('input', { bubbles: true }));
      e.dispatchEvent(new Event('change', { bubbles: true }));
    };
    window.__compassFill = (username, password) => {
      const pw = passwordFields()[0];
      if (!pw) return false;
      const user = usernameFor(pw);
      if (user && username) setValue(user, username);
      setValue(pw, password);
      return true;
    };
    let last = '';
    const capture = () => {
      const pw = passwordFields().find((p) => p.value);
      if (!pw) return;
      const user = usernameFor(pw);
      const form = pw.form;
      const message = {
        type: 'submit',
        username: user ? user.value : '',
        password: pw.value,
        usernameElement: name(user),
        passwordElement: name(pw),
        action: form && form.action ? form.action : '',
      };
      const key = JSON.stringify(message);
      if (key === last) return;
      last = key;
      post(message);
    };
    document.addEventListener('submit', capture, true);
    document.addEventListener('click', (e) => {
      const b = e.target.closest && e.target.closest('button, input[type=submit]');
      if (b) setTimeout(capture, 0);
    }, true);
    document.addEventListener('keydown', (e) => { if (e.key === 'Enter') setTimeout(capture, 0); }, true);
    window.addEventListener('pagehide', capture);
    const report = () => { if (passwordFields().length) post({ type: 'form' }); };
    report();
    new MutationObserver(() => { report(); }).observe(document.documentElement, { childList: true, subtree: true });
  })();
  """#
}
