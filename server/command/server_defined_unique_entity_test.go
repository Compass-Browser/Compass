package command_test

import (
	"context"
	"sort"
	"testing"

	"github.com/brave/go-sync/ptr"
	"github.com/stretchr/testify/suite"

	"github.com/brave/go-sync/command"
	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/datastore/datastoretest"
)

type ServerDefinedUniqueEntityTestSuite struct {
	suite.Suite

	db *datastore.SQLite
}

type SyncAttrs struct {
	ClientID string
	Name     *string
	DataType *int
	ParentID *string
	Version  *int64
	Deleted  *bool
	Folder   *bool
}

func (suite *ServerDefinedUniqueEntityTestSuite) SetupSuite() {
	var err error
	suite.db, err = datastoretest.NewSQLite(suite.T().TempDir())
	suite.Require().NoError(err, "Failed to open SQLite datastore")
}

func (suite *ServerDefinedUniqueEntityTestSuite) SetupTest() {
	suite.Require().NoError(
		datastoretest.ResetTable(suite.db), "Failed to reset table")
}

func (suite *ServerDefinedUniqueEntityTestSuite) TearDownTest() {
	suite.Require().NoError(
		datastoretest.DeleteTable(suite.db), "Failed to delete table")
}

func (suite *ServerDefinedUniqueEntityTestSuite) TestInsertServerDefinedUniqueEntities() {
	suite.Require().NoError(
		command.InsertServerDefinedUniqueEntities(context.Background(), suite.db, "client1"),
		"InsertServerDefinedUniqueEntities should succeed")
	suite.Require().NoError(
		command.InsertServerDefinedUniqueEntities(context.Background(), suite.db, "client1"),
		"InsertServerDefinedUniqueEntities again for a same client should succeed")

	expectedSyncAttrsMap := map[string]*SyncAttrs{
		command.NigoriTag: {
			ClientID: "client1",
			Name:     ptr.String(command.NigoriName),
			DataType: ptr.Int(int(nigoriType)),
			ParentID: ptr.String("0"),
			Version:  ptr.Int64(1),
			Deleted:  ptr.Bool(false),
			Folder:   ptr.Bool(true),
		},
		command.BookmarksTag: {
			ClientID: "client1",
			Name:     ptr.String(command.BookmarksName),
			DataType: ptr.Int(int(bookmarkType)),
			ParentID: ptr.String("0"),
			Version:  ptr.Int64(1),
			Deleted:  ptr.Bool(false),
			Folder:   ptr.Bool(true),
		},
		command.OtherBookmarksTag: {
			ClientID: "client1",
			Name:     ptr.String(command.OtherBookmarksName),
			DataType: ptr.Int(int(bookmarkType)),
			Version:  ptr.Int64(1),
			Deleted:  ptr.Bool(false),
			Folder:   ptr.Bool(true),
		},
		command.SyncedBookmarksTag: {
			ClientID: "client1",
			Name:     ptr.String(command.SyncedBookmarksName),
			DataType: ptr.Int(int(bookmarkType)),
			Version:  ptr.Int64(1),
			Deleted:  ptr.Bool(false),
			Folder:   ptr.Bool(true),
		},
		command.BookmarkBarTag: {
			ClientID: "client1",
			Name:     ptr.String(command.BookmarkBarName),
			DataType: ptr.Int(int(bookmarkType)),
			Version:  ptr.Int64(1),
			Deleted:  ptr.Bool(false),
			Folder:   ptr.Bool(true),
		},
	}

	// Tag items should be inserted for each server tags.
	expectedTagItems := []datastore.ServerClientUniqueTagItem{}
	for key := range expectedSyncAttrsMap {
		expectedTagItems = append(expectedTagItems,
			datastore.ServerClientUniqueTagItem{ClientID: "client1", ID: "Server#" + key})
	}
	tagItems, err := datastoretest.ScanTagItems(suite.db)
	suite.Require().NoError(err, "ScanTagItems should succeed")

	// Check that Ctime and Mtime have been set, reset to zero value for subsequent
	// tests
	for i := range tagItems {
		suite.NotNil(tagItems[i].Ctime)
		suite.NotNil(tagItems[i].Mtime)

		tagItems[i].Ctime = nil
		tagItems[i].Mtime = nil
	}

	sort.Sort(datastore.TagItemByClientIDID(tagItems))
	sort.Sort(datastore.TagItemByClientIDID(expectedTagItems))
	suite.Equal(expectedTagItems, tagItems)

	syncItems, err := datastoretest.ScanSyncEntities(suite.db)
	suite.Require().NoError(err, "ScanSyncEntities should succeed")

	// Find bookmark root folder to update parentID of its subfolders.
	var bookmarksRootID string
	for _, item := range syncItems {
		if *item.ServerDefinedUniqueTag == command.BookmarksTag {
			bookmarksRootID = item.ID
			expectedSyncAttrsMap[command.OtherBookmarksTag].ParentID = &bookmarksRootID
			expectedSyncAttrsMap[command.SyncedBookmarksTag].ParentID = &bookmarksRootID
			expectedSyncAttrsMap[command.BookmarkBarTag].ParentID = &bookmarksRootID
			break
		}
	}
	suite.NotEmpty(bookmarksRootID, "Cannot find ID of bookmarks root folder")

	// For each item returned by ScanSyncEntities, make sure it is in the map and
	// its value is matched, then remove it from the map.
	for _, item := range syncItems {
		syncAttrs := SyncAttrs{
			ClientID: item.ClientID,
			Name:     item.Name,
			DataType: item.DataType,
			ParentID: item.ParentID,
			Version:  item.Version,
			Deleted:  item.Deleted,
			Folder:   item.Folder,
		}

		suite.NotNil(item.ServerDefinedUniqueTag)
		suite.Equal(syncAttrs, *expectedSyncAttrsMap[*item.ServerDefinedUniqueTag])
		delete(expectedSyncAttrsMap, *item.ServerDefinedUniqueTag)
	}
	suite.Empty(expectedSyncAttrsMap)

	suite.Require().NoError(
		command.InsertServerDefinedUniqueEntities(context.Background(), suite.db, "client2"),
		"InsertServerDefinedUniqueEntities should succeed for another client")
}

func TestServerDefinedUniqueEntityTestSuite(t *testing.T) {
	suite.Run(t, new(ServerDefinedUniqueEntityTestSuite))
}
