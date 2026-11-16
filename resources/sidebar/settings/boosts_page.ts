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

// What a Boost's author shows as.
const BADGES: Record<string, string> = {
  claude: 'by Claude',
  shared: 'Shared',
};

// Boosts: compass://settings/boosts (docs/design/boosts.md). The profile's
// Boosts by site, each with a switch, Share, Edit and Delete, and a badge
// for those Claude made or someone shared; the editor has the name, the site
// pattern, the CSS and the JavaScript and where it runs. "boosts?add=<host>"
// (the page menu's "Boost This Site…") opens the editor for that host, or
// its Boost if it has one; "boosts?edit=<id>" opens that Boost;
// "boosts?import=<payload>" (a Boost link opened in Compass) previews a
// shared Boost to add, as Import… does for a file.
export class CompassBoostsPageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  private boosts_: Boost[] = [];
  // The id of the Boost being edited; empty while adding one.
  private editing_ = '';
  // The shared Boost being previewed, as a link's payload.
  private importing_ = '';
  // The Boost whose share options are open.
  private sharing_ = '';

  protected get pageTitle() {
    return 'Boosts';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$('addBoost').addEventListener('click', () => this.openForm_());
    this.$('importBoost').addEventListener('click', () => this.chooseFile_());
    this.$('confirmImport').addEventListener('click', () => this.addShared_());
    this.$('cancelImport').addEventListener('click', () => this.closeImport_());
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
          this.$('boostForm').hidden && this.$('boostImport').hidden) {
        this.refresh_();
      }
    });
    this.refresh_().then(() => {
      const params = new URLSearchParams(window.location.search);
      const shared = params.get('import');
      if (shared !== null) {
        this.preview_(shared);
        return;
      }
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
      items.push(heading);
      for (const boost of sites.get(site)!) {
        items.push(this.renderBoost_(boost));
        if (boost.id === this.sharing_) {
          items.push(this.renderShare_(boost));
        }
      }
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
    const badgeText = BADGES[boost.author];
    if (badgeText) {
      const badge = document.createElement('span');
      badge.className = 'boosts-badge';
      badge.textContent = badgeText;
      row.append(badge);
    }

    const share = document.createElement('button');
    share.className = 'secondary boosts-share-button';
    share.textContent = 'Share';
    share.setAttribute('aria-label', `Share ${boost.name}`);
    share.setAttribute('aria-expanded', String(this.sharing_ === boost.id));
    share.addEventListener('click', () => {
      this.sharing_ = this.sharing_ === boost.id ? '' : boost.id;
      this.render_();
    });

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

    row.append(share, edit, remove);
    return row;
  }

  // Share's options under |boost|'s row: Copy Link (unless the link would
  // be too long) and Save File….
  private renderShare_(boost: Boost): HTMLElement {
    const item = document.createElement('li');
    item.className = 'boosts-share';
    const note = document.createElement('span');
    note.className = 'boosts-share-note';

    const copy = document.createElement('button');
    copy.className = 'secondary';
    copy.textContent = 'Copy Link';
    copy.hidden = true;
    copy.addEventListener('click', async () => {
      const {copied} = await this.handler_.copyBoostShareLink(boost.id);
      note.textContent = copied ? 'Link copied.' : 'Couldn’t copy the link.';
    });

    const save = document.createElement('button');
    save.className = 'secondary';
    save.textContent = 'Save File…';
    save.addEventListener('click', async () => {
      const {saved} = await this.handler_.saveBoostFile(boost.id);
      if (saved) {
        note.textContent = 'File saved.';
      }
    });

    this.handler_.getBoostShareLink(boost.id).then(({link}) => {
      copy.hidden = !link;
      item.dataset['link'] = link;
      if (!link) {
        note.textContent =
            'This Boost is too big for a link. Share it as a file.';
      }
    });
    item.append(copy, save, note);
    return item;
  }

  // Import…: a .compassboost file, previewed as a link is.
  private async chooseFile_() {
    const {payload, error} = await this.handler_.chooseBoostFile();
    if (payload || error) {
      this.preview_(payload, error);
    }
  }

  // Shows what |payload| would add, or why it can't (|error|, or the
  // browser's verdict).
  private async preview_(payload: string, error = '') {
    this.closeForm_();
    this.importing_ = '';
    let boost: Boost|null = null;
    if (!error) {
      if (!payload) {
        error = 'This Boost link is broken: part of it may be missing.';
      } else {
        ({boost, error} = await this.handler_.previewSharedBoost(payload));
      }
    }
    const ok = !!boost;
    this.show('importDetails', ok);
    this.show('confirmImport', ok);
    const note = this.$('importNote');
    note.textContent = error;
    note.hidden = ok;
    if (boost) {
      this.importing_ = payload;
      this.$('importName').textContent = boost.name;
      this.$('importMatch').textContent = boost.match;
      this.$('importCss').textContent = boost.css;
      this.show('importCssBlock', !!boost.css.trim());
      this.$('importJs').textContent = boost.js;
      const hasJs = !!boost.js.trim();
      this.show('importJsBlock', hasJs);
      this.$('importWarning').textContent = hasJs ?
          `This Boost runs JavaScript on ${boost.match}. Only add it if ` +
              'you trust whoever shared it.' :
          '';
      this.show('importWarning', hasJs);
      this.$('importWorld').textContent = boost.jsWorld === 'main' ?
          'It runs with the page’s own scripts, so it can see and change ' +
              'everything they do.' :
          'It runs apart from the page’s scripts, with access to the ' +
              'page itself.';
    }
    this.show('boostImport', true);
    this.show('addBoost', false);
    this.show('importBoost', false);
    this.$('boostImport').scrollIntoView({block: 'nearest'});
  }

  private async addShared_() {
    if (!this.importing_) {
      return;
    }
    const {saved, error} = await this.handler_.addSharedBoost(this.importing_);
    if (!saved) {
      const note = this.$('importNote');
      note.textContent = error;
      note.hidden = false;
      return;
    }
    this.closeImport_();
    await this.refresh_();
  }

  private closeImport_() {
    this.importing_ = '';
    this.show('boostImport', false);
    this.show('addBoost', true);
    this.show('importBoost', true);
    // A reload doesn't ask again.
    if (new URLSearchParams(window.location.search).has('import')) {
      window.history.replaceState(
          window.history.state, '', window.location.pathname);
    }
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
    this.show('boostImport', false);
    this.importing_ = '';
    this.show('boostForm', true);
    this.show('addBoost', false);
    this.show('importBoost', false);
    (boost || site ? this.$('boostCss') : this.$('boostMatch')).focus();
  }

  private closeForm_() {
    this.editing_ = '';
    this.show('boostForm', false);
    this.show('addBoost', true);
    this.show('importBoost', true);
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
