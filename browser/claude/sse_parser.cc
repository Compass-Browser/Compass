// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/sse_parser.h"

#include <utility>

namespace compass::claude {

SseParser::SseParser() = default;
SseParser::~SseParser() = default;

std::vector<SseParser::Event> SseParser::Feed(std::string_view chunk) {
  std::vector<Event> events;
  while (!chunk.empty()) {
    const size_t end = chunk.find('\n');
    if (end == std::string_view::npos) {
      line_.append(chunk);
      break;
    }
    line_.append(chunk.substr(0, end));
    chunk.remove_prefix(end + 1);
    std::string_view line = line_;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    HandleLine(line, events);
    line_.clear();
  }
  return events;
}

void SseParser::Reset() {
  line_.clear();
  name_.clear();
  data_.clear();
  has_data_ = false;
}

bool SseParser::HasPartialEvent() const {
  return !line_.empty() || has_data_ || !name_.empty();
}

void SseParser::HandleLine(std::string_view line, std::vector<Event>& events) {
  if (line.empty()) {
    // An event ends; one with no data is dropped, as EventSource does.
    if (has_data_) {
      events.push_back({std::move(name_), std::move(data_)});
    }
    name_.clear();
    data_.clear();
    has_data_ = false;
    return;
  }
  if (line.front() == ':') {
    // A comment (keep-alive).
    return;
  }
  std::string_view field = line;
  std::string_view value;
  if (const size_t colon = line.find(':'); colon != std::string_view::npos) {
    field = line.substr(0, colon);
    value = line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') {
      value.remove_prefix(1);
    }
  }
  if (field == "event") {
    name_ = std::string(value);
  } else if (field == "data") {
    if (has_data_) {
      data_.push_back('\n');
    }
    data_.append(value);
    has_data_ = true;
  }
  // "id" and "retry" don't matter here.
}

}  // namespace compass::claude
