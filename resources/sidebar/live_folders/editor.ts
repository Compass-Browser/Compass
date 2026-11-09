// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The live folder editor: New Live Folder… (pick a plugin, then fill in its
// form) and Edit Source…. The form is built from the plugin's manifest:
// its config fields, the secrets it asks for (kept on this device only) and,
// if it wants them, consent to send the profile's cookies to its hosts
// (docs/design/live-folders.md, "UI").

import {CrLitElement, html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from '../browser_proxy.js';
import type {LiveConfigField, LivePlugin} from '../sidebar.mojom-webui.js';

type FieldValue = string|boolean;

export class CompassLiveFolderEditorElement extends CrLitElement {
  static get is() {
    return 'compass-live-folder-editor';
  }

  static override get properties() {
    return {
      parentId: {type: String},
      folderId: {type: String},
      plugins_: {type: Array},
      plugin_: {type: Object},
      values_: {type: Object},
      secrets_: {type: Object},
      grantCookies_: {type: Boolean},
      title_: {type: String},
      error_: {type: String},
      saving_: {type: Boolean},
    };
  }

  // For a new folder: where it goes. For an edit: the folder.
  accessor parentId: string|null = null;
  accessor folderId: string|null = null;
  protected accessor plugins_: LivePlugin[] = [];
  protected accessor plugin_: LivePlugin|null = null;
  protected accessor values_: Record<string, FieldValue> = {};
  protected accessor secrets_: Record<string, string> = {};
  protected accessor grantCookies_: boolean = false;
  protected accessor title_: string = '';
  protected accessor error_: string = '';
  protected accessor saving_: boolean = false;

  private handler_ = SidebarBrowserProxy.getInstance().handler;

  // In the light DOM: the app's styles apply.
  override createRenderRoot() {
    return this;
  }

  override connectedCallback() {
    super.connectedCallback();
    this.load_();
  }

  private async load_() {
    const {plugins} = await this.handler_.getLivePlugins();
    this.plugins_ = plugins;
    if (!this.folderId) {
      return;
    }
    const {source} = await this.handler_.getLiveFolderSource(this.folderId);
    if (!source) {
      this.close_();
      return;
    }
    this.plugin_ = plugins.find(p => p.id === source.pluginId) ?? null;
    if (!this.plugin_) {
      this.error_ = 'This folder\'s plugin isn\'t installed on this device.';
      return;
    }
    this.title_ = source.title;
    this.grantCookies_ = source.cookiesGranted;
    const config = JSON.parse(source.configJson) as Record<string, unknown>;
    const values: Record<string, FieldValue> = {};
    for (const field of this.plugin_.config) {
      const value = config[field.key];
      if (typeof value === 'boolean') {
        values[field.key] = value;
      } else if (value !== undefined && value !== null) {
        values[field.key] = String(value);
      }
    }
    this.values_ = values;
  }

  override render() {
    const editing = !!this.folderId;
    // clang-format off
    return html`
<div class="live-editor" role="dialog"
    aria-label="${editing ? 'Edit live folder' : 'New live folder'}"
    @keydown="${this.onKeydown_}">
  <div class="live-editor-header">
    ${this.plugin_ && !editing ? html`
      <button class="live-editor-back" title="Back" aria-label="Back"
          @click="${this.onBackClick_}">‹</button>` : nothing}
    <span>${editing ? 'Edit Source' : this.plugin_ ? this.plugin_.name :
                                                     'New Live Folder'}</span>
  </div>
  ${this.plugin_ ? this.renderForm_(this.plugin_, editing) :
                   this.renderPicker_()}
</div>`;
    // clang-format on
  }

  private renderPicker_() {
    // clang-format off
    return html`
<div class="live-editor-plugins" role="list">
  ${this.plugins_.map(plugin => html`
    <button class="live-editor-plugin" role="listitem"
        @click="${() => this.pick_(plugin)}">
      <span class="live-icon" aria-hidden="true"
          style="${plugin.iconPath ? `--live-icon: url("/${plugin.iconPath}")` :
                                     ''}"></span>
      <span class="live-editor-plugin-text">
        <span class="live-editor-plugin-name">${plugin.name}</span>
        <span class="live-editor-hint">${plugin.description}</span>
      </span>
    </button>`)}
</div>
<div class="live-editor-buttons">
  <button @click="${this.close_}">Cancel</button>
</div>`;
    // clang-format on
  }

  private renderForm_(plugin: LivePlugin, editing: boolean) {
    // clang-format off
    return html`
<form class="live-editor-form" @submit="${this.onSubmit_}">
  ${!editing ? html`
    <label class="live-editor-field">
      <span>Name</span>
      <input type="text" .value="${this.title_}" placeholder="${plugin.name}"
          @input="${(e: Event) =>
              this.title_ = (e.target as HTMLInputElement).value}">
    </label>` : nothing}
  ${plugin.config.map(field => this.renderField_(field))}
  ${plugin.secrets.map(secret => html`
    <label class="live-editor-field">
      <span>${secret.label}${secret.optional ? ' (optional)' : ''}</span>
      <input type="password" autocomplete="off"
          placeholder="${secret.hasValue ? 'Saved: leave blank to keep' : ''}"
          .value="${this.secrets_[secret.key] ?? ''}"
          @input="${(e: Event) => this.secrets_ = {
            ...this.secrets_,
            [secret.key]: (e.target as HTMLInputElement).value,
          }}">
      ${secret.help ? html`
        <span class="live-editor-hint">${secret.help}</span>` : nothing}
      <span class="live-editor-hint">Kept on this device only.</span>
    </label>`)}
  ${plugin.cookieHosts.length ? html`
    <label class="live-editor-check">
      <input type="checkbox" .checked="${this.grantCookies_}"
          @change="${(e: Event) => this.grantCookies_ =
              (e.target as HTMLInputElement).checked}">
      <span>Send your cookies for ${this.describeHosts_(plugin)}, so it
        can see what you see signed in</span>
    </label>` : nothing}
  ${this.error_ ? html`
    <div class="live-editor-error" role="alert">${this.error_}</div>` :
    nothing}
  <div class="live-editor-buttons">
    <button type="button" @click="${this.close_}">Cancel</button>
    <button type="submit" class="action" ?disabled="${this.saving_}">
      ${editing ? 'Save' : 'Create'}
    </button>
  </div>
</form>`;
    // clang-format on
  }

  private renderField_(field: LiveConfigField) {
    const value = this.values_[field.key] ?? this.defaultOf_(field);
    const label = field.label + (field.optional ? ' (optional)' : '');
    const set = (v: FieldValue) => this.values_ = {
      ...this.values_,
      [field.key]: v,
    };
    // clang-format off
    switch (field.type) {
      case 'bool':
        return html`
<label class="live-editor-check">
  <input type="checkbox" .checked="${value === true || value === 'true'}"
      @change="${(e: Event) => set((e.target as HTMLInputElement).checked)}">
  <span>${label}</span>
</label>`;
      case 'select':
        return html`
<label class="live-editor-field">
  <span>${label}</span>
  <select @change="${(e: Event) =>
      set((e.target as HTMLSelectElement).value)}">
    ${field.options.map(option => html`
      <option value="${option.value}" ?selected="${option.value === value}">
        ${option.label}
      </option>`)}
  </select>
</label>`;
      default:
        return html`
<label class="live-editor-field">
  <span>${label}</span>
  <input type="${field.type === 'number' ? 'number' :
                 field.type === 'url' ? 'url' : 'text'}"
      .value="${String(value ?? '')}" placeholder="${field.placeholder}"
      min="${field.min ?? nothing}" max="${field.max ?? nothing}"
      @input="${(e: Event) => set((e.target as HTMLInputElement).value)}">
</label>`;
    }
    // clang-format on
  }

  private defaultOf_(field: LiveConfigField): FieldValue|undefined {
    if (!field.defaultJson) {
      return undefined;
    }
    const value = JSON.parse(field.defaultJson) as unknown;
    return typeof value === 'boolean' ? value : String(value);
  }

  // "{config:url}" reads as the site the folder shows.
  private describeHosts_(plugin: LivePlugin): string {
    const named = plugin.cookieHosts.map(
        host => host.startsWith('{') ? 'the site you enter' : host);
    return [...new Set(named)].join(', ');
  }

  private pick_(plugin: LivePlugin) {
    this.plugin_ = plugin;
    this.values_ = {};
    this.secrets_ = {};
    this.grantCookies_ = false;
    this.error_ = '';
  }

  protected onBackClick_() {
    this.plugin_ = null;
    this.error_ = '';
  }

  protected onKeydown_(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      e.stopPropagation();
      this.close_();
    }
  }

  protected async onSubmit_(e: Event) {
    e.preventDefault();
    const plugin = this.plugin_;
    if (!plugin || this.saving_) {
      return;
    }
    const config: Record<string, unknown> = {};
    for (const field of plugin.config) {
      const value = this.values_[field.key];
      if (value === undefined || value === '') {
        continue;
      }
      config[field.key] = field.type === 'number' && typeof value === 'string' ?
          Number(value) :
          value;
    }
    const secrets: Record<string, string> = {};
    for (const [key, value] of Object.entries(this.secrets_)) {
      if (value) {
        secrets[key] = value;
      }
    }
    this.saving_ = true;
    const {error} = await this.handler_.saveLiveFolder({
      folderId: this.folderId,
      parentId: this.parentId,
      pluginId: plugin.id,
      title: this.title_.trim(),
      configJson: JSON.stringify(config),
      secrets,
      grantCookies: this.grantCookies_,
    });
    this.saving_ = false;
    if (error) {
      this.error_ = error;
    } else {
      this.close_();
    }
  }

  private close_() {
    this.dispatchEvent(new CustomEvent('close', {bubbles: true, composed: true}));
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-live-folder-editor': CompassLiveFolderEditorElement;
  }
}

customElements.define(
    CompassLiveFolderEditorElement.is, CompassLiveFolderEditorElement);
