// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The look of Compass's WebUI bubbles (the share sheet, the space editor):
// the browser's colors for the bubble (compass::BubbleLook), set on the
// page as CSS variables, so the page matches the bubble's frame.

import type {Look} from './bubble.mojom-webui.js';

export function applyBubbleLook(look: Look) {
  const root = document.documentElement;
  const colors: Record<string, string> = {
    '--background': look.background,
    '--card': look.card,
    '--pill': look.pill,
    '--track': look.track,
    '--hairline': look.hairline,
    '--text': look.text,
    '--text-dim': look.textDim,
    '--accent': look.accent,
  };
  for (const [name, value] of Object.entries(colors)) {
    root.style.setProperty(name, value);
  }
  root.toggleAttribute('dark', look.dark);
  root.style.colorScheme = look.dark ? 'dark' : 'light';
}
