// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_CLAUDE_SSE_PARSER_H_
#define COMPASS_BROWSER_CLAUDE_SSE_PARSER_H_

#include <string>
#include <string_view>
#include <vector>

namespace compass::claude {

// Splits a server-sent events stream (the Messages API's streaming replies)
// into events, however the bytes arrive. Follows the parts of the
// EventSource format the API uses: "event:" and "data:" fields, multi-line
// data joined with "\n", ":" comments, and lines ended by "\n" or "\r\n". An
// event ends at a blank line.
class SseParser {
 public:
  struct Event {
    // "message_start", "content_block_delta", "ping", "error", ...; empty
    // when the server sent no "event:" field.
    std::string name;
    std::string data;
  };

  SseParser();
  SseParser(const SseParser&) = delete;
  SseParser& operator=(const SseParser&) = delete;
  ~SseParser();

  // Adds |chunk| and returns the events it completed, in order.
  std::vector<Event> Feed(std::string_view chunk);

  // Forgets what was fed, for another stream.
  void Reset();

  // Bytes of an event not yet complete; a stream that ends with some left
  // was cut off.
  bool HasPartialEvent() const;

 private:
  void HandleLine(std::string_view line, std::vector<Event>& events);

  // The current line, until its end arrives.
  std::string line_;
  std::string name_;
  std::string data_;
  bool has_data_ = false;
};

}  // namespace compass::claude

#endif  // COMPASS_BROWSER_CLAUDE_SSE_PARSER_H_
