// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The reader view's script (docs/design/reader-mode.md), shared by desktop
// and iOS. Loaded by reader.html after reader.js.
//
// The article comes from either
//   - the page's URL, `?article=<url>` of a JSON file to fetch (desktop:
//     chrome-untrusted://compass-reader/?article=article/<token>), or
//   - the host calling compassReaderView.render(article) (iOS).
//
// The host may define globalThis.compassReaderHost before this script runs:
//   loadSettings() -> settings object or null
//   saveSettings(settings)
//   close()             leave Reader (default: window.close())
// Without one, the settings live in this origin's localStorage, which is
// per profile.
//
// The article's HTML is sanitized again here (reader.js's sanitize()), and
// the page's Content Security Policy runs no script but these.

(() => {
  'use strict';

  const kSettingsKey = 'compass.reader.settings';
  const kSizes = [14, 15, 17, 19, 21, 24, 28, 32];
  const kDefaults = {font: 'serif', size: 19, width: 'medium', theme: 'auto'};
  const kChoices = {
    font: ['serif', 'sans'],
    width: ['narrow', 'medium', 'wide'],
    theme: ['auto', 'light', 'sepia', 'dark'],
  };
  const kWordsPerMinute = 230;

  const host = globalThis.compassReaderHost || {};
  const $ = (id) => document.getElementById(id);
  const root = document.documentElement;

  function validSettings(value) {
    const settings = {...kDefaults};
    if (!value || typeof value !== 'object') {
      return settings;
    }
    for (const key of Object.keys(kChoices)) {
      if (kChoices[key].includes(value[key])) {
        settings[key] = value[key];
      }
    }
    if (kSizes.includes(value.size)) {
      settings.size = value.size;
    }
    return settings;
  }

  function loadSettings() {
    try {
      if (host.loadSettings) {
        return validSettings(host.loadSettings());
      }
      return validSettings(JSON.parse(localStorage.getItem(kSettingsKey)));
    } catch (e) {
      return validSettings(null);
    }
  }

  function saveSettings(settings) {
    try {
      if (host.saveSettings) {
        host.saveSettings(settings);
      } else {
        localStorage.setItem(kSettingsKey, JSON.stringify(settings));
      }
    } catch (e) {
      // Settings are a convenience: Reader works without them.
    }
  }

  let settings = loadSettings();

  function applySettings() {
    root.dataset.theme = settings.theme;
    root.dataset.font = settings.font;
    root.dataset.width = settings.width;
    root.style.setProperty('--size', settings.size + 'px');
    for (const button of document.querySelectorAll('[data-setting]')) {
      const on = settings[button.dataset.setting] === button.dataset.value;
      button.setAttribute('role', 'radio');
      button.setAttribute('aria-checked', String(on));
    }
    $('size-label').textContent = settings.size + ' px';
    $('smaller').disabled = settings.size === kSizes[0];
    $('larger').disabled = settings.size === kSizes[kSizes.length - 1];
  }

  function change(key, value) {
    settings = validSettings({...settings, [key]: value});
    applySettings();
    saveSettings(settings);
  }

  function stepSize(step) {
    const index = kSizes.indexOf(settings.size);
    const next = kSizes[Math.max(0, Math.min(kSizes.length - 1,
                                             index + step))];
    change('size', next);
  }

  function readingTime(text) {
    const words = (text.match(/\S+/g) || []).length;
    const minutes = Math.max(1, Math.round(words / kWordsPerMinute));
    return minutes + ' min read';
  }

  function hostOf(url) {
    try {
      return new URL(url).hostname.replace(/^www\./, '');
    } catch (e) {
      return '';
    }
  }

  function showError(message) {
    $('page').hidden = true;
    $('error').hidden = false;
    $('error').textContent = message;
    document.body.dataset.state = 'error';
  }

  // Shows |article|, reader.js parse()'s result.
  function render(article) {
    stopListening();
    if (!article || !article.ok) {
      showError(article && article.error || 'This page has no article.');
      return;
    }
    const url = typeof article.url === 'string' ? article.url : '';
    const safeOriginal = /^https?:/i.test(url) ? url : '';
    document.title = article.title || hostOf(url) || 'Reader';
    root.lang = article.lang || '';
    root.dir = article.dir === 'rtl' ? 'rtl' : 'ltr';
    $('site').textContent = article.siteName || hostOf(url);
    $('title').textContent = article.title || '';
    $('byline').textContent = article.byline || '';

    const html = globalThis.__compassReader.sanitize(article.content || '',
                                                    safeOriginal || undefined);
    $('content').innerHTML = html;
    $('meta').textContent = readingTime($('content').textContent);
    if (safeOriginal) {
      $('original').href = safeOriginal;
      $('original').textContent = 'Original page · ' + hostOf(url);
    } else {
      $('original').removeAttribute('href');
    }
    $('page').hidden = false;
    $('error').hidden = true;
    document.body.dataset.state = 'ready';
    updateListen();
  }

  // Read aloud with the system's own voices (localService), so the text
  // never leaves the device.
  let speaking = false;

  function localVoice() {
    if (!globalThis.speechSynthesis) {
      return null;
    }
    const voices = speechSynthesis.getVoices().filter((v) => v.localService);
    const lang = (root.lang || 'en').toLowerCase();
    const base = lang.split('-')[0];
    return voices.find((v) => v.lang.toLowerCase() === lang) ||
        voices.find((v) => v.lang.toLowerCase().split(/[-_]/)[0] === base) ||
        null;
  }

  function updateListen() {
    const button = $('listen');
    button.hidden = !localVoice();
    button.setAttribute('aria-pressed', String(speaking));
    button.title = speaking ? 'Stop' : 'Listen';
    button.setAttribute('aria-label', button.title);
  }

  function stopListening() {
    if (speaking && globalThis.speechSynthesis) {
      speechSynthesis.cancel();
    }
    speaking = false;
  }

  function listen() {
    if (speaking) {
      stopListening();
      updateListen();
      return;
    }
    const voice = localVoice();
    if (!voice) {
      return;
    }
    const text = [$('title').textContent, $('content').innerText]
                     .join('.\n\n');
    // Long texts in paragraphs: some engines stop after a few minutes of
    // one utterance.
    const parts = text.split(/\n{2,}/).map((p) => p.trim()).filter(Boolean);
    speaking = true;
    parts.forEach((part, i) => {
      const utterance = new SpeechSynthesisUtterance(part);
      utterance.voice = voice;
      utterance.lang = voice.lang;
      if (i === parts.length - 1) {
        utterance.onend = () => {
          speaking = false;
          updateListen();
        };
      }
      speechSynthesis.speak(utterance);
    });
    updateListen();
  }

  function close() {
    stopListening();
    if (host.close) {
      host.close();
    } else {
      window.close();
    }
  }

  function toggleSettings(open) {
    const panel = $('settings');
    panel.hidden = open === undefined ? !panel.hidden : !open;
    $('settings-button').setAttribute('aria-expanded', String(!panel.hidden));
  }

  function bind() {
    $('close').addEventListener('click', close);
    $('listen').addEventListener('click', listen);
    $('settings-button').addEventListener('click', () => toggleSettings());
    $('smaller').addEventListener('click', () => stepSize(-1));
    $('larger').addEventListener('click', () => stepSize(1));
    for (const button of document.querySelectorAll('[data-setting]')) {
      button.addEventListener('click', () => {
        change(button.dataset.setting, button.dataset.value);
      });
    }
    // A click elsewhere closes the panel.
    document.addEventListener('click', (e) => {
      if (!$('settings').hidden && !e.target.closest('#settings') &&
          !e.target.closest('#settings-button')) {
        toggleSettings(false);
      }
    });
    document.addEventListener('keydown', (e) => {
      // Esc closes the panel first; with it closed, Esc goes on to the
      // browser, which leaves Reader.
      if (e.key === 'Escape' && !$('settings').hidden) {
        toggleSettings(false);
        e.preventDefault();
        e.stopPropagation();
      }
    });
    if (globalThis.speechSynthesis) {
      speechSynthesis.addEventListener('voiceschanged', updateListen);
    }
    window.addEventListener('pagehide', stopListening);
  }

  async function loadFromUrl() {
    const source = new URLSearchParams(location.search).get('article');
    if (!source) {
      return;
    }
    try {
      const response = await fetch(source, {credentials: 'omit'});
      render(await response.json());
    } catch (e) {
      showError('This article could not be shown.');
    }
  }

  globalThis.compassReaderView = {
    render,
    getSettings: () => ({...settings}),
    setSettings(value) {
      settings = validSettings({...settings, ...value});
      applySettings();
      saveSettings(settings);
    },
  };

  bind();
  applySettings();
  loadFromUrl();
})();
