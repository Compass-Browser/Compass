// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html, nothing} from '//resources/lit/v3_0/lit.rollup.js';

import type {LiveConfigField, LivePlugin} from '../sidebar.mojom-webui.js';

import type {CompassLiveFolderEditorElement} from './editor.js';

function iconStyle(plugin: LivePlugin): string {
  return plugin.iconUrl ? `--live-icon: url("${plugin.iconUrl}")` : '';
}

function renderPicker(el: CompassLiveFolderEditorElement) {
  // clang-format off
  return html`
<div class="live-editor-plugins" role="list">
  ${el.plugins.map(plugin => html`
    <button class="live-editor-plugin" role="listitem"
        @click="${() => el.pick(plugin)}">
      <span class="live-icon" aria-hidden="true"
          style="${iconStyle(plugin)}"></span>
      <span class="live-editor-plugin-text">
        <span class="live-editor-plugin-name">${plugin.name}</span>
        <span class="live-editor-hint">${plugin.description}</span>
      </span>
    </button>`)}
</div>
<div class="live-editor-buttons">
  <button @click="${el.close}">Cancel</button>
</div>`;
  // clang-format on
}

function renderField(
    el: CompassLiveFolderEditorElement, field: LiveConfigField) {
  const value = el.fieldValue(field);
  const label = field.label + (field.optional ? ' (optional)' : '');
  // clang-format off
  switch (field.type) {
    case 'bool':
      return html`
<label class="live-editor-check">
  <input type="checkbox" .checked="${value === true || value === 'true'}"
      @change="${(e: Event) =>
          el.setValue(field, (e.target as HTMLInputElement).checked)}">
  <span>${label}</span>
</label>`;
    case 'select':
      return html`
<label class="live-editor-field">
  <span>${label}</span>
  <select @change="${(e: Event) =>
      el.setValue(field, (e.target as HTMLSelectElement).value)}">
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
      @input="${(e: Event) =>
          el.setValue(field, (e.target as HTMLInputElement).value)}">
</label>`;
  }
  // clang-format on
}

function renderForm(
    el: CompassLiveFolderEditorElement, plugin: LivePlugin, editing: boolean) {
  // clang-format off
  return html`
<form class="live-editor-form" @submit="${el.onSubmit}">
  ${!editing ? html`
    <label class="live-editor-field">
      <span>Name</span>
      <input type="text" .value="${el.folderTitle}" placeholder="${plugin.name}"
          @input="${(e: Event) =>
              el.folderTitle = (e.target as HTMLInputElement).value}">
    </label>` : nothing}
  ${plugin.config.map(field => renderField(el, field))}
  ${plugin.secrets.map(secret => html`
    <label class="live-editor-field">
      <span>${secret.label}${secret.optional ? ' (optional)' : ''}</span>
      <input type="password" autocomplete="off"
          placeholder="${secret.hasValue ? 'Saved: leave blank to keep' : ''}"
          .value="${el.secrets[secret.key] ?? ''}"
          @input="${(e: Event) => el.setSecret(
              secret.key, (e.target as HTMLInputElement).value)}">
      ${secret.help ? html`
        <span class="live-editor-hint">${secret.help}</span>` : nothing}
      <span class="live-editor-hint">Kept on this device only.</span>
    </label>`)}
  ${plugin.cookieHosts.length ? html`
    <label class="live-editor-check">
      <input type="checkbox" .checked="${el.grantCookies}"
          @change="${(e: Event) => el.grantCookies =
              (e.target as HTMLInputElement).checked}">
      <span>Send your cookies for ${el.describeHosts(plugin)}, so it can see
        what you see signed in</span>
    </label>` : nothing}
  ${el.error ? html`
    <div class="live-editor-error" role="alert">${el.error}</div>` : nothing}
  <div class="live-editor-buttons">
    <button type="button" @click="${el.close}">Cancel</button>
    <button type="submit" class="action" ?disabled="${el.saving}">
      ${editing ? 'Save' : 'Create'}
    </button>
  </div>
</form>`;
  // clang-format on
}

export function getHtml(this: CompassLiveFolderEditorElement) {
  const editing = !!this.folderId;
  const plugin = this.plugin;
  // clang-format off
  return html`<!--_html_template_start_-->
<div class="live-editor" role="dialog"
    aria-label="${editing ? 'Edit live folder' : 'New live folder'}"
    @keydown="${this.onKeydown}">
  <div class="live-editor-header">
    ${plugin && !editing ? html`
      <button class="live-editor-back" title="Back" aria-label="Back"
          @click="${this.onBackClick}">‹</button>` : nothing}
    <span>${editing ? 'Edit Source' : plugin ? plugin.name :
                                               'New Live Folder'}</span>
  </div>
  ${plugin ? renderForm(this, plugin, editing) : renderPicker(this)}
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
