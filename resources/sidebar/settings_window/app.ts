// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../settings/arc_import_page.js';
import '../settings/boosts_page.js';
import '../settings/claude_page.js';
import '../settings/dev_mode_page.js';
import '../settings/links_page.js';
import '../settings/plugins_page.js';
import '../settings/shortcuts_page.js';
import '../settings/sync_page.js';
import './advanced_pane.js';
import './general_pane.js';
import './icon.js';
import './passwords_pane.js';
import './privacy_pane.js';
import './profile_section.js';

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {SpaceColor} from '../sidebar.mojom-webui.js';

import {getHtml} from './app.html.js';
import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import type {LinkRow, Pane, PaneSection} from './panes.js';
import {PANES, resolvePath} from './panes.js';

// The toolbar's height (window.css, #toolbar), which the window adds to the
// pane's to take the height it needs (SettingsWindow::SetContentHeight()).
const TOOLBAR_HEIGHT = 78;

// The pane the window opens at with no path: the last one shown.
const LAST_PANE_KEY = 'lastPane';

// A search result: a section or a row, and the pane it's in.
export interface SearchResult {
  pane: Pane;
  label: string;
  detail: string;
  // The path that shows it.
  path: string;
  // The row to highlight there, if a row.
  rowId: string;
}

function readLastPane(): string {
  try {
    return window.localStorage.getItem(LAST_PANE_KEY) || '';
  } catch {
    return '';
  }
}

function writeLastPane(id: string) {
  try {
    window.localStorage.setItem(LAST_PANE_KEY, id);
  } catch {
    // Not kept: the window opens at General next time.
  }
}

// The settings window (docs/design/settings-window.md): a toolbar with the
// panes as tabs (and search), and the shown pane under it. Compass's sections
// are the same elements chrome://settings shows (settings/*_page.ts), kept
// once made; window.css gives them the window's look. Rendered in the light
// DOM, so that one stylesheet reaches the sections' markup too.
export class CompassSettingsWindowAppElement extends CrLitElement {
  static get is() {
    return 'compass-settings-window-app';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      pane_: {type: Object},
      query_: {type: String},
      results_: {type: Array},
    };
  }

  protected accessor pane_: Pane = PANES[0]!;
  protected accessor query_: string = '';
  protected accessor results_: SearchResult[] = [];

  protected readonly panes = PANES;
  protected readonly isMac = navigator.userAgent.includes('Mac OS X');

  private proxy_ = SettingsWindowBrowserProxy.getInstance();
  // The sections made so far, by tag.
  private sections_ = new Map<string, HTMLElement>();
  private resizeObserver_: ResizeObserver|null = null;
  private reportedHeight_ = 0;

  override createRenderRoot() {
    return this;
  }

  // Whether search shows its results instead of the pane.
  protected isSearching_(): boolean {
    return !!this.query_.trim();
  }

  protected getTitle_(): string {
    return this.isSearching_() ? 'Search' : this.pane_.label;
  }

  protected isSelected_(pane: Pane): boolean {
    return !this.isSearching_() && pane === this.pane_;
  }

  // A pane of one section and no rows: its heading is the title row's.
  protected isSinglePane_(): boolean {
    return this.pane_.sections.length === 1 && !this.pane_.groups.length;
  }

  protected getQuery_(): string {
    return this.query_.trim();
  }

  override connectedCallback() {
    super.connectedCallback();
    document.documentElement.classList.toggle('mac', this.isMac);
    const router = this.proxy_.callbackRouter;
    router.showPath.addListener((path: string) => this.showPath_(path));
    router.setAccent.addListener(
        (color: SpaceColor|null) => this.setAccent_(color));
    this.proxy_.handler.getAccent().then(
        ({color}: {color: SpaceColor|null}) => this.setAccent_(color));
    document.addEventListener('keydown', e => this.onKeyDown_(e));
    this.showPath_(
        window.location.pathname.replace(/^\//, '') + window.location.search);
  }

  override firstUpdated() {
    const content = this.querySelector<HTMLElement>('#content')!;
    this.resizeObserver_ = new ResizeObserver(() => this.reportHeight_());
    this.resizeObserver_.observe(content);
  }

  // The window takes the height the pane needs, within the screen.
  private reportHeight_() {
    const content = this.querySelector<HTMLElement>('#content');
    if (!content) {
      return;
    }
    const height = Math.ceil(TOOLBAR_HEIGHT + content.offsetHeight);
    if (height !== this.reportedHeight_) {
      this.reportedHeight_ = height;
      this.proxy_.handler.setContentHeight(height);
    }
  }

  // The accent is the profile's active space's color (a fixed blue for a
  // grey space): switches, the selected tab, focus rings, the sections'
  // buttons.
  private setAccent_(color: SpaceColor|null) {
    const root = document.documentElement.style;
    if (color) {
      root.setProperty(
          '--accent', `rgb(${color.red}, ${color.green}, ${color.blue})`);
    } else {
      root.removeProperty('--accent');
    }
  }

  // Shows |path| ("boosts?add=example.com", "sync", "" for the last pane).
  private async showPath_(path: string) {
    let resolved = resolvePath(path);
    if (!resolved) {
      resolved = resolvePath(readLastPane()) || {pane: PANES[0]!, section: null};
      path = resolved.pane.id;
    }
    const {pane, section} = resolved;
    // A path with a query is for the section to act on ("?add=", "?edit=",
    // "?import="): it reads it as it's made, so it's made again.
    if (path.includes('?')) {
      for (const paneSection of pane.sections) {
        this.sections_.delete(paneSection.tag);
      }
    }
    window.history.replaceState(null, '', '/' + path);
    writeLastPane(pane.id);
    this.query_ = '';
    this.results_ = [];
    this.pane_ = pane;
    await this.updateComplete;
    this.mountSections_(pane);
    const scroller = this.querySelector<HTMLElement>('#scroller')!;
    if (section) {
      this.sections_.get(section.tag)?.scrollIntoView({block: 'start'});
    } else {
      scroller.scrollTop = 0;
    }
    this.reportHeight_();
  }

  private mountSections_(pane: Pane) {
    const container = this.querySelector<HTMLElement>('#sections')!;
    container.replaceChildren(...pane.sections.map(s => this.sectionFor_(s)));
  }

  private sectionFor_(section: PaneSection): HTMLElement {
    let element = this.sections_.get(section.tag);
    if (!element) {
      element = document.createElement(section.tag);
      element.dataset['path'] = section.path;
      this.sections_.set(section.tag, element);
    }
    return element;
  }

  protected onTabClick_(e: Event) {
    const id = (e.currentTarget as HTMLElement).dataset['pane']!;
    this.showPath_(id);
  }

  // Left and right move between the tabs, as in a toolbar.
  protected onTabKeydown_(e: KeyboardEvent) {
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') {
      return;
    }
    e.preventDefault();
    const index = PANES.indexOf(this.pane_);
    const next = (index + (e.key === 'ArrowRight' ? 1 : -1) + PANES.length) %
        PANES.length;
    this.showPath_(PANES[next]!.id).then(() => {
      this.querySelector<HTMLElement>(`.tab[data-pane="${PANES[next]!.id}"]`)
          ?.focus();
    });
  }

  protected onRowClick_(e: Event) {
    const id = (e.currentTarget as HTMLElement).dataset['row']!;
    const row = this.findRow_(id);
    if (row) {
      this.openRow_(row);
    }
  }

  private findRow_(id: string): LinkRow|null {
    for (const pane of PANES) {
      for (const group of pane.groups) {
        const row = group.rows.find(r => r.id === id);
        if (row) {
          return row;
        }
      }
    }
    return null;
  }

  private openRow_(row: LinkRow) {
    if (row.url) {
      this.proxy_.handler.openUrl(row.url);
    }
  }

  protected onSearchInput_(e: Event) {
    this.query_ = (e.target as HTMLInputElement).value;
    this.results_ = this.search_(this.query_);
  }

  protected onSearchKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter' && this.results_.length) {
      e.preventDefault();
      this.openResult_(this.results_[0]!);
    }
  }

  protected onResultClick_(e: Event) {
    const index = Number((e.currentTarget as HTMLElement).dataset['index']);
    const result = this.results_[index];
    if (result) {
      this.openResult_(result);
    }
  }

  private async openResult_(result: SearchResult) {
    await this.showPath_(result.path);
    if (result.rowId) {
      // A section's element renders its rows once connected.
      await new Promise(resolve => requestAnimationFrame(resolve));
      const row = this.querySelector<HTMLElement>(
          `[data-row="${CSS.escape(result.rowId)}"]`);
      if (row) {
        row.scrollIntoView({block: 'center'});
        this.flash_(row);
      }
    } else {
      const section = resolvePath(result.path)?.pane.sections.find(
          s => s.path === result.path.split('?')[0]);
      const element = section && this.sections_.get(section.tag);
      if (element) {
        this.flash_(element);
      }
    }
  }

  private flash_(element: HTMLElement) {
    element.classList.remove('flash');
    // Restarts the animation.
    void element.offsetWidth;
    element.classList.add('flash');
  }

  // Every pane's rows and sections whose words have all of |query|'s.
  private search_(query: string): SearchResult[] {
    const words = query.toLowerCase().split(/\s+/).filter(w => w);
    if (!words.length) {
      return [];
    }
    const matches = (text: string) => {
      const haystack = text.toLowerCase();
      return words.every(w => haystack.includes(w));
    };
    const results: SearchResult[] = [];
    for (const pane of PANES) {
      for (const section of pane.sections) {
        for (const row of section.searchRows || []) {
          if (matches([row.label, row.keywords || '', pane.label].join(' '))) {
            results.push({
              pane,
              label: row.label,
              detail: pane.label,
              path: section.path,
              rowId: row.id,
            });
          }
        }
        if (section.searchRows) {
          // Its rows stand for it.
          continue;
        }
        const element = this.sections_.get(section.tag);
        const title = element?.querySelector('.compass-title')?.textContent ||
            pane.label;
        const text = [title, pane.label, section.keywords,
                      element?.textContent || ''].join(' ');
        if (matches(text)) {
          results.push({
            pane,
            label: title,
            detail: pane.label,
            path: section.path,
            rowId: '',
          });
        }
      }
      for (const group of pane.groups) {
        for (const row of group.rows) {
          if (matches([row.label, row.detail, row.keywords || '',
                       group.title, pane.label].join(' '))) {
            results.push({
              pane,
              label: row.label,
              detail: `${pane.label} › ${group.title}`,
              path: pane.id,
              rowId: row.id,
            });
          }
        }
      }
    }
    return results;
  }

  private onKeyDown_(e: KeyboardEvent) {
    const command = this.isMac ? e.metaKey : e.ctrlKey;
    // Cmd+F (Ctrl+F): search.
    if (command && e.key === 'f') {
      e.preventDefault();
      this.querySelector<HTMLInputElement>('#searchInput')?.focus();
      return;
    }
    // Cmd+W (Ctrl+W) closes the window, as the system's settings windows.
    if (command && e.key === 'w') {
      e.preventDefault();
      this.proxy_.handler.close();
      return;
    }
    if (e.key === 'Escape' && !e.defaultPrevented) {
      if (this.query_) {
        this.query_ = '';
        this.results_ = [];
        return;
      }
      // Not while a section's own field or dialog has it (it handles Esc).
      const target = e.composedPath()[0] as HTMLElement;
      if (target === document.body || target.closest('#toolbar')) {
        this.proxy_.handler.close();
      }
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-window-app': CompassSettingsWindowAppElement;
  }
}

customElements.define(
    CompassSettingsWindowAppElement.is, CompassSettingsWindowAppElement);

ColorChangeUpdater.forDocument().start();
