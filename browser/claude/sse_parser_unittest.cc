// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/sse_parser.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

constexpr char kStream[] =
    "event: message_start\n"
    "data: {\"type\":\"message_start\"}\n"
    "\n"
    "event: ping\n"
    "data: {\"type\": \"ping\"}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"delta\":{\"text\":\"Hi\"}}\n"
    "\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n"
    "\n";

std::vector<std::string> Names(const std::vector<SseParser::Event>& events) {
  std::vector<std::string> names;
  for (const SseParser::Event& event : events) {
    names.push_back(event.name);
  }
  return names;
}

TEST(SseParserTest, SplitsEvents) {
  SseParser parser;
  std::vector<SseParser::Event> events = parser.Feed(kStream);
  EXPECT_EQ(Names(events),
            (std::vector<std::string>{"message_start", "ping",
                                      "content_block_delta", "message_stop"}));
  EXPECT_EQ(events[2].data,
            "{\"type\":\"content_block_delta\",\"delta\":{\"text\":\"Hi\"}}");
  EXPECT_FALSE(parser.HasPartialEvent());
}

TEST(SseParserTest, AnyChunking) {
  // One byte at a time gives the same events.
  SseParser parser;
  std::vector<SseParser::Event> events;
  for (char c : std::string(kStream)) {
    for (SseParser::Event& event : parser.Feed(std::string(1, c))) {
      events.push_back(std::move(event));
    }
  }
  EXPECT_EQ(events.size(), 4u);
  EXPECT_EQ(events[3].name, "message_stop");
}

TEST(SseParserTest, CrlfCommentsAndMultilineData) {
  SseParser parser;
  std::vector<SseParser::Event> events = parser.Feed(
      ": keep-alive\r\n"
      "event: x\r\n"
      "data: one\r\n"
      "data:two\r\n"
      "id: 7\r\n"
      "\r\n");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].name, "x");
  EXPECT_EQ(events[0].data, "one\ntwo");
}

TEST(SseParserTest, EventsWithoutDataAreDropped) {
  SseParser parser;
  EXPECT_TRUE(parser.Feed("event: empty\n\n").empty());
}

TEST(SseParserTest, PartialEventAtTheEnd) {
  SseParser parser;
  EXPECT_TRUE(parser.Feed("event: message_delta\ndata: {").empty());
  EXPECT_TRUE(parser.HasPartialEvent());
  parser.Reset();
  EXPECT_FALSE(parser.HasPartialEvent());
}

}  // namespace
}  // namespace compass::claude
