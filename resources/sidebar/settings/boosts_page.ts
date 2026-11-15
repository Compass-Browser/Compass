// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {Boost} from '../sidebar.mojom-webui.js';
import {SettingsHandler} from '../sidebar.mojom-webui.js';

import {getTemplate} from './boosts_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// The site a Boost's match is about, to group by: its host without "*." or
// a path.
function siteOf(match: string): string {
  return match.replace(/^\*\./, '').split('/')[0] || match;
}

// Boosts: compass://settings/boosts (docs/design/boosts.md). The profile's
// Boosts by site, each with a switch, Edit and Delete, and a badge for those
// Claude made; the editor has the name, the site pattern, the CSS and the
// JavaScript and where it runs. "boosts?add=<host>" (the page menu's "Boost
// This Site…") opens the editor for that host, or its Boost if it has one;
// "boosts?edit=<id>" opens that Boost.
export class CompassBoostsPageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  private boosts_: Boost[] = [];
  // The id of the Boost being edited; empty while adding one.
  private editing_ = '';

  protected get pageTitle() {
    return 'Boosts';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$('addBoost').addEventListener('click', () => this.openForm_());
    this.$('cancelBoost').addEventListener('click', () => this.closeForm_());
    this.$('boostForm').addEventListener('submit', e => {
      e.preventDefault();
      this.save_();
    });
    // Tab inserts a tab in the code, as in an editor.
    for (const id of ['boostCss', 'boostJs']) {
      const area = this.$<HTMLTextAreaElement>(id);
      area.addEventListener('keydown', e => {
        if (e.key !== 'Tab' || e.shiftKey || e.metaKey || e.ctrlKey ||
            e.altKey) {
          return;
        }
        e.preventDefault();
        area.setRangeText('  ', area.selectionStart, area.selectionEnd, 'end');
      });
    }
    // Boosts change elsewhere too (Claude, sync).
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible' &&
          this.$('boostForm').hidden) {
        this.refresh_();
      }
    });
    this.refresh_().then(() => {
      const params = new URLSearchParams(window.location.search);
      const edit = params.get('edit');
      const add = params.get('add');
      const existing = edit ? this.boosts_.find(b => b.id === edit) :
          add ? this.boosts_.find(b => siteOf(b.match) === siteOf(add)) :
                undefined;
      if (existing) {
        this.openForm_(existing);
      } else if (add) {
        this.openForm_(null, add);
      }
    });
  }

  private async refresh_() {
    const {boosts} = await this.handler_.getBoosts();
    this.boosts_ = boosts;
    this.render_();
  }

  private render_() {
    const list = this.$('boosts');
    const sites = new Map<string, Boost[]>();
    for (const boost of this.boosts_) {
      const site = siteOf(boost.match);
      sites.set(site, [...(sites.get(site) ?? []), boost]);
    }
    const items: HTMLElement[] = [];
    for (const site of [...sites.keys()].sort()) {
      const heading = document.createElement('li');
      heading.className = 'boosts-site';
      heading.textContent = site;
      items.push(heading, ...sites.get(site)!.map(b => this.renderBoost_(b)));
    }
    list.replaceChildren(...items);
    this.show('noBoosts', this.boosts_.length === 0);
  }

  private renderBoost_(boost: Boost): HTMLElement {
    const row = document.createElement('li');
    row.className = 'boosts-row';
    row.dataset['id'] = boost.id;
    row.toggleAttribute('disabled', !boost.enabled);

    const enabled = document.createElement('input');
    enabled.type = 'checkbox';
    enabled.checked = boost.enabled;
    enabled.setAttribute('aria-label', `Turn on ${boost.name}`);
    enabled.addEventListener('change', async () => {
      await this.handler_.setBoostEnabled(boost.id, enabled.checked);
      await this.refresh_();
    });

    const name = document.createElement('span');
    name.className = 'boosts-name';
    name.textContent = boost.name;
    name.title = boost.match;

    const kinds = document.createElement('span');
    kinds.className = 'boosts-kinds';
    kinds.textContent =
        [boost.css.trim() ? 'CSS' : '', boost.js.trim() ? 'JavaScript' : '']
            .filter(Boolean)
            .join(' + ');

    row.append(enabled, name, kinds);
    if (boost.author === 'claude') {
      const badge = document.createElement('span');
      badge.className = 'boosts-badge';
      badge.textContent = 'by Claude';
      row.append(badge);
    }

    const edit = document.createElement('button');
    edit.className = 'secondary';
    edit.textContent = 'Edit';
    edit.addEventListener('click', () => this.openForm_(boost));

    const remove = document.createElement('button');
    remove.className = 'secondary';
    remove.textContent = 'Delete';
    remove.setAttribute('aria-label', `Delete ${boost.name}`);
    remove.addEventListener('click', async () => {
      await this.handler_.deleteBoost(boost.id);
      if (this.editing_ === boost.id) {
        this.closeForm_();
      }
      await this.refresh_();
    });

    row.append(edit, remove);
    return row;
  }

  // Editing |boost|, or adding one for |site| (empty: typed).
  private openForm_(boost: Boost|null = null, site = '') {
    this.editing_ = boost ? boost.id : '';
    this.$<HTMLInputElement>('boostName').value = boost?.name ?? '';
    this.$<HTMLInputElement>('boostMatch').value = boost?.match ?? site;
    this.$<HTMLTextAreaElement>('boostCss').value = boost?.css ?? '';
    this.$<HTMLTextAreaElement>('boostJs').value = boost?.js ?? '';
    this.$<HTMLSelectElement>('boostWorld').value =
        boost?.jsWorld === 'main' ? 'main' : 'isolated';
    this.show('boostNote', false);
    this.show('boostForm', true);
    this.show('addBoost', false);
    (boost || site ? this.$('boostCss') : this.$('boostMatch')).focus();
  }

  private closeForm_() {
    this.editing_ = '';
    this.show('boostForm', false);
    this.show('addBoost', true);
  }

  private async save_() {
    const existing = this.boosts_.find(b => b.id === this.editing_);
    const {saved, error} = await this.handler_.saveBoost({
      id: this.editing_,
      name: this.$<HTMLInputElement>('boostName').value,
      match: this.$<HTMLInputElement>('boostMatch').value,
      css: this.$<HTMLTextAreaElement>('boostCss').value,
      js: this.$<HTMLTextAreaElement>('boostJs').value,
      jsWorld: this.$<HTMLSelectElement>('boostWorld').value,
      enabled: existing ? existing.enabled : true,
      author: existing ? existing.author : 'user',
    });
    if (!saved) {
      const note = this.$('boostNote');
      note.textContent = error;
      note.hidden = false;
      return;
    }
    this.closeForm_();
    await this.refresh_();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-boosts-page': CompassBoostsPageElement;
  }
}

customElements.define('compass-boosts-page', CompassBoostsPageElement);
