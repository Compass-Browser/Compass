// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/key_events.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

TEST(KeyEventsTest, NamedKeysAndCharacters) {
  auto presses = ParseKeys("Enter tab ArrowDown a B 7 , F5", /*mac=*/false);
  ASSERT_TRUE(presses.has_value());
  ASSERT_EQ(presses->size(), 8u);
  EXPECT_EQ((*presses)[0].key, "Enter");
  EXPECT_EQ((*presses)[0].text, "\r");
  EXPECT_EQ((*presses)[0].key_code, 13);
  EXPECT_EQ((*presses)[1].key, "Tab");
  EXPECT_EQ((*presses)[1].text, "");
  EXPECT_EQ((*presses)[2].code, "ArrowDown");
  EXPECT_EQ((*presses)[3].code, "KeyA");
  EXPECT_EQ((*presses)[3].text, "a");
  EXPECT_EQ((*presses)[3].key_code, 'A');
  // An uppercase letter is Shift and the letter.
  EXPECT_EQ((*presses)[4].key, "B");
  EXPECT_EQ((*presses)[4].modifiers, 8);
  EXPECT_EQ((*presses)[5].code, "Digit7");
  EXPECT_EQ((*presses)[6].code, "Comma");
  EXPECT_EQ((*presses)[7].key, "F5");
  EXPECT_EQ((*presses)[7].key_code, 116);
  EXPECT_TRUE((*presses)[0].commands.empty());
}

TEST(KeyEventsTest, CombosTypeNothing) {
  auto presses = ParseKeys("cmd+shift+z ctrl+a shift+a ctrl++", false);
  ASSERT_TRUE(presses.has_value());
  ASSERT_EQ(presses->size(), 4u);
  EXPECT_EQ((*presses)[0].modifiers, 4 | 8);
  EXPECT_EQ((*presses)[0].text, "");
  EXPECT_EQ((*presses)[1].modifiers, 2);
  // Shift alone keeps the text, shifted.
  EXPECT_EQ((*presses)[2].text, "A");
  EXPECT_EQ((*presses)[2].key, "A");
  EXPECT_EQ((*presses)[3].key, "+");
  EXPECT_EQ((*presses)[3].modifiers, 2);

  base::DictValue down = KeyDownParams((*presses)[0]);
  EXPECT_EQ(*down.FindString("type"), "rawKeyDown");
  EXPECT_FALSE(down.contains("text"));
  EXPECT_EQ(*KeyDownParams((*presses)[2]).FindString("type"), "keyDown");
  EXPECT_EQ(*KeyUpParams((*presses)[2]).FindString("type"), "keyUp");
}

TEST(KeyEventsTest, MacEditingCommands) {
  auto presses = ParseKeys("cmd+a Backspace cmd+shift+z shift+ArrowLeft x",
                           /*mac=*/true);
  ASSERT_TRUE(presses.has_value());
  EXPECT_EQ((*presses)[0].commands, std::vector<std::string>{"selectAll"});
  EXPECT_EQ((*presses)[1].commands,
            std::vector<std::string>{"deleteBackward"});
  EXPECT_EQ((*presses)[2].commands, std::vector<std::string>{"redo"});
  EXPECT_EQ((*presses)[3].commands,
            std::vector<std::string>{"moveLeftAndModifySelection"});
  EXPECT_TRUE((*presses)[4].commands.empty());
  EXPECT_EQ(KeyDownParams((*presses)[0]).FindList("commands")->size(), 1u);
}

TEST(KeyEventsTest, Errors) {
  EXPECT_FALSE(ParseKeys("", false).has_value());
  EXPECT_FALSE(ParseKeys("hyper+a", false).has_value());
  EXPECT_FALSE(ParseKeys("NotAKey", false).has_value());
  EXPECT_FALSE(ParseKeys("F13", false).has_value());
  // A modifier alone is a key.
  auto shift = ParseKeys("shift", false);
  ASSERT_TRUE(shift.has_value());
  EXPECT_EQ(shift->front().key, "Shift");
}

}  // namespace
}  // namespace compass::claude
