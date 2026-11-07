// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_COMPASS_SYNC_NOTIFIER_H_
#define COMPASS_COMPONENTS_SYNC_COMPASS_SYNC_NOTIFIER_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/power_monitor/power_observer.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/sync/base/data_type.h"
#include "net/base/backoff_entry.h"
#include "net/base/network_change_notifier.h"
#include "services/network/public/cpp/network_connection_tracker.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace compass::sync {

// One open event stream (GET <server>/compass/events). Destroying it closes
// the connection; no client method is called after that.
class EventStream {
 public:
  // What the stream reports. It may be destroyed from any of these.
  class Client {
   public:
    // The response started, with this HTTP status (0: none).
    virtual void OnStreamStarted(int http_status) = 0;
    // Bytes of the body, as they arrive.
    virtual void OnStreamData(std::string_view data) = 0;
    // The server or the network ended the stream, or it never started.
    virtual void OnStreamEnded() = 0;

   protected:
    virtual ~Client();
  };

  virtual ~EventStream();
};

// Opens a stream to |url|, signed with |token|, reporting to |client|.
using EventStreamFactory =
    base::RepeatingCallback<std::unique_ptr<EventStream>(const GURL& url,
                                                         const std::string&
                                                             token,
                                                         EventStream::Client*)>;

// The real one: network::SimpleURLLoader in streaming mode
// (DownloadAsStream with a SimpleURLLoaderStreamConsumer), no timeout (the
// notifier's own idle watchdog ends dead streams).
EventStreamFactory MakeNetworkEventStreamFactory(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

namespace internal {

// One Server-Sent Event.
struct ServerSentEvent {
  std::string type;  // "message" if the stream gave none.
  std::string data;
};

// Splits a text/event-stream body into events, as the HTML spec's parser
// does for the fields Compass uses (event, data; comments and other fields
// are skipped). Lines end with LF, CRLF or CR.
class ServerSentEventParser {
 public:
  ServerSentEventParser();
  ServerSentEventParser(const ServerSentEventParser&) = delete;
  ServerSentEventParser& operator=(const ServerSentEventParser&) = delete;
  ~ServerSentEventParser();

  // Forgets everything (a new stream).
  void Reset();

  // Adds |bytes| and returns the events they complete. False if a line
  // grew beyond the limit (the stream should be dropped).
  bool Feed(std::string_view bytes, std::vector<ServerSentEvent>* events);

 private:
  void HandleLine(std::string_view line, std::vector<ServerSentEvent>* events);

  std::string buffer_;
  bool skip_lf_ = false;  // The last chunk ended with CR.
  std::string event_type_;
  std::string data_;
  bool has_data_ = false;
};

// The data types of a "changed" event's data, {"types": [field numbers]}.
// Field numbers Chromium doesn't know are left out. nullopt if the data
// isn't of that form (the caller then refreshes everything).
std::optional<syncer::DataTypeSet> ParseChangedTypes(std::string_view data);

}  // namespace internal

// Keeps a stream of change notifications open to the chain's server
// (docs/design/sync.md, "Change notifications") and asks the sync engine for
// updates as soon as another device commits, instead of waiting for the
// poll. Owned by CompassSyncService while its chain runs.
//
// It reconnects with exponential backoff (1 s to 5 min, jittered), at once
// when the network changes or the Mac wakes, and when the stream has been
// silent for longer than the server's keep-alive allows. After a reconnect
// it refreshes every type: changes made while it was away weren't announced.
class CompassSyncNotifier
    : public network::NetworkConnectionTracker::NetworkConnectionObserver,
      public base::PowerSuspendObserver,
      public EventStream::Client {
 public:
  // A fresh sync token, or nullopt if the key isn't there yet.
  using TokenGetter = base::RepeatingCallback<std::optional<std::string>()>;
  // This device's cache GUID, so the server leaves out its own commits;
  // empty if not known yet.
  using ClientIdGetter = base::RepeatingCallback<std::string()>;
  // Asks the engine for updates of |types| (ProtocolTypes() for "all").
  using RefreshCallback = base::RepeatingCallback<void(syncer::DataTypeSet)>;

  // A refresh waits this long for more events (a burst of commits from
  // another device), then asks once for the union of their types.
  static constexpr base::TimeDelta kRefreshDelay = base::Milliseconds(250);
  // A network change or a wake reconnects after this (they come in bursts).
  static constexpr base::TimeDelta kReconnectSoonDelay =
      base::Milliseconds(500);
  // The server sends a keep-alive every 25 s; this long without a byte
  // means the stream is dead (a NAT or proxy dropped it silently).
  static constexpr base::TimeDelta kIdleTimeout = base::Seconds(75);
  // A stream that lasted this long was healthy: backoff starts over.
  static constexpr base::TimeDelta kHealthyDuration = base::Seconds(30);
  static const net::BackoffEntry::Policy kBackoffPolicy;

  // |network| and power observation are optional (null in some tests).
  CompassSyncNotifier(const GURL& server_url,
                      EventStreamFactory stream_factory,
                      TokenGetter token_getter,
                      ClientIdGetter client_id_getter,
                      RefreshCallback refresh,
                      network::NetworkConnectionTracker* network);
  CompassSyncNotifier(const CompassSyncNotifier&) = delete;
  CompassSyncNotifier& operator=(const CompassSyncNotifier&) = delete;
  ~CompassSyncNotifier() override;

  // Connects now.
  void Start();

  bool IsConnected() const { return connected_; }
  int failures() const { return backoff_.failure_count(); }

  // <server path>/compass/events?client_id=<client id>.
  static GURL MakeEventsUrl(const GURL& server_url,
                            std::string_view client_id);

  // network::NetworkConnectionTracker::NetworkConnectionObserver:
  void OnConnectionChanged(
      net::NetworkChangeNotifier::ConnectionType type) override;

  // base::PowerSuspendObserver:
  void OnResume() override;

  // EventStream::Client:
  void OnStreamStarted(int http_status) override;
  void OnStreamData(std::string_view data) override;
  void OnStreamEnded() override;

 private:
  void Connect();
  // Closes the stream, if any, without scheduling anything.
  void Drop();
  // Drops the stream and tries again after the backoff.
  void Fail(std::string_view why);
  // Drops the stream and connects after kReconnectSoonDelay, backoff reset.
  void ReconnectSoon(std::string_view why);
  void OnIdle();
  void OnChanged(syncer::DataTypeSet types);
  void FlushRefresh();

  const GURL server_url_;
  EventStreamFactory stream_factory_;
  TokenGetter token_getter_;
  ClientIdGetter client_id_getter_;
  RefreshCallback refresh_;
  raw_ptr<network::NetworkConnectionTracker> network_;
  bool observing_power_ = false;

  std::unique_ptr<EventStream> stream_;
  internal::ServerSentEventParser parser_;
  bool connected_ = false;
  // A stream was up before: the next one refreshes everything.
  bool was_connected_ = false;
  base::TimeTicks connected_at_;
  net::BackoffEntry backoff_;
  base::OneShotTimer retry_timer_;
  base::OneShotTimer idle_timer_;
  base::OneShotTimer refresh_timer_;
  syncer::DataTypeSet pending_types_;

  base::WeakPtrFactory<CompassSyncNotifier> weak_factory_{this};
};

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_COMPASS_SYNC_NOTIFIER_H_
