// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {LiveConfigField, LivePlugin, LiveSecretField} from '../sidebar.mojom-webui.js';

import {i18n, i18nF} from '../i18n.js';

import type {CompassLiveFolderEditorAppElement} from './app.js';

export function getHtml(this: CompassLiveFolderEditorAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
${this.loaded ? html`
<header>
  ${this.hasBack() ? html`
    <button id="back" class="back" title="${i18n('back')}"
        aria-label="${i18n('back')}" @click="${this.onHeaderBackClick}">
      <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M7.5 2L3.5 6l4 4"></path></svg>
    </button>` : nothing}
  <h1>${this.heading()}</h1>
  <button id="close" title="${i18n('close')}" aria-label="${i18n('close')}"
      @click="${this.onCancelClick}">
    <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M2 2l8 8M10 2l-8 8"></path></svg>
  </button>
</header>
<main role="dialog" aria-label="${this.dialogLabel()}">

  ${this.installOpen ? html`
    <div class="card">
      <label class="field">
        <span class="label">${i18n('liveFoldersManifestAddress')}</span>
        <input id="installUrl" type="url" .value="${this.installUrl}"
            placeholder="https://example.com/plugin/manifest.json"
            @input="${this.onInstallUrlInput}">
      </label>
    </div>
    ${this.preview ? html`
      <div class="card padded preview">
        <div class="plugin-name">
          ${this.preview.name} ${this.preview.version}
        </div>
        <div class="hint">${this.preview.description}</div>
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
        <div class="hint">${i18nF('liveFoldersFrom', this.preview.source)}</div>
      </div>` : nothing}
    ${this.error ? html`
      <div class="error" role="alert">${this.error}</div>` : nothing}
    <div class="actions">
      <span class="spacer"></span>
      <button class="pill" @click="${this.onInstallCancelClick}">
        ${i18n('cancel')}
      </button>
      ${this.preview ? html`
        <button id="install" class="pill primary" ?disabled="${this.saving}"
            @click="${this.onInstallConfirmClick}">
          ${i18n('liveFoldersInstall')}
        </button>` : html`
        <button id="review" class="pill primary"
            @click="${this.onReviewClick}">
          ${i18n('liveFoldersReview')}
        </button>`}
    </div>` : nothing}

  ${!this.installOpen && this.plugin ? html`
    <form id="form" @submit="${this.onSubmit}">
      <div class="card">
        ${!this.folderId ? html`
          <label class="field">
            <span class="label">${i18n('liveFoldersName')}</span>
            <input id="title" type="text" .value="${this.folderTitle}"
                placeholder="${this.plugin.name}"
                @input="${this.onTitleInput}">
          </label>` : nothing}
        ${this.plugin.config.map((field: LiveConfigField) => html`
          ${field.type === 'bool' ? html`
            <label class="check">
              <input type="checkbox" .checked="${this.isChecked(field)}"
                  data-key="${field.key}" @change="${this.onCheckChange}">
              <span>${this.fieldLabel(field)}</span>
            </label>` : nothing}
          ${field.type === 'select' ? html`
            <label class="field">
              <span class="label">${this.fieldLabel(field)}</span>
              <select data-key="${field.key}" @change="${this.onFieldChange}">
                ${field.options.map(option => html`
                  <option value="${option.value}"
                      ?selected="${option.value === this.fieldValue(field)}">
                    ${option.label}
                  </option>`)}
              </select>
            </label>` : nothing}
          ${field.type !== 'bool' && field.type !== 'select' ? html`
            <label class="field">
              <span class="label">${this.fieldLabel(field)}</span>
              <input type="${this.inputType(field)}"
                  .value="${this.fieldText(field)}"
                  placeholder="${field.placeholder}"
                  min="${field.min ?? nothing}" max="${field.max ?? nothing}"
                  data-key="${field.key}" @input="${this.onFieldInput}">
            </label>` : nothing}`)}
      </div>
      ${this.plugin.secrets.length ? html`
        <div class="card">
          ${this.plugin.secrets.map((secret: LiveSecretField) => html`
            <label class="field">
              <span class="label">${secret.optional ?
                  i18nF('liveFoldersOptional', secret.label) : secret.label}</span>
              <input type="password" autocomplete="off"
                  placeholder="${secret.hasValue ? i18n('liveFoldersSecretSaved') : ''}"
                  .value="${this.secrets[secret.key] ?? ''}"
                  data-key="${secret.key}" @input="${this.onSecretInput}">
              ${secret.help ? html`
                <span class="hint">${secret.help}</span>` : nothing}
            </label>`)}
        </div>
        <div class="footnote">${i18n('liveFoldersSecretLocal')}</div>` :
        nothing}
      ${this.plugin.cookieHosts.length ? html`
        <div class="card">
          <label class="check">
            <input id="grantCookies" type="checkbox"
                .checked="${this.grantCookies}"
                @change="${this.onGrantChange}">
            <span>${i18nF('liveFoldersGrantCookies',
                          this.describeHosts(this.plugin.cookieHosts))}</span>
          </label>
        </div>` : nothing}
      ${this.error ? html`
        <div class="error" role="alert">${this.error}</div>` : nothing}
      <div class="actions">
        <span class="spacer"></span>
        <button type="button" class="pill" @click="${this.onCancelClick}">
          ${i18n('cancel')}
        </button>
        <button id="save" type="submit" class="pill primary"
            ?disabled="${this.saving}">
          ${this.folderId ? i18n('save') : i18n('liveFoldersCreate')}
        </button>
      </div>
    </form>` : nothing}

  ${!this.installOpen && !this.plugin && this.folderId ? html`
    ${this.error ? html`
      <div class="error" role="alert">${this.error}</div>` : nothing}
    <div class="actions">
      <span class="spacer"></span>
      ${this.missingPluginUrl ? html`
        <button class="pill primary" @click="${this.onInstallMissingClick}">
          ${i18n('liveFoldersInstallMissing')}
        </button>` : nothing}
      <button class="pill" @click="${this.onCancelClick}">${i18n('close')}</button>
    </div>` : nothing}

  ${!this.installOpen && !this.plugin && !this.folderId ? html`
    <div id="plugins" class="card" role="list">
      ${this.plugins.map((plugin: LivePlugin) => html`
        <div class="plugin-row" role="listitem">
          <button class="live-editor-plugin" data-id="${plugin.id}"
              @click="${this.onPluginClick}">
            <span class="live-icon" aria-hidden="true"
                style="${this.iconStyle(plugin)}"></span>
            <span class="plugin-text">
              <span class="plugin-name">${plugin.name}</span>
              <span class="hint">${plugin.description}</span>
            </span>
          </button>
          ${plugin.local ? html`
            <span class="local" title="${i18n('liveFoldersLocalTooltip')}">
              ${i18n('liveFoldersLocal')}
            </span>` : nothing}
          ${!plugin.builtin && !plugin.local ? html`
            <button class="remove"
                title="${i18nF('liveFoldersRemovePlugin', plugin.name)}"
                aria-label="${i18nF('liveFoldersRemovePlugin', plugin.name)}"
                data-id="${plugin.id}" @click="${this.onRemoveClick}">
              <svg viewBox="0 0 12 12" aria-hidden="true"><path d="M3 3l6 6M9 3l-6 6"></path></svg>
            </button>` : nothing}
        </div>`)}
    </div>
    <div class="actions">
      <button id="installPlugin" class="pill"
          @click="${this.onInstallPluginClick}">
        ${i18n('liveFoldersInstallPlugin')}
      </button>
      <button id="openFolder" class="pill"
          title="${i18n('liveFoldersOpenFolderTooltip')}"
          @click="${this.onOpenFolderClick}">
        ${i18n('liveFoldersOpenFolder')}
      </button>
    </div>` : nothing}
</main>` : ''}
<!--_html_template_end_-->`;
  // clang-format on
}
