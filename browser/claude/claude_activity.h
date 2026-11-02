// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_CLAUDE_ACTIVITY_H_
#define COMPASS_BROWSER_CLAUDE_CLAUDE_ACTIVITY_H_

#include "base/observer_list_types.h"

namespace content {
class WebContents;
}  // namespace content

namespace compass::claude {

// Which tabs Claude is acting in right now (the chat's tools, or an MCP
// client's), for the sidebar's tint on their rows (docs/design/claude.md,
// "Claude-is-acting indicator"). ClaudeTools sets it; the UI observes.
class ClaudeActivity {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // |contents| started or stopped being acted in.
    virtual void OnClaudeActingChanged(content::WebContents* contents) = 0;
  };

  // Counted: two sessions acting in one tab keep it acting until both stop.
  static void Begin(content::WebContents* contents);
  static void End(content::WebContents* contents);
  static bool IsActing(const content::WebContents* contents);

  static void AddObserver(Observer* observer);
  static void RemoveObserver(Observer* observer);
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_CLAUDE_ACTIVITY_H_
