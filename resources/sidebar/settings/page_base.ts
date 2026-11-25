// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../settings_window/icon.js';

// Compass's settings sections, each an element shown in the settings window
// (docs/design/settings-window.md, which re-skins them), and alone on its own
// page (chrome://compass-sync, chrome://compass-settings/shortcuts.html,
// chrome://compass-import), which the end-to-end steps and browser tests
// drive. See docs/design/settings.md.
//
// The page's markup goes in the element's light DOM, so on the standalone
// pages `document.getElementById()` still finds it, and the window's
// stylesheet reaches it. Its styles are scoped under `.compass-page`.

// A heading and a card, for the standalone pages (the window restyles them).
const BASE_STYLE = `
.compass-page {
  color: var(--cr-primary-text-color, var(--color-sys-on-surface, #1f1f1f));
  display: block;
  margin-bottom: 24px;
}

.compass-page > .compass-title {
  font-size: 16px;
  font-weight: 500;
  line-height: 24px;
  margin: 0;
  padding: 16px 20px;
}

.compass-page > .compass-card {
  background: var(--cr-card-background-color,
      var(--color-sys-base-container-elevated, #fff));
  border-radius: var(--cr-card-border-radius, 12px);
  box-shadow: var(--cr-card-shadow, none);
  padding: 16px 20px 20px;
}

.compass-page [hidden] {
  display: none !important;
}
`;

export abstract class CompassSettingsPage extends HTMLElement {
  private content_: HTMLElement|null = null;

  // The section's heading.
  protected abstract get pageTitle(): string;
  // The page's markup (its .html file).
  protected abstract getPageTemplate(): string|TrustedHTML;
  // Runs once, when the markup is in place.
  protected abstract setUp(): void;

  connectedCallback() {
    if (this.content_) {
      return;
    }
    const style = document.createElement('style');
    style.textContent = BASE_STYLE;
    this.content_ = document.createElement('div');
    this.content_.className = 'compass-page';
    const title = document.createElement('h2');
    title.className = 'compass-title';
    title.textContent = this.pageTitle;
    const card = document.createElement('div');
    card.className = 'compass-card';
    card.innerHTML = this.getPageTemplate();
    this.content_.append(title, card);
    this.append(style, this.content_);
    this.setUp();
  }

  protected $<T extends HTMLElement = HTMLElement>(id: string): T {
    return this.querySelector<T>(`#${id}`)!;
  }

  protected show(id: string, visible: boolean) {
    this.$(id).hidden = !visible;
  }

  // Views (docs/design/settings-window.md, "Pages and views"): the markup's
  // blocks marked data-page-view="name" (one of them "main") are shown one at
  // a time; a [data-open-view="name"] button opens one. Each view but main
  // gets a "‹ |backLabel|" link and its data-title as a heading. The path's
  // ?view= picks the first one, and showing one puts it in the path.
  private view_ = 'main';

  protected get view(): string {
    return this.view_;
  }

  protected setUpViews(backLabel: string) {
    for (const block of this.querySelectorAll<HTMLElement>(
             '[data-page-view]')) {
      if (block.dataset['pageView'] === 'main') {
        continue;
      }
      const back = document.createElement('button');
      back.className = 'back';
      back.type = 'button';
      back.dataset['back'] = '';
      const icon = document.createElement('compass-settings-icon');
      icon.setAttribute('name', 'back');
      back.append(icon, backLabel);
      back.addEventListener('click', () => this.setView('main'));
      const title = document.createElement('h2');
      title.className = 'subpage-title';
      title.textContent = block.dataset['title'] || '';
      block.prepend(back, title);
    }
    for (const button of this.querySelectorAll<HTMLElement>(
             '[data-open-view]')) {
      button.addEventListener(
          'click', () => this.setView(button.dataset['openView']!));
    }
    const view = new URLSearchParams(window.location.search).get('view');
    this.setView(view && this.querySelector(`[data-page-view="${view}"]`) ?
                     view :
                     'main',
                 /*updatePath=*/ false);
  }

  protected setView(view: string, updatePath = true) {
    this.view_ = view;
    for (const block of this.querySelectorAll<HTMLElement>(
             '[data-page-view]')) {
      block.hidden = block.dataset['pageView'] !== view;
    }
    const nested = view !== 'main';
    this.toggleAttribute('nested', nested);
    this.dispatchEvent(new CustomEvent('settings-view', {bubbles: true}));
    if (updatePath) {
      const url = new URL(window.location.href);
      if (nested) {
        url.searchParams.set('view', view);
      } else {
        url.searchParams.delete('view');
      }
      window.history.replaceState(null, '', url.pathname + url.search);
      const scroller = document.getElementById('scroller');
      if (scroller) {
        scroller.scrollTop = 0;
      }
    }
  }

  // Tells the settings window this section's status, for the row that opens
  // its page (panes.ts, Subpage).
  protected setSubpageDetail(detail: string) {
    this.dispatchEvent(
        new CustomEvent('subpage-detail', {bubbles: true, detail}));
  }
}
