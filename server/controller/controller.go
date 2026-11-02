package controller

import (
	"compress/gzip"
	"io"
	"net/http"
	"slices"

	"github.com/brave-intl/bat-go/libs/closers"
	"github.com/brave-intl/bat-go/libs/middleware"
	"github.com/go-chi/chi/v5"
	"github.com/rs/zerolog/log"
	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/cache"
	"github.com/brave/go-sync/command"
	"github.com/brave/go-sync/datastore"
	syncMiddleware "github.com/brave/go-sync/middleware"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
	syncContext "github.com/brave/go-sync/synccontext"
)

const (
	payloadLimit10MB = 1024 * 1024 * 10
)

// CommitNotifier is told, after a commit, which data types it changed
// (their EntitySpecifics field numbers), for chain |clientID|, by the device
// with |cacheGUID| (Compass: change notifications, events.Hub.Notify), and
// which of them it added entities of (|created|: push for sent tabs,
// push.Service.OnCommit).
type CommitNotifier func(clientID, cacheGUID string, types, created []int)

// SyncRouter add routers for command and auth endpoint requests. |notify|
// may be nil.
func SyncRouter(cache *cache.Cache, datastore datastore.Datastore, notify CommitNotifier) chi.Router {
	r := chi.NewRouter()
	r.Use(syncMiddleware.Auth)
	r.Use(syncMiddleware.DisabledChain)
	r.Method("POST", "/command/", middleware.InstrumentHandler("Command", CommandWithNotifier(cache, datastore, notify)))
	return r
}

// Command handles GetUpdates and Commit requests from sync clients.
func Command(cache *cache.Cache, db datastore.Datastore) http.HandlerFunc {
	return CommandWithNotifier(cache, db, nil)
}

// newEntries says which of a commit's entries create an entity (version 0,
// not deleted), read before the commit runs (it updates the entries'
// versions in place).
func newEntries(msg *sync_pb.ClientToServerMessage) []bool {
	if msg.GetMessageContents() != sync_pb.ClientToServerMessage_COMMIT || msg.Commit == nil {
		return nil
	}
	out := make([]bool, len(msg.Commit.Entries))
	for i, entry := range msg.Commit.Entries {
		out[i] = entry.GetVersion() == 0 && !entry.GetDeleted()
	}
	return out
}

// changedTypes returns the data types of the entries a commit stored, and
// of those it created (|isNew|, from newEntries); each sorted, without
// duplicates.
func changedTypes(msg *sync_pb.ClientToServerMessage, rsp *sync_pb.ClientToServerResponse, isNew []bool) (types, created []int) {
	if msg.GetMessageContents() != sync_pb.ClientToServerMessage_COMMIT || msg.Commit == nil || rsp.Commit == nil {
		return nil, nil
	}
	seen := map[int]bool{}
	seenCreated := map[int]bool{}
	for i, entry := range msg.Commit.Entries {
		if i >= len(rsp.Commit.Entryresponse) {
			break
		}
		if rsp.Commit.Entryresponse[i].GetResponseType() != sync_pb.CommitResponse_SUCCESS || entry.Specifics == nil {
			continue
		}
		t, err := datastore.DataTypeOfSpecifics(entry.Specifics)
		if err != nil {
			continue
		}
		if !seen[t] {
			seen[t] = true
			types = append(types, t)
		}
		if i < len(isNew) && isNew[i] && !seenCreated[t] {
			seenCreated[t] = true
			created = append(created, t)
		}
	}
	slices.Sort(types)
	slices.Sort(created)
	return types, created
}

// CommandWithNotifier is Command that also tells |notify| (if not nil) what
// each commit changed.
func CommandWithNotifier(cache *cache.Cache, db datastore.Datastore, notify CommitNotifier) http.HandlerFunc {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		ctx := r.Context()
		clientID, ok := ctx.Value(syncContext.ContextKeyClientID).(string)
		if !ok {
			http.Error(w, "missing client id", http.StatusUnauthorized)
			return
		}

		reader := r.Body
		// Create a gzip reader if needed.
		if r.Header.Get("Content-Encoding") == "gzip" {
			gr, err := gzip.NewReader(r.Body)
			if err != nil {
				log.Error().Err(err).Msg("Create gzip reader failed")
				http.Error(w, "Create gzip reader failed", http.StatusInternalServerError)
				return
			}
			defer closers.Panic(ctx, gr)
			reader = gr
		}

		msg, err := io.ReadAll(io.LimitReader(reader, payloadLimit10MB))
		if err != nil {
			log.Error().Err(err).Msg("Read request body failed")
			http.Error(w, "Read request body error", http.StatusInternalServerError)
			return
		}

		// Unmarshal into ClientToServerMessage
		pb := &sync_pb.ClientToServerMessage{}
		err = proto.Unmarshal(msg, pb)
		if err != nil {
			log.Error().Err(err).Msg("Unmarshall ClientToServerMessage failed")
			http.Error(w, "Unmarshal error", http.StatusInternalServerError)
			return
		}

		isNew := newEntries(pb)
		pbRsp := &sync_pb.ClientToServerResponse{}
		err = command.HandleClientToServerMessage(ctx, cache, pb, pbRsp, db, clientID)
		if err != nil {
			log.Error().Err(err).Msg("Handle command message failed")
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}

		out, err := proto.Marshal(pbRsp)
		if err != nil {
			log.Error().Err(err).Msg("Marshall ClientToServerResponse failed")
			http.Error(w, "Marshal Error", http.StatusInternalServerError)
			return
		}

		if notify != nil {
			if types, created := changedTypes(pb, pbRsp, isNew); len(types) > 0 {
				notify(clientID, pb.Commit.GetCacheGuid(), types, created)
			}
		}

		w.WriteHeader(http.StatusOK)
		_, err = w.Write(out)
		if err != nil {
			log.Error().Err(err).Msg("Write HTTP response body failed")
			return
		}
	})
}
