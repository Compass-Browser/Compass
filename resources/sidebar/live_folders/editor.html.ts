// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {LiveConfigField, LivePlugin, LiveSecretField} from '../sidebar.mojom-webui.js';

import type {CompassLiveFolderEditorElement} from './editor.js';

export function getHtml(this: CompassLiveFolderEditorElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div class="live-editor" role="dialog" aria-label="${this.dialogLabel()}"
    @keydown="${this.onKeydown}">
  <div class="live-editor-header">
    ${this.plugin && !this.folderId && !this.installOpen ? html`
      <button class="live-editor-back" title="Back" aria-label="Back"
          @click="${this.onBackClick}">‹</button>` : nothing}
    <span>${this.heading()}</span>
  </div>

  ${this.installOpen ? html`
    <div class="live-editor-form">
      <label class="live-editor-field">
        <span>Address of the plugin's manifest.json</span>
        <input type="url" .value="${this.installUrl}"
            placeholder="https://example.com/plugin/manifest.json"
            @input="${this.onInstallUrlInput}">
      </label>
      ${this.preview ? html`
        <div class="live-editor-preview">
          <div class="live-editor-plugin-name">
            ${this.preview.name} ${this.preview.version}
          </div>
          <div class="live-editor-hint">${this.preview.description}</div>
          <div>Reaches: ${this.describeHosts(this.preview.hosts)}</div>
          ${this.preview.cookieHosts.length ? html`
            <div>Can ask for your cookies for:
              ${this.describeHosts(this.preview.cookieHosts)}</div>` :
            nothing}
          ${this.preview.secrets.length ? html`
            <div>Asks for: ${this.preview.secrets.join(', ')}</div>` :
            nothing}
          <div class="live-editor-hint">From ${this.preview.source}</div>
        </div>` : nothing}
      ${this.error ? html`
        <div class="live-editor-error" role="alert">${this.error}</div>` :
        nothing}
      <div class="live-editor-buttons">
        <button @click="${this.onInstallCancelClick}">Cancel</button>
        ${this.preview ? html`
          <button class="action" ?disabled="${this.saving}"
              @click="${this.onInstallConfirmClick}">Install</button>` : html`
          <button class="action" @click="${this.onReviewClick}">Review</button>`}
      </div>
    </div>` : nothing}

  ${!this.installOpen && this.plugin ? html`
    <form class="live-editor-form" @submit="${this.onSubmit}">
      ${!this.folderId ? html`
        <label class="live-editor-field">
          <span>Name</span>
          <input type="text" .value="${this.folderTitle}"
              placeholder="${this.plugin.name}"
              @input="${this.onTitleInput}">
        </label>` : nothing}
      ${this.plugin.config.map((field: LiveConfigField) => html`
        ${field.type === 'bool' ? html`
          <label class="live-editor-check">
            <input type="checkbox" .checked="${this.isChecked(field)}"
                data-key="${field.key}" @change="${this.onCheckChange}">
            <span>${this.fieldLabel(field)}</span>
          </label>` : nothing}
        ${field.type === 'select' ? html`
          <label class="live-editor-field">
            <span>${this.fieldLabel(field)}</span>
            <select data-key="${field.key}" @change="${this.onFieldChange}">
              ${field.options.map(option => html`
                <option value="${option.value}"
                    ?selected="${option.value === this.fieldValue(field)}">
                  ${option.label}
                </option>`)}
            </select>
          </label>` : nothing}
        ${field.type !== 'bool' && field.type !== 'select' ? html`
          <label class="live-editor-field">
            <span>${this.fieldLabel(field)}</span>
            <input type="${this.inputType(field)}"
                .value="${this.fieldText(field)}"
                placeholder="${field.placeholder}"
                min="${field.min ?? nothing}" max="${field.max ?? nothing}"
                data-key="${field.key}" @input="${this.onFieldInput}">
          </label>` : nothing}`)}
      ${this.plugin.secrets.map((secret: LiveSecretField) => html`
        <label class="live-editor-field">
          <span>${secret.label}${secret.optional ? ' (optional)' : ''}</span>
          <input type="password" autocomplete="off"
              placeholder="${secret.hasValue ? 'Saved: leave blank to keep' :
                                               ''}"
              .value="${this.secrets[secret.key] ?? ''}"
              data-key="${secret.key}" @input="${this.onSecretInput}">
          ${secret.help ? html`
            <span class="live-editor-hint">${secret.help}</span>` : nothing}
          <span class="live-editor-hint">Kept on this device only.</span>
        </label>`)}
      ${this.plugin.cookieHosts.length ? html`
        <label class="live-editor-check">
          <input type="checkbox" .checked="${this.grantCookies}"
              @change="${this.onGrantChange}">
          <span>Send your cookies for
            ${this.describeHosts(this.plugin.cookieHosts)}, so it can see
            what you see signed in</span>
        </label>` : nothing}
      ${this.error ? html`
        <div class="live-editor-error" role="alert">${this.error}</div>` :
        nothing}
      <div class="live-editor-buttons">
        <button type="button" @click="${this.onCancelClick}">Cancel</button>
        <button type="submit" class="action" ?disabled="${this.saving}">
          ${this.folderId ? 'Save' : 'Create'}
        </button>
      </div>
    </form>` : nothing}

  ${!this.installOpen && !this.plugin && this.folderId ? html`
    ${this.error ? html`
      <div class="live-editor-error" role="alert">${this.error}</div>` :
      nothing}
    <div class="live-editor-buttons">
      ${this.missingPluginUrl ? html`
        <button class="action" @click="${this.onInstallMissingClick}">
          Install the plugin
        </button>` : nothing}
      <button @click="${this.onCancelClick}">Close</button>
    </div>` : nothing}

  ${!this.installOpen && !this.plugin && !this.folderId ? html`
    <div class="live-editor-plugins" role="list">
      ${this.plugins.map((plugin: LivePlugin) => html`
        <div class="live-editor-plugin-row" role="listitem">
          <button class="live-editor-plugin" data-id="${plugin.id}"
              @click="${this.onPluginClick}">
            <span class="live-icon" aria-hidden="true"
                style="${this.iconStyle(plugin)}"></span>
            <span class="live-editor-plugin-text">
              <span class="live-editor-plugin-name">${plugin.name}</span>
              <span class="live-editor-hint">${plugin.description}</span>
            </span>
          </button>
          ${plugin.local ? html`
            <span class="live-editor-local"
                title="From the plugins folder: remove its folder to remove it">
              Local
            </span>` : nothing}
          ${!plugin.builtin && !plugin.local ? html`
            <button class="live-editor-remove" title="Remove ${plugin.name}"
                aria-label="Remove ${plugin.name}" data-id="${plugin.id}"
                @click="${this.onRemoveClick}">&times;</button>` : nothing}
        </div>`)}
    </div>
    <div class="live-editor-buttons">
      <button @click="${this.onInstallPluginClick}">Install a plugin…</button>
      <button title="Put a plugin's folder, with its manifest.json, in it"
          @click="${this.onOpenFolderClick}">Open plugins folder</button>
      <button @click="${this.onCancelClick}">Cancel</button>
    </div>` : nothing}
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
