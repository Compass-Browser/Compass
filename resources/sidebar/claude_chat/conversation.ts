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
  // The tools' results going back to Claude, not shown as a message (the
  // tool rows of the reply before show them).
  tools?: boolean;
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

// A tool call's state, as its row in the reply shows it.
export interface ToolView {
  running: boolean;
  isError?: boolean;
  // The result's text.
  text?: string;
  // A screenshot, as a data: URL.
  image?: string;
}

// How many screenshots the request keeps; older ones are replaced by a note
// (each costs about 1,200 tokens, and only recent ones matter).
const MAX_SCREENSHOTS = 2;

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

// Replaces all but the last MAX_SCREENSHOTS images in tool results with a
// note. The stored conversation keeps them, for the panel.
function pruneScreenshots(
    messages: Array<{role: string, content: ContentBlock[]}>) {
  let kept = 0;
  for (let m = messages.length - 1; m >= 0; m--) {
    const message = messages[m]!;
    if (message.role !== 'user') {
      continue;
    }
    message.content = message.content.map(block => {
      if (block.type !== 'tool_result' || !Array.isArray(block['content'])) {
        return block;
      }
      const inner = block['content'] as ContentBlock[];
      if (!inner.some(b => b.type === 'image')) {
        return block;
      }
      if (kept < MAX_SCREENSHOTS) {
        kept++;
        return block;
      }
      return {
        ...block,
        content: inner.map(
            b => b.type === 'image' ?
                {type: 'text', text: '(An earlier screenshot, removed.)'} :
                b),
      };
    });
  }
  return messages;
}

export class Conversation {
  turns: Turn[] = [];
  // Each tool call's state, by tool_use id.
  tools = new Map<string, ToolView>();
  // Partial JSON of streaming tool_use blocks, by index.
  private partialJson_ = new Map<number, string>();

  clear() {
    this.turns = [];
    this.tools.clear();
    this.partialJson_.clear();
  }

  setTool(id: string, view: ToolView) {
    this.tools.set(id, view);
  }

  addToolResults(results: ContentBlock[]) {
    this.turns.push({role: 'user', tools: true, text: '', content: results});
  }

  // The tool calls of the last reply that have no result (stopped, or the
  // reply failed): each gets one, or the API refuses the next request.
  closeOpenTools(why: string) {
    const last = this.turns[this.turns.length - 1];
    if (last?.role !== 'assistant') {
      return;
    }
    const open = last.content.filter(
        block => block.type === 'tool_use' && block.id);
    if (!open.length) {
      return;
    }
    this.addToolResults(open.map(block => {
      const view = this.tools.get(block.id!);
      if (!view || view.running) {
        this.tools.set(block.id!, {running: false, isError: true, text: why});
      }
      return {
        type: 'tool_result',
        tool_use_id: block.id,
        content: [{type: 'text', text: why}],
        is_error: true,
      };
    }));
  }

  addUser(turn: UserTurn) {
    this.turns.push(turn);
  }

  // The messages to send: the conversation without failed replies, and
  // without a message whose reply failed (it's sent again on Retry).
  // Consecutive user turns (tool results, then a new message) are one
  // message. Only the latest screenshots stay.
  messages(): Array<{role: string, content: ContentBlock[]}> {
    const messages: Array<{role: string, content: ContentBlock[]}> = [];
    for (let i = 0; i < this.turns.length; i++) {
      const turn = this.turns[i]!;
      const next = this.turns[i + 1];
      if (turn.role === 'user' && !turn.tools && next?.role === 'assistant' &&
          next.error && !next.content.length) {
        i++;
        continue;
      }
      if (turn.role === 'assistant' && turn.streaming) {
        continue;
      }
      // A reply stopped while Claude thought has a thinking block without
      // its signature, which the API refuses: it goes.
      const content = turn.role === 'assistant' ?
          turn.content.filter(
              block => block.type !== 'thinking' || !!block.signature) :
          turn.content;
      if (!content.length) {
        continue;
      }
      const previous = messages[messages.length - 1];
      if (previous?.role === turn.role) {
        previous.content = [...previous.content, ...content];
      } else {
        messages.push({role: turn.role, content});
      }
    }
    return pruneScreenshots(messages);
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
