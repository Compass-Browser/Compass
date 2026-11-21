// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {SettingsHandler} from '../sidebar.mojom-webui.js';

import {getTemplate} from './dev_mode_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Development Mode: compass://settings/devMode (docs/design/dev-mode.md).
// The sites in development mode, by host pattern: on them reload skips the
// cache, a bar above the page has DevTools' buttons, and the Network panel
// shows the requests from before it opened. Sites are added here, from the
// site information popup, the command bar or with Cmd+Shift+D.
export class CompassDevModePageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  private hosts_: string[] = [];

  protected get pageTitle() {
    return 'Development Mode';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$('devModeForm').addEventListener('submit', e => {
      e.preventDefault();
      this.add_();
    });
    // Sites are also added from the page (the site popup, the command bar).
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') {
        this.refresh_();
      }
    });
    this.refresh_();
  }

  private async refresh_() {
    const {hosts} = await this.handler_.getDevModeHosts();
    this.hosts_ = hosts;
    this.render_();
  }

  private render_() {
    this.$('devModeHosts')
        .replaceChildren(...this.hosts_.map(host => this.renderHost_(host)));
    this.show('noDevModeHosts', this.hosts_.length === 0);
  }

  private renderHost_(host: string): HTMLElement {
    const row = document.createElement('li');
    row.className = 'dev-mode-row';
    row.dataset['host'] = host;

    const name = document.createElement('span');
    name.className = 'dev-mode-host';
    name.textContent = host;

    const remove = document.createElement('button');
    remove.className = 'secondary';
    remove.textContent = 'Remove';
    remove.setAttribute('aria-label', `Remove ${host}`);
    remove.addEventListener('click', async () => {
      await this.handler_.removeDevModeHost(host);
      await this.refresh_();
    });

    row.append(name, remove);
    return row;
  }

  private async add_() {
    const input = this.$<HTMLInputElement>('devModeHost');
    const typed = input.value.trim();
    const note = this.$('devModeNote');
    if (!typed) {
      note.textContent = 'Type a site, like localhost or example.com.';
      note.hidden = false;
      return;
    }
    const {host} = await this.handler_.addDevModeHost(typed);
    if (host === null || host === undefined) {
      note.textContent =
          `"${typed}" isn't a site, like localhost or example.com.`;
      note.hidden = false;
      return;
    }
    note.hidden = true;
    input.value = '';
    await this.refresh_();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-dev-mode-page': CompassDevModePageElement;
  }
}

customElements.define('compass-dev-mode-page', CompassDevModePageElement);
