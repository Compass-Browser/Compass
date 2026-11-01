// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';

import type {Shortcut, ShortcutKeys} from './sidebar.mojom-webui.js';
import {SettingsHandler} from './sidebar.mojom-webui.js';

// chrome://compass-settings: Compass's own settings, its keyboard shortcuts
// for now. See docs/design/shortcuts.md "Customizing".
const handler = SettingsHandler.getRemote();

ColorChangeUpdater.forDocument().start();

// Keys that are only modifiers: recording waits for the key they go with.
const MODIFIER_KEYS =
    new Set(['Shift', 'Control', 'Alt', 'Meta', 'CapsLock', 'Fn', 'AltGraph']);

// A note under a shortcut's row: why keys can't be used, or which shortcut
// has them, with a button to take them anyway.
interface Message {
  text: string;
  error: boolean;
  // Keys to give the shortcut once the user agrees to take them.
  take?: ShortcutKeys;
}

let shortcuts: Shortcut[] = [];
// The shortcut whose keys the page is recording.
let recordingId: string|null = null;
const messages = new Map<string, Message>();

function $<T extends HTMLElement>(id: string): T {
  return document.getElementById(id) as T;
}

function element<K extends keyof HTMLElementTagNameMap>(
    tag: K, className: string, text = ''): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  node.className = className;
  node.textContent = text;
  return node;
}

function describeKeys(keys: string[]): string {
  return keys.length ? keys.join('   ') : 'None';
}

function startRecording(id: string) {
  recordingId = id;
  messages.delete(id);
  handler.setRecording(true);
  render();
}

function stopRecording() {
  if (recordingId === null) {
    return;
  }
  recordingId = null;
  handler.setRecording(false);
  render();
}

async function refresh() {
  ({shortcuts} = await handler.getShortcuts());
  render();
}

async function apply(id: string, keys: ShortcutKeys) {
  messages.delete(id);
  await handler.setShortcut(id, keys);
  await refresh();
}

function renderRow(shortcut: Shortcut): HTMLLIElement {
  const item = element('li', '');
  item.dataset['id'] = shortcut.id;
  const row = element('div', 'row');
  const text = element('div', 'label');
  text.append(element('div', '', shortcut.label));
  if (shortcut.customized) {
    text.append(element(
        'div', 'default', `Default: ${describeKeys(shortcut.defaultKeys)}`));
  }

  const recording = recordingId === shortcut.id;
  const keys = element(
      'button', 'keys',
      recording ? 'Press keys…' : describeKeys(shortcut.keys));
  keys.classList.toggle('none', !recording && !shortcut.keys.length);
  keys.classList.toggle('recording', recording);
  keys.title = recording ? 'Esc cancels, Delete removes the shortcut' :
                           'Click to change';
  keys.addEventListener('click', () => {
    if (recording) {
      stopRecording();
    } else {
      startRecording(shortcut.id);
    }
  });

  const reset = element('button', 'link', 'Reset');
  reset.classList.toggle('hidden', !shortcut.customized);
  reset.addEventListener('click', async () => {
    messages.delete(shortcut.id);
    stopRecording();
    await handler.resetShortcut(shortcut.id);
    await refresh();
  });

  row.append(text, keys, reset);
  item.append(row);

  const message = messages.get(shortcut.id);
  if (message) {
    const note = element('div', 'message', message.text);
    note.classList.toggle('error', message.error);
    if (message.take) {
      const take = message.take;
      const use = element('button', 'link', 'Use here');
      use.addEventListener('click', () => apply(shortcut.id, take));
      const cancel = element('button', 'link', 'Cancel');
      cancel.addEventListener('click', () => {
        messages.delete(shortcut.id);
        render();
      });
      note.append(' ', use, cancel);
    }
    item.append(note);
  }

  if (recording) {
    // After render() rebuilt it.
    queueMicrotask(() => keys.focus());
  }
  return item;
}

function render() {
  const container = $('sections');
  container.replaceChildren();
  // Sections in the order their first shortcut comes.
  const sections = new Map<string, Shortcut[]>();
  for (const shortcut of shortcuts) {
    const list = sections.get(shortcut.section) ?? [];
    list.push(shortcut);
    sections.set(shortcut.section, list);
  }
  for (const [name, list] of sections) {
    container.append(element('h3', '', name));
    const items = element('ul', '');
    items.append(...list.map(renderRow));
    container.append(items);
  }
  $<HTMLButtonElement>('resetAll').disabled =
      !shortcuts.some(shortcut => shortcut.customized);
}

async function onKeyDown(e: KeyboardEvent) {
  if (recordingId === null) {
    return;
  }
  // Nothing else gets the keys: not the page, not the browser (which also
  // holds its own shortcuts back while recording).
  e.preventDefault();
  e.stopPropagation();
  if (MODIFIER_KEYS.has(e.key) || e.repeat) {
    return;
  }
  const id = recordingId;
  const plain = !e.metaKey && !e.ctrlKey && !e.altKey && !e.shiftKey;
  if (plain && e.key === 'Escape') {
    stopRecording();
    return;
  }
  if (plain && (e.key === 'Backspace' || e.key === 'Delete')) {
    stopRecording();
    await handler.removeShortcut(id);
    await refresh();
    return;
  }

  const keys: ShortcutKeys = {
    keyCode: e.keyCode,
    shift: e.shiftKey,
    ctrl: e.ctrlKey,
    alt: e.altKey,
    meta: e.metaKey,
  };
  const {check} = await handler.checkShortcut(id, keys);
  if (recordingId !== id) {
    return;
  }
  if (check.error) {
    // Still recording: the user tries other keys.
    messages.set(id, {text: `${check.text}: ${check.error}`, error: true});
    render();
    return;
  }
  stopRecording();
  if (check.conflictId) {
    messages.set(id, {
      text: `${check.text} is the shortcut for “${check.conflictLabel}”. ` +
          'Use it here instead?',
      error: false,
      take: keys,
    });
    render();
    return;
  }
  await apply(id, keys);
}

document.addEventListener('keydown', onKeyDown, {capture: true});

// The browser's shortcuts must not stay off once the user is elsewhere.
window.addEventListener('blur', stopRecording);

// Shortcuts changed from another settings tab.
window.addEventListener('focus', () => {
  if (recordingId === null) {
    refresh();
  }
});

$('resetAll').addEventListener('click', async () => {
  messages.clear();
  stopRecording();
  await handler.resetAllShortcuts();
  await refresh();
});

refresh();
