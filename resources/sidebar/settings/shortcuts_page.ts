// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {Shortcut, ShortcutKeys} from '../sidebar.mojom-webui.js';
import {SettingsHandler} from '../sidebar.mojom-webui.js';

import {CompassSettingsPage} from './page_base.js';
import {getTemplate} from './shortcuts_page.html.js';

// Keyboard shortcuts: compass://settings/shortcuts, and alone on
// chrome://compass-settings. See docs/design/shortcuts.md "Customizing".

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

export class CompassShortcutsPageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  private shortcuts_: Shortcut[] = [];
  // The shortcut whose keys the page is recording.
  private recordingId_: string|null = null;
  private messages_ = new Map<string, Message>();

  protected get pageTitle() {
    return 'Keyboard shortcuts';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    document.addEventListener(
        'keydown', e => this.onKeyDown_(e), {capture: true});

    // The browser's shortcuts must not stay off once the user is elsewhere.
    window.addEventListener('blur', () => this.stopRecording_());

    // Shortcuts changed from another settings tab.
    window.addEventListener('focus', () => {
      if (this.recordingId_ === null) {
        this.refresh_();
      }
    });

    this.$('resetAll').addEventListener('click', async () => {
      this.messages_.clear();
      this.stopRecording_();
      await this.handler_.resetAllShortcuts();
      await this.refresh_();
    });

    this.refresh_();
  }

  disconnectedCallback() {
    // Leaving the section (chrome://settings keeps the element) mustn't
    // leave the browser's shortcuts off.
    this.stopRecording_();
  }

  private startRecording_(id: string) {
    this.recordingId_ = id;
    this.messages_.delete(id);
    this.handler_.setRecording(true);
    this.render_();
  }

  private stopRecording_() {
    if (this.recordingId_ === null) {
      return;
    }
    this.recordingId_ = null;
    this.handler_.setRecording(false);
    this.render_();
  }

  private async refresh_() {
    ({shortcuts: this.shortcuts_} = await this.handler_.getShortcuts());
    this.render_();
  }

  private async apply_(id: string, keys: ShortcutKeys) {
    this.messages_.delete(id);
    await this.handler_.setShortcut(id, keys);
    await this.refresh_();
  }

  private renderRow_(shortcut: Shortcut): HTMLLIElement {
    const item = element('li', '');
    item.dataset['id'] = shortcut.id;
    const row = element('div', 'row');
    const text = element('div', 'label');
    text.append(element('div', '', shortcut.label));
    if (shortcut.customized) {
      text.append(element(
          'div', 'default', `Default: ${describeKeys(shortcut.defaultKeys)}`));
    }

    const recording = this.recordingId_ === shortcut.id;
    const keys = element(
        'button', 'keys',
        recording ? 'Press keys…' : describeKeys(shortcut.keys));
    keys.classList.toggle('none', !recording && !shortcut.keys.length);
    keys.classList.toggle('recording', recording);
    keys.title = recording ? 'Esc cancels, Delete removes the shortcut' :
                             'Click to change';
    keys.addEventListener('click', () => {
      if (recording) {
        this.stopRecording_();
      } else {
        this.startRecording_(shortcut.id);
      }
    });

    const reset = element('button', 'link', 'Reset');
    reset.classList.toggle('hidden', !shortcut.customized);
    reset.addEventListener('click', async () => {
      this.messages_.delete(shortcut.id);
      this.stopRecording_();
      await this.handler_.resetShortcut(shortcut.id);
      await this.refresh_();
    });

    row.append(text, keys, reset);
    item.append(row);

    const message = this.messages_.get(shortcut.id);
    if (message) {
      const note = element('div', 'message', message.text);
      note.classList.toggle('error', message.error);
      if (message.take) {
        const take = message.take;
        const use = element('button', 'link', 'Use here');
        use.addEventListener('click', () => this.apply_(shortcut.id, take));
        const cancel = element('button', 'link', 'Cancel');
        cancel.addEventListener('click', () => {
          this.messages_.delete(shortcut.id);
          this.render_();
        });
        note.append(' ', use, cancel);
      }
      item.append(note);
    }

    if (recording) {
      // After render_() rebuilt it.
      queueMicrotask(() => keys.focus());
    }
    return item;
  }

  private render_() {
    const container = this.$('sections');
    container.replaceChildren();
    // Sections in the order their first shortcut comes.
    const sections = new Map<string, Shortcut[]>();
    for (const shortcut of this.shortcuts_) {
      const list = sections.get(shortcut.section) ?? [];
      list.push(shortcut);
      sections.set(shortcut.section, list);
    }
    for (const [name, list] of sections) {
      container.append(element('h3', '', name));
      const items = element('ul', '');
      items.append(...list.map(shortcut => this.renderRow_(shortcut)));
      container.append(items);
    }
    this.$<HTMLButtonElement>('resetAll').disabled =
        !this.shortcuts_.some(shortcut => shortcut.customized);
  }

  private async onKeyDown_(e: KeyboardEvent) {
    if (this.recordingId_ === null) {
      return;
    }
    // Nothing else gets the keys: not the page, not the browser (which also
    // holds its own shortcuts back while recording).
    e.preventDefault();
    e.stopPropagation();
    if (MODIFIER_KEYS.has(e.key) || e.repeat) {
      return;
    }
    const id = this.recordingId_;
    const plain = !e.metaKey && !e.ctrlKey && !e.altKey && !e.shiftKey;
    if (plain && e.key === 'Escape') {
      this.stopRecording_();
      return;
    }
    if (plain && (e.key === 'Backspace' || e.key === 'Delete')) {
      this.stopRecording_();
      await this.handler_.removeShortcut(id);
      await this.refresh_();
      return;
    }

    const keys: ShortcutKeys = {
      keyCode: e.keyCode,
      shift: e.shiftKey,
      ctrl: e.ctrlKey,
      alt: e.altKey,
      meta: e.metaKey,
    };
    const {check} = await this.handler_.checkShortcut(id, keys);
    if (this.recordingId_ !== id) {
      return;
    }
    if (check.error) {
      // Still recording: the user tries other keys.
      this.messages_.set(
          id, {text: `${check.text}: ${check.error}`, error: true});
      this.render_();
      return;
    }
    this.stopRecording_();
    if (check.conflictId) {
      this.messages_.set(id, {
        text: `${check.text} is the shortcut for “${check.conflictLabel}”. ` +
            'Use it here instead?',
        error: false,
        take: keys,
      });
      this.render_();
      return;
    }
    await this.apply_(id, keys);
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-shortcuts-page': CompassShortcutsPageElement;
  }
}

customElements.define('compass-shortcuts-page', CompassShortcutsPageElement);
