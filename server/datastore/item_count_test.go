package datastore_test

import (
	"context"
	"sort"
	"testing"

	"github.com/stretchr/testify/suite"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/datastore/datastoretest"
)

type ItemCountTestSuite struct {
	suite.Suite

	db *datastore.SQLite
}

func (suite *ItemCountTestSuite) SetupSuite() {
	var err error
	suite.db, err = datastoretest.NewSQLite(suite.T().TempDir())
	suite.Require().NoError(err, "Failed to open SQLite datastore")
}

func (suite *ItemCountTestSuite) SetupTest() {
	suite.Require().NoError(
		datastoretest.ResetTable(suite.db), "Failed to reset table")
}

func (suite *ItemCountTestSuite) TearDownTest() {
	suite.Require().NoError(
		datastoretest.DeleteTable(suite.db), "Failed to delete table")
}

func (suite *ItemCountTestSuite) TestGetClientItemCount() {
	// Insert two items for test.
	items := []datastore.ClientItemCounts{
		{ClientID: "client1", ID: "client1", ItemCount: 5},
		{ClientID: "client2", ID: "client2", ItemCount: 10},
	}
	for _, item := range items {
		existing := datastore.ClientItemCounts{
			ClientID: item.ClientID,
			ID:       item.ID,
			Version:  datastore.CurrentCountVersion,
		}
		suite.Require().NoError(
			suite.db.UpdateClientItemCount(context.Background(), &existing, item.ItemCount, 0))
	}

	for _, item := range items {
		count, err := suite.db.GetClientItemCount(context.Background(), item.ClientID)
		suite.Require().NoError(err, "GetClientItemCount should succeed")
		suite.Equal(count.ItemCount, item.ItemCount, "ItemCount should match")
	}

	// Non-exist client item count should succeed with count = 0.
	count, err := suite.db.GetClientItemCount(context.Background(), "client3")
	suite.Require().NoError(err, "Get non-exist ClientItemCount should succeed")
	suite.Equal(0, count.ItemCount)
}

func (suite *ItemCountTestSuite) TestUpdateClientItemCount() {
	items := []datastore.ClientItemCounts{
		{ClientID: "client1", ID: "client1", ItemCount: 1},
		{ClientID: "client1", ID: "client1", ItemCount: 5},
		{ClientID: "client2", ID: "client2", ItemCount: 10},
	}
	expectedItems := []datastore.ClientItemCounts{
		{ClientID: "client1", ID: "client1", ItemCount: 6},
		{ClientID: "client2", ID: "client2", ItemCount: 10},
	}

	for _, item := range items {
		count, err := suite.db.GetClientItemCount(context.Background(), item.ClientID)
		suite.Require().NoError(err)
		suite.Require().NoError(
			suite.db.UpdateClientItemCount(context.Background(), count, item.ItemCount, 0))
	}

	clientCountItems, err := datastoretest.ScanClientItemCounts(suite.db)
	suite.Require().NoError(err, "ScanClientItemCounts should succeed")
	sort.Sort(datastore.ClientItemCountByClientID(clientCountItems))
	sort.Sort(datastore.ClientItemCountByClientID(expectedItems))
	for i := range clientCountItems {
		clientCountItems[i].Version = 0
		clientCountItems[i].LastPeriodChangeTime = 0
	}
	suite.Equal(expectedItems, clientCountItems)
}

func TestItemCountTestSuite(t *testing.T) {
	suite.Run(t, new(ItemCountTestSuite))
}
