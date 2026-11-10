// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The live folder editor: New Live Folder… (pick a plugin, then fill in its
// form) and Edit Source…. The form is built from the plugin's manifest:
// its config fields, the secrets it asks for (kept on this device only) and,
// if it wants them, consent to send the profile's cookies to its hosts
// (docs/design/live-folders.md, "UI").

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {SidebarBrowserProxy} from '../browser_proxy.js';
import type {LiveConfigField, LivePlugin} from '../sidebar.mojom-webui.js';

import {getHtml} from './editor.html.js';

export type FieldValue = string|boolean;

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

  private handler_ = SidebarBrowserProxy.getInstance().handler;

  override connectedCallback() {
    super.connectedCallback();
    this.load_();
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

  // The field's value: what the user entered, else the plugin's default.
  valueOf(field: LiveConfigField): FieldValue|undefined {
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

  setValue(field: LiveConfigField, value: FieldValue) {
    this.values = {...this.values, [field.key]: value};
  }

  setSecret(key: string, value: string) {
    this.secrets = {...this.secrets, [key]: value};
  }

  // "{config:url}" reads as the site the folder shows.
  describeHosts(plugin: LivePlugin): string {
    const named = plugin.cookieHosts.map(
        host => host.startsWith('{') ? 'the site you enter' : host);
    return [...new Set(named)].join(', ');
  }

  pick(plugin: LivePlugin) {
    this.plugin = plugin;
    this.values = {};
    this.secrets = {};
    this.grantCookies = false;
    this.error = '';
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
