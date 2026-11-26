// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {i18n, i18nF, i18nPlural} from '../i18n.js';
import {SettingsHandler} from '../sidebar.mojom-webui.js';
import type {LiveConfigField, LivePluginPreview, PluginSettings} from '../sidebar.mojom-webui.js';

import {CompassSettingsPage} from './page_base.js';
import {getTemplate} from './plugins_page.html.js';

// The labels' keys in loadTimeData.
const SOURCE_LABELS: Record<string, string> = {
  builtin: 'pluginsPageSourceBuiltin',
  installed: 'pluginsPageSourceInstalled',
  local: 'pluginsPageSourceLocal',
  claude: 'pluginsPageSourceClaude',
};

// What a plugin reaches, its hosts with "the address you enter" for those
// the user types in.
function hostList(hosts: string[]): string {
  return hosts.join(', ').replace(
      /\{(config|secret):[^}]+\}/g, i18n('pluginsPageAddressYouEnter'));
}

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
    return i18n('pluginsPageTitle');
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
            i18nF(
                'pluginsPageMeta',
                SOURCE_LABELS[plugin.source] ?
                    i18n(SOURCE_LABELS[plugin.source]!) :
                    plugin.source,
                plugin.version)));
    summary.append(name);
    if (plugin.folder) {
      summary.append(
          element('span', 'plugin-chip', i18n('pluginsPageLiveFolder')));
    }
    if (plugin.preview) {
      summary.append(
          element('span', 'plugin-chip', i18n('pluginsPageTabPreview')));
    }
    if (plugin.badge) {
      summary.append(element('span', 'plugin-chip', i18n('pluginsPageBadge')));
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
      body.append(element('h3', undefined, i18n('pluginsPageTabPreview')));
      body.append(element(
          'p', 'plugin-hint',
          i18nF(
              plugin.badge ? 'pluginsPagePreviewBadgeHint' :
                             'pluginsPagePreviewHint',
              plugin.previewSites.join(', '))));
      body.append(this.renderToggle_(
          i18n('pluginsPageShowPreviews'), plugin.previewEnabled,
          enabled => this.handler_.setPluginPreviewEnabled(plugin.id, enabled),
          `preview-${plugin.id}`));
      if (plugin.sessionSite) {
        const reach = plugin.relatedHosts.length ? ' ' +
                i18nF('pluginsPageSessionAlsoReaches',
                      plugin.relatedHosts.join(', ')) :
                                                   '';
        body.append(this.renderToggle_(
            i18nF('pluginsPageUseSession', plugin.sessionSite),
            plugin.sessionGranted,
            granted =>
                this.handler_.setPluginSessionGranted(plugin.id, granted),
            `session-${plugin.id}`));
        body.append(element(
            'p', 'plugin-hint',
            i18nF('pluginsPageSessionHint', plugin.sessionSite) + reach));
      }
      if (plugin.previewConfig.length) {
        body.append(this.renderConfig_(plugin));
      }
    }

    if (plugin.folder) {
      body.append(element('h3', undefined, i18n('pluginsPageLiveFolders')));
      const facts = [
        i18nPlural('pluginsPageUsedBy', plugin.folderCount),
      ];
      if (plugin.hosts.length) {
        facts.push(i18nF('pluginsPageReaches', hostList(plugin.hosts)));
      }
      if (plugin.cookieHosts.length) {
        facts.push(i18n('pluginsPageCanUseLogin'));
      }
      body.append(list(facts));
    }

    body.append(element('h3', undefined, i18n('pluginsPageDeveloper')));
    const actions = element('div', 'plugins-actions');
    const viewSource =
        element('button', 'secondary', i18n('pluginsPageViewSource'));
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
      const remove = element('button', 'secondary', i18n('remove'));
      remove.addEventListener('click', () => this.remove_(plugin));
      actions.append(remove);
    }
    body.append(actions, source);
    if (plugin.log.length) {
      body.append(
          element('p', 'plugin-hint', i18n('pluginsPageLastLog')),
          element('pre', 'plugin-log', plugin.log.join('\n')));
    }
    if (plugin.sourceUrl) {
      body.append(element(
          'p', 'plugin-hint', i18nF('pluginsPageFrom', plugin.sourceUrl)));
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
    const save = element('button', undefined, i18n('save'));
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
      status.textContent = error || i18n('pluginsPageSaved');
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
    const question = plugin.folderCount ?
        i18nPlural(
            'pluginsPageConfirmRemoveUsed', plugin.folderCount, plugin.name) :
        i18nF('pluginsPageConfirmRemove', plugin.name);
    if (!confirm(question)) {
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
      this.note_(error || i18n('pluginsPageNoPlugin'));
      return;
    }
    this.reviewed_ = preview;
    const review = this.$('installReview');
    const facts: string[] = [];
    if (preview.previewSites.length) {
      facts.push(
          i18nF('pluginsPageReviewReads', preview.previewSites.join(', ')));
    }
    if (preview.sessionSite) {
      facts.push(i18nF('pluginsPageReviewSession', preview.sessionSite));
    }
    if (preview.relatedHosts.length) {
      facts.push(i18nF(
          'pluginsPageReviewAlsoReaches', preview.relatedHosts.join(', ')));
    }
    if (preview.hosts.length) {
      facts.push(
          i18nF('pluginsPageReviewFoldersReach', hostList(preview.hosts)));
    }
    if (preview.cookieHosts.length) {
      facts.push(i18n('pluginsPageReviewFoldersLogin'));
    }
    if (preview.secrets.length) {
      facts.push(i18nF('pluginsPageReviewAsksFor', preview.secrets.join(', ')));
    }
    const install =
        element('button', undefined, i18n('pluginsPageInstallButton'));
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
