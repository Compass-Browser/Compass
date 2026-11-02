// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The chat's conversation, kept by the page: the Messages API's messages, and
// what the panel shows of them. Streamed replies are put together from their
// events into the same content blocks the API would have returned, so every
// block (thinking with its signature, fallback blocks, tool use later) goes
// back unchanged in the next request, as the API wants. See
// docs/design/claude.md, "Chat panel".

// A content block, as the API sends it. Only the fields the panel reads are
// named; the rest are kept as they came.
export interface ContentBlock {
  type: string;
  text?: string;
  thinking?: string;
  signature?: string;
  id?: string;
  name?: string;
  input?: unknown;
  [field: string]: unknown;
}

export interface UserTurn {
  role: 'user';
  // What the user typed.
  text: string;
  // The page attached to it, if any.
  pageTitle?: string;
  pageUrl?: string;
  // What goes to the API.
  content: ContentBlock[];
}

export interface AssistantTurn {
  role: 'assistant';
  content: ContentBlock[];
  // Set while it streams.
  streaming: boolean;
  stopReason?: string;
  error?: string;
}

export type Turn = UserTurn|AssistantTurn;

// The fields of a streamed event the conversation reads.
interface StreamEvent {
  index: number;
  content_block?: ContentBlock;
  delta?: {
    type?: string,
    text?: string,
    thinking?: string,
    signature?: string,
    partial_json?: string,
    stop_reason?: string,
  };
}

// The page's text, as the user's message carries it.
export function pageBlock(
    url: string, title: string, text: string, truncated: boolean): string {
  const attribute = (value: string) => value.replace(/[&"<>]/g, c => {
    return {'&': '&amp;', '"': '&quot;', '<': '&lt;', '>': '&gt;'}[c]!;
  });
  return `<page url="${attribute(url)}" title="${attribute(title)}"${
      truncated ? ' truncated="true"' : ''}>\n${text}\n</page>`;
}

export class Conversation {
  turns: Turn[] = [];
  // Partial JSON of streaming tool_use blocks, by index.
  private partialJson_ = new Map<number, string>();

  clear() {
    this.turns = [];
    this.partialJson_.clear();
  }

  addUser(turn: UserTurn) {
    this.turns.push(turn);
  }

  // The messages to send: the conversation without failed exchanges (a
  // reply that failed, and the message it answered).
  messages(): Array<{role: string, content: ContentBlock[]}> {
    const messages: Array<{role: string, content: ContentBlock[]}> = [];
    for (let i = 0; i < this.turns.length; i++) {
      const turn = this.turns[i]!;
      const next = this.turns[i + 1];
      if (turn.role === 'user' && next?.role === 'assistant' && next.error &&
          !next.content.length) {
        i++;
        continue;
      }
      if (turn.role === 'assistant' && (turn.streaming || !turn.content.length)) {
        continue;
      }
      messages.push({role: turn.role, content: turn.content});
    }
    return messages;
  }

  get streamingTurn(): AssistantTurn|null {
    const last = this.turns[this.turns.length - 1];
    return last?.role === 'assistant' && last.streaming ? last : null;
  }

  startReply(): AssistantTurn {
    const turn: AssistantTurn = {role: 'assistant', content: [], streaming: true};
    this.turns.push(turn);
    this.partialJson_.clear();
    return turn;
  }

  // Applies a streamed event to the reply being streamed.
  applyEvent(name: string, data: string) {
    const turn = this.streamingTurn;
    if (!turn) {
      return;
    }
    let event: StreamEvent;
    try {
      event = JSON.parse(data) as StreamEvent;
    } catch {
      return;
    }
    switch (name) {
      case 'content_block_start':
        turn.content[event.index] = {type: 'text', ...event.content_block};
        break;
      case 'content_block_delta': {
        const block = turn.content[event.index];
        const delta = event.delta || {};
        if (!block) {
          break;
        }
        switch (delta.type) {
          case 'text_delta':
            block.text = (block.text || '') + delta.text;
            break;
          case 'thinking_delta':
            block.thinking = (block.thinking || '') + delta.thinking;
            break;
          case 'signature_delta':
            block.signature = (block.signature || '') + delta.signature;
            break;
          case 'input_json_delta':
            this.partialJson_.set(
                event.index,
                (this.partialJson_.get(event.index) || '') +
                    delta.partial_json);
            break;
        }
        break;
      }
      case 'content_block_stop': {
        const block = turn.content[event.index];
        const json = this.partialJson_.get(event.index);
        if (block && json !== undefined) {
          try {
            block.input = JSON.parse(json || '{}');
          } catch {
            block.input = {};
          }
          this.partialJson_.delete(event.index);
        }
        break;
      }
      case 'message_delta':
        if (event.delta?.stop_reason) {
          turn.stopReason = event.delta.stop_reason;
        }
        break;
    }
  }

  finishReply(error: string) {
    const turn = this.streamingTurn;
    if (!turn) {
      return;
    }
    turn.streaming = false;
    // Holes, if a block never started.
    turn.content = turn.content.filter(block => !!block);
    if (error) {
      turn.error = error;
    }
  }
}
