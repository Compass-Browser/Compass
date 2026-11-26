// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {LiveConfigField, LivePlugin, LiveSecretField} from '../sidebar.mojom-webui.js';

import {i18n, i18nF} from '../i18n.js';

import type {CompassLiveFolderEditorElement} from './editor.js';

export function getHtml(this: CompassLiveFolderEditorElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div class="live-editor" role="dialog" aria-label="${this.dialogLabel()}"
    @keydown="${this.onKeydown}">
  <div class="live-editor-header">
    ${this.plugin && !this.folderId && !this.installOpen ? html`
      <button class="live-editor-back" title="${i18n('back')}"
          aria-label="${i18n('back')}"
          @click="${this.onBackClick}">‹</button>` : nothing}
    <span>${this.heading()}</span>
  </div>

  ${this.installOpen ? html`
    <div class="live-editor-form">
      <label class="live-editor-field">
        <span>${i18n('liveFoldersManifestAddress')}</span>
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
          <div>${i18nF('liveFoldersReaches',
                       this.describeHosts(this.preview.hosts))}</div>
          ${this.preview.cookieHosts.length ? html`
            <div>${i18nF('liveFoldersCookieHosts',
                         this.describeHosts(this.preview.cookieHosts))}</div>` :
            nothing}
          ${this.preview.secrets.length ? html`
            <div>${i18nF('liveFoldersAsksFor',
                       this.preview.secrets.join(', '))}</div>` :
            nothing}
          <div class="live-editor-hint">
            ${i18nF('liveFoldersFrom', this.preview.source)}
          </div>
        </div>` : nothing}
      ${this.error ? html`
        <div class="live-editor-error" role="alert">${this.error}</div>` :
        nothing}
      <div class="live-editor-buttons">
        <button @click="${this.onInstallCancelClick}">${i18n('cancel')}</button>
        ${this.preview ? html`
          <button class="action" ?disabled="${this.saving}"
              @click="${this.onInstallConfirmClick}">
            ${i18n('liveFoldersInstall')}
          </button>` : html`
          <button class="action" @click="${this.onReviewClick}">
            ${i18n('liveFoldersReview')}
          </button>`}
      </div>
    </div>` : nothing}

  ${!this.installOpen && this.plugin ? html`
    <form class="live-editor-form" @submit="${this.onSubmit}">
      ${!this.folderId ? html`
        <label class="live-editor-field">
          <span>${i18n('liveFoldersName')}</span>
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
          <span>${secret.optional ? i18nF('liveFoldersOptional', secret.label) :
                                   secret.label}</span>
          <input type="password" autocomplete="off"
              placeholder="${secret.hasValue ? i18n('liveFoldersSecretSaved') : ''}"
              .value="${this.secrets[secret.key] ?? ''}"
              data-key="${secret.key}" @input="${this.onSecretInput}">
          ${secret.help ? html`
            <span class="live-editor-hint">${secret.help}</span>` : nothing}
          <span class="live-editor-hint">${i18n('liveFoldersSecretLocal')}</span>
        </label>`)}
      ${this.plugin.cookieHosts.length ? html`
        <label class="live-editor-check">
          <input type="checkbox" .checked="${this.grantCookies}"
              @change="${this.onGrantChange}">
          <span>${i18nF('liveFoldersGrantCookies',
                        this.describeHosts(this.plugin.cookieHosts))}</span>
        </label>` : nothing}
      ${this.error ? html`
        <div class="live-editor-error" role="alert">${this.error}</div>` :
        nothing}
      <div class="live-editor-buttons">
        <button type="button" @click="${this.onCancelClick}">
          ${i18n('cancel')}
        </button>
        <button type="submit" class="action" ?disabled="${this.saving}">
          ${this.folderId ? i18n('save') : i18n('liveFoldersCreate')}
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
          ${i18n('liveFoldersInstallMissing')}
        </button>` : nothing}
      <button @click="${this.onCancelClick}">${i18n('close')}</button>
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
                title="${i18n('liveFoldersLocalTooltip')}">
              ${i18n('liveFoldersLocal')}
            </span>` : nothing}
          ${!plugin.builtin && !plugin.local ? html`
            <button class="live-editor-remove"
                title="${i18nF('liveFoldersRemovePlugin', plugin.name)}"
                aria-label="${i18nF('liveFoldersRemovePlugin', plugin.name)}" data-id="${plugin.id}"
                @click="${this.onRemoveClick}">&times;</button>` : nothing}
        </div>`)}
    </div>
    <div class="live-editor-buttons">
      <button @click="${this.onInstallPluginClick}">
        ${i18n('liveFoldersInstallPlugin')}
      </button>
      <button title="${i18n('liveFoldersOpenFolderTooltip')}"
          @click="${this.onOpenFolderClick}">
        ${i18n('liveFoldersOpenFolder')}
      </button>
      <button @click="${this.onCancelClick}">${i18n('cancel')}</button>
    </div>` : nothing}
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
