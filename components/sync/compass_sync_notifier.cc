// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/compass_sync_notifier.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/power_monitor/power_monitor.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace compass::sync {
namespace {

// Longest line (or event data) accepted from the server. Events are a few
// dozen bytes.
constexpr size_t kMaxLineSize = 64 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("compass_sync_notifications", R"(
        semantics {
          sender: "Compass sync change notifications"
          description:
            "Keeps one request open to the user's sync server, which says "
            "when another of the user's devices changed synced data, so this "
            "device fetches the change at once instead of at its next poll."
          trigger: "Runs while sync is on."
          data: "The sync chain's signed token and this device's sync cache "
                "GUID. The server sends back only which data types changed."
          destination: OTHER
          destination_other: "The self-hosted sync server the user set."
        }
        policy {
          cookies_allowed: NO
          setting: "Leaving sync in Compass's sync page stops it."
          policy_exception_justification: "Not implemented."
        })");

// A stream over network::SimpleURLLoader, body read as it arrives.
class NetworkEventStream : public EventStream,
                           public network::SimpleURLLoaderStreamConsumer {
 public:
  NetworkEventStream(network::SharedURLLoaderFactory* url_loader_factory,
                     const GURL& url,
                     const std::string& token,
                     Client* client);
  NetworkEventStream(const NetworkEventStream&) = delete;
  NetworkEventStream& operator=(const NetworkEventStream&) = delete;
  ~NetworkEventStream() override;

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view data, base::OnceClosure resume) override;
  void OnComplete(bool success) override;
  void OnRetry(base::OnceClosure start_retry) override;

 private:
  void OnResponseStarted(const GURL& final_url,
                         const network::mojom::URLResponseHead& head);

  const raw_ptr<Client> client_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::WeakPtrFactory<NetworkEventStream> weak_factory_{this};
};

NetworkEventStream::NetworkEventStream(
    network::SharedURLLoaderFactory* url_loader_factory,
    const GURL& url,
    const std::string& token,
    Client* client)
    : client_(client) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader(net::HttpRequestHeaders::kAuthorization,
                             base::StrCat({"Bearer ", token}));
  request->headers.SetHeader(net::HttpRequestHeaders::kAccept,
                             "text/event-stream");
  request->headers.SetHeader(net::HttpRequestHeaders::kCacheControl,
                             "no-store");
  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  // No timeout: the stream is meant to stay open. The notifier ends a
  // silent one.
  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &NetworkEventStream::OnResponseStarted, base::Unretained(this)));
  loader_->DownloadAsStream(url_loader_factory, this);
}

NetworkEventStream::~NetworkEventStream() = default;

void NetworkEventStream::OnDataReceived(std::string_view data,
                                        base::OnceClosure resume) {
  // The client may destroy this stream (and the loader) while handling it.
  base::WeakPtr<NetworkEventStream> self = weak_factory_.GetWeakPtr();
  client_->OnStreamData(data);
  if (self) {
    std::move(resume).Run();
  }
}

void NetworkEventStream::OnComplete(bool success) {
  client_->OnStreamEnded();
}

void NetworkEventStream::OnRetry(base::OnceClosure start_retry) {
  // No retries are set; the notifier reconnects itself.
  client_->OnStreamEnded();
}

void NetworkEventStream::OnResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  client_->OnStreamStarted(head.headers ? head.headers->response_code() : 0);
}

}  // namespace

EventStream::Client::~Client() = default;
EventStream::~EventStream() = default;

EventStreamFactory MakeNetworkEventStreamFactory(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  return base::BindRepeating(
      [](scoped_refptr<network::SharedURLLoaderFactory> factory,
         const GURL& url, const std::string& token,
         EventStream::Client* client) -> std::unique_ptr<EventStream> {
        return std::make_unique<NetworkEventStream>(factory.get(), url, token,
                                                    client);
      },
      std::move(url_loader_factory));
}

namespace internal {

ServerSentEventParser::ServerSentEventParser() = default;
ServerSentEventParser::~ServerSentEventParser() = default;

void ServerSentEventParser::Reset() {
  buffer_.clear();
  skip_lf_ = false;
  event_type_.clear();
  data_.clear();
  has_data_ = false;
}

bool ServerSentEventParser::Feed(std::string_view bytes,
                                 std::vector<ServerSentEvent>* events) {
  for (const char c : bytes) {
    if (skip_lf_) {
      skip_lf_ = false;
      if (c == '\n') {
        continue;  // The LF of a CRLF.
      }
    }
    if (c == '\r' || c == '\n') {
      skip_lf_ = c == '\r';
      HandleLine(buffer_, events);
      buffer_.clear();
      if (data_.size() > kMaxLineSize) {
        return false;
      }
      continue;
    }
    buffer_.push_back(c);
    if (buffer_.size() > kMaxLineSize) {
      return false;
    }
  }
  return true;
}

void ServerSentEventParser::HandleLine(std::string_view line,
                                       std::vector<ServerSentEvent>* events) {
  if (line.empty()) {
    // Dispatch.
    if (has_data_) {
      events->push_back(
          {event_type_.empty() ? std::string("message") : event_type_, data_});
    }
    event_type_.clear();
    data_.clear();
    has_data_ = false;
    return;
  }
  if (line.front() == ':') {
    return;  // A comment (the server's keep-alive).
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
    event_type_ = std::string(value);
  } else if (field == "data") {
    if (has_data_) {
      data_.push_back('\n');
    }
    data_.append(value);
    has_data_ = true;
  }
  // "id", "retry" and anything else: not used.
}

std::optional<syncer::DataTypeSet> ParseChangedTypes(std::string_view data) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(data, base::JSON_PARSE_RFC);
  const base::ListValue* list = dict ? dict->FindList("types") : nullptr;
  if (!list) {
    return std::nullopt;
  }
  syncer::DataTypeSet types;
  for (const base::Value& item : *list) {
    const std::optional<int> field_number = item.GetIfInt();
    if (!field_number) {
      continue;
    }
    const syncer::DataType type =
        syncer::GetDataTypeFromSpecificsFieldNumber(*field_number);
    if (type != syncer::UNSPECIFIED) {
      types.Put(type);
    }
  }
  return types;
}

}  // namespace internal

// static
const net::BackoffEntry::Policy CompassSyncNotifier::kBackoffPolicy = {
    // num_errors_to_ignore
    0,
    // initial_delay_ms
    1000,
    // multiply_factor
    2.0,
    // jitter_factor: up to 20% shorter, so devices that lost the server
    // together don't all come back in the same second.
    0.2,
    // maximum_backoff_ms: 5 min.
    5 * 60 * 1000,
    // entry_lifetime_ms: never discard.
    -1,
    // always_use_initial_delay
    false,
};

CompassSyncNotifier::CompassSyncNotifier(
    const GURL& server_url,
    EventStreamFactory stream_factory,
    TokenGetter token_getter,
    ClientIdGetter client_id_getter,
    RefreshCallback refresh,
    network::NetworkConnectionTracker* network)
    : server_url_(server_url),
      stream_factory_(std::move(stream_factory)),
      token_getter_(std::move(token_getter)),
      client_id_getter_(std::move(client_id_getter)),
      refresh_(std::move(refresh)),
      network_(network),
      backoff_(&kBackoffPolicy) {
  if (network_) {
    network_->AddNetworkConnectionObserver(this);
  }
  if (base::PowerMonitor* power = base::PowerMonitor::GetInstance()) {
    power->AddPowerSuspendObserver(this);
    observing_power_ = true;
  }
}

CompassSyncNotifier::~CompassSyncNotifier() {
  Drop();
  if (network_) {
    network_->RemoveNetworkConnectionObserver(this);
  }
  if (observing_power_) {
    base::PowerMonitor::GetInstance()->RemovePowerSuspendObserver(this);
  }
  if (was_connected_) {
    VLOG(1) << "[compass-sync] notifier: stopped";
  }
}

// static
GURL CompassSyncNotifier::MakeEventsUrl(const GURL& server_url,
                                        std::string_view client_id) {
  std::string path(server_url.GetPath());
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  path += "/compass/events";
  const std::string query =
      client_id.empty()
          ? std::string()
          : base::StrCat({"client_id=",
                          base::EscapeQueryParamValue(client_id,
                                                      /*use_plus=*/false)});
  GURL::Replacements replacements;
  replacements.SetPathStr(path);
  if (query.empty()) {
    replacements.ClearQuery();
  } else {
    replacements.SetQueryStr(query);
  }
  replacements.ClearRef();
  return server_url.ReplaceComponents(replacements);
}

void CompassSyncNotifier::Start() {
  Connect();
}

void CompassSyncNotifier::Connect() {
  retry_timer_.Stop();
  Drop();
  std::optional<std::string> token = token_getter_.Run();
  if (!token || token->empty()) {
    Fail("no sync key yet");
    return;
  }
  const GURL url = MakeEventsUrl(server_url_, client_id_getter_.Run());
  VLOG(1) << "[compass-sync] notifier: connecting to " << url.spec();
  parser_.Reset();
  // Also bounds the wait for the response to start.
  idle_timer_.Start(FROM_HERE, kIdleTimeout, this,
                    &CompassSyncNotifier::OnIdle);
  stream_ = stream_factory_.Run(url, *token, this);
}

void CompassSyncNotifier::Drop() {
  stream_.reset();
  connected_ = false;
  idle_timer_.Stop();
}

void CompassSyncNotifier::Fail(std::string_view why) {
  const bool healthy = connected_ && base::TimeTicks::Now() - connected_at_ >=
                                         kHealthyDuration;
  Drop();
  if (healthy) {
    backoff_.Reset();
  }
  backoff_.InformOfRequest(/*succeeded=*/false);
  const base::TimeDelta delay = backoff_.GetTimeUntilRelease();
  VLOG(1) << "[compass-sync] notifier: " << why << "; reconnecting in "
          << delay.InMilliseconds() << " ms (failure "
          << backoff_.failure_count() << ")";
  retry_timer_.Start(FROM_HERE, delay, this, &CompassSyncNotifier::Connect);
}

void CompassSyncNotifier::ReconnectSoon(std::string_view why) {
  Drop();
  backoff_.Reset();
  VLOG(1) << "[compass-sync] notifier: " << why << "; reconnecting";
  retry_timer_.Start(FROM_HERE, kReconnectSoonDelay, this,
                     &CompassSyncNotifier::Connect);
}

void CompassSyncNotifier::OnIdle() {
  Fail(connected_ ? "stream silent too long" : "no response");
}

void CompassSyncNotifier::OnConnectionChanged(
    net::NetworkChangeNotifier::ConnectionType type) {
  if (type == net::NetworkChangeNotifier::CONNECTION_NONE) {
    // A hint only (sync.md): keep trying, with backoff, rather than wait
    // for a change that may not be reported.
    Fail("network down");
    return;
  }
  ReconnectSoon("network changed");
}

void CompassSyncNotifier::OnResume() {
  ReconnectSoon("woke up");
}

void CompassSyncNotifier::OnStreamStarted(int http_status) {
  if (http_status != 200) {
    Fail(base::StrCat({"HTTP ", base::NumberToString(http_status)}));
    return;
  }
  connected_ = true;
  connected_at_ = base::TimeTicks::Now();
  idle_timer_.Start(FROM_HERE, kIdleTimeout, this,
                    &CompassSyncNotifier::OnIdle);
  VLOG(1) << "[compass-sync] notifier: connected";
  if (was_connected_) {
    // Changes made while the stream was down weren't announced.
    pending_types_ = syncer::ProtocolTypes();
    refresh_timer_.Stop();
    FlushRefresh();
  }
  was_connected_ = true;
}

void CompassSyncNotifier::OnStreamData(std::string_view data) {
  if (!connected_) {
    return;
  }
  idle_timer_.Start(FROM_HERE, kIdleTimeout, this,
                    &CompassSyncNotifier::OnIdle);
  std::vector<internal::ServerSentEvent> events;
  if (!parser_.Feed(data, &events)) {
    Fail("line too long");
    return;
  }
  for (const internal::ServerSentEvent& event : events) {
    if (event.type != "changed") {
      continue;
    }
    std::optional<syncer::DataTypeSet> types =
        internal::ParseChangedTypes(event.data);
    LOG_IF(WARNING, !types) << "Compass sync: unreadable change event";
    OnChanged(types.value_or(syncer::ProtocolTypes()));
  }
}

void CompassSyncNotifier::OnStreamEnded() {
  Fail(connected_ ? "stream closed" : "couldn't connect");
}

void CompassSyncNotifier::OnChanged(syncer::DataTypeSet types) {
  VLOG(1) << "[compass-sync] notifier: changed "
          << syncer::DataTypeSetToDebugString(types);
  if (types.empty()) {
    return;
  }
  pending_types_.PutAll(types);
  if (!refresh_timer_.IsRunning()) {
    refresh_timer_.Start(FROM_HERE, kRefreshDelay, this,
                         &CompassSyncNotifier::FlushRefresh);
  }
}

void CompassSyncNotifier::FlushRefresh() {
  if (pending_types_.empty()) {
    return;
  }
  const syncer::DataTypeSet types = pending_types_;
  pending_types_.Clear();
  VLOG(1) << "[compass-sync] notifier: refresh "
          << syncer::DataTypeSetToDebugString(types);
  refresh_.Run(types);
}

}  // namespace compass::sync
