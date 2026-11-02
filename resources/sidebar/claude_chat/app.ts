// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ColorChangeUpdater} from '//resources/cr_components/color_change_listener/colors_css_updater.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';

import type {ActivePage, Setup} from '../chat.mojom-webui.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import {ClaudeChatBrowserProxy} from './browser_proxy.js';
import type {AssistantTurn, ContentBlock, ToolView, UserTurn} from './conversation.js';
import {Conversation, pageBlock} from './conversation.js';
import './turn.js';

// How many rounds of tools a message may take before Claude stops and the
// user is asked to go on.
const MAX_TOOL_ROUNDS = 40;

// A tool result's content as the panel shows it.
function toolView(result: {content: ContentBlock[], is_error: boolean}):
    ToolView {
  const text = result.content.filter(b => b.type === 'text')
                   .map(b => b.text || '')
                   .join('\n');
  const image = result.content.find(b => b.type === 'image');
  const source = image?.['source'] as {media_type: string, data: string} |
      undefined;
  return {
    running: false,
    isError: result.is_error,
    text,
    image: source ? `data:${source.media_type};base64,${source.data}` :
                    undefined,
  };
}

// The chat with Claude, in the window's side panel. The page keeps the
// conversation and sends it whole for each reply (the browser adds the key,
// the model and the system prompt); each message can carry the active page's
// text. Enter sends, Shift+Enter is a new line, Esc stops a reply. See
// docs/design/claude.md, "Chat panel".
export class CompassClaudeChatAppElement extends CrLitElement {
  static get is() {
    return 'compass-claude-chat-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      attachPage_: {type: Boolean},
      hasKey_: {type: Boolean},
      model_: {type: String},
      pageReadable_: {type: Boolean},
      pageTitle_: {type: String},
      pageUrl_: {type: String},
      revision_: {type: Number},
      streaming_: {type: Boolean},
      taskTitle_: {type: String},
      text_: {type: String},
    };
  }

  protected accessor attachPage_: boolean = true;
  protected accessor hasKey_: boolean = true;
  protected accessor model_: string = '';
  protected accessor pageReadable_: boolean = false;
  protected accessor pageTitle_: string = '';
  protected accessor pageUrl_: string = '';
  // Bumped when the conversation changes, to render it again.
  protected accessor revision_: number = 0;
  // From a message being sent until Claude's last reply to it, tools and
  // all.
  protected accessor streaming_: boolean = false;
  // The tab Claude acts on during the task.
  protected accessor taskTitle_: string = '';
  protected accessor text_: string = '';

  conversation = new Conversation();
  protected suggestions_ = [
    'Summarize this page',
    'What are the key points?',
    'Explain this page simply',
  ];

  private browserProxy_ = ClaudeChatBrowserProxy.getInstance();
  private listenerIds_: number[] = [];
  // The page last attached in this conversation, not sent again unchanged.
  private attachedUrl_ = '';
  // Follow the reply down unless the user scrolled up.
  private stickToBottom_ = true;
  // Bumped by each message and by Stop, so a late tool result from a task
  // that ended is dropped.
  private task_ = 0;
  private toolRounds_ = 0;

  override connectedCallback() {
    super.connectedCallback();
    // Follows the window's theme, which follows its space.
    ColorChangeUpdater.forDocument().start();
    const router = this.browserProxy_.callbackRouter;
    this.listenerIds_ = [
      router.onStreamEvent.addListener(this.onStreamEvent_.bind(this)),
      router.onStreamDone.addListener(this.onStreamDone_.bind(this)),
      router.onSetupChanged.addListener(this.applySetup_.bind(this)),
      router.onActivePageChanged.addListener(this.applyPage_.bind(this)),
      router.onAsk.addListener(this.ask_.bind(this)),
    ];
    document.addEventListener('keydown', e => {
      if (e.key === 'Escape' && this.streaming_) {
        e.preventDefault();
        this.stop_();
      }
    });
    this.load_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    for (const id of this.listenerIds_) {
      this.browserProxy_.callbackRouter.removeListener(id);
    }
    this.listenerIds_ = [];
  }

  override firstUpdated() {
    this.messages_.addEventListener('scroll', () => {
      const m = this.messages_;
      this.stickToBottom_ = m.scrollHeight - m.scrollTop - m.clientHeight < 24;
    });
  }

  private async load_() {
    const handler = this.browserProxy_.handler;
    const [{setup}, {page}] =
        await Promise.all([handler.getSetup(), handler.getActivePage()]);
    this.applySetup_(setup);
    this.applyPage_(page);
    await this.updateComplete;
    handler.showUI();
    this.input_.focus();
    const {text} = await handler.takeQuestion();
    if (text) {
      this.ask_(text);
    }
  }

  // The command bar's "Ask Claude". While a reply streams, the question
  // waits in the field.
  private ask_(text: string) {
    if (this.streaming_) {
      this.input_.value = text;
      this.onInputInput_();
      this.input_.focus();
      return;
    }
    this.attachPage_ = true;
    this.send_(text);
  }

  private get input_(): HTMLTextAreaElement {
    return this.shadowRoot.querySelector<HTMLTextAreaElement>('#input')!;
  }

  private get messages_(): HTMLElement {
    return this.shadowRoot.querySelector<HTMLElement>('#messages')!;
  }

  private applySetup_(setup: Setup) {
    this.hasKey_ = setup.hasKey;
    this.model_ = setup.model;
  }

  private applyPage_(page: ActivePage) {
    if (page.url !== this.pageUrl_) {
      // Another page: offer it again.
      this.attachPage_ = true;
    }
    this.pageUrl_ = page.url;
    this.pageTitle_ = page.title;
    this.pageReadable_ = page.readable;
  }

  protected canSend_(): boolean {
    return this.hasKey_ && !this.streaming_ && this.text_.trim() !== '';
  }

  // Tool results show in the rows of the reply that asked for them.
  protected getShownTurns_() {
    return this.conversation.turns.filter(
        turn => turn.role !== 'user' || !turn.tools);
  }

  protected hasTurns_(): boolean {
    return this.conversation.turns.length > 0;
  }

  protected getAttachTitle_(): string {
    return this.attachPage_ ? 'Claude gets this page\'s text with your message' :
                              'Not sent';
  }

  private changed_() {
    this.revision_++;
    this.scrollDown_();
  }

  private async scrollDown_() {
    await this.updateComplete;
    if (this.stickToBottom_) {
      const m = this.messages_;
      m.scrollTop = m.scrollHeight;
    }
  }

  private async send_(text: string) {
    text = text.trim();
    if (!text || this.streaming_ || !this.hasKey_) {
      return;
    }
    this.streaming_ = true;
    this.task_++;
    this.toolRounds_ = 0;
    this.taskTitle_ = this.pageTitle_ || this.pageUrl_;
    this.text_ = '';
    this.input_.value = '';
    this.resizeInput_();
    // Claude's tools act on this tab until the next message.
    this.browserProxy_.handler.startTask();
    // A reply that failed or stopped with tools open: they get results.
    this.conversation.closeOpenTools('Not run: the reply ended first.');

    const content: ContentBlock[] = [];
    const turn: UserTurn = {role: 'user', text, content};
    if (this.pageReadable_ && this.attachPage_ &&
        this.pageUrl_ !== this.attachedUrl_) {
      const {page, error} =
          await this.browserProxy_.handler.readActivePage();
      if (page) {
        content.push({
          type: 'text',
          text: pageBlock(page.url, page.title, page.text, page.truncated),
        });
        turn.pageTitle = page.title || page.url;
        turn.pageUrl = page.url;
        this.attachedUrl_ = page.url;
      } else if (error) {
        console.warn('Claude: the page wasn\'t attached:', error);
      }
    }
    content.push({type: 'text', text});
    this.conversation.addUser(turn);
    const messages = this.conversation.messages();
    this.conversation.startReply();
    this.stickToBottom_ = true;
    this.changed_();
    this.browserProxy_.handler.send(JSON.stringify(messages));
  }

  private stop_() {
    this.task_++;
    this.browserProxy_.handler.stop();
    this.conversation.finishReply('');
    this.conversation.closeOpenTools('Stopped by the user.');
    this.streaming_ = false;
    this.changed_();
  }

  private onStreamEvent_(name: string, data: string) {
    this.conversation.applyEvent(name, data);
    this.changed_();
  }

  private onStreamDone_(error: string) {
    const turn = this.conversation.streamingTurn;
    this.conversation.finishReply(error);
    if (!error && turn?.stopReason === 'tool_use' && this.streaming_) {
      this.changed_();
      this.runTools_(turn);
      return;
    }
    this.endTask_();
  }

  private endTask_() {
    this.streaming_ = false;
    this.changed_();
    this.input_.focus();
  }

  // Runs the reply's tool calls one after the other, then asks for the next
  // reply with their results. The browser checks what each tool may do.
  private async runTools_(turn: AssistantTurn) {
    const task = this.task_;
    if (++this.toolRounds_ > MAX_TOOL_ROUNDS) {
      this.conversation.closeOpenTools(
          'Not run: Claude used many steps; the user may ask to go on.');
      turn.error = `Claude stopped after ${MAX_TOOL_ROUNDS} steps. ` +
          'Send "go on" to let it continue.';
      this.endTask_();
      return;
    }
    const results: ContentBlock[] = [];
    for (const use of turn.content) {
      if (use.type !== 'tool_use' || !use.id) {
        continue;
      }
      this.conversation.setTool(use.id, {running: true});
      this.changed_();
      const {resultJson} = await this.browserProxy_.handler.runTool(
          use.name || '', JSON.stringify(use.input || {}));
      if (task !== this.task_) {
        // Stopped, or another message: its results are closed already.
        return;
      }
      let result: {content: ContentBlock[], is_error: boolean};
      try {
        result = JSON.parse(resultJson);
      } catch {
        result = {
          content: [{type: 'text', text: 'The tool failed.'}],
          is_error: true,
        };
      }
      this.conversation.setTool(use.id, toolView(result));
      results.push({
        type: 'tool_result',
        tool_use_id: use.id,
        content: result.content,
        is_error: result.is_error,
      });
      this.changed_();
    }
    this.conversation.addToolResults(results);
    const messages = this.conversation.messages();
    this.conversation.startReply();
    this.changed_();
    this.browserProxy_.handler.send(JSON.stringify(messages));
  }

  private resizeInput_() {
    const input = this.input_;
    input.style.height = 'auto';
    input.style.height = `${Math.min(input.scrollHeight, 160)}px`;
  }

  protected onInputInput_() {
    this.text_ = this.input_.value;
    this.resizeInput_();
  }

  protected onInputKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      this.send_(this.input_.value);
    }
  }

  protected onSendClick_() {
    this.send_(this.input_.value);
  }

  protected onStopClick_() {
    this.stop_();
  }

  protected onSuggestionClick_(e: Event) {
    const text = (e.currentTarget as HTMLElement).dataset['text'] || '';
    this.attachPage_ = true;
    this.send_(text);
  }

  protected onAttachClick_() {
    this.attachPage_ = !this.attachPage_;
  }

  protected onTurnRetry_() {
    // The failed exchange goes; its message is sent again.
    const turns = this.conversation.turns;
    const failed = turns.pop();
    const asked = turns.pop();
    if (failed?.role !== 'assistant' || asked?.role !== 'user') {
      return;
    }
    if (asked.tools) {
      // A reply to tool results failed: ask for it again.
      turns.push(asked);
      this.streaming_ = true;
      this.task_++;
      const messages = this.conversation.messages();
      this.conversation.startReply();
      this.changed_();
      this.browserProxy_.handler.send(JSON.stringify(messages));
      return;
    }
    if (asked.pageUrl === this.attachedUrl_) {
      this.attachedUrl_ = '';
    }
    this.changed_();
    this.send_(asked.text);
  }

  protected onNewChatClick_() {
    if (this.streaming_) {
      this.stop_();
    }
    this.conversation.clear();
    this.attachedUrl_ = '';
    this.attachPage_ = true;
    this.changed_();
    this.input_.focus();
  }

  protected onCloseClick_() {
    this.browserProxy_.handler.closePanel();
  }

  protected onOpenSettingsClick_() {
    this.browserProxy_.handler.openSettings();
  }

  // Links in replies open in a tab, not in the panel.
  protected onMessagesClick_(e: MouseEvent) {
    // The link is inside a turn's shadow tree.
    const link = e.composedPath().find(
        (node): node is HTMLAnchorElement => node instanceof HTMLAnchorElement);
    if (link) {
      e.preventDefault();
      this.browserProxy_.handler.openLink(link.href);
    }
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'compass-claude-chat-app': CompassClaudeChatAppElement;
  }
}

customElements.define(
    CompassClaudeChatAppElement.is, CompassClaudeChatAppElement);
