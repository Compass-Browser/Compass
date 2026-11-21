// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
}
