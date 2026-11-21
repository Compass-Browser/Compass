// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './icon.js';

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {SearchEngineDetails} from '../sidebar.mojom-webui.js';

import {SettingsWindowBrowserProxy} from './browser_proxy.js';
import {getHtml} from './search_engines.html.js';

// A search engine being added (no id) or edited.
export interface EngineDraft {
  id: string;
  name: string;
  keyword: string;
  url: string;
  error: string;
}

// General › Manage search engines and site search (docs/design/
// settings-window.md, phase 5b): the search engines (make one the default,
// edit, remove) and site search (on or off, add, edit, remove), by
// KeywordEditorController's rules. Fires "close" for the way back.
export class CompassSettingsSearchEnginesElement extends CrLitElement {
  static get is() {
    return 'compass-settings-search-engines';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      engines_: {type: Array},
      draft_: {type: Object},
    };
  }

  protected accessor engines_: SearchEngineDetails[] = [];
  // The engine being added or edited, if any.
  protected accessor draft_: EngineDraft|null = null;

  private handler_ = SettingsWindowBrowserProxy.getInstance().handler;
  private listenerId_: number|null = null;

  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.listenerId_ = SettingsWindowBrowserProxy.getInstance()
                           .callbackRouter.searchEnginesChanged.addListener(
                               () => this.load_());
    this.load_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.listenerId_ !== null) {
      SettingsWindowBrowserProxy.getInstance().callbackRouter.removeListener(
          this.listenerId_);
      this.listenerId_ = null;
    }
  }

  // Template helpers.

  // The search engines (prepopulated, and the default), then site search.
  protected getEngines_(siteSearch: boolean): SearchEngineDetails[] {
    return this.engines_.filter(e => e.isSiteSearch === siteSearch);
  }

  protected getEngineDetail_(engine: SearchEngineDetails): string {
    return engine.isDefault ? `${engine.keyword} · Default` : engine.keyword;
  }

  protected getDraftTitle_(): string {
    return this.draft_?.id ? 'Edit search engine' : 'Add site search';
  }

  // Events.

  protected onBackClick_() {
    this.fire('close');
  }

  protected onMakeEngineDefaultClick_(e: Event) {
    this.handler_.setDefaultSearchEngine(
        (e.currentTarget as HTMLElement).dataset['id']!);
  }

  protected onEditEngineClick_(e: Event) {
    const id = (e.currentTarget as HTMLElement).dataset['id']!;
    const engine = this.engines_.find(d => d.id === id);
    if (engine) {
      this.draft_ = {
        id,
        name: engine.name,
        keyword: engine.keyword,
        url: engine.url,
        error: '',
      };
    }
  }

  protected onAddEngineClick_() {
    this.draft_ = {id: '', name: '', keyword: '', url: '', error: ''};
  }

  protected onRemoveEngineClick_(e: Event) {
    this.handler_.removeSearchEngine(
        (e.currentTarget as HTMLElement).dataset['id']!);
  }

  protected onEngineActiveChange_(e: Event) {
    const input = e.target as HTMLInputElement;
    this.handler_.setSearchEngineActive(input.dataset['id']!, input.checked);
  }

  // A field of the draft (data-field: name, keyword, url).
  protected onDraftInput_(e: Event) {
    const input = e.target as HTMLInputElement;
    this.draft_ = {
      ...this.draft_!,
      [input.dataset['field']!]: input.value,
      error: '',
    };
  }

  protected onDraftCancelClick_() {
    this.draft_ = null;
  }

  protected async onDraftSaveClick_() {
    const draft = this.draft_;
    if (!draft) {
      return;
    }
    const {error} = await this.handler_.saveSearchEngine(
        draft.id, draft.name.trim(), draft.keyword.trim(), draft.url.trim());
    if (error) {
      this.draft_ = {...draft, error};
      return;
    }
    this.draft_ = null;
    this.load_();
  }

  private async load_() {
    const {engines} = await this.handler_.getSearchEngineDetails();
    this.engines_ = engines;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-settings-search-engines': CompassSettingsSearchEnginesElement;
  }
}

customElements.define(
    CompassSettingsSearchEnginesElement.is,
    CompassSettingsSearchEnginesElement);
