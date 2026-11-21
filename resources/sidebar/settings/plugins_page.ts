// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {SettingsHandler} from '../sidebar.mojom-webui.js';
import type {LiveConfigField, LivePluginPreview, PluginSettings} from '../sidebar.mojom-webui.js';

import {CompassSettingsPage} from './page_base.js';
import {getTemplate} from './plugins_page.html.js';

const SOURCE_LABELS: Record<string, string> = {
  builtin: 'Built-in',
  installed: 'Installed',
  local: 'Local',
  claude: 'Made by Claude',
};

function element<K extends keyof HTMLElementTagNameMap>(
    tag: K, className?: string, text?: string): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  if (className) {
    el.className = className;
  }
  if (text !== undefined) {
    el.textContent = text;
  }
  return el;
}

function list(items: string[]): HTMLUListElement {
  const ul = element('ul', 'plugin-facts');
  for (const item of items) {
    ul.append(element('li', undefined, item));
  }
  return ul;
}

// Plugins: compass://settings/plugins (docs/design/settings.md, "Plugins").
// Every live folder and tab preview plugin: what it does and reaches, its
// tab preview's settings (on or off, its options, the tab's session), its
// source, and installing or removing plugins.
export class CompassPluginsPageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  // Rows the user opened stay open across refreshes.
  private open_ = new Set<string>();
  private reviewed_: LivePluginPreview|null = null;

  protected get pageTitle() {
    return 'Plugins';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$<HTMLInputElement>('tabPreviewsEnabled')
        .addEventListener('change', async e => {
          await this.handler_.setTabPreviewsEnabled(
              (e.target as HTMLInputElement).checked);
        });
    this.$('installPlugin').addEventListener('click', () => {
      this.show('installForm', true);
      this.$<HTMLInputElement>('installUrl').focus();
    });
    this.$('installCancel').addEventListener('click', () => this.endInstall_());
    this.$('installForm').addEventListener('submit', e => {
      e.preventDefault();
      this.review_();
    });
    this.$('openPluginsFolder')
        .addEventListener('click', () => this.handler_.openPluginsFolder());
    this.$('makeWithClaude')
        .addEventListener('click', () => this.handler_.makePluginWithClaude());
    // A deep link opens one plugin: compass://settings/plugins?id=<id>.
    const id = new URLSearchParams(location.search).get('id');
    if (id) {
      this.open_.add(id);
    }
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') {
        this.refresh_();
      }
    });
    this.refresh_();
  }

  private async refresh_() {
    const {previewsEnabled, plugins, claudeCanMake} =
        await this.handler_.getPlugins();
    this.$<HTMLInputElement>('tabPreviewsEnabled').checked = previewsEnabled;
    this.show('makeWithClaude', claudeCanMake);
    this.$('pluginList')
        .replaceChildren(...plugins.map(p => this.renderPlugin_(p)));
  }

  private note_(text: string) {
    const note = this.$('pluginsNote');
    note.textContent = text;
    note.hidden = !text;
  }

  private renderPlugin_(plugin: PluginSettings): HTMLElement {
    const row = element('li', 'plugin');
    row.dataset['id'] = plugin.id;
    const details = element('details');
    details.open = this.open_.has(plugin.id);
    details.addEventListener('toggle', () => {
      if (details.open) {
        this.open_.add(plugin.id);
      } else {
        this.open_.delete(plugin.id);
      }
    });

    const summary = element('summary');
    if (plugin.iconUrl) {
      const icon = element('img', 'plugin-icon');
      icon.src = plugin.iconUrl;
      icon.alt = '';
      summary.append(icon);
    } else {
      summary.append(element('span', 'plugin-icon'));
    }
    const name = element('span', 'plugin-name');
    name.append(
        element('span', undefined, plugin.name),
        element(
            'span', 'plugin-meta',
            `${SOURCE_LABELS[plugin.source] ?? plugin.source} · ` +
                `version ${plugin.version}`));
    summary.append(name);
    if (plugin.folder) {
      summary.append(element('span', 'plugin-chip', 'Live folder'));
    }
    if (plugin.preview) {
      summary.append(element('span', 'plugin-chip', 'Tab preview'));
    }
    if (plugin.badge) {
      summary.append(element('span', 'plugin-chip', 'Badge'));
    }
    details.append(summary, this.renderBody_(plugin));
    row.append(details);
    return row;
  }

  private renderBody_(plugin: PluginSettings): HTMLElement {
    const body = element('div', 'plugin-body');
    if (plugin.description) {
      body.append(element('p', 'plugin-hint', plugin.description));
    }

    if (plugin.preview) {
      body.append(element('h3', undefined, 'Tab preview'));
      body.append(element(
          'p', 'plugin-hint',
          `Reads pages on ${plugin.previewSites.join(', ')} to show what's ` +
              'new when you hover their tabs' +
              (plugin.badge ? ', and a count on their tabs in the sidebar' :
                              '') +
              '. It runs apart from the page and can\'t send what it reads ' +
              'anywhere.'));
      body.append(this.renderToggle_(
          'Show its previews', plugin.previewEnabled,
          enabled => this.handler_.setPluginPreviewEnabled(plugin.id, enabled),
          `preview-${plugin.id}`));
      if (plugin.sessionSite) {
        const reach = plugin.relatedHosts.length ?
            ` It can also reach ${plugin.relatedHosts.join(', ')}, without ` +
                'your login there.' :
            '';
        body.append(this.renderToggle_(
            `Advanced: use your session on ${plugin.sessionSite}`,
            plugin.sessionGranted,
            granted => this.handler_.setPluginSessionGranted(plugin.id, granted),
            `session-${plugin.id}`));
        body.append(element(
            'p', 'plugin-hint',
            `With this on, it can make requests to ${plugin.sessionSite} as ` +
                'you, for what the page doesn\'t show (or when the tab ' +
                `isn't loaded).${reach}`));
      }
      if (plugin.previewConfig.length) {
        body.append(this.renderConfig_(plugin));
      }
    }

    if (plugin.folder) {
      body.append(element('h3', undefined, 'Live folders'));
      const facts = [
        plugin.folderCount === 1 ? 'Used by 1 folder' :
                                   `Used by ${plugin.folderCount} folders`,
      ];
      if (plugin.hosts.length) {
        facts.push(`Reaches ${plugin.hosts.join(', ')}`.replace(
            /\{(config|secret):[^}]+\}/g, 'the address you enter'));
      }
      if (plugin.cookieHosts.length) {
        facts.push('Can use your login on those sites, if you allow it per ' +
                   'folder');
      }
      body.append(list(facts));
    }

    body.append(element('h3', undefined, 'Developer'));
    const actions = element('div', 'plugins-actions');
    const viewSource = element('button', 'secondary', 'View source');
    const source = element('pre', 'plugin-source');
    source.hidden = true;
    viewSource.addEventListener('click', async () => {
      if (!source.hidden) {
        source.hidden = true;
        return;
      }
      const {manifest, entry} = await this.handler_.getPluginSource(plugin.id);
      source.textContent =
          `// manifest.json\n${manifest}\n\n// Entry script\n${entry}`;
      source.hidden = false;
    });
    actions.append(viewSource);
    if (plugin.source !== 'builtin') {
      const remove = element('button', 'secondary', 'Remove');
      remove.addEventListener('click', () => this.remove_(plugin));
      actions.append(remove);
    }
    body.append(actions, source);
    if (plugin.log.length) {
      body.append(element('p', 'plugin-hint', 'Its last preview logged:'),
                  element('pre', 'plugin-log', plugin.log.join('\n')));
    }
    if (plugin.sourceUrl) {
      body.append(element('p', 'plugin-hint', `From ${plugin.sourceUrl}`));
    }
    return body;
  }

  private renderToggle_(
      label: string, checked: boolean,
      change: (value: boolean) => Promise<unknown>, id: string): HTMLElement {
    const row = element('div', 'plugins-row');
    const input = element('input');
    input.type = 'checkbox';
    input.id = `plugin-toggle-${id}`;
    input.checked = checked;
    input.addEventListener('change', async () => {
      await change(input.checked);
      await this.refresh_();
    });
    const text = element('label', undefined, label);
    text.htmlFor = input.id;
    row.append(text, input);
    return row;
  }

  // The preview's options, as the live folder editor builds a folder's.
  private renderConfig_(plugin: PluginSettings): HTMLElement {
    const form = element('form');
    let values: Record<string, unknown> = {};
    try {
      values = JSON.parse(plugin.previewConfigJson || '{}');
    } catch {
      values = {};
    }
    const inputs = new Map<string, HTMLInputElement|HTMLSelectElement>();
    for (const field of plugin.previewConfig) {
      const row = element('label', 'plugin-field');
      row.append(element('span', undefined, field.label));
      const input = this.renderField_(field, values[field.key]);
      inputs.set(field.key, input);
      row.append(input);
      form.append(row);
    }
    const save = element('button', undefined, 'Save');
    save.type = 'submit';
    const status = element('span', 'plugin-hint');
    const actions = element('div', 'plugins-actions');
    actions.append(save, status);
    form.append(actions);
    form.addEventListener('submit', async e => {
      e.preventDefault();
      const config: Record<string, unknown> = {};
      for (const field of plugin.previewConfig) {
        const input = inputs.get(field.key)!;
        if (field.type === 'bool') {
          config[field.key] = (input as HTMLInputElement).checked;
        } else if (field.type === 'number') {
          if (input.value !== '') {
            config[field.key] = Number(input.value);
          }
        } else if (input.value !== '') {
          config[field.key] = input.value;
        }
      }
      const {error} = await this.handler_.setPluginPreviewConfig(
          plugin.id, JSON.stringify(config));
      status.textContent = error || 'Saved';
    });
    return form;
  }

  private renderField_(field: LiveConfigField, value: unknown):
      HTMLInputElement|HTMLSelectElement {
    let fallback: unknown = undefined;
    try {
      fallback = field.defaultJson ? JSON.parse(field.defaultJson) : undefined;
    } catch {
      fallback = undefined;
    }
    const current = value === undefined ? fallback : value;
    if (field.type === 'select') {
      const select = element('select');
      for (const option of field.options) {
        const o = element('option', undefined, option.label);
        o.value = option.value;
        o.selected = option.value === current;
        select.append(o);
      }
      return select;
    }
    const input = element('input');
    if (field.type === 'bool') {
      input.type = 'checkbox';
      input.checked = current === true;
      return input;
    }
    input.type = field.type === 'number' ? 'number' :
        field.type === 'url'            ? 'url' :
                                          'text';
    if (field.min !== null && field.min !== undefined) {
      input.min = String(field.min);
    }
    if (field.max !== null && field.max !== undefined) {
      input.max = String(field.max);
    }
    input.placeholder = field.placeholder;
    input.value = current === undefined || current === null ? '' :
                                                               String(current);
    return input;
  }

  private async remove_(plugin: PluginSettings) {
    const folders = plugin.folderCount ?
        ` ${plugin.folderCount} live folder${
            plugin.folderCount === 1 ? '' : 's'} use it and will stop ` +
            'refreshing.' :
        '';
    if (!confirm(`Remove ${plugin.name}?${folders}`)) {
      return;
    }
    await this.handler_.removePlugin(plugin.id);
    await this.refresh_();
  }

  private endInstall_() {
    this.reviewed_ = null;
    this.show('installForm', false);
    this.show('installReview', false);
    this.$<HTMLInputElement>('installUrl').value = '';
    this.note_('');
  }

  // Shows what a plugin reaches before installing it.
  private async review_() {
    const url = this.$<HTMLInputElement>('installUrl').value.trim();
    if (!url) {
      return;
    }
    this.note_('');
    const {preview, error} = await this.handler_.previewPlugin(url);
    if (!preview) {
      this.note_(error || 'That address doesn\'t have a plugin.');
      return;
    }
    this.reviewed_ = preview;
    const review = this.$('installReview');
    const facts: string[] = [];
    if (preview.previewSites.length) {
      facts.push(`Reads pages on ${preview.previewSites.join(', ')} to show ` +
                 'a tab preview');
    }
    if (preview.sessionSite) {
      facts.push(`Can use your session on ${preview.sessionSite}, if you ` +
                 'allow it');
    }
    if (preview.relatedHosts.length) {
      facts.push(`Can also reach ${preview.relatedHosts.join(', ')}`);
    }
    if (preview.hosts.length) {
      facts.push(`Live folders reach ${preview.hosts.join(', ')}`.replace(
          /\{(config|secret):[^}]+\}/g, 'the address you enter'));
    }
    if (preview.cookieHosts.length) {
      facts.push('Live folders can use your login, if you allow it per folder');
    }
    if (preview.secrets.length) {
      facts.push(`Asks for: ${preview.secrets.join(', ')}`);
    }
    const install = element('button', undefined, 'Install');
    install.addEventListener('click', async () => {
      if (!this.reviewed_) {
        return;
      }
      const {error} = await this.handler_.installPlugin(
          this.reviewed_.source, this.reviewed_.sha256);
      if (error) {
        this.note_(error);
        return;
      }
      this.endInstall_();
      await this.refresh_();
    });
    review.replaceChildren(
        element('strong', undefined,
                `${preview.name} ${preview.version}`),
        element('p', 'plugin-hint', preview.description), list(facts),
        install);
    review.hidden = false;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-plugins-page': CompassPluginsPageElement;
  }
}

customElements.define('compass-plugins-page', CompassPluginsPageElement);
