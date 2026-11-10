// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The live folder editor: New Live Folder… (pick a plugin, then fill in its
// form) and Edit Source…, and installing plugins (review what one reaches
// and asks for, then install it) or removing them. Local plugins are
// folders put into the plugins folder, which the editor opens; the list
// reads them again whenever it shows. The form is built from
// the plugin's manifest: its config fields, the secrets it asks for (kept on
// this device only) and, if it wants them, consent to send the profile's
// cookies to its hosts (docs/design/live-folders.md, "UI").

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from '../browser_proxy.js';
import type {LiveConfigField, LivePlugin, LivePluginPreview} from '../sidebar.mojom-webui.js';

import {getHtml} from './editor.html.js';

export type FieldValue = string|boolean;

function keyOf(e: Event): string {
  return (e.currentTarget as HTMLElement).dataset['key'] ?? '';
}

function idOf(e: Event): string {
  return (e.currentTarget as HTMLElement).dataset['id'] ?? '';
}

export class CompassLiveFolderEditorElement extends CrLitElement {
  static get is() {
    return 'compass-live-folder-editor';
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      parentId: {type: String},
      folderId: {type: String},
      plugins: {type: Array},
      plugin: {type: Object},
      values: {type: Object},
      secrets: {type: Object},
      grantCookies: {type: Boolean},
      folderTitle: {type: String},
      error: {type: String},
      saving: {type: Boolean},
      installOpen: {type: Boolean},
      installUrl: {type: String},
      preview: {type: Object},
      missingPluginUrl: {type: String},
    };
  }

  // For a new folder: where it goes. For an edit: the folder.
  accessor parentId: string|null = null;
  accessor folderId: string|null = null;
  // What the template shows and edits.
  accessor plugins: LivePlugin[] = [];
  accessor plugin: LivePlugin|null = null;
  accessor values: Record<string, FieldValue> = {};
  accessor secrets: Record<string, string> = {};
  accessor grantCookies: boolean = false;
  accessor folderTitle: string = '';
  accessor error: string = '';
  accessor saving: boolean = false;
  // Installing a plugin: its address, then what it is, for review.
  accessor installOpen: boolean = false;
  accessor installUrl: string = '';
  accessor preview: LivePluginPreview|null = null;
  // The edited folder's plugin isn't on this device: where it comes from.
  accessor missingPluginUrl: string = '';
  private missingPluginSha256_ = '';

  private handler_ = SidebarBrowserProxy.getInstance().handler;
  // Back from the file manager: local plugins may have been added.
  private onWindowFocus_ = () => {
    if (!this.folderId && !this.plugin && !this.installOpen) {
      this.load_();
    }
  };

  override connectedCallback() {
    super.connectedCallback();
    this.load_();
    window.addEventListener('focus', this.onWindowFocus_);
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    window.removeEventListener('focus', this.onWindowFocus_);
  }

  // In the light DOM: the app's styles apply.
  override createRenderRoot() {
    return this;
  }

  private async load_() {
    const {plugins} = await this.handler_.getLivePlugins();
    this.plugins = plugins;
    if (!this.folderId) {
      return;
    }
    const {source} = await this.handler_.getLiveFolderSource(this.folderId);
    if (!source) {
      this.close();
      return;
    }
    this.plugin = plugins.find(p => p.id === source.pluginId) ?? null;
    if (!this.plugin) {
      this.error = 'This folder\'s plugin isn\'t installed on this device.';
      this.missingPluginUrl = source.pluginUrl;
      this.missingPluginSha256_ = source.pluginSha256;
      return;
    }
    this.folderTitle = source.title;
    this.grantCookies = source.cookiesGranted;
    const config = JSON.parse(source.configJson) as Record<string, unknown>;
    const values: Record<string, FieldValue> = {};
    for (const field of this.plugin.config) {
      const value = config[field.key];
      if (typeof value === 'boolean') {
        values[field.key] = value;
      } else if (value !== undefined && value !== null) {
        values[field.key] = String(value);
      }
    }
    this.values = values;
  }

  dialogLabel(): string {
    return this.folderId ? 'Edit live folder' : 'New live folder';
  }

  heading(): string {
    if (this.installOpen) {
      return 'Install a Plugin';
    }
    if (this.folderId) {
      return 'Edit Source';
    }
    return this.plugin ? this.plugin.name : 'New Live Folder';
  }

  // "{config:url}" reads as the address the user enters.
  describeHosts(hosts: string[]): string {
    if (!hosts.length) {
      return 'nothing';
    }
    const named = hosts.map(
        host => host.startsWith('{') ? 'the address you enter' : host);
    return [...new Set(named)].join(', ');
  }

  iconStyle(plugin: LivePlugin): string {
    return plugin.iconUrl ? `--live-icon: url("${plugin.iconUrl}")` : '';
  }

  fieldLabel(field: LiveConfigField): string {
    return field.label + (field.optional ? ' (optional)' : '');
  }

  // The field's value: what the user entered, else the plugin's default.
  fieldValue(field: LiveConfigField): FieldValue|undefined {
    const entered = this.values[field.key];
    if (entered !== undefined) {
      return entered;
    }
    if (!field.defaultJson) {
      return undefined;
    }
    const value = JSON.parse(field.defaultJson) as unknown;
    return typeof value === 'boolean' ? value : String(value);
  }

  fieldText(field: LiveConfigField): string {
    const value = this.fieldValue(field);
    return value === undefined ? '' : String(value);
  }

  isChecked(field: LiveConfigField): boolean {
    const value = this.fieldValue(field);
    return value === true || value === 'true';
  }

  inputType(field: LiveConfigField): string {
    return field.type === 'number' ? 'number' :
        field.type === 'url'       ? 'url' :
                                     'text';
  }

  onTitleInput(e: Event) {
    this.folderTitle = (e.target as HTMLInputElement).value;
  }

  onFieldInput(e: Event) {
    this.values = {
      ...this.values,
      [keyOf(e)]: (e.target as HTMLInputElement | HTMLSelectElement).value,
    };
  }

  onFieldChange(e: Event) {
    this.onFieldInput(e);
  }

  onCheckChange(e: Event) {
    this.values = {
      ...this.values,
      [keyOf(e)]: (e.target as HTMLInputElement).checked,
    };
  }

  onSecretInput(e: Event) {
    this.secrets = {
      ...this.secrets,
      [keyOf(e)]: (e.target as HTMLInputElement).value,
    };
  }

  onGrantChange(e: Event) {
    this.grantCookies = (e.target as HTMLInputElement).checked;
  }

  onPluginClick(e: Event) {
    this.plugin = this.plugins.find(p => p.id === idOf(e)) ?? null;
    this.values = {};
    this.secrets = {};
    this.grantCookies = false;
    this.error = '';
  }

  async onRemoveClick(e: Event) {
    await this.handler_.removeLivePlugin(idOf(e));
    const {plugins} = await this.handler_.getLivePlugins();
    this.plugins = plugins;
  }

  onOpenFolderClick() {
    this.handler_.openLivePluginsFolder();
  }

  onBackClick() {
    this.plugin = null;
    this.error = '';
  }

  onKeydown(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      e.stopPropagation();
      this.close();
    }
  }

  onInstallPluginClick() {
    this.openInstall_('');
  }

  onInstallMissingClick() {
    this.openInstall_(this.missingPluginUrl);
  }

  onInstallUrlInput(e: Event) {
    this.installUrl = (e.target as HTMLInputElement).value;
    this.preview = null;
  }

  onInstallCancelClick() {
    this.installOpen = false;
    this.preview = null;
    this.error = '';
  }

  private openInstall_(url: string) {
    this.installOpen = true;
    this.installUrl = url;
    this.preview = null;
    this.error = '';
    if (url) {
      this.onReviewClick();
    }
  }

  async onReviewClick() {
    this.error = '';
    this.preview = null;
    const {preview, error} =
        await this.handler_.previewLivePlugin(this.installUrl.trim());
    if (!preview) {
      this.error = error;
      return;
    }
    if (this.missingPluginSha256_ &&
        preview.sha256 !== this.missingPluginSha256_) {
      this.error = 'The plugin at that address isn\'t the one this folder ' +
          'was made with: it has changed since. Review it before installing.';
    }
    this.preview = preview;
  }

  async onInstallConfirmClick() {
    const preview = this.preview;
    if (!preview || this.saving) {
      return;
    }
    this.saving = true;
    const {error} =
        await this.handler_.installLivePlugin(preview.source, preview.sha256);
    this.saving = false;
    if (error) {
      this.error = error;
      return;
    }
    this.installOpen = false;
    this.preview = null;
    this.error = '';
    this.missingPluginUrl = '';
    this.missingPluginSha256_ = '';
    // Picks it up: in the list, or for the folder being edited.
    await this.load_();
    if (!this.folderId) {
      this.plugin = this.plugins.find(p => p.id === preview.id) ?? null;
    }
  }

  async onSubmit(e: Event) {
    e.preventDefault();
    const plugin = this.plugin;
    if (!plugin || this.saving) {
      return;
    }
    const config: Record<string, unknown> = {};
    for (const field of plugin.config) {
      const value = this.values[field.key];
      if (value === undefined || value === '') {
        continue;
      }
      config[field.key] = field.type === 'number' && typeof value === 'string' ?
          Number(value) :
          value;
    }
    const secrets: Record<string, string> = {};
    for (const [key, value] of Object.entries(this.secrets)) {
      if (value) {
        secrets[key] = value;
      }
    }
    this.saving = true;
    const {error} = await this.handler_.saveLiveFolder({
      folderId: this.folderId,
      parentId: this.parentId,
      pluginId: plugin.id,
      title: this.folderTitle.trim(),
      configJson: JSON.stringify(config),
      secrets,
      grantCookies: this.grantCookies,
    });
    this.saving = false;
    if (error) {
      this.error = error;
    } else {
      this.close();
    }
  }

  onCancelClick() {
    this.close();
  }

  close() {
    this.fire('close');
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-live-folder-editor': CompassLiveFolderEditorElement;
  }
}

customElements.define(
    CompassLiveFolderEditorElement.is, CompassLiveFolderEditorElement);
