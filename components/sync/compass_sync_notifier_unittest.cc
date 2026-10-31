// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_sync_notifier.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/power_monitor_test.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "components/sync/base/data_type.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_network_connection_tracker.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::sync {
namespace {

using internal::ParseChangedTypes;
using internal::ServerSentEvent;
using internal::ServerSentEventParser;

constexpr int kBookmarksField = 32904;
constexpr int kPasswordsField = 45873;
constexpr int kCompassSpacesField = 970000;

// The server URL the notifier is given.
constexpr char kServer[] = "https://sync.example.com/v2";
constexpr char kEventsUrl[] =
    "https://sync.example.com/v2/compass/events?client_id=guid-1";

std::string ChangedEvent(std::vector<int> fields) {
  std::string list;
  for (int field : fields) {
    if (!list.empty()) {
      list += ",";
    }
    list += std::to_string(field);
  }
  return "event: changed\ndata: {\"types\":[" + list + "]}\n\n";
}

// Parser and helpers ---------------------------------------------------------

TEST(CompassSyncNotifierUrlTest, EventsUrl) {
  EXPECT_EQ(GURL("https://h.example/v2/compass/events?client_id=abc"),
            CompassSyncNotifier::MakeEventsUrl(GURL("https://h.example/v2"),
                                               "abc"));
  EXPECT_EQ(GURL("http://h.example:8295/v2/compass/events"),
            CompassSyncNotifier::MakeEventsUrl(
                GURL("http://h.example:8295/v2/?x=1#y"), ""));
  // Cache GUIDs are base64: escaped.
  EXPECT_EQ(GURL("https://h.example/v2/compass/events?client_id=a%2Bb%2Fc%3D"),
            CompassSyncNotifier::MakeEventsUrl(GURL("https://h.example/v2"),
                                               "a+b/c="));
}

std::vector<ServerSentEvent> ParseAll(const std::vector<std::string>& chunks) {
  ServerSentEventParser parser;
  std::vector<ServerSentEvent> events;
  for (const std::string& chunk : chunks) {
    EXPECT_TRUE(parser.Feed(chunk, &events));
  }
  return events;
}

TEST(CompassSyncNotifierParserTest, EventsAcrossChunks) {
  const std::vector<ServerSentEvent> events = ParseAll(
      {": connected\n\nevent: chan", "ged\ndata: {\"types\":[1]}", "\n",
       "\n: keep-alive\n\n", "data: plain\n\n"});
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ("changed", events[0].type);
  EXPECT_EQ("{\"types\":[1]}", events[0].data);
  EXPECT_EQ("message", events[1].type);
  EXPECT_EQ("plain", events[1].data);
}

TEST(CompassSyncNotifierParserTest, LineEndsAndFields) {
  // CRLF split between chunks, a bare CR, data over two lines, a value
  // without the space, and fields Compass doesn't use.
  const std::vector<ServerSentEvent> events =
      ParseAll({"event: a\r", "\ndata:x\rdata: y\r\n", "id: 7\nretry: 5\n\n",
                "event: no-data\n\n", "event: b\ndata\n\n"});
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ("a", events[0].type);
  EXPECT_EQ("x\ny", events[0].data);
  // An event without data isn't dispatched, and its type doesn't leak.
  EXPECT_EQ("b", events[1].type);
  EXPECT_EQ("", events[1].data);
}

TEST(CompassSyncNotifierParserTest, OverlongLineFails) {
  ServerSentEventParser parser;
  std::vector<ServerSentEvent> events;
  EXPECT_FALSE(parser.Feed(std::string(70 * 1024, 'x'), &events));
}

TEST(CompassSyncNotifierParserTest, ChangedTypes) {
  EXPECT_EQ(syncer::DataTypeSet({syncer::BOOKMARKS, syncer::PASSWORDS,
                                 syncer::COMPASS_SPACES}),
            ParseChangedTypes(
                "{\"types\":[32904, 45873, 970000, 1234567, \"x\"]}"));
  EXPECT_EQ(syncer::DataTypeSet(), ParseChangedTypes("{\"types\":[]}"));
  EXPECT_FALSE(ParseChangedTypes("{\"types\":32904}").has_value());
  EXPECT_FALSE(ParseChangedTypes("{}").has_value());
  EXPECT_FALSE(ParseChangedTypes("not json").has_value());
}

TEST(CompassSyncNotifierParserTest, FieldNumbersMapToTypes) {
  // The server sends EntitySpecifics field numbers.
  EXPECT_EQ(kBookmarksField,
            syncer::GetSpecificsFieldNumberFromDataType(syncer::BOOKMARKS));
  EXPECT_EQ(kPasswordsField,
            syncer::GetSpecificsFieldNumberFromDataType(syncer::PASSWORDS));
  EXPECT_EQ(kCompassSpacesField, syncer::GetSpecificsFieldNumberFromDataType(
                                     syncer::COMPASS_SPACES));
}

// The notifier, on a fake transport -------------------------------------------

class CompassSyncNotifierTest;

class FakeStream : public EventStream {
 public:
  FakeStream(CompassSyncNotifierTest* test, Client* client)
      : test_(test), client_(client) {}
  ~FakeStream() override;

  Client* client() { return client_; }

 private:
  raw_ptr<CompassSyncNotifierTest> test_;
  raw_ptr<Client> client_;
};

class CompassSyncNotifierTest : public testing::Test {
 public:
  // Called from ~FakeStream.
  void StreamGone(FakeStream* stream) {
    if (stream_ == stream) {
      stream_ = nullptr;
    }
  }

 protected:
  CompassSyncNotifierTest()
      : network_(network::TestNetworkConnectionTracker::CreateInstance()) {}

  void SetUp() override { notifier_ = MakeNotifier(); }

  std::unique_ptr<CompassSyncNotifier> MakeNotifier(
      std::optional<EventStreamFactory> factory = std::nullopt) {
    EventStreamFactory stream_factory;
    if (factory) {
      stream_factory = std::move(*factory);
    } else {
      stream_factory = base::BindLambdaForTesting(
          [this](const GURL& url, const std::string& token,
                 EventStream::Client* client) -> std::unique_ptr<EventStream> {
            ++connects_;
            last_url_ = url;
            last_token_ = token;
            auto stream = std::make_unique<FakeStream>(this, client);
            stream_ = stream.get();
            return stream;
          });
    }
    return std::make_unique<CompassSyncNotifier>(
        GURL(kServer), std::move(stream_factory),
        base::BindLambdaForTesting([this]() -> std::optional<std::string> {
          if (!token_) {
            return std::nullopt;
          }
          return *token_ + "-" + std::to_string(++tokens_made_);
        }),
        base::BindLambdaForTesting([this]() { return client_id_; }),
        base::BindLambdaForTesting(
            [this](syncer::DataTypeSet types) { refreshes_.push_back(types); }),
        network::TestNetworkConnectionTracker::GetInstance());
  }

  // The open stream starts with |status|, and says hello.
  void Answer(int status = 200) {
    ASSERT_TRUE(stream_);
    stream_->client()->OnStreamStarted(status);
    if (stream_) {
      stream_->client()->OnStreamData(": connected\n\n");
    }
  }

  void Send(const std::string& data) {
    ASSERT_TRUE(stream_);
    stream_->client()->OnStreamData(data);
  }

  void End() {
    ASSERT_TRUE(stream_);
    stream_->client()->OnStreamEnded();
  }

  // Waits (in 100 ms steps, at most a little over the 5 min cap) for the
  // next connection attempt.
  void WaitForConnect() {
    for (int i = 0; i < 3100 && !stream_; ++i) {
      task_environment_.FastForwardBy(base::Milliseconds(100));
    }
    ASSERT_TRUE(stream_);
  }

  // Fails the stream |n| times, waiting out each backoff.
  void FailTimes(int n) {
    for (int i = 0; i < n; ++i) {
      End();
      ASSERT_FALSE(stream_);
      WaitForConnect();
    }
  }

  void FastForwardBy(base::TimeDelta delta) {
    task_environment_.FastForwardBy(delta);
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::test::ScopedPowerMonitorTestSource power_;
  std::unique_ptr<network::TestNetworkConnectionTracker> network_;

  std::optional<std::string> token_ = "token";
  int tokens_made_ = 0;
  std::string client_id_ = "guid-1";
  int connects_ = 0;
  GURL last_url_;
  std::string last_token_;
  raw_ptr<FakeStream> stream_ = nullptr;
  std::vector<syncer::DataTypeSet> refreshes_;

  std::unique_ptr<CompassSyncNotifier> notifier_;
};

FakeStream::~FakeStream() {
  test_->StreamGone(this);
}

TEST_F(CompassSyncNotifierTest, ConnectsWithAFreshTokenAndTheDevicesId) {
  notifier_->Start();
  EXPECT_EQ(1, connects_);
  EXPECT_EQ(GURL(kEventsUrl), last_url_);
  EXPECT_EQ("token-1", last_token_);
  EXPECT_FALSE(notifier_->IsConnected());
  Answer();
  EXPECT_TRUE(notifier_->IsConnected());
  // The first connection refreshes nothing: the engine just started.
  EXPECT_TRUE(refreshes_.empty());

  End();
  FastForwardBy(base::Seconds(1));
  EXPECT_EQ(2, connects_);
  EXPECT_EQ("token-2", last_token_);
}

TEST_F(CompassSyncNotifierTest, AChangeRefreshesItsTypesAfterAShortDelay) {
  notifier_->Start();
  Answer();
  Send(ChangedEvent({kBookmarksField}));
  EXPECT_TRUE(refreshes_.empty());
  FastForwardBy(CompassSyncNotifier::kRefreshDelay);
  ASSERT_EQ(1u, refreshes_.size());
  EXPECT_EQ(syncer::DataTypeSet({syncer::BOOKMARKS}), refreshes_[0]);
}

TEST_F(CompassSyncNotifierTest, ABurstIsOneRefresh) {
  notifier_->Start();
  Answer();
  Send(ChangedEvent({kBookmarksField}));
  FastForwardBy(base::Milliseconds(100));
  // Split across chunks, as the network may deliver it.
  const std::string event = ChangedEvent({kPasswordsField, kCompassSpacesField});
  Send(event.substr(0, 10));
  Send(event.substr(10));
  FastForwardBy(CompassSyncNotifier::kRefreshDelay);
  ASSERT_EQ(1u, refreshes_.size());
  EXPECT_EQ(syncer::DataTypeSet({syncer::BOOKMARKS, syncer::PASSWORDS,
                                 syncer::COMPASS_SPACES}),
            refreshes_[0]);

  // The next change is a refresh of its own.
  Send(ChangedEvent({kBookmarksField}));
  FastForwardBy(CompassSyncNotifier::kRefreshDelay);
  ASSERT_EQ(2u, refreshes_.size());
  EXPECT_EQ(syncer::DataTypeSet({syncer::BOOKMARKS}), refreshes_[1]);
}

TEST_F(CompassSyncNotifierTest, KeepAlivesAndOtherEventsRefreshNothing) {
  notifier_->Start();
  Answer();
  Send(": keep-alive\n\n");
  Send("event: hello\ndata: {}\n\n");
  // Only types Chromium doesn't know.
  Send(ChangedEvent({1234567}));
  FastForwardBy(base::Seconds(1));
  EXPECT_TRUE(refreshes_.empty());
}

TEST_F(CompassSyncNotifierTest, AnUnreadableChangeRefreshesEverything) {
  notifier_->Start();
  Answer();
  Send("event: changed\ndata: {oops\n\n");
  FastForwardBy(CompassSyncNotifier::kRefreshDelay);
  ASSERT_EQ(1u, refreshes_.size());
  EXPECT_EQ(syncer::ProtocolTypes(), refreshes_[0]);
}

TEST_F(CompassSyncNotifierTest, BacksOffExponentiallyWithJitter) {
  notifier_->Start();
  // 1 s, 2 s, 4 s, ... up to 5 min, each up to 20% shorter.
  base::TimeDelta expected = base::Seconds(1);
  for (int i = 1; i <= 12; ++i) {
    SCOPED_TRACE(i);
    End();
    EXPECT_EQ(i, notifier_->failures());
    const int before = connects_;
    FastForwardBy(expected * 0.8 - base::Milliseconds(1));
    EXPECT_EQ(before, connects_) << "too early";
    FastForwardBy(expected * 0.2 + base::Milliseconds(1));
    EXPECT_EQ(before + 1, connects_) << "too late";
    expected = std::min(expected * 2, base::Minutes(5));
  }
}

TEST_F(CompassSyncNotifierTest, AHealthyStreamStartsBackoffOver) {
  notifier_->Start();
  FailTimes(5);
  EXPECT_EQ(5, notifier_->failures());

  // A stream that ends quickly doesn't count as healthy...
  Answer();
  End();
  EXPECT_EQ(6, notifier_->failures());
  WaitForConnect();

  // ...one that lasted does: the next try is within a second.
  Answer();
  FastForwardBy(CompassSyncNotifier::kHealthyDuration);
  End();
  EXPECT_EQ(1, notifier_->failures());
  const int before = connects_;
  FastForwardBy(base::Seconds(1));
  EXPECT_EQ(before + 1, connects_);
}

TEST_F(CompassSyncNotifierTest, AReconnectRefreshesEverything) {
  notifier_->Start();
  Answer();
  End();
  FastForwardBy(base::Seconds(1));
  EXPECT_TRUE(refreshes_.empty());
  Answer();
  // At once: changes made while it was down weren't announced.
  ASSERT_EQ(1u, refreshes_.size());
  EXPECT_EQ(syncer::ProtocolTypes(), refreshes_[0]);
}

TEST_F(CompassSyncNotifierTest, HttpErrorsBackOff) {
  notifier_->Start();
  Answer(401);
  EXPECT_FALSE(stream_);
  EXPECT_FALSE(notifier_->IsConnected());
  FastForwardBy(base::Seconds(1));
  EXPECT_EQ(2, connects_);
  Answer(429);
  EXPECT_EQ(2, notifier_->failures());
  FastForwardBy(base::Seconds(2));
  EXPECT_EQ(3, connects_);
}

TEST_F(CompassSyncNotifierTest, ASilentStreamIsReplaced) {
  notifier_->Start();
  Answer();
  // Keep-alives every 25 s keep it.
  for (int i = 0; i < 8; ++i) {
    FastForwardBy(base::Seconds(25));
    Send(": keep-alive\n\n");
  }
  EXPECT_EQ(1, connects_);
  EXPECT_TRUE(notifier_->IsConnected());
  // Silence does not.
  FastForwardBy(CompassSyncNotifier::kIdleTimeout);
  EXPECT_FALSE(notifier_->IsConnected());
  FastForwardBy(base::Seconds(1));
  EXPECT_EQ(2, connects_);
}

TEST_F(CompassSyncNotifierTest, NoAnswerIsAFailure) {
  notifier_->Start();
  FastForwardBy(CompassSyncNotifier::kIdleTimeout);
  EXPECT_FALSE(stream_);
  EXPECT_EQ(1, notifier_->failures());
}

TEST_F(CompassSyncNotifierTest, ANetworkChangeReconnectsAtOnce) {
  notifier_->Start();
  FailTimes(6);  // The next try would be ~1 min away.
  End();
  const int before = connects_;
  network_->SetConnectionType(net::NetworkChangeNotifier::CONNECTION_WIFI);
  FastForwardBy(CompassSyncNotifier::kReconnectSoonDelay);
  EXPECT_EQ(before + 1, connects_);
  EXPECT_EQ(0, notifier_->failures());

  // A connected stream is replaced too (its socket may be gone).
  Answer();
  network_->SetConnectionType(net::NetworkChangeNotifier::CONNECTION_ETHERNET);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(notifier_->IsConnected());
  FastForwardBy(CompassSyncNotifier::kReconnectSoonDelay);
  EXPECT_EQ(before + 2, connects_);
}

TEST_F(CompassSyncNotifierTest, OfflineKeepsTryingWithBackoff) {
  notifier_->Start();
  Answer();
  network_->SetConnectionType(net::NetworkChangeNotifier::CONNECTION_NONE);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(notifier_->IsConnected());
  EXPECT_EQ(1, notifier_->failures());
  FastForwardBy(base::Seconds(1));
  EXPECT_EQ(2, connects_);
}

TEST_F(CompassSyncNotifierTest, WakingUpReconnectsAtOnce) {
  notifier_->Start();
  FailTimes(6);
  End();
  const int before = connects_;
  power_.GenerateSuspendEvent();
  power_.GenerateResumeEvent();
  FastForwardBy(CompassSyncNotifier::kReconnectSoonDelay);
  EXPECT_EQ(before + 1, connects_);
}

TEST_F(CompassSyncNotifierTest, WaitsForTheKey) {
  token_ = std::nullopt;
  notifier_->Start();
  EXPECT_EQ(0, connects_);
  token_ = "token";
  FastForwardBy(base::Seconds(1));
  EXPECT_EQ(1, connects_);
}

TEST_F(CompassSyncNotifierTest, WithoutAnIdTheDeviceHearsItsOwnCommits) {
  client_id_.clear();
  notifier_->Start();
  EXPECT_EQ(GURL("https://sync.example.com/v2/compass/events"), last_url_);
}

TEST_F(CompassSyncNotifierTest, StopsWhenDestroyed) {
  // CompassSyncService destroys it when the chain stops (leave, shutdown).
  notifier_->Start();
  Answer();
  Send(ChangedEvent({kBookmarksField}));
  notifier_.reset();
  EXPECT_FALSE(stream_);
  FastForwardBy(base::Minutes(10));
  network_->SetConnectionType(net::NetworkChangeNotifier::CONNECTION_WIFI);
  power_.GenerateResumeEvent();
  FastForwardBy(base::Minutes(10));
  EXPECT_EQ(1, connects_);
  EXPECT_TRUE(refreshes_.empty());
}

// The notifier over network::SimpleURLLoader ----------------------------------

class CompassSyncNotifierNetworkTest : public CompassSyncNotifierTest {
 protected:
  void SetUp() override {
    notifier_ = MakeNotifier(MakeNetworkEventStreamFactory(
        url_loader_factory_.GetSafeWeakWrapper()));
  }

  void TearDown() override { notifier_.reset(); }

  network::TestURLLoaderFactory url_loader_factory_;
};

TEST_F(CompassSyncNotifierNetworkTest, StreamsTheBodyAsItArrives) {
  notifier_->Start();
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(1, url_loader_factory_.NumPending());
  network::TestURLLoaderFactory::PendingRequest* pending =
      url_loader_factory_.GetPendingRequest(0);
  EXPECT_EQ(GURL(kEventsUrl), pending->request.url);
  EXPECT_EQ("GET", pending->request.method);
  EXPECT_EQ("Bearer token-1",
            pending->request.headers.GetHeader("Authorization"));
  EXPECT_EQ("text/event-stream", pending->request.headers.GetHeader("Accept"));

  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  ASSERT_EQ(MOJO_RESULT_OK, mojo::CreateDataPipe(nullptr, producer, consumer));
  network::mojom::URLResponseHeadPtr head =
      network::CreateURLResponseHead(net::HTTP_OK);
  head->mime_type = "text/event-stream";
  pending->client->OnReceiveResponse(std::move(head), std::move(consumer),
                                     std::nullopt);
  const std::string hello = ": connected\n\n";
  ASSERT_EQ(MOJO_RESULT_OK,
            producer->WriteAllData(base::as_byte_span(hello)));
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(notifier_->IsConnected());

  // An event while the response is still open.
  const std::string event = ChangedEvent({kBookmarksField});
  ASSERT_EQ(MOJO_RESULT_OK,
            producer->WriteAllData(base::as_byte_span(event)));
  base::RunLoop().RunUntilIdle();
  FastForwardBy(CompassSyncNotifier::kRefreshDelay);
  ASSERT_EQ(1u, refreshes_.size());
  EXPECT_EQ(syncer::DataTypeSet({syncer::BOOKMARKS}), refreshes_[0]);

  // The connection drops: the notifier comes back.
  producer.reset();
  url_loader_factory_.GetPendingRequest(0)->client->OnComplete(
      network::URLLoaderCompletionStatus(net::ERR_CONNECTION_RESET));
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(notifier_->IsConnected());
  EXPECT_EQ(1, notifier_->failures());
  FastForwardBy(base::Seconds(1));
  EXPECT_EQ(2u, url_loader_factory_.total_requests());
}

TEST_F(CompassSyncNotifierNetworkTest, AWholeResponseIsReadToo) {
  url_loader_factory_.AddResponse(
      kEventsUrl, ": connected\n\n" + ChangedEvent({kCompassSpacesField}));
  notifier_->Start();
  base::RunLoop().RunUntilIdle();
  FastForwardBy(CompassSyncNotifier::kRefreshDelay);
  ASSERT_EQ(1u, refreshes_.size());
  EXPECT_EQ(syncer::DataTypeSet({syncer::COMPASS_SPACES}), refreshes_[0]);
  // The server ended it: retry with backoff.
  EXPECT_FALSE(notifier_->IsConnected());
  EXPECT_EQ(1, notifier_->failures());
}

TEST_F(CompassSyncNotifierNetworkTest, AnAuthErrorBacksOff) {
  url_loader_factory_.AddResponse(kEventsUrl, "{\"error\":\"invalid\"}",
                                  net::HTTP_UNAUTHORIZED);
  notifier_->Start();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(notifier_->IsConnected());
  EXPECT_EQ(1, notifier_->failures());
  EXPECT_TRUE(refreshes_.empty());
}

}  // namespace
}  // namespace compass::sync
