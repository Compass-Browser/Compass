// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import {i18n, i18nF} from '../i18n.js';

import type {ContentBlock, MemoryView, ToolView, Turn} from './conversation.js';
import {renderMarkdown} from './markdown.js';
import {getCss} from './turn.css.js';
import {getHtml} from './turn.html.js';

// One message of the chat: the user's, with the page it carried, or
// Claude's, rendered from its content blocks as they stream in. The turn is
// changed in place; |revision| tells the element to render it again. Fires
// "retry" from a failed reply's Retry button, and "undo-memory" (with the
// tool_use id) from a memory chip's Undo.
export class CompassClaudeTurnElement extends CrLitElement {
  static get is() {
    return 'compass-claude-turn';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      turn: {type: Object},
      tools: {type: Object},
      revision: {type: Number},
    };
  }

  accessor turn: Turn|null = null;
  // The tool calls' states, by id (the conversation's).
  accessor tools: Map<string, ToolView> = new Map();
  accessor revision: number = 0;

  protected isUser_(): boolean {
    return this.turn?.role === 'user';
  }

  protected getText_(): string {
    return this.turn?.role === 'user' ? this.turn.text : '';
  }

  protected getPageTitle_(): string {
    return this.turn?.role === 'user' ? this.turn.pageTitle || '' : '';
  }

  protected getPageUrl_(): string {
    return this.turn?.role === 'user' ? this.turn.pageUrl || '' : '';
  }

  protected getBlocks_(): ContentBlock[] {
    return this.turn?.role === 'assistant' ? this.turn.content : [];
  }

  // The last block of a reply still streaming.
  protected isLive_(index: number): boolean {
    return this.turn?.role === 'assistant' && this.turn.streaming &&
        index === this.turn.content.length - 1;
  }

  protected showThinking_(item: ContentBlock, index: number): boolean {
    return item.type === 'thinking' && (!!item.thinking || this.isLive_(index));
  }

  protected isPending_(): boolean {
    return this.turn?.role === 'assistant' && this.turn.streaming &&
        !this.turn.content.length;
  }

  protected getStopReason_(): string {
    return this.turn?.role === 'assistant' ? this.turn.stopReason || '' : '';
  }

  protected getError_(): string {
    return this.turn?.role === 'assistant' ? this.turn.error || '' : '';
  }

  protected renderText_(text: string|undefined): DocumentFragment {
    return renderMarkdown(text || '');
  }

  protected getTool_(item: ContentBlock): ToolView {
    return this.tools.get(item.id || '') || {running: false};
  }

  // What a memory tool changed, once it has: shown as a chip, not a tool.
  protected getMemory_(item: ContentBlock): MemoryView|undefined {
    return item.type === 'tool_use' ? this.getTool_(item).memory : undefined;
  }

  protected getMemoryLabel_(item: ContentBlock): string {
    const memory = this.getMemory_(item);
    if (!memory) {
      return '';
    }
    if (memory.boost || memory.plugin) {
      const kind = memory.kind === 'deleted' ? 'Deleted' :
          memory.kind === 'updated'          ? 'Updated' :
                                               'Saved';
      return i18nF(
          `claudeChat${memory.boost ? 'Boost' : 'Plugin'}${kind}`,
          memory.text);
    }
    const kind = memory.kind === 'forgot' ? 'Forgot' :
        memory.kind === 'updated'         ? 'Updated' :
                                            'Saved';
    return i18nF(`claudeChatMemory${kind}`, memory.text);
  }

  protected onUndoMemoryClick_(e: Event) {
    this.fire('undo-memory', (e.currentTarget as HTMLElement).dataset['id']);
  }

  // A tool call, in a few words.
  protected getToolLabel_(item: ContentBlock): string {
    const input = (item.input || {}) as Record<string, unknown>;
    const short = (value: unknown, max = 48) => {
      const text = String(value ?? '');
      return text.length > max ? text.slice(0, max - 1) + '…' : text;
    };
    // The message |key| ("Click"), |key| + 'Ref' for an element ("Click
    // e12") or |key| + 'At' for a point ("Click at 10, 20").
    const where = (key: string) => input['ref'] ?
        i18nF(key + 'Ref', String(input['ref'])) :
        Array.isArray(input['coordinate']) ?
        i18nF(key + 'At', input['coordinate'].join(', ')) :
        i18n(key);
    switch (item.name) {
      case 'read_page':
        return i18n('claudeChatToolReadPage');
      case 'get_page_text':
        return i18n('claudeChatToolGetPageText');
      case 'find':
        return i18nF('claudeChatToolFind', short(input['query']));
      case 'screenshot':
        return i18n('claudeChatToolScreenshot');
      case 'read_console':
        return i18n('claudeChatToolReadConsole');
      case 'run_js':
        return i18n('claudeChatToolRunJs');
      case 'click':
        return where('claudeChatToolClick');
      case 'hover':
        return where('claudeChatToolHover');
      case 'type':
        return i18nF('claudeChatToolType', short(input['text']));
      case 'key':
        return i18nF('claudeChatToolKey', short(input['keys']));
      case 'scroll':
        return i18nF('claudeChatToolScroll', String(input['direction'] || ''))
            .trim();
      case 'form_input':
        return where('claudeChatToolFill');
      case 'navigate':
        return i18nF('claudeChatToolNavigate', short(input['url']));
      case 'wait':
        return i18nF('claudeChatToolWait', String(input['seconds'] ?? ''));
      case 'remember':
        return i18nF('claudeChatToolRemember', short(input['text']));
      case 'update_memory':
        return i18nF('claudeChatToolUpdateMemory', short(input['text']));
      case 'forget':
        return i18n('claudeChatToolForget');
      case 'list_boosts':
        return input['host'] ?
            i18nF('claudeChatToolListBoostsFor', short(input['host'])) :
            i18n('claudeChatToolListBoosts');
      case 'save_boost':
        return i18nF('claudeChatToolSaveBoost', short(input['match']));
      case 'delete_boost':
        return i18n('claudeChatToolDeleteBoost');
      case 'list_plugins':
        return i18n('claudeChatToolListPlugins');
      case 'get_plugin':
        return i18nF('claudeChatToolGetPlugin', short(input['id']));
      case 'test_plugin':
        return input['mode'] === 'folder' ?
            i18n('claudeChatToolTestFolderPlugin') :
            i18n('claudeChatToolTestPreviewPlugin');
      case 'save_plugin':
        return i18n('claudeChatToolSavePlugin');
      case 'delete_plugin':
        return i18nF('claudeChatToolDeletePlugin', short(input['id']));
      case 'save_file':
        return i18nF('claudeChatToolSaveFile', short(input['name']));
      case 'list_files':
        return input['path'] ?
            i18nF('claudeChatToolListPath', short(input['path'])) :
            i18n('claudeChatToolListFolder');
      case 'read_file':
        return i18nF('claudeChatToolReadFile', short(input['path']));
      case 'write_file':
        return i18nF('claudeChatToolWriteFile', short(input['path']));
      case 'network_requests':
        return i18n('claudeChatToolNetworkRequests');
      case 'network_response':
        return i18nF('claudeChatToolNetworkResponse', short(input['id']));
      case 'inspect_element':
        return i18nF(
            'claudeChatToolInspect', short(input['ref'] || input['selector']));
      case 'page_storage':
        return i18n('claudeChatToolPageStorage');
      case 'edit_page_storage':
        return input['action'] === 'clear' ?
            i18nF('claudeChatToolClearStorage', short(input['store'])) :
            i18nF(
                input['action'] === 'set' ? 'claudeChatToolSetStorage' :
                                            'claudeChatToolRemoveStorage',
                short(input['name']), short(input['store']));
      case 'performance':
        return i18n('claudeChatToolPerformance');
      case 'emulate':
        return input['reset'] ?
            i18n('claudeChatToolStopEmulating') :
            i18nF('claudeChatToolEmulate', short(input['device'] || ''))
                .trim();
      // Claude Code's own tools.
      case 'Bash':
        return i18nF('claudeChatToolRun', short(input['command']));
      case 'Read':
        return i18nF('claudeChatToolReadFile', short(input['file_path']));
      case 'Write':
        return i18nF('claudeChatToolWriteFile', short(input['file_path']));
      case 'Edit':
        return i18nF('claudeChatToolEditFile', short(input['file_path']));
      case 'Glob':
      case 'Grep':
        return i18nF('claudeChatToolSearchFiles', short(input['pattern']));
      case 'WebFetch':
        return i18nF('claudeChatToolFetch', short(input['url']));
      case 'WebSearch':
        return i18nF('claudeChatToolSearchWeb', short(input['query']));
      default:
        return item.name || i18n('claudeChatToolGeneric');
    }
  }

  protected getToolInput_(item: ContentBlock): string {
    return JSON.stringify(item.input ?? {}, null, 2);
  }

  protected onRetryClick_() {
    this.fire('retry');
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-claude-turn': CompassClaudeTurnElement;
  }
}

customElements.define(CompassClaudeTurnElement.is, CompassClaudeTurnElement);
