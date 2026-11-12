// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The models Compass offers without asking the API: the chat's model menu
// and Claude settings before a connection test lists what the key can use.
// See docs/design/claude.md.

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
