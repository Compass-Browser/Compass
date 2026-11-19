// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The models Compass offers without asking the API: the chat's model menu
// and Claude settings before a connection test lists what the key can use.
// See docs/design/claude.md, and docs/design/ai-providers.md for the other
// providers (ai_provider.cc's KnownModels() has the same lists).

export interface KnownModel {
  id: string;
  displayName: string;
}

export const KNOWN_MODELS: KnownModel[] = [
  {id: 'claude-opus-5-5', displayName: 'Claude Opus 5.5'},
  {id: 'claude-sonnet-5-5', displayName: 'Claude Sonnet 5.5'},
  {id: 'claude-haiku-5-5', displayName: 'Claude Haiku 5.5'},
  {id: 'claude-fable-5-1', displayName: 'Claude Fable 5.1'},
];

// The ids known on 2026-11-18; Test connection's list is the truth.
const OPENAI_MODELS: KnownModel[] = [
  {id: 'gpt-6.2-astra', displayName: 'GPT-6.2 Astra'},
  {id: 'gpt-6.2-sol', displayName: 'GPT-6.2 Sol'},
  {id: 'gpt-6.1-terra', displayName: 'GPT-6.1 Terra'},
  {id: 'gpt-6-luna', displayName: 'GPT-6 Luna'},
];

const GEMINI_MODELS: KnownModel[] = [
  {id: 'gemini-2.5-pro', displayName: 'Gemini 2.5 Pro'},
  {id: 'gemini-2.5-flash', displayName: 'Gemini 2.5 Flash'},
  {id: 'gemini-2.5-flash-lite', displayName: 'Gemini 2.5 Flash-Lite'},
];

// The known models of |provider| (ProviderId(): "anthropic", "openai",
// "gemini", "compatible"). None for an OpenAI-compatible server: only it
// knows its models.
export function knownModels(provider: string): KnownModel[] {
  switch (provider) {
    case 'openai':
      return OPENAI_MODELS;
    case 'gemini':
      return GEMINI_MODELS;
    case 'compatible':
      return [];
    default:
      return KNOWN_MODELS;
  }
}
