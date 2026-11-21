// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "compass/browser/dev_mode/devtools_frontend_hook.h"
#include "compass/browser/dev_mode/network_log_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::dev_mode {
namespace {

base::DictValue Request(std::string id) {
  return base::DictValue().Set("requestId", std::move(id));
}

std::vector<std::string> Methods(const NetworkLogBuffer& buffer) {
  std::vector<std::string> methods;
  buffer.ForEach([&](const std::string& method, const base::DictValue&) {
    methods.push_back(method);
  });
  return methods;
}

TEST(NetworkLogBufferTest, KeepsRequestEventsGroupedByRequest) {
  NetworkLogBuffer buffer;
  buffer.Add("Network.requestWillBeSent", Request("1"));
  buffer.Add("Network.requestWillBeSent", Request("2"));
  buffer.Add("Network.dataReceived", Request("1"));
  buffer.Add("Network.responseReceived", Request("1"));
  buffer.Add("Network.loadingFinished", Request("2"));
  // Not a request's, or no request named.
  buffer.Add("Page.frameNavigated", Request("1"));
  buffer.Add("Network.loadingFinished", base::DictValue());

  EXPECT_EQ(buffer.request_count(), 2u);
  EXPECT_TRUE(buffer.Contains("1"));
  EXPECT_FALSE(buffer.Contains("3"));
  EXPECT_EQ(Methods(buffer), (std::vector<std::string>{
                                 "Network.requestWillBeSent",
                                 "Network.responseReceived",
                                 "Network.requestWillBeSent",
                                 "Network.loadingFinished",
                             }));
  buffer.Clear();
  EXPECT_TRUE(buffer.empty());
}

TEST(NetworkLogBufferTest, DropsTheOldestRequests) {
  NetworkLogBuffer buffer;
  for (size_t i = 0; i < NetworkLogBuffer::kMaxRequests + 5; ++i) {
    buffer.Add("Network.requestWillBeSent", Request(base::NumberToString(i)));
  }
  EXPECT_EQ(buffer.request_count(), NetworkLogBuffer::kMaxRequests);
  EXPECT_FALSE(buffer.Contains("4"));
  EXPECT_TRUE(buffer.Contains("5"));
}

TEST(NetworkLogBufferTest, CapsWebSocketFrames) {
  NetworkLogBuffer buffer;
  buffer.Add("Network.webSocketCreated", Request("ws"));
  for (size_t i = 0; i < NetworkLogBuffer::kMaxEventsPerRequest + 10; ++i) {
    buffer.Add("Network.webSocketFrameReceived", Request("ws"));
  }
  buffer.Add("Network.webSocketClosed", Request("ws"));
  const std::vector<std::string> methods = Methods(buffer);
  EXPECT_EQ(methods.size(), NetworkLogBuffer::kMaxEventsPerRequest + 1);
  EXPECT_EQ(methods.back(), "Network.webSocketClosed");
}

class FakeDelegate : public DevToolsFrontendHook::Delegate {
 public:
  NetworkLogBuffer buffer;
  bool recording = true;
  std::vector<std::string> sent_to_frontend;
  std::vector<std::pair<std::string, base::DictValue>> sent_to_log;
  base::OnceCallback<void(DevToolsFrontendHook::Result)> log_callback;

  const NetworkLogBuffer* GetBuffer() override {
    return recording ? &buffer : nullptr;
  }
  void SendToLog(const std::string& method,
                 base::DictValue params,
                 base::OnceCallback<void(DevToolsFrontendHook::Result)>
                     callback) override {
    sent_to_log.emplace_back(method, std::move(params));
    log_callback = std::move(callback);
  }
  std::string GetPageTargetId() override { return "PAGE"; }
  void DispatchToFrontend(const std::string& message) override {
    sent_to_frontend.push_back(message);
  }
};

class DevToolsFrontendHookTest : public testing::Test {
 protected:
  DevToolsFrontendHookTest() {
    auto delegate = std::make_unique<FakeDelegate>();
    delegate_ = delegate.get();
    hook_ = std::make_unique<DevToolsFrontendHook>(std::move(delegate));
    delegate_->buffer.Add("Network.requestWillBeSent", Request("A"));
    delegate_->buffer.Add("Network.loadingFinished", Request("A"));
  }

  void AttachPage(std::string session = "S1") {
    hook_->OnMessageFromAgent(base::StrCat(
        {R"({"method":"Target.attachedToTarget","params":{"sessionId":")",
         session,
         R"(","targetInfo":{"targetId":"PAGE","type":"page"}}})"}));
  }

  base::DictValue Sent(size_t i) {
    return base::JSONReader::ReadDict(delegate_->sent_to_frontend.at(i),
                                      base::JSON_PARSE_RFC)
        .value_or(base::DictValue());
  }

  raw_ptr<FakeDelegate> delegate_;
  std::unique_ptr<DevToolsFrontendHook> hook_;
};

TEST_F(DevToolsFrontendHookTest, ReplaysAfterThePagesNetworkEnable) {
  AttachPage();
  EXPECT_FALSE(hook_->OnMessageFromFrontend(
      R"({"id":7,"method":"Network.enable","params":{},"sessionId":"S1"})"));
  EXPECT_TRUE(delegate_->sent_to_frontend.empty());

  hook_->OnMessageFromAgent(R"({"id":7,"result":{},"sessionId":"S1"})");
  ASSERT_EQ(delegate_->sent_to_frontend.size(), 2u);
  base::DictValue first = Sent(0);
  EXPECT_EQ(*first.FindString("method"), "Network.requestWillBeSent");
  EXPECT_EQ(*first.FindString("sessionId"), "S1");
  EXPECT_EQ(*first.FindStringByDottedPath("params.requestId"), "A");

  // Once only.
  hook_->OnMessageFromAgent(R"({"id":7,"result":{},"sessionId":"S1"})");
  EXPECT_EQ(delegate_->sent_to_frontend.size(), 2u);
}

TEST_F(DevToolsFrontendHookTest, OnlyForThePagesSession) {
  AttachPage();
  // An iframe's or a worker's session.
  hook_->OnMessageFromAgent(
      R"({"method":"Target.attachedToTarget","params":{"sessionId":"W",)"
      R"("targetInfo":{"targetId":"WORKER","type":"worker"}}})");
  hook_->OnMessageFromFrontend(
      R"({"id":3,"method":"Network.enable","params":{},"sessionId":"W"})");
  hook_->OnMessageFromAgent(R"({"id":3,"result":{},"sessionId":"W"})");
  EXPECT_TRUE(delegate_->sent_to_frontend.empty());
}

TEST_F(DevToolsFrontendHookTest, NothingWhenNotRecording) {
  delegate_->recording = false;
  AttachPage();
  hook_->OnMessageFromFrontend(
      R"({"id":7,"method":"Network.enable","params":{},"sessionId":"S1"})");
  hook_->OnMessageFromAgent(R"({"id":7,"result":{},"sessionId":"S1"})");
  EXPECT_TRUE(delegate_->sent_to_frontend.empty());
}

TEST_F(DevToolsFrontendHookTest, AnswersReplayedBodiesFromTheLog) {
  AttachPage();
  hook_->OnMessageFromFrontend(
      R"({"id":7,"method":"Network.enable","params":{},"sessionId":"S1"})");
  hook_->OnMessageFromAgent(R"({"id":7,"result":{},"sessionId":"S1"})");
  delegate_->sent_to_frontend.clear();

  // A request the frontend saw itself goes to the page.
  EXPECT_FALSE(hook_->OnMessageFromFrontend(
      R"({"id":9,"method":"Network.getResponseBody",)"
      R"("params":{"requestId":"B"},"sessionId":"S1"})"));
  EXPECT_TRUE(delegate_->sent_to_log.empty());

  EXPECT_TRUE(hook_->OnMessageFromFrontend(
      R"({"id":10,"method":"Network.getResponseBody",)"
      R"("params":{"requestId":"A"},"sessionId":"S1"})"));
  ASSERT_EQ(delegate_->sent_to_log.size(), 1u);
  EXPECT_EQ(delegate_->sent_to_log[0].first, "Network.getResponseBody");
  EXPECT_EQ(*delegate_->sent_to_log[0].second.FindString("requestId"), "A");

  std::move(delegate_->log_callback)
      .Run(base::DictValue().Set("body", "hi").Set("base64Encoded", false));
  ASSERT_EQ(delegate_->sent_to_frontend.size(), 1u);
  base::DictValue reply = Sent(0);
  EXPECT_EQ(reply.FindInt("id"), 10);
  EXPECT_EQ(*reply.FindString("sessionId"), "S1");
  EXPECT_EQ(*reply.FindStringByDottedPath("result.body"), "hi");

  EXPECT_TRUE(hook_->OnMessageFromFrontend(
      R"({"id":11,"method":"Network.getRequestPostData",)"
      R"("params":{"requestId":"A"},"sessionId":"S1"})"));
  std::move(delegate_->log_callback).Run(base::unexpected("gone"));
  base::DictValue error = Sent(1);
  EXPECT_EQ(error.FindInt("id"), 11);
  EXPECT_EQ(*error.FindStringByDottedPath("error.message"), "gone");
}

TEST_F(DevToolsFrontendHookTest, ANewSessionStartsOver) {
  AttachPage("S1");
  hook_->OnMessageFromFrontend(
      R"({"id":7,"method":"Network.enable","params":{},"sessionId":"S1"})");
  hook_->OnMessageFromAgent(R"({"id":7,"result":{},"sessionId":"S1"})");
  // DevTools reloaded itself: the old session's requests aren't known to
  // the new one until it enables Network again.
  AttachPage("S2");
  EXPECT_FALSE(hook_->OnMessageFromFrontend(
      R"({"id":12,"method":"Network.getResponseBody",)"
      R"("params":{"requestId":"A"},"sessionId":"S2"})"));
}

}  // namespace
}  // namespace compass::dev_mode
