// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// chrome://compass-archive/: the window's space's archived Today entries, in
// a bubble beside the sidebar, pointing at Today's archive button
// (docs/design/sidebar.md, "Today auto-archive"). Clicking one brings it
// back to Today and closes the bubble; × drops one, Clear drops them all.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {applyBubbleLook} from '../bubble_look.js';
import {getFaviconUrl} from '../favicon.js';
import {i18n, i18nF, i18nPlural} from '../i18n.js';
import type {ArchivedEntry, ArchivedTab} from '../sidebar.mojom-webui.js';
import {ARCHIVE_DELAYS} from '../space_options.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {ArchiveBrowserProxy} from './browser_proxy.js';

const RELATIVE_TIME = new Intl.RelativeTimeFormat(undefined, {numeric: 'auto'});

export class CompassArchiveAppElement extends CrLitElement {
  static get is() {
    return 'compass-archive-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      entries_: {type: Array},
      archiveAfterHours_: {type: Number},
      loaded_: {type: Boolean},
    };
  }

  protected accessor entries_: ArchivedEntry[] = [];
  protected accessor archiveAfterHours_: number = 0;
  protected accessor loaded_: boolean = false;

  private readonly proxy_ = ArchiveBrowserProxy.getInstance();
  private shown_ = false;

  override connectedCallback() {
    super.connectedCallback();
    ColorChangeUpdater.forDocument().start();
    this.proxy_.callbackRouter.archiveChanged.addListener(
        () => this.load_());
    window.matchMedia('(prefers-color-scheme: dark)')
        .addEventListener('change', () => this.applyLook_());
    this.start_();
  }

  private async start_() {
    await this.applyLook_();
    await this.load_();
    await this.updateComplete;
    if (!this.shown_) {
      this.shown_ = true;
      this.proxy_.handler.showUI();
    }
  }

  private async applyLook_() {
    const {look} = await this.proxy_.handler.getLook();
    applyBubbleLook(look);
  }

  private async load_() {
    const {entries, archiveAfterHours} =
        await this.proxy_.handler.getArchive();
    this.entries_ = entries;
    this.archiveAfterHours_ = archiveAfterHours;
    this.loaded_ = true;
  }

  protected isEmpty_(): boolean {
    return this.entries_.length === 0;
  }

  protected getHint_(): string {
    const hours = this.archiveAfterHours_;
    if (hours === 0) {
      return i18n('sidebarArchiveOff');
    }
    const delay = ARCHIVE_DELAYS.find(delay => delay.hours === hours)?.label ??
        i18nPlural('spaceOptionsHours', hours);
    return i18nF('sidebarArchiveEmpty', delay);
  }

  // A split shows its panes' titles side by side.
  protected getTitle_(entry: ArchivedEntry): string {
    return entry.tabs.map((tab: ArchivedTab) => tab.title || tab.url)
        .join(' | ');
  }

  protected getFaviconUrl_(entry: ArchivedEntry): string {
    // The bubble's page is in the window's profile.
    return getFaviconUrl(entry.tabs[0]?.url ?? '', 16, null);
  }

  protected getAge_(entry: ArchivedEntry): string {
    const minutes = (entry.archivedAt - Date.now()) / 60000;
    if (minutes > -60) {
      return RELATIVE_TIME.format(Math.round(minutes), 'minute');
    }
    if (minutes > -24 * 60) {
      return RELATIVE_TIME.format(Math.round(minutes / 60), 'hour');
    }
    return RELATIVE_TIME.format(Math.round(minutes / (24 * 60)), 'day');
  }

  protected onEntryClick_(e: Event) {
    this.proxy_.handler.restoreArchived(
        (e.currentTarget as HTMLElement).dataset['archivedId']!);
  }

  protected onRemoveClick_(e: Event) {
    e.stopPropagation();
    this.proxy_.handler.removeArchived(
        (e.currentTarget as HTMLElement).dataset['archivedId']!);
  }

  protected onClearClick_() {
    this.proxy_.handler.clearArchive();
  }

  protected onCloseClick_() {
    this.proxy_.handler.close();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-archive-app': CompassArchiveAppElement;
  }
}

customElements.define(CompassArchiveAppElement.is, CompassArchiveAppElement);
