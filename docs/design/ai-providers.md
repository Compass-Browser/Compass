# Other AI providers: OpenAI and Gemini

Compass's AI features (the chat, its browser tools, memory and files, and
the sidebar's small requests: names, Tidy Today, short titles, Ask in the
command bar) speak Anthropic's Messages API (claude.md, ai-sidebar.md).
This adds OpenAI and Google Gemini, and any server that speaks OpenAI's
Chat Completions (OpenRouter, Ollama, LM Studio, a company gateway), with
the user's own key. Claude stays the default and the reference: the
prompts and tools are written and tested for it.

## Goal

- **Providers** (one at a time, per device): Anthropic (as today), OpenAI,
  Google Gemini, and "Other (OpenAI-compatible)" with a base URL.
- **Everything works on each:** the chat with every tool, memory, files,
  developer tools, Boosts, and the sidebar features, each with its own
  model as today.
- **Nothing else changes:** the conversation, the tool loop, the tools, the
  approvals, the prompts and the UI stay as they are. A user who never
  picks another provider sends the same bytes as before.
- **Not:** a ChatGPT or Gemini subscription (those apps don't lend them to
  other apps; API keys only), Claude Code mode (desktop, Claude only),
  subscription tokens (`claude setup-token`, Anthropic only), running
  several providers at once, or per-chat provider choice.

## Design: Anthropic's format inside, a translation at the edge

Each platform keeps the conversation as Messages API content blocks
(`Conversation`, conversation.ts) and applies streamed Messages events to
it. The adapter sits where the bytes go out and come in:

1. **The request.** The body built as today (`ChatRequest`,
   `BuildChatRequest()`, `ClaudeQuick.body`, the command bar's) is turned
   into a Chat Completions body just before it's sent.
2. **The stream.** Chat Completions chunks are turned back into the
   Messages events they stand for (`content_block_start`/`_delta`/`_stop`,
   `message_delta` with a stop reason, `message_stop`), which the rest of
   the code already applies.
3. **The one-shot reply** (the sidebar's requests): `choices[0].message`
   becomes the text `ClaudeQuick` parses, its refusal a refusal.

So the conversation, the tool loop, the screenshot pruning, Retry and Stop
don't know which provider answered. One adapter per implementation: the
shared C++ (`browser/claude/`, desktop and Android) and iOS
(`CompassClaude`).

**Why Chat Completions** and not OpenAI's Responses API or Gemini's own
`generateContent`: Gemini serves it (at
`https://generativelanguage.googleapis.com/v1beta/openai/`), and so do
OpenRouter, Ollama, LM Studio, vLLM and most gateways. One adapter covers
them all. **OpenAI itself gets the Responses API** (below): its GPT-6
models call tools only there.

### The request, field by field

| Messages API (what Compass builds) | Chat Completions (what goes out) |
|---|---|
| `system` blocks | one `system` message, the blocks' text joined by a blank line; "You are Claude," becomes "You are an AI assistant," |
| user `text` blocks | a user message: a string, or parts when there are images |
| user `image` (base64) | an `image_url` part with a `data:` URL |
| assistant `text` blocks | the assistant message's `content` |
| assistant `thinking` blocks | left out |
| assistant `tool_use` | `tool_calls[]`: `{id, type: "function", function: {name, arguments}}`, the input as a JSON string |
| user `tool_result` | a `tool` message per result (`tool_call_id`, its text; "Error: " first when `is_error`), right after the assistant message |
| an image in a `tool_result` (screenshots) | the tool message says "(The screenshot follows.)", and a user message with the images comes after the tool messages: Chat Completions takes no image in a tool message |
| `tools[]` (`name`, `description`, `input_schema`) | `tools[]`: `{type: "function", function: {name, description, parameters}}` |
| `cache_control`, `thinking`, `fallbacks`, betas, `anthropic-version` | dropped |
| `max_tokens` | dropped (below) |
| `stream: true` | `stream: true` |

- **No token limit.** The chat's 64,000 is past most models' output limits
  (refused), and for reasoning models a small limit (the sidebar's 400)
  is spent on reasoning before any answer. The prompts keep answers short,
  and `ClaudeQuick` already refuses a reply over 8 KB.
- **Light requests** (no `thinking` in the body: the sidebar's and the
  command bar's) ask for little reasoning: `reasoning_effort: "low"` for
  OpenAI's reasoning models (`gpt-5…`, `o…`) and Gemini 2.5 and later;
  nothing for other servers, which may refuse the field.
- **JSON replies.** The sidebar's requests (which must answer one JSON
  object) add `response_format: {type: "json_object"}` on OpenAI and
  Gemini.
- **Headers:** `Authorization: Bearer <key>`, `content-type`, and
  `accept: text/event-stream` when streamed.
- **Paths** under the base URL: `chat/completions` and `models` (the base
  URL carries the version: `https://api.openai.com/v1`).

### The stream

Server-sent events with only `data:` lines, each a chunk, ending with
`data: [DONE]`.

- `choices[0].delta.content` → a `text` block (opened on its first
  piece, `text_delta`s after).
- `delta.reasoning_content` or `delta.reasoning` (DeepSeek, OpenRouter,
  some local servers) → a `thinking` block, shown as Claude's is; with no
  signature, it isn't sent back (Conversation already drops those).
- `delta.tool_calls[]` → a `tool_use` block per call, by the call's
  `index` (or its `id` when a server leaves the index out, as Gemini's
  can): `content_block_start` with `{id, name, input: {}}`, then
  `input_json_delta`s with the argument pieces. A call without an id gets
  one (`call_<n>`), kept in the conversation, so its result matches.
- `finish_reason` → the stop reason: any tool call seen → `tool_use`
  (Gemini answers "stop" even then), `length` → `max_tokens`,
  `content_filter` → `refusal`, else `end_turn`.
- At `[DONE]` (or the end of a stream that had a finish reason): every
  open block stops, then `message_delta` and `message_stop`. A stream
  that ends with neither is "The reply ended early", as today.
- A chunk `{"error": {...}}` is the reply's error.
- Chunks without choices (usage) are skipped.

### OpenAI: the Responses API

OpenAI's model pages (read 2026-11-18): GPT-6.2 Astra, GPT-6.2 Sol and
GPT-6.1 Terra take tools only through `POST /v1/responses`; GPT-6 Luna takes
them on Chat Completions only with reasoning off. So the OpenAI provider
speaks Responses (`OpenAIResponses`), stateless (`store: false`: each
request carries the whole conversation, as with Claude).

| Messages API | Responses |
|---|---|
| `system` blocks | `instructions`, joined, the name made neutral |
| user `text` / `image` | a user input message: a string, or `input_text` / `input_image` parts (a data URL) |
| assistant `text` | an assistant input message (a string) |
| assistant `thinking` | left out (below) |
| assistant `tool_use` | a `function_call` item (`call_id`, `name`, `arguments` as a JSON string) |
| user `tool_result` | a `function_call_output` item (`call_id`, `output`: the text, "Error: " first when `is_error`); images as for Chat Completions, in a user message after the outputs |
| `tools[]` | `{type: "function", name, description, parameters}` |
| a light request | `reasoning: {effort: "low"}` for gpt-5, gpt-6 and o-series ids |
| a JSON reply | `text: {format: {type: "json_object"}}` |
| `cache_control`, `thinking`, `max_tokens`, betas | dropped |

The stream's events (each `data:` carries its `type`):
`response.output_item.added` with a `function_call` item starts a
`tool_use` block (its `call_id`, `name`); `response.function_call_arguments.delta`
gives `input_json_delta`s, and `response.output_item.done` gives the whole
arguments when no delta came; `response.output_text.delta` and
`response.refusal.delta` write a text block, `response.reasoning_summary_text.delta`
a thinking block (no signature, so not sent back); `response.completed` ends
it (`tool_use` if a call came, else `end_turn`), `response.incomplete` with
`max_tokens` or, for `content_filter`, `refusal`; `response.failed` and
`error` are the reply's error. Without `response.completed` the reply ended
early. The one-shot reply is the `message` items' `output_text`; a
`refusal` part is a refusal.

Not yet: reasoning items going back with their `encrypted_content`
(`include: ["reasoning.encrypted_content"]`), which OpenAI says helps tool
use across turns; reasoning summaries (`reasoning.summary`), which need a
verified organization.

### Errors and models

- Errors read `error.message`, also when the body is an array (Gemini's
  `[{"error": …}]`), else the per-status texts, worded for the provider
  ("The server is overloaded", not "Claude is overloaded").
- "Test connection" is `GET models`. OpenAI's list is cut to chat models
  (not embeddings, audio, images, moderation, realtime or transcription);
  Gemini's ids lose their `models/` prefix.

## Settings

Settings → Claude gets a **Provider** picker first: Anthropic, OpenAI,
Google Gemini, Other (OpenAI-compatible).

- **A key per provider**, each in the same store as today's (iOS: the
  device-only Keychain, `claude-api-key` for Anthropic, `ai-api-key-<provider>`
  for the others; desktop and Android: the OS-encrypted prefs as today).
  Switching providers keeps the others' keys.
- **Base URL:** Anthropic keeps Advanced (gateways, Bedrock, Vertex) and
  its auth style; OpenAI and Gemini use theirs; Other needs one (the same
  rules: https, or http to this device).
- **Models per provider:** the chat's and each sidebar feature's. The
  Anthropic keys stay as they are; another provider's add `.<provider>`
  (`compass.claude.tidy_today_model.openai`). Defaults until Test
  connection lists the key's models:

  | | chat | sidebar | command bar |
  |---|---|---|---|
  | OpenAI | `gpt-6.1-terra` | `gpt-6-luna` | `gpt-6-luna` |
  | Gemini | `gemini-2.5-pro` | `gemini-2.5-flash` | `gemini-2.5-flash` |
  | Other | typed (Model ID) | the chat's | the chat's |

  The OpenAI picker offers `gpt-6.2-astra`, `gpt-6.2-sol`, `gpt-6.1-terra`
  and `gpt-6-luna` ($8/$40, $2/$10, $1/$5 and $0.10/$0.50 per million
  tokens in and out, 2026-11-18); gpt-5 and gpt-5-mini are deprecated.
  These are the ids known when this was written; Test connection's list
  is the truth.
- Subscription tokens, their risks switch, Claude Code mode and the auth
  style are Anthropic's only and hide for the others.
- **Names** (iOS): the settings section is **AI** (Settings → AI). The
  chat sheet's title and the "Ask …" entries (the page menu's tile, the
  command bar's row, the sheet's field, ⌘⇧E's menu item) name the
  provider's assistant (`AIProvider.assistantName`, `askTitle`): Ask
  Claude, Ask GPT, Ask Gemini, or Ask AI for an OpenAI-compatible server.
  The settings say "AI" too ("Let AI Use the Whole Browser", "AI in the
  Sidebar", "The AI can…"), and so do the tool refusals that name them
  ("Compass settings, AI"); only Anthropic's own options (subscription
  tokens, the Claude API endpoint) say Claude. The shared
  `system_prompt.md` still names the desktop's "Claude" settings: the
  desktop's page is renamed with P4.

## Privacy

What the chat and the sidebar's requests send (claude.md, ai-sidebar.md)
goes to the picked provider, under its terms, instead of Anthropic. The
settings say so under the picker. Incognito still never asks.

## Quality

The prompts and tool descriptions are tuned for Claude. Other models will
use the browser tools less well; the first round only checks that they
work (each tool called and answered, a multi-step task), and the prompts
get per-provider notes only where a model misreads them. Known risks:
Gemini's function declarations take a subset of JSON Schema, so a tool
whose `input_schema` uses more is refused; a model may call several tools
at once (the loop already runs them one after the other).

## Per platform

- **iOS** (first): `AIProvider` and the translations
  (`OpenAIChat` for Chat Completions, `OpenAIResponses` for OpenAI,
  `OpenAIEdge` picking one: request, stream, reply) in `CompassClaude`,
  unit tested; `ClaudeEndpoint` gains the provider (paths, headers, errors,
  models); `ClaudeClient.stream` and `ClaudeQuick` translate when the
  provider isn't Anthropic; `ClaudeSettings` and `ClaudeSettingsView` get
  the picker, keys and models per provider.
- **Desktop and Android** (shared C++, `browser/claude/`): the same
  translation in `claude_api.cc` (endpoint, paths, headers), a request
  translator next to `claude_request.cc`, and the stream translator in
  `ClaudeClient` before events reach the chat page (WebUI) and Android's
  bridge; `ClaudeQuick` as on iOS. Prefs: `compass.claude.provider` and the
  per-provider keys and models. The desktop settings page and Android's
  settings get the picker. To agree with the Android work before it
  starts, since both build on the same C++.

## Milestones

| | |
|---|---|
| P1 | iOS: provider, translation and tests; settings; the chat and sidebar on OpenAI and Gemini. Done 2026-11-18 (`AIProvider`, `OpenAIChat`, `OpenAIResponses`, `OpenAIChatTests`, `OpenAIResponsesTests`); not yet tried with real keys |
| P2 | iOS: checked with real keys (OpenAI, Gemini, a local Ollama); prompt notes where needed |
| P3 | Desktop and Android: the C++ translation and tests; settings on both |
| P4 | Names (above) and privacy text on every platform |

## Verification

- **Unit tests** (iOS `OpenAIChatTests` and `OpenAIResponsesTests`, desktop `OpenAIChatTest`): every
  row of the request table; a tool round with a screenshot; streams
  recorded from OpenAI and Gemini (text, reasoning, one and several tool
  calls, a call without an index or id, `length`, `content_filter`, an
  error chunk, an early end); the one-shot reply and its refusal; errors
  (object, array, text); the model lists.
- **The sidebar's tests** run once more against a fake OpenAI-format
  server (`ClaudeQuickTests`).
- **By hand** with real keys: a chat that reads a page, clicks and fills a
  form, takes a screenshot and saves a memory; each sidebar feature; Test
  connection.
