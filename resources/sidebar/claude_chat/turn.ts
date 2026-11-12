// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

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
    const verb = memory.kind === 'forgot' ? 'Forgot' :
        memory.kind === 'updated'         ? 'Updated memory' :
                                            'Saved to memory';
    return `${verb}: ${memory.text}`;
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
    const where = () => input['ref'] ? ` ${input['ref']}` :
        Array.isArray(input['coordinate']) ?
                                       ` at ${input['coordinate'].join(', ')}` :
                                       '';
    switch (item.name) {
      case 'read_page':
        return 'Read the page';
      case 'get_page_text':
        return 'Read the page\'s text';
      case 'find':
        return `Find “${short(input['query'])}”`;
      case 'screenshot':
        return 'Screenshot';
      case 'read_console':
        return 'Read the console';
      case 'run_js':
        return 'Run JavaScript';
      case 'click':
        return `Click${where()}`;
      case 'hover':
        return `Point${where()}`;
      case 'type':
        return `Type “${short(input['text'])}”`;
      case 'key':
        return `Press ${short(input['keys'])}`;
      case 'scroll':
        return `Scroll ${input['direction'] || ''}`;
      case 'form_input':
        return `Fill${where()}`;
      case 'navigate':
        return `Go to ${short(input['url'])}`;
      case 'wait':
        return `Wait ${input['seconds'] ?? ''} s`;
      case 'remember':
        return `Remember “${short(input['text'])}”`;
      case 'update_memory':
        return `Update memory “${short(input['text'])}”`;
      case 'forget':
        return 'Forget a memory';
      case 'save_file':
        return `Save “${short(input['name'])}”`;
      case 'list_files':
        return input['path'] ? `List “${short(input['path'])}”` :
                               'List the folder';
      case 'read_file':
        return `Read “${short(input['path'])}”`;
      case 'write_file':
        return `Write “${short(input['path'])}”`;
      // Claude Code's own tools.
      case 'Bash':
        return `Run ${short(input['command'])}`;
      case 'Read':
        return `Read “${short(input['file_path'])}”`;
      case 'Write':
        return `Write “${short(input['file_path'])}”`;
      case 'Edit':
        return `Edit “${short(input['file_path'])}”`;
      case 'Glob':
      case 'Grep':
        return `Search files for “${short(input['pattern'])}”`;
      case 'WebFetch':
        return `Fetch ${short(input['url'])}`;
      case 'WebSearch':
        return `Search the web for “${short(input['query'])}”`;
      default:
        return item.name || 'Tool';
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
