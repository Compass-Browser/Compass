// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {Model, SettingsState} from '../claude.mojom-webui.js';
import {AuthStyle, SettingsHandler} from '../claude.mojom-webui.js';

import {getTemplate} from './claude_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Claude: compass://settings/claude. The key is typed here once and goes to
// the browser, which stores it encrypted; the page only ever gets its hint
// back. See docs/design/claude.md, "Settings".

// Offered before a connection test lists what the key can use.
const KNOWN_MODELS: Model[] = [
  {id: 'claude-opus-5-5', displayName: 'Claude Opus 5.5'},
  {id: 'claude-sonnet-5-5', displayName: 'Claude Sonnet 5.5'},
  {id: 'claude-haiku-5-5', displayName: 'Claude Haiku 5.5'},
  {id: 'claude-fable-5-1', displayName: 'Claude Fable 5.1'},
];

export class CompassClaudePageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  private state_: SettingsState|null = null;
  private models_: Model[] = KNOWN_MODELS;

  protected get pageTitle() {
    return 'Claude';
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$('saveKey').addEventListener('click', () => this.saveKey_());
    this.$<HTMLInputElement>('apiKey').addEventListener('keydown', e => {
      if (e.key === 'Enter') {
        this.saveKey_();
      }
    });
    this.$('replaceKey').addEventListener('click', () => {
      this.show('keySaved', false);
      this.show('keyEntry', true);
      this.$<HTMLInputElement>('apiKey').focus();
    });
    this.$('removeKey').addEventListener('click', async () => {
      await this.handler_.setApiKey('');
      this.note_('keyNote', 'Key removed.', false);
      await this.refresh_();
    });
    this.$('test').addEventListener('click', () => this.test_());
    this.$<HTMLSelectElement>('model').addEventListener('change', () => {
      this.handler_.setModel(this.$<HTMLSelectElement>('model').value);
    });
    this.$('saveEndpoint').addEventListener('click', () => this.saveEndpoint_());
    this.$<HTMLInputElement>('browserScope')
        .addEventListener('change', () => {
          this.handler_.setBrowserScope(
              this.$<HTMLInputElement>('browserScope').checked);
        });
    this.$<HTMLInputElement>('mcpEnabled').addEventListener('change', async () => {
      // Mojo keeps the order: the state read next has the change.
      this.handler_.setMcpEnabled(
          this.$<HTMLInputElement>('mcpEnabled').checked);
      await this.refresh_();
    });
    this.$('copyMcpCommand').addEventListener('click', () => {
      navigator.clipboard.writeText(this.$('mcpCommand').textContent || '');
      this.$('copyMcpCommand').textContent = 'Copied';
    });
    this.refresh_();
  }

  private async refresh_() {
    const {state} = await this.handler_.getState();
    this.state_ = state;
    this.show('keySaved', state.hasKey);
    this.show('keyEntry', !state.hasKey);
    this.$('keyHint').textContent = state.keyHint;
    this.$<HTMLInputElement>('baseUrl').value = state.baseUrl;
    this.$<HTMLSelectElement>('authStyle').value =
        state.authStyle === AuthStyle.kBearer ? 'bearer' : 'apiKey';
    this.$<HTMLDetailsElement>('advanced').open =
        state.baseUrl !== '' || state.authStyle === AuthStyle.kBearer;
    this.$<HTMLInputElement>('browserScope').checked = state.browserScope;
    this.renderMcp_(state);
    this.renderModels_();
  }

  // "Allow MCP clients", how to add Compass to a client, the clients
  // always allowed.
  private renderMcp_(state: SettingsState) {
    this.show('mcp', state.mcpSupported);
    this.show('mcpUnsupported', !state.mcpSupported);
    this.$<HTMLInputElement>('mcpEnabled').checked = state.mcpEnabled;
    this.show('mcpSetup', state.mcpEnabled);
    this.$('mcpCommand').textContent = state.mcpCommand;
    this.$('mcpBinary').textContent =
        state.mcpCommand.replace(/^claude mcp add compass -- /, '');
    this.$('copyMcpCommand').textContent = 'Copy';
    this.show('mcpClients', state.mcpAllowedClients.length > 0);
    this.$('mcpClientList').replaceChildren(
        ...state.mcpAllowedClients.map(client => {
          const item = document.createElement('li');
          const name = document.createElement('span');
          name.textContent = client;
          const forget = document.createElement('button');
          forget.className = 'secondary';
          forget.textContent = 'Forget';
          forget.addEventListener('click', async () => {
            this.handler_.forgetMcpClient(client);
            await this.refresh_();
          });
          item.append(name, forget);
          return item;
        }));
  }

  // The models a test listed, or the known ones; the chosen one is always
  // there, also a custom one (a gateway's).
  private renderModels_() {
    const select = this.$<HTMLSelectElement>('model');
    const chosen = this.state_?.model || KNOWN_MODELS[0]!.id;
    const models = [...this.models_];
    if (!models.some(m => m.id === chosen)) {
      models.unshift({id: chosen, displayName: chosen});
    }
    select.replaceChildren(...models.map(model => {
      const option = document.createElement('option');
      option.value = model.id;
      option.textContent = model.displayName === model.id ?
          model.id :
          `${model.displayName} (${model.id})`;
      return option;
    }));
    select.value = chosen;
  }

  private async saveKey_() {
    const input = this.$<HTMLInputElement>('apiKey');
    const key = input.value.trim();
    if (!key) {
      this.note_('keyNote', 'Paste a key first.', true);
      return;
    }
    this.$<HTMLButtonElement>('saveKey').disabled = true;
    const {saved} = await this.handler_.setApiKey(key);
    this.$<HTMLButtonElement>('saveKey').disabled = false;
    input.value = '';
    if (!saved) {
      this.note_(
          'keyNote',
          'The key couldn\'t be stored: the system\'s secure storage ' +
              'refused it.',
          true);
      return;
    }
    this.show('keyNote', false);
    await this.refresh_();
    // A new key: check it right away.
    await this.test_();
  }

  private async saveEndpoint_() {
    const baseUrl = this.$<HTMLInputElement>('baseUrl').value.trim();
    const authStyle = this.$<HTMLSelectElement>('authStyle').value ===
            'bearer' ?
        AuthStyle.kBearer :
        AuthStyle.kApiKey;
    const {ok} = await this.handler_.setEndpoint(baseUrl, authStyle);
    if (!ok) {
      this.note_(
          'endpointNote',
          'Use an https:// URL (http:// only to this computer), without a ' +
              'query.',
          true);
      return;
    }
    this.note_('endpointNote', 'Saved.', false);
    await this.refresh_();
  }

  private async test_() {
    const button = this.$<HTMLButtonElement>('test');
    button.disabled = true;
    this.note_('testNote', 'Connecting…', false);
    const {result} = await this.handler_.testConnection();
    button.disabled = false;
    if (!result.ok) {
      this.note_('testNote', result.error, true);
      return;
    }
    if (result.models.length) {
      this.models_ = result.models;
    }
    this.renderModels_();
    this.note_(
        'testNote',
        `Connected: ${result.models.length} model${
            result.models.length === 1 ? '' : 's'} available.`,
        false);
  }

  private note_(id: string, text: string, error: boolean) {
    const note = this.$(id);
    note.textContent = text;
    note.classList.toggle('error', error);
    note.classList.toggle('ok', !error);
    note.hidden = false;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-claude-page': CompassClaudePageElement;
  }
}

customElements.define('compass-claude-page', CompassClaudePageElement);
