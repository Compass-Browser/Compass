package controller_test

import (
	"bytes"
	"compress/gzip"
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/brave/go-sync/ptr"
	"github.com/stretchr/testify/suite"
	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/auth/authtest"
	"github.com/brave/go-sync/cache"
	"github.com/brave/go-sync/controller"
	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/datastore/datastoretest"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
	syncContext "github.com/brave/go-sync/synccontext"
)

type ControllerTestSuite struct {
	suite.Suite

	db    *datastore.SQLite
	cache *cache.Cache
}

func (suite *ControllerTestSuite) SetupSuite() {
	var err error
	suite.db, err = datastoretest.NewSQLite(suite.T().TempDir())
	suite.Require().NoError(err, "Failed to open SQLite datastore")

	suite.cache = cache.NewCache(cache.NewMemoryClient())
}

func (suite *ControllerTestSuite) SetupTest() {
	suite.Require().NoError(
		datastoretest.ResetTable(suite.db), "Failed to reset table")
}

func (suite *ControllerTestSuite) TearDownTest() {
	suite.Require().NoError(
		datastoretest.DeleteTable(suite.db), "Failed to delete table")
	suite.Require().NoError(
		suite.cache.FlushAll(context.Background()), "Failed to clear cache")
}

func (suite *ControllerTestSuite) TestAccount() {}

func (suite *ControllerTestSuite) TestCommand() {
	// Generate request body.
	commitMsg := &sync_pb.CommitMessage{
		Entries: []*sync_pb.SyncEntity{
			{
				IdString: ptr.String("id"),
				Version:  ptr.Int64(1),
				Deleted:  ptr.Bool(false),
				Folder:   ptr.Bool(false),
				Specifics: &sync_pb.EntitySpecifics{
					SpecificsVariant: &sync_pb.EntitySpecifics_Nigori{
						Nigori: &sync_pb.NigoriSpecifics{},
					},
				},
			},
		},
		CacheGuid: ptr.String("cache_guid"),
	}
	commit := sync_pb.ClientToServerMessage_COMMIT
	msg := &sync_pb.ClientToServerMessage{
		MessageContents: &commit,
		Commit:          commitMsg,
		Share:           ptr.String(""),
	}

	body, err := proto.Marshal(msg)
	suite.Require().NoError(err, "proto.Marshal should succeed")

	req, err := http.NewRequest("POST", "v2/command/", bytes.NewBuffer(body))
	suite.Require().NoError(err, "NewRequest should succeed")
	req.Header.Set("Authorization", "Bearer token")

	handler := controller.Command(suite.cache, suite.db)

	// Test unauthorized response.
	rr := httptest.NewRecorder()
	handler.ServeHTTP(rr, req)
	suite.Require().Equal(http.StatusUnauthorized, rr.Code)

	ctx := context.WithValue(context.Background(), syncContext.ContextKeyClientID, "clientID")
	req, err = http.NewRequestWithContext(ctx, "POST", "v2/command/", bytes.NewBuffer(body))
	suite.Require().NoError(err, "NewRequestWithContext should succeed")

	// Generate a valid token to use.
	token, _, _, err := authtest.GenerateToken(time.Now().UnixMilli())
	suite.Require().NoError(err, "generate token should succeed")
	req.Header.Set("Authorization", "Bearer "+token)

	// Test message without gzip.
	rr = httptest.NewRecorder()
	handler.ServeHTTP(rr, req)
	suite.Require().Equal(http.StatusOK, rr.Code)

	// Test message with gzip.
	buf := new(bytes.Buffer)
	zw := gzip.NewWriter(buf)
	_, err = zw.Write(body)
	suite.Require().NoError(err, "gzip write should succeed")
	err = zw.Close()
	suite.Require().NoError(err, "gzip close should succeed")

	req, err = http.NewRequestWithContext(ctx, "POST", "v2/command/", buf)
	suite.Require().NoError(err, "NewRequest should succeed")
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Encoding", "gzip")

	rr = httptest.NewRecorder()
	handler.ServeHTTP(rr, req)
	suite.Require().Equal(http.StatusOK, rr.Code)
}

func TestControllerTestSuite(t *testing.T) {
	suite.Run(t, new(ControllerTestSuite))
}
