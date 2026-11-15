// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {colorToCss} from '../colors.js';
import type {LinkRule, LinkRuleSpace} from '../sidebar.mojom-webui.js';
import {SettingsHandler} from '../sidebar.mojom-webui.js';
import {findSpaceSymbol, SPACE_SYMBOL_VIEW_BOX} from '../space_icons.js';

import {getTemplate} from './links_page.html.js';
import {CompassSettingsPage} from './page_base.js';

const SVG_NS = 'http://www.w3.org/2000/svg';

// Links from Other Apps: compass://settings/links (docs/design/
// link-routing.md). A rule sends links from other apps on a domain (and its
// subdomains) to a space, instead of a little window. Rules are listed by
// domain with their space's icon and color; one whose space was deleted is
// listed as broken. "links?add=<domain>" (the command bar's "Route this site
// to space…") opens the form for that domain.
export class CompassLinksPageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  private rules_: LinkRule[] = [];
  private spaces_: LinkRuleSpace[] = [];
  // The domain of the rule being edited; empty while adding one.
  private editing_ = '';

  protected get pageTitle() {
    return 'Links from Other Apps';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$('addLinkRule').addEventListener('click', () => this.openForm_());
    this.$('cancelLinkRule').addEventListener('click', () => this.closeForm_());
    this.$('linkRuleForm').addEventListener('submit', e => {
      e.preventDefault();
      this.save_();
    });
    // Rules can change elsewhere (the little window's menu, sync).
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') {
        this.refresh_();
      }
    });
    this.refresh_().then(() => {
      const add = new URLSearchParams(window.location.search).get('add');
      if (add) {
        this.openForm_(add);
      }
    });
  }

  private async refresh_() {
    const {rules, spaces} = await this.handler_.getLinkRules();
    this.rules_ = rules;
    this.spaces_ = spaces;
    this.render_();
  }

  private findSpace_(id: string): LinkRuleSpace|null {
    return this.spaces_.find(space => space.id === id) ?? null;
  }

  private render_() {
    const list = this.$('linkRules');
    list.replaceChildren(...this.rules_.map(rule => this.renderRule_(rule)));
    this.show('noLinkRules', this.rules_.length === 0);
    this.$<HTMLButtonElement>('addLinkRule').disabled =
        this.spaces_.length === 0;
  }

  private renderRule_(rule: LinkRule): HTMLElement {
    const row = document.createElement('li');
    row.className = 'links-row';
    row.dataset['domain'] = rule.domain;

    const domain = document.createElement('span');
    domain.className = 'links-domain';
    domain.textContent = rule.domain;

    const target = document.createElement('span');
    target.className = 'links-space';
    const space = rule.broken ? null : this.findSpace_(rule.spaceId);
    if (space) {
      target.append(this.renderIcon_(space));
      const name = document.createElement('span');
      name.textContent = space.name;
      target.append(name);
    } else {
      target.classList.add('links-broken');
      target.textContent = 'Space deleted: opens in a little window';
    }

    const edit = document.createElement('button');
    edit.className = 'secondary';
    edit.textContent = 'Edit';
    edit.addEventListener('click', () => this.openForm_(rule.domain, rule));

    const remove = document.createElement('button');
    remove.className = 'secondary';
    remove.textContent = 'Delete';
    remove.setAttribute('aria-label', `Delete ${rule.domain}`);
    remove.addEventListener('click', async () => {
      await this.handler_.removeLinkRule(rule.domain);
      if (this.editing_ === rule.domain) {
        this.closeForm_();
      }
      await this.refresh_();
    });

    row.append(domain, target, edit, remove);
    return row;
  }

  // The space's icon on its color, as the switcher shows it: a symbol, an
  // emoji, or its name's first letter.
  private renderIcon_(space: LinkRuleSpace): HTMLElement {
    const icon = document.createElement('span');
    icon.className = 'links-space-icon';
    icon.style.background = colorToCss(space.color ?? null);
    const symbol =
        space.icon?.symbol ? findSpaceSymbol(space.icon.symbol) : null;
    if (symbol) {
      const svg = document.createElementNS(SVG_NS, 'svg');
      svg.setAttribute('viewBox', SPACE_SYMBOL_VIEW_BOX);
      const path = document.createElementNS(SVG_NS, 'path');
      path.setAttribute('d', symbol.path);
      svg.append(path);
      icon.append(svg);
    } else if (space.icon?.emoji) {
      icon.classList.add('emoji');
      icon.style.background = 'none';
      icon.textContent = space.icon.emoji;
    } else {
      icon.textContent = (space.name.trim()[0] ?? '').toUpperCase();
    }
    icon.setAttribute('aria-hidden', 'true');
    return icon;
  }

  // Adding a rule for |domain| (empty: typed), or editing |rule|.
  private openForm_(domain = '', rule: LinkRule|null = null) {
    this.editing_ = rule ? rule.domain : '';
    const select = this.$<HTMLSelectElement>('linkSpace');
    select.replaceChildren(...this.spaces_.map(space => {
      const option = document.createElement('option');
      option.value = space.id;
      option.textContent = space.name;
      return option;
    }));
    const existing = rule ?? this.rules_.find(r => r.domain === domain);
    if (existing && this.findSpace_(existing.spaceId)) {
      select.value = existing.spaceId;
    }
    const input = this.$<HTMLInputElement>('linkDomain');
    input.value = domain;
    this.show('linkNote', false);
    this.show('linkRuleForm', true);
    this.show('addLinkRule', false);
    input.focus();
  }

  private closeForm_() {
    this.editing_ = '';
    this.show('linkRuleForm', false);
    this.show('addLinkRule', true);
  }

  private async save_() {
    const domain = this.$<HTMLInputElement>('linkDomain').value;
    const spaceId = this.$<HTMLSelectElement>('linkSpace').value;
    const {domain: saved} =
        await this.handler_.setLinkRule(domain, spaceId, this.editing_);
    if (saved === null || saved === undefined) {
      const note = this.$('linkNote');
      note.textContent = domain.trim() ?
          `"${domain.trim()}" isn't a site's domain, like github.com.` :
          'Type a site\'s domain, like github.com.';
      note.hidden = false;
      return;
    }
    this.closeForm_();
    await this.refresh_();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-links-page': CompassLinksPageElement;
  }
}

customElements.define('compass-links-page', CompassLinksPageElement);
