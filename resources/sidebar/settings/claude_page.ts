// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../settings_window/icon.js';

import type {Memory, Model, SettingsHandlerRemote, SettingsState} from '../claude.mojom-webui.js';
import {AuthStyle, Backend, FilesAccess, SettingsHandler} from '../claude.mojom-webui.js';
import type {KnownModel} from '../claude_models.js';
import {knownModels} from '../claude_models.js';
import {i18n, i18nF, i18nPlural} from '../i18n.js';

// A sidebar feature's model, by default (claude_quick.h, kDefaultQuickModel).
const QUICK_MODEL_DEFAULT = 'claude-haiku-5-5';

// What the key field says under it, for a provider other than Anthropic
// (whose text is in the page, with its subscription tokens).
const KEY_HELP: Record<string, string> = {
  openai: i18n('claudePageKeyHelpOpenai'),
  gemini: i18n('claudePageKeyHelpGemini'),
  compatible: i18n('claudePageKeyHelpCompatible'),
};

// Claude in the sidebar (docs/design/ai-sidebar.md): each feature is a row
// of its own switch and the model it asks. A new feature adds an entry.
interface SidebarFeature {
  id: string;
  name: string;
  description: string;
  enabled: (state: SettingsState) => boolean;
  model: (state: SettingsState) => string;
  setEnabled: (handler: SettingsHandlerRemote, on: boolean) => void;
  setModel: (handler: SettingsHandlerRemote, model: string) => void;
  // The model when its setting names none; QUICK_MODEL_DEFAULT if unset.
  defaultModel?: string;
  // Apple Intelligence can answer it ("Answer with").
  onDevice?: boolean;
}

const SIDEBAR_FEATURES: SidebarFeature[] = [
  {
    id: 'nameLinkFolders',
    name: i18n('claudePageNameLinkFolders'),
    description: i18n('claudePageNameLinkFoldersDetail'),
    enabled: state => state.nameLinkFolders,
    model: state => state.nameLinkFoldersModel,
    setEnabled: (handler, on) => handler.setNameLinkFolders(on),
    setModel: (handler, model) => handler.setNameLinkFoldersModel(model),
    onDevice: true,
  },
  {
    id: 'tidyToday',
    name: i18n('claudePageTidyToday'),
    description: i18n('claudePageTidyTodayDetail'),
    enabled: state => state.tidyToday,
    model: state => state.tidyTodayModel,
    setEnabled: (handler, on) => handler.setTidyToday(on),
    setModel: (handler, model) => handler.setTidyTodayModel(model),
    onDevice: true,
  },
  {
    id: 'shortTabTitles',
    name: i18n('claudePageShortTabTitles'),
    description: i18n('claudePageShortTabTitlesDetail'),
    enabled: state => state.shortTabTitles,
    model: state => state.shortTabTitlesModel,
    setEnabled: (handler, on) => handler.setShortTabTitles(on),
    setModel: (handler, model) => handler.setShortTabTitlesModel(model),
    onDevice: true,
  },
  {
    id: 'commandBarAsk',
    name: i18n('claudePageCommandBarAsk'),
    description: i18n('claudePageCommandBarAskDetail'),
    enabled: state => state.commandBarAsk,
    model: state => state.commandBarAskModel,
    setEnabled: (handler, on) => handler.setCommandBarAsk(on),
    setModel: (handler, model) => handler.setCommandBarAskModel(model),
    defaultModel: 'claude-sonnet-5-5',
  },
];

import {getTemplate} from './claude_page.html.js';
import {CompassSettingsPage} from './page_base.js';

// Claude: compass://settings/claude. The key is typed here once and goes to
// the browser, which stores it encrypted; the page only ever gets its hint
// back. Or the chat runs on the user's Claude Code (D8). See
// docs/design/claude.md, "Settings".

export class CompassClaudePageElement extends CompassSettingsPage {
  private handler_ = SettingsHandler.getRemote();
  private state_: SettingsState|null = null;
  // Why Apple Intelligence can't answer ('' if it can); null until asked.
  private onDeviceUnavailable_: string|null = null;
  private checkingOnDevice_ = false;
  // The provider the models below are for; another resets them.
  private provider_ = 'anthropic';
  private models_: Array<Model|KnownModel> = knownModels('anthropic');
  // Test connection listed the models (an OpenAI-compatible server's can
  // then be picked, not only typed).
  private modelsListed_ = false;
  // How many memories Claude keeps, for the Memory row.
  private memoryCount_ = 0;

  protected get pageTitle() {
    return i18n('claudePageTitle');
  }

  protected getPageTemplate() {
    return getTemplate();
  }

  protected setUp() {
    this.$<HTMLSelectElement>('provider')
        .addEventListener('change', async () => {
          // Mojo keeps the order: the state read next is the new
          // provider's.
          this.handler_.setProvider(this.$<HTMLSelectElement>('provider').value);
          for (const id of ['keyNote', 'testNote', 'endpointNote',
                            'serverNote', 'chatgptNote']) {
            this.show(id, false);
          }
          this.$<HTMLInputElement>('apiKey').value = '';
          await this.refresh_();
        });
    this.$('saveServer').addEventListener('click', () => this.saveServer_());
    this.$<HTMLInputElement>('serverUrl').addEventListener('keydown', e => {
      if (e.key === 'Enter') {
        this.saveServer_();
      }
    });
    this.$<HTMLInputElement>('modelId').addEventListener('change', async () => {
      this.handler_.setModel(this.$<HTMLInputElement>('modelId').value);
      await this.refresh_();
    });
    this.$('chatgptSignIn')
        .addEventListener('click', () => this.signInWithChatGPT_());
    this.$('chatgptSignOut').addEventListener('click', async () => {
      const {revoked} = await this.handler_.signOutOfChatGPT();
      this.note_(
          'chatgptNote',
          revoked ? i18n('claudePageSignedOut') :
                    i18n('claudePageSignedOutNotRevoked'),
          !revoked);
      await this.refresh_();
    });
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
      this.note_('keyNote', i18n('claudePageKeyRemoved'), false);
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
      this.note_('claudeCodePathNote', i18n('claudePageSavedDot'), false);
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
    this.$<HTMLSelectElement>('model').addEventListener('change', async () => {
      this.handler_.setModel(this.$<HTMLSelectElement>('model').value);
      await this.refresh_();
    });
    this.setUpSidebarFeatures_();
    this.$('saveEndpoint').addEventListener('click', () => this.saveEndpoint_());
    this.$<HTMLInputElement>('browserScope')
        .addEventListener('change', () => {
          this.handler_.setBrowserScope(
              this.$<HTMLInputElement>('browserScope').checked);
        });
    this.$<HTMLInputElement>('devtools').addEventListener('change', () => {
      this.handler_.setDevTools(this.$<HTMLInputElement>('devtools').checked);
    });
    this.$<HTMLInputElement>('boosts').addEventListener('change', () => {
      this.handler_.setBoosts(this.$<HTMLInputElement>('boosts').checked);
    });
    this.$<HTMLInputElement>('plugins').addEventListener('change', () => {
      this.handler_.setPlugins(this.$<HTMLInputElement>('plugins').checked);
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
      this.$('copyMcpCommand').textContent = i18n('claudePageCopied');
    });
    this.setUpMemory_();
    this.setUpFiles_();
    // The main view's rows open the rest (claude_page.html, data-page-view).
    this.setUpViews(i18n('claudePageBackLabel'));
    this.refresh_();
  }

  // Files (docs/design/claude.md, "Files").
  private setUpFiles_() {
    const choices: Array<[string, FilesAccess]> = [
      ['filesOff', FilesAccess.kOff],
      ['filesSave', FilesAccess.kSave],
      ['filesFolder', FilesAccess.kFolder],
    ];
    for (const [id, access] of choices) {
      this.$<HTMLInputElement>(id).addEventListener('change', async () => {
        this.handler_.setFilesAccess(access);
        await this.refresh_();
        if (access === FilesAccess.kFolder && !this.state_?.filesFolder) {
          await this.handler_.chooseFilesFolder();
          await this.refresh_();
        }
      });
    }
    this.$('chooseFilesFolder').addEventListener('click', async () => {
      await this.handler_.chooseFilesFolder();
      await this.refresh_();
    });
    this.$('clearFilesFolder').addEventListener('click', async () => {
      this.handler_.clearFilesFolder();
      await this.refresh_();
    });
    this.$<HTMLInputElement>('claudeCodeOwnTools')
        .addEventListener('change', () => {
          this.handler_.setClaudeCodeOwnTools(
              this.$<HTMLInputElement>('claudeCodeOwnTools').checked);
        });
  }

  private renderFiles_(state: SettingsState) {
    const access = state.filesAccess;
    this.$<HTMLInputElement>('filesOff').checked = access === FilesAccess.kOff;
    this.$<HTMLInputElement>('filesSave').checked =
        access === FilesAccess.kSave;
    this.$<HTMLInputElement>('filesFolder').checked =
        access === FilesAccess.kFolder;
    this.show('filesFolderRow', access !== FilesAccess.kOff);
    this.show('filesAsk', access !== FilesAccess.kOff);
    const path = this.$('filesFolderPath');
    if (access === FilesAccess.kSave) {
      path.textContent = state.filesFolder ||
          i18nF('claudePageDownloadsFolder', state.defaultSaveFolder);
    } else {
      path.textContent = state.filesFolder || i18n('claudePageNoFolder');
    }
    path.title = path.textContent;
    this.show('clearFilesFolder',
              access === FilesAccess.kSave && state.filesFolder !== '');
    // Never Claude Code with another provider (the state says so).
    const claudeCode = state.backend === Backend.kClaudeCode;
    this.show('ownToolsRow', claudeCode && access === FilesAccess.kFolder);
    const ownTools = this.$<HTMLInputElement>('claudeCodeOwnTools');
    ownTools.checked = state.claudeCodeOwnTools;
    ownTools.disabled = state.filesFolder === '';
  }

  private async refresh_() {
    const {state} = await this.handler_.getState();
    this.state_ = state;
    if (state.provider !== this.provider_) {
      // Another provider's models: its known ones until a test lists them.
      this.provider_ = state.provider;
      this.models_ = knownModels(state.provider);
      this.modelsListed_ = false;
    }
    this.renderProvider_(state);
    const claudeCode = state.backend === Backend.kClaudeCode;
    this.$<HTMLInputElement>('backendApiKey').checked = !claudeCode;
    this.$<HTMLInputElement>('backendClaudeCode').checked = claudeCode;
    this.show('apiKeySection', !claudeCode);
    // An OpenAI-compatible server's URL is above, in its own section.
    this.show('advanced', !claudeCode && state.provider !== 'compatible');
    this.show('claudeCodeSection', claudeCode);
    this.show('claudeCodeNoTools', !state.claudeCodeTools);
    this.$<HTMLInputElement>('claudeCodePath').value = state.claudeCodePath;
    this.renderChatGPT_(state);
    this.show('keySaved', state.hasKey);
    this.show('keyEntry', !state.hasKey);
    this.$('keyHint').textContent = state.keyHint;
    this.$<HTMLInputElement>('baseUrl').value = state.baseUrl;
    this.$<HTMLSelectElement>('authStyle').value =
        state.authStyle === AuthStyle.kBearer ? 'bearer' : 'apiKey';
    this.$<HTMLDetailsElement>('advanced').open = state.baseUrl !== '' ||
        (state.provider === 'anthropic' &&
         state.authStyle === AuthStyle.kBearer);
    this.$<HTMLInputElement>('browserScope').checked = state.browserScope;
    this.$<HTMLInputElement>('devtools').checked = state.devtools;
    this.$<HTMLInputElement>('boosts').checked = state.boosts;
    this.$<HTMLInputElement>('plugins').checked = state.plugins;
    this.renderSidebarFeatures_(state);
    this.$<HTMLInputElement>('acceptTokenRisks').checked =
        state.acceptTokenRisks;
    this.renderMcp_(state);
    this.renderFiles_(state);
    this.renderModels_();
    this.show('memory', state.memorySupported);
    this.$<HTMLInputElement>('memoryEnabled').checked = state.memoryEnabled;
    this.$<HTMLInputElement>('memorySave').checked = state.memorySave;
    this.show('memoryDetails', state.memoryEnabled);
    if (state.memorySupported) {
      await this.refreshMemories_();
    }
    this.renderRows_(state);
  }

  // The main view's rows: what each view has, in a few words.
  private renderRows_(state: SettingsState) {
    const claudeCode = state.backend === Backend.kClaudeCode;
    const provider =
        state.providers.find(p => p.id === state.provider)?.displayName ||
        state.provider;
    const compatible = state.provider === 'compatible';
    const model = compatible ?
        state.model :
        this.$<HTMLSelectElement>('model').selectedOptions[0]?.textContent ||
            state.model;
    const answers = claudeCode ? i18n('claudePageClaudeCodeHere') : provider;
    const ready = claudeCode || state.setUp;
    this.$('connectionRowDetail').textContent = ready ?
        [answers, model].filter(part => part).join(' · ') :
        i18nF('claudePageNotSetUp', answers);

    this.$('sidebarRow').hidden = !state.sidebarSupported;
    const on = SIDEBAR_FEATURES.filter(f => f.enabled(state)).length;
    const onDevice = state.onDeviceSupported && state.sidebarOnDevice;
    this.$('sidebarRowDetail').textContent =
        (on ? i18nF('claudePageFeaturesOn', on, SIDEBAR_FEATURES.length) :
              i18n('claudePageOff')) +
        (onDevice ? ' · Apple Intelligence' : '');

    const files = this.$('filesRowDetail');
    if (state.filesAccess === FilesAccess.kOff) {
      files.textContent = i18n('claudePageOff');
    } else if (state.filesAccess === FilesAccess.kSave) {
      files.textContent = i18n('claudePageSavesNewFiles');
    } else {
      files.textContent = state.filesFolder || i18n('claudePageNoFolder');
    }

    this.$('memoryRow').hidden = !state.memorySupported;
    this.renderMemoryRow_();
  }

  private renderMemoryRow_() {
    const enabled = this.state_?.memoryEnabled;
    const count = this.memoryCount_;
    let detail = i18n('claudePageOff');
    if (enabled) {
      detail = i18nPlural('claudePageMemoryOn', count);
    }
    this.$('memoryRowDetail').textContent = detail;
  }

  // The provider picker, and what follows it: Anthropic's own options
  // (Claude Code, subscription tokens, the auth style) only for Anthropic,
  // an OpenAI-compatible server's URL and typed model only for one.
  private renderProvider_(state: SettingsState) {
    const select = this.$<HTMLSelectElement>('provider');
    select.replaceChildren(...state.providers.map(provider => {
      const option = document.createElement('option');
      option.value = provider.id;
      option.textContent = provider.displayName;
      return option;
    }));
    select.value = state.provider;
    this.$('privacyNote').textContent = state.privacyNote;
    const anthropic = state.provider === 'anthropic';
    const compatible = state.provider === 'compatible';
    this.show('backendSection', anthropic);
    this.show('tokenRisksRow', anthropic);
    this.show('authStyleRow', anthropic);
    this.show('keyHelpAnthropic', anthropic);
    this.show('keyHelpOther', !anthropic);
    this.$('keyHelpOther').textContent = KEY_HELP[state.provider] || '';
    this.$<HTMLInputElement>('apiKey').placeholder = state.keyPlaceholder;
    this.show('endpointHintAnthropic', anthropic);
    this.show('endpointHintOther', !anthropic);
    this.$<HTMLInputElement>('baseUrl').placeholder = state.defaultBaseUrl;
    this.show('serverSection', compatible);
    this.$<HTMLInputElement>('serverUrl').value =
        compatible ? state.baseUrl : '';
    const modelId = this.$<HTMLInputElement>('modelId');
    this.show('modelId', compatible);
    modelId.value = compatible ? state.model : '';
    this.$('modelLabel').setAttribute('for', compatible ? 'modelId' : 'model');
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
      if (!window.confirm(i18n('claudePageConfirmDeleteAll'))) {
        return;
      }
      this.handler_.removeAllMemories();
      await this.refreshMemories_();
    });
  }

  private async refreshMemories_() {
    const {memories} = await this.handler_.getMemories();
    this.memoryCount_ = memories.length;
    this.renderMemoryRow_();
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
    edit.textContent = i18n('edit');
    const remove = document.createElement('button');
    remove.className = 'secondary';
    remove.textContent = i18n('delete');
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
      edit.textContent = i18n('save');
      text.replaceWith(input);
      input.focus();
    });
    item.append(text, edit, remove);
    return item;
  }

  // Claude in the sidebar (docs/design/ai-sidebar.md): a row per feature,
  // its switch and its model picker (the chat's models).
  private setUpSidebarFeatures_() {
    const rows = this.$('sidebarFeatureRows');
    for (const feature of SIDEBAR_FEATURES) {
      const label = document.createElement('label');
      label.className = 'claude-scope';
      const input = document.createElement('input');
      input.type = 'checkbox';
      input.id = feature.id;
      input.addEventListener(
          'change', () => feature.setEnabled(this.handler_, input.checked));
      const text = document.createElement('span');
      const name = document.createElement('span');
      name.className = 'name';
      name.textContent = feature.name;
      const description = document.createElement('span');
      description.className = 'hint block';
      description.textContent = feature.description;
      text.append(name, description);
      label.append(input, text);

      const modelRow = document.createElement('div');
      modelRow.className = 'feature-model';
      modelRow.id = `${feature.id}ModelRow`;
      const modelLabel = document.createElement('label');
      modelLabel.htmlFor = `${feature.id}Model`;
      modelLabel.textContent = i18n('claudePageModel');
      const select = document.createElement('select');
      select.id = `${feature.id}Model`;
      select.addEventListener(
          'change', () => feature.setModel(this.handler_, select.value));
      modelRow.append(modelLabel, select);
      rows.append(label, modelRow);
    }
    const answerWith = async (onDevice: boolean) => {
      // Mojo keeps the order: the state read next has the change.
      this.handler_.setSidebarOnDevice(onDevice);
      this.onDeviceUnavailable_ = null;
      await this.refresh_();
    };
    this.$('sidebarChat').addEventListener('change', () => answerWith(false));
    this.$('sidebarOnDevice')
        .addEventListener('change', () => answerWith(true));
  }

  // Each switch is off, and can't be turned on, until what answers it can:
  // the provider, set up (a key, a gateway's, or an OpenAI-compatible
  // server's URL; Claude Code mode doesn't answer these requests), or with
  // "Answer with" Apple Intelligence, available (checkOnDevice(), whose
  // answer renders again). Apple Intelligence's features have no model.
  private renderSidebarFeatures_(state: SettingsState) {
    this.show('sidebarFeatures', state.sidebarSupported);
    this.show('sidebarAnswerWith', state.onDeviceSupported);
    const onDevice = state.onDeviceSupported && state.sidebarOnDevice;
    this.$<HTMLInputElement>('sidebarChat').checked = !onDevice;
    this.$<HTMLInputElement>('sidebarOnDevice').checked = onDevice;
    const deviceReady = onDevice && this.onDeviceUnavailable_ === '';
    let needsKey = false;
    for (const feature of SIDEBAR_FEATURES) {
      const local = onDevice && !!feature.onDevice;
      const usable = local ? deviceReady : state.setUp;
      needsKey = needsKey || (!local && !state.setUp);
      const input = this.$<HTMLInputElement>(feature.id);
      input.checked = feature.enabled(state) && usable;
      input.disabled = !usable;
      this.$<HTMLSelectElement>(`${feature.id}Model`).disabled = !usable;
      this.show(`${feature.id}ModelRow`, !local);
    }
    this.show('onDeviceUnavailable', onDevice && !deviceReady);
    this.$('onDeviceUnavailable').textContent = this.onDeviceUnavailable_ ??
        i18n('claudePageCheckingAppleIntelligence');
    this.show('sidebarNeedsKey', needsKey);
    this.$('sidebarNeedsKey').textContent = onDevice ?
        i18n('claudePageNeedsKeyCommandBar') :
        state.onDeviceSupported ? i18n('claudePageNeedsKeyOrDevice') :
                                  i18n('claudePageNeedsKey');
    if (onDevice && this.onDeviceUnavailable_ === null &&
        !this.checkingOnDevice_) {
      this.checkOnDevice_();
    }
  }

  // Asks whether Apple Intelligence can answer, then shows the switches
  // again. Each visit to the page asks once; a change of "Answer with"
  // asks again.
  private async checkOnDevice_() {
    this.checkingOnDevice_ = true;
    const {unavailable} = await this.handler_.checkOnDevice();
    this.checkingOnDevice_ = false;
    this.onDeviceUnavailable_ = unavailable;
    if (this.state_) {
      this.renderSidebarFeatures_(this.state_);
    }
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
    this.$('copyMcpCommand').textContent = i18n('copy');
    this.show('mcpClients', state.mcpAllowedClients.length > 0);
    this.$('mcpClientList').replaceChildren(
        ...state.mcpAllowedClients.map(client => {
          const item = document.createElement('li');
          const name = document.createElement('span');
          name.textContent = client;
          const forget = document.createElement('button');
          forget.className = 'secondary';
          forget.textContent = i18n('claudePageForget');
          forget.addEventListener('click', async () => {
            this.handler_.forgetMcpClient(client);
            await this.refresh_();
          });
          item.append(name, forget);
          return item;
        }));
  }

  // The models a test listed, or the provider's known ones, in the chat's
  // picker and each sidebar feature's; the chosen one is always there, also
  // a custom one (a gateway's). An OpenAI-compatible server knows its models
  // only once a test lists them (the chat's is typed until then), and its
  // features can ask the chat's model ("Same as chat", empty).
  private renderModels_() {
    const anthropic = this.provider_ === 'anthropic';
    const compatible = this.provider_ === 'compatible';
    this.show('model', !compatible || this.modelsListed_);
    this.renderModelSelect_(
        'model', this.state_?.model || this.models_[0]?.id || '');
    for (const feature of SIDEBAR_FEATURES) {
      const chosen = this.state_ ? feature.model(this.state_) : '';
      this.renderModelSelect_(
          `${feature.id}Model`,
          anthropic ? chosen || feature.defaultModel || QUICK_MODEL_DEFAULT :
                      chosen,
          /*sameAsChat=*/ compatible);
    }
  }

  private renderModelSelect_(id: string, chosen: string,
                             sameAsChat: boolean = false) {
    const select = this.$<HTMLSelectElement>(id);
    const models = [...this.models_];
    if (chosen && !models.some(m => m.id === chosen)) {
      models.unshift({id: chosen, displayName: chosen});
    }
    if (sameAsChat) {
      models.unshift({id: '', displayName: i18n('claudePageSameAsChat')});
    }
    select.replaceChildren(...models.map(model => {
      const option = document.createElement('option');
      option.value = model.id;
      option.textContent = !model.id || model.displayName === model.id ?
          model.displayName :
          `${model.displayName} (${model.id})`;
      return option;
    }));
    select.value = chosen;
  }

  // Sign in with ChatGPT (docs/design/ai-providers.md): OpenAI only.
  private renderChatGPT_(state: SettingsState) {
    const openai = state.provider === 'openai';
    this.show('chatgptSection', openai);
    this.show('chatgptSignedOut', !state.chatgptSignedIn);
    this.show('chatgptSignedIn', state.chatgptSignedIn);
    this.$('chatgptAccount').textContent = state.chatgptEmail ?
        i18nF('claudePageSignedInAs', state.chatgptEmail) :
        i18n('claudePageSignedIn');
    if (openai && state.chatgptNote) {
      this.note_('chatgptNote', state.chatgptNote, true);
    }
  }

  private async signInWithChatGPT_() {
    const button = this.$<HTMLButtonElement>('chatgptSignIn');
    button.disabled = true;
    this.note_('chatgptNote', i18n('claudePageFinishSignIn'), false);
    const {error} = await this.handler_.signInWithChatGPT();
    button.disabled = false;
    if (error) {
      this.note_('chatgptNote', error, true);
      return;
    }
    this.show('chatgptNote', false);
    this.modelsListed_ = false;
    await this.refresh_();
    // The plan's own models.
    await this.test_();
  }

  private async saveKey_() {
    const input = this.$<HTMLInputElement>('apiKey');
    const key = input.value.trim();
    if (!key) {
      this.note_('keyNote', i18n('claudePagePasteKey'), true);
      return;
    }
    this.$<HTMLButtonElement>('saveKey').disabled = true;
    const {saved} = await this.handler_.setApiKey(key);
    this.$<HTMLButtonElement>('saveKey').disabled = false;
    input.value = '';
    if (!saved) {
      this.note_('keyNote', i18n('claudePageKeyNotStored'), true);
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
      this.note_('endpointNote', i18n('claudePageBadUrl'), true);
      return;
    }
    this.note_('endpointNote', i18n('claudePageSavedDot'), false);
    await this.refresh_();
  }

  // An OpenAI-compatible server's URL: empty clears it (no server yet).
  private async saveServer_() {
    const url = this.$<HTMLInputElement>('serverUrl').value.trim();
    const {ok} = await this.handler_.setEndpoint(url, AuthStyle.kApiKey);
    if (!ok) {
      this.note_('serverNote', i18n('claudePageBadUrl'), true);
      return;
    }
    this.note_(
        'serverNote',
        url ? i18n('claudePageSavedDot') : i18n('claudePageServerRemoved'),
        false);
    await this.refresh_();
  }

  private async test_() {
    const button = this.$<HTMLButtonElement>('test');
    button.disabled = true;
    this.note_('testNote', i18n('claudePageConnecting'), false);
    const provider = this.provider_;
    const {result} = await this.handler_.testConnection();
    button.disabled = false;
    if (provider !== this.provider_) {
      // The provider changed meanwhile: these are another's models.
      return;
    }
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
      this.modelsListed_ = true;
    }
    this.renderModels_();
    this.note_(
        'testNote', i18nPlural('claudePageConnected', result.models.length),
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
