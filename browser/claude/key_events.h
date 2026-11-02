// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_KEY_EVENTS_H_
#define COMPASS_BROWSER_CLAUDE_KEY_EVENTS_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/types/expected.h"
#include "base/values.h"

namespace compass::claude {

// One key press of the key tool, with its modifiers.
struct KeyPress {
  KeyPress();
  KeyPress(const KeyPress&);
  KeyPress& operator=(const KeyPress&);
  ~KeyPress();

  std::string key;   // KeyboardEvent.key: "Enter", "a", "A".
  std::string code;  // KeyboardEvent.code: "Enter", "KeyA".
  int key_code = 0;  // Windows virtual key code.
  // What the key types; empty for shortcuts and keys that type nothing.
  std::string text;
  // DevTools' Input modifiers: Alt 1, Ctrl 2, Meta 4, Shift 8.
  int modifiers = 0;
  // macOS editing commands ("selectAll", "deleteBackward"): on macOS the
  // renderer edits on these, not on the key itself.
  std::vector<std::string> commands;
};

// Parses the key tool's |keys|: combos separated by spaces, each keys
// joined with + ("cmd+shift+z Enter Tab"). Modifier names: shift, ctrl
// (control), alt (option), cmd (meta, command). |mac| adds the editing
// commands.
base::expected<std::vector<KeyPress>, std::string> ParseKeys(
    std::string_view keys,
    bool mac);

// DevTools Input.dispatchKeyEvent params: the key down, then up.
base::DictValue KeyDownParams(const KeyPress& press);
base::DictValue KeyUpParams(const KeyPress& press);

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_KEY_EVENTS_H_
