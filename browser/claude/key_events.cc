// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/key_events.h"

#include <array>
#include <optional>

#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"

namespace compass::claude {

namespace {

constexpr int kAlt = 1;
constexpr int kCtrl = 2;
constexpr int kMeta = 4;
constexpr int kShift = 8;

struct NamedKey {
  const char* name;  // Lowercase, as Claude may write it.
  const char* key;
  const char* code;
  int key_code;
  const char* text;
};

constexpr auto kNamedKeys = std::to_array<NamedKey>({
    {"enter", "Enter", "Enter", 13, "\r"},
    {"return", "Enter", "Enter", 13, "\r"},
    {"tab", "Tab", "Tab", 9, ""},
    {"escape", "Escape", "Escape", 27, ""},
    {"esc", "Escape", "Escape", 27, ""},
    {"backspace", "Backspace", "Backspace", 8, ""},
    {"delete", "Delete", "Delete", 46, ""},
    {"space", " ", "Space", 32, " "},
    {"arrowleft", "ArrowLeft", "ArrowLeft", 37, ""},
    {"left", "ArrowLeft", "ArrowLeft", 37, ""},
    {"arrowup", "ArrowUp", "ArrowUp", 38, ""},
    {"up", "ArrowUp", "ArrowUp", 38, ""},
    {"arrowright", "ArrowRight", "ArrowRight", 39, ""},
    {"right", "ArrowRight", "ArrowRight", 39, ""},
    {"arrowdown", "ArrowDown", "ArrowDown", 40, ""},
    {"down", "ArrowDown", "ArrowDown", 40, ""},
    {"home", "Home", "Home", 36, ""},
    {"end", "End", "End", 35, ""},
    {"pageup", "PageUp", "PageUp", 33, ""},
    {"pagedown", "PageDown", "PageDown", 34, ""},
    {"insert", "Insert", "Insert", 45, ""},
});

// Punctuation on a US keyboard: the character, its code and key code.
struct Punctuation {
  char c;
  const char* code;
  int key_code;
};

constexpr auto kPunctuation = std::to_array<Punctuation>({
    {'-', "Minus", 189}, {'=', "Equal", 187},        {'[', "BracketLeft", 219},
    {']', "BracketRight", 221}, {'\\', "Backslash", 220},
    {';', "Semicolon", 186},    {'\'', "Quote", 222}, {',', "Comma", 188},
    {'.', "Period", 190},       {'/', "Slash", 191},  {'`', "Backquote", 192},
});

std::optional<int> Modifier(std::string_view name) {
  if (name == "shift") {
    return kShift;
  }
  if (name == "ctrl" || name == "control") {
    return kCtrl;
  }
  if (name == "alt" || name == "option" || name == "opt") {
    return kAlt;
  }
  if (name == "cmd" || name == "meta" || name == "command" ||
      name == "super") {
    return kMeta;
  }
  return std::nullopt;
}

std::optional<KeyPress> Key(std::string_view name) {
  const std::string lower = base::ToLowerASCII(name);
  for (const NamedKey& named : kNamedKeys) {
    if (lower == named.name) {
      KeyPress press;
      press.key = named.key;
      press.code = named.code;
      press.key_code = named.key_code;
      press.text = named.text;
      return press;
    }
  }
  if (lower.size() >= 2 && lower.size() <= 3 && lower[0] == 'f') {
    int n = 0;
    for (char c : lower.substr(1)) {
      if (!base::IsAsciiDigit(c)) {
        return std::nullopt;
      }
      n = n * 10 + (c - '0');
    }
    if (n < 1 || n > 12) {
      return std::nullopt;
    }
    KeyPress press;
    press.key = base::StrCat({"F", lower.substr(1)});
    press.code = press.key;
    press.key_code = 111 + n;
    return press;
  }
  if (name.size() != 1) {
    return std::nullopt;
  }
  const char c = name[0];
  KeyPress press;
  press.key = std::string(1, c);
  press.text = press.key;
  if (base::IsAsciiAlpha(c)) {
    const char upper = base::ToUpperASCII(c);
    press.code = base::StrCat({"Key", std::string(1, upper)});
    press.key_code = upper;
    if (base::IsAsciiUpper(c)) {
      press.modifiers |= kShift;
    }
    return press;
  }
  if (base::IsAsciiDigit(c)) {
    press.code = base::StrCat({"Digit", std::string(1, c)});
    press.key_code = c;
    return press;
  }
  for (const Punctuation& p : kPunctuation) {
    if (p.c == c) {
      press.code = p.code;
      press.key_code = p.key_code;
      return press;
    }
  }
  if (base::IsAsciiPrintable(c)) {
    // Shifted punctuation ("!", "?"): the key without a code.
    return press;
  }
  return std::nullopt;
}

// macOS's key bindings for editing, as Chromium's Mac port sends them.
std::vector<std::string> MacCommands(const KeyPress& press) {
  const int m = press.modifiers;
  const std::string& key = press.code.starts_with("Key")
                               ? press.code
                               : press.key;
  struct Binding {
    int modifiers;
    const char* key;
    const char* command;
  };
  static constexpr auto kBindings = std::to_array<Binding>({
      {0, "Backspace", "deleteBackward"},
      {kShift, "Backspace", "deleteBackward"},
      {kAlt, "Backspace", "deleteWordBackward"},
      {kMeta, "Backspace", "deleteToBeginningOfLine"},
      {0, "Delete", "deleteForward"},
      {kAlt, "Delete", "deleteWordForward"},
      {0, "Enter", "insertNewline"},
      {kShift, "Enter", "insertNewline"},
      {0, "ArrowLeft", "moveLeft"},
      {0, "ArrowRight", "moveRight"},
      {0, "ArrowUp", "moveUp"},
      {0, "ArrowDown", "moveDown"},
      {kShift, "ArrowLeft", "moveLeftAndModifySelection"},
      {kShift, "ArrowRight", "moveRightAndModifySelection"},
      {kShift, "ArrowUp", "moveUpAndModifySelection"},
      {kShift, "ArrowDown", "moveDownAndModifySelection"},
      {kAlt, "ArrowLeft", "moveWordLeft"},
      {kAlt, "ArrowRight", "moveWordRight"},
      {kAlt | kShift, "ArrowLeft", "moveWordLeftAndModifySelection"},
      {kAlt | kShift, "ArrowRight", "moveWordRightAndModifySelection"},
      {kMeta, "ArrowLeft", "moveToBeginningOfLine"},
      {kMeta, "ArrowRight", "moveToEndOfLine"},
      {kMeta, "ArrowUp", "moveToBeginningOfDocument"},
      {kMeta, "ArrowDown", "moveToEndOfDocument"},
      {kMeta | kShift, "ArrowLeft", "moveToBeginningOfLineAndModifySelection"},
      {kMeta | kShift, "ArrowRight", "moveToEndOfLineAndModifySelection"},
      {0, "Home", "scrollToBeginningOfDocument"},
      {0, "End", "scrollToEndOfDocument"},
      {0, "PageUp", "scrollPageUp"},
      {0, "PageDown", "scrollPageDown"},
      {kMeta, "KeyA", "selectAll"},
      {kMeta, "KeyC", "copy"},
      {kMeta, "KeyX", "cut"},
      {kMeta, "KeyV", "paste"},
      {kMeta, "KeyZ", "undo"},
      {kMeta | kShift, "KeyZ", "redo"},
  });
  for (const Binding& binding : kBindings) {
    if (binding.modifiers == m && key == binding.key) {
      return {binding.command};
    }
  }
  return {};
}

}  // namespace

KeyPress::KeyPress() = default;
KeyPress::KeyPress(const KeyPress&) = default;
KeyPress& KeyPress::operator=(const KeyPress&) = default;
KeyPress::~KeyPress() = default;

base::expected<std::vector<KeyPress>, std::string> ParseKeys(
    std::string_view keys,
    bool mac) {
  std::vector<KeyPress> presses;
  for (std::string_view combo : base::SplitStringPiece(
           keys, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    // "+" alone, or a combo ending in "++", is the plus key.
    std::vector<std::string_view> parts;
    if (combo == "+") {
      parts.push_back("+");
    } else {
      parts = base::SplitStringPiece(combo, "+", base::TRIM_WHITESPACE,
                                     base::SPLIT_WANT_ALL);
      if (combo.ends_with("++")) {
        parts.pop_back();
        parts.back() = "+";
      }
    }
    int modifiers = 0;
    std::optional<KeyPress> press;
    for (size_t i = 0; i < parts.size(); ++i) {
      const std::string lower = base::ToLowerASCII(parts[i]);
      if (i + 1 < parts.size()) {
        std::optional<int> modifier = Modifier(lower);
        if (!modifier) {
          return base::unexpected(
              base::StrCat({"Unknown modifier \"", parts[i], "\" in \"",
                            combo, "\"."}));
        }
        modifiers |= *modifier;
        continue;
      }
      // A modifier alone ("shift") is pressed as itself.
      if (std::optional<int> modifier = Modifier(lower)) {
        KeyPress alone;
        alone.key = *modifier == kShift  ? "Shift"
                    : *modifier == kCtrl ? "Control"
                    : *modifier == kAlt  ? "Alt"
                                         : "Meta";
        alone.code = alone.key == "Meta" ? "MetaLeft" : alone.key + "Left";
        alone.key_code = *modifier == kShift  ? 16
                         : *modifier == kCtrl ? 17
                         : *modifier == kAlt  ? 18
                                              : 91;
        press = alone;
      } else {
        press = Key(parts[i]);
      }
      if (!press) {
        return base::unexpected(
            base::StrCat({"Unknown key \"", parts[i], "\" in \"", combo,
                          "\". Use names like Enter, Tab, Escape, "
                          "Backspace, ArrowDown, F5, or one character."}));
      }
    }
    if (!press) {
      return base::unexpected(base::StrCat({"No key in \"", combo, "\"."}));
    }
    press->modifiers |= modifiers;
    // Shortcuts type nothing; Shift alone keeps the text.
    if (press->modifiers & (kCtrl | kMeta | kAlt)) {
      press->text.clear();
    } else if ((press->modifiers & kShift) && press->text.size() == 1 &&
               base::IsAsciiLower(press->text[0])) {
      press->text = base::ToUpperASCII(press->text);
      press->key = press->text;
    }
    if (mac) {
      press->commands = MacCommands(*press);
    }
    presses.push_back(std::move(*press));
  }
  if (presses.empty()) {
    return base::unexpected("No keys given.");
  }
  return presses;
}

base::DictValue KeyDownParams(const KeyPress& press) {
  base::DictValue params;
  params.Set("type", press.text.empty() ? "rawKeyDown" : "keyDown");
  params.Set("modifiers", press.modifiers);
  params.Set("key", press.key);
  params.Set("code", press.code);
  params.Set("windowsVirtualKeyCode", press.key_code);
  params.Set("nativeVirtualKeyCode", press.key_code);
  if (!press.text.empty()) {
    params.Set("text", press.text);
    params.Set("unmodifiedText", press.text);
  }
  if (!press.commands.empty()) {
    base::ListValue commands;
    for (const std::string& command : press.commands) {
      commands.Append(command);
    }
    params.Set("commands", std::move(commands));
  }
  return params;
}

base::DictValue KeyUpParams(const KeyPress& press) {
  return base::DictValue()
      .Set("type", "keyUp")
      .Set("modifiers", press.modifiers)
      .Set("key", press.key)
      .Set("code", press.code)
      .Set("windowsVirtualKeyCode", press.key_code)
      .Set("nativeVirtualKeyCode", press.key_code);
}

}  // namespace compass::claude
