// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {Memory, Model, SettingsState} from '../claude.mojom-webui.js';
import {AuthStyle, Backend, SettingsHandler} from '../claude.mojom-webui.js';

import {KNOWN_MODELS} from '../claude_models.js';

import {getTemplate} from './claude_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Claude: compass://settings/claude. The key is typed here once and goes to
// the browser, which stores it encrypted; the page only ever gets its hint
// back. Or the chat runs on the user's Claude Code (D8). See
// docs/design/claude.md, "Settings".

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
    for (const id of ['backendApiKey', 'backendClaudeCode']) {
      this.$<HTMLInputElement>(id).addEventListener('change', async () => {
        const claudeCode =
            this.$<HTMLInputElement>('backendClaudeCode').checked;
        this.handler_.setBackend(
            claudeCode ? Backend.kClaudeCode : Backend.kApiKey);
        this.show('testNote', false);
        await this.refresh_();
        if (claudeCode) {
          // Say right away whether it's there and signed in.
          await this.test_();
        }
      });
    }
    const savePath = async () => {
      this.handler_.setClaudeCodePath(
          this.$<HTMLInputElement>('claudeCodePath').value);
      this.note_('claudeCodePathNote', 'Saved.', false);
      await this.refresh_();
      await this.test_();
    };
    this.$('saveClaudeCodePath').addEventListener('click', savePath);
    this.$<HTMLInputElement>('claudeCodePath')
        .addEventListener('keydown', e => {
          if (e.key === 'Enter') {
            savePath();
          }
        });
    this.$<HTMLSelectElement>('model').addEventListener('change', () => {
      this.handler_.setModel(this.$<HTMLSelectElement>('model').value);
    });
    this.$('saveEndpoint').addEventListener('click', () => this.saveEndpoint_());
    this.$<HTMLInputElement>('browserScope')
        .addEventListener('change', () => {
          this.handler_.setBrowserScope(
              this.$<HTMLInputElement>('browserScope').checked);
        });
    this.$<HTMLInputElement>('acceptTokenRisks')
        .addEventListener('change', () => {
          this.handler_.setAcceptTokenRisks(
              this.$<HTMLInputElement>('acceptTokenRisks').checked);
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
    this.setUpMemory_();
    this.refresh_();
  }

  private async refresh_() {
    const {state} = await this.handler_.getState();
    this.state_ = state;
    const claudeCode = state.backend === Backend.kClaudeCode;
    this.$<HTMLInputElement>('backendApiKey').checked = !claudeCode;
    this.$<HTMLInputElement>('backendClaudeCode').checked = claudeCode;
    this.show('apiKeySection', !claudeCode);
    this.show('advanced', !claudeCode);
    this.show('claudeCodeSection', claudeCode);
    this.show('claudeCodeNoTools', !state.claudeCodeTools);
    this.$<HTMLInputElement>('claudeCodePath').value = state.claudeCodePath;
    this.show('keySaved', state.hasKey);
    this.show('keyEntry', !state.hasKey);
    this.$('keyHint').textContent = state.keyHint;
    this.$<HTMLInputElement>('baseUrl').value = state.baseUrl;
    this.$<HTMLSelectElement>('authStyle').value =
        state.authStyle === AuthStyle.kBearer ? 'bearer' : 'apiKey';
    this.$<HTMLDetailsElement>('advanced').open =
        state.baseUrl !== '' || state.authStyle === AuthStyle.kBearer;
    this.$<HTMLInputElement>('browserScope').checked = state.browserScope;
    this.$<HTMLInputElement>('acceptTokenRisks').checked =
        state.acceptTokenRisks;
    this.renderMcp_(state);
    this.renderModels_();
    this.show('memory', state.memorySupported);
    this.$<HTMLInputElement>('memoryEnabled').checked = state.memoryEnabled;
    this.$<HTMLInputElement>('memorySave').checked = state.memorySave;
    this.show('memoryDetails', state.memoryEnabled);
    if (state.memorySupported) {
      await this.refreshMemories_();
    }
  }

  private setUpMemory_() {
    this.$<HTMLInputElement>('memoryEnabled')
        .addEventListener('change', async () => {
          this.handler_.setMemoryEnabled(
              this.$<HTMLInputElement>('memoryEnabled').checked);
          await this.refresh_();
        });
    this.$<HTMLInputElement>('memorySave').addEventListener('change', () => {
      this.handler_.setMemorySave(
          this.$<HTMLInputElement>('memorySave').checked);
    });
    const add = async () => {
      const input = this.$<HTMLInputElement>('memoryNew');
      if (!input.value.trim()) {
        return;
      }
      const {error} = await this.handler_.addMemory(input.value);
      if (error) {
        this.note_('memoryNote', error, true);
        return;
      }
      input.value = '';
      this.show('memoryNote', false);
      await this.refreshMemories_();
    };
    this.$('addMemory').addEventListener('click', add);
    this.$<HTMLInputElement>('memoryNew').addEventListener('keydown', e => {
      if (e.key === 'Enter') {
        add();
      }
    });
    this.$('removeAllMemories').addEventListener('click', async () => {
      if (!window.confirm(
              'Delete everything Claude remembers about you, on all your ' +
              'devices?')) {
        return;
      }
      this.handler_.removeAllMemories();
      await this.refreshMemories_();
    });
  }

  private async refreshMemories_() {
    const {memories} = await this.handler_.getMemories();
    this.show('memoryEmpty', memories.length === 0);
    this.show('removeAllMemories', memories.length > 0);
    this.$('memoryList')
        .replaceChildren(...memories.map(memory => this.memoryItem_(memory)));
  }

  // A memory: its text, Edit (in place) and Delete.
  private memoryItem_(memory: Memory): HTMLLIElement {
    const item = document.createElement('li');
    const text = document.createElement('span');
    text.textContent = memory.text;
    const edit = document.createElement('button');
    edit.className = 'secondary';
    edit.textContent = 'Edit';
    const remove = document.createElement('button');
    remove.className = 'secondary';
    remove.textContent = 'Delete';
    remove.addEventListener('click', async () => {
      this.handler_.removeMemory(memory.id);
      await this.refreshMemories_();
    });
    let input: HTMLInputElement|null = null;
    const save = async () => {
      const {error} =
          await this.handler_.updateMemory(memory.id, input?.value || '');
      if (error) {
        this.note_('memoryNote', error, true);
        return;
      }
      this.show('memoryNote', false);
      await this.refreshMemories_();
    };
    edit.addEventListener('click', () => {
      if (input) {
        save();
        return;
      }
      input = document.createElement('input');
      input.type = 'text';
      input.maxLength = 500;
      input.value = memory.text;
      input.addEventListener('keydown', e => {
        if (e.key === 'Enter') {
          save();
        } else if (e.key === 'Escape') {
          this.refreshMemories_();
        }
      });
      edit.textContent = 'Save';
      text.replaceWith(input);
      input.focus();
    });
    item.append(text, edit, remove);
    return item;
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
    if (result.detail) {
      // Claude Code: the models it takes are Claude's own.
      this.note_('testNote', result.detail, false);
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
