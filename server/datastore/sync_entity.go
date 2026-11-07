package datastore

import (
	"errors"
	"fmt"
	"reflect"
	"strconv"
	"strings"
	"time"

	"github.com/google/uuid"
	"github.com/rs/zerolog/log"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

const (
	clientTagItemPrefix              = "Client#"
	serverTagItemPrefix              = "Server#"
	disabledChainID                  = "disabled_chain"
	reasonDeleted                    = "deleted"
	HistoryTypeID                int = 963985
	HistoryDeleteDirectiveTypeID int = 150251
	// DefaultHistoryDays is how long history and history delete directive
	// entities stay after their last write, by default.
	DefaultHistoryDays = 14
	// MaxHistoryDays caps COMPASS_SYNC_HISTORY_DAYS: Compass devices keep
	// the delete directives they processed for 30 days and then remove
	// them from the server (sync.md, "History"), so history kept
	// longer could come back on a device that joins later.
	MaxHistoryDays = 30
)

// HistoryExpirationIntervalSecs is the expiration time for history and
// history delete directive entities in seconds (Compass: a variable, set by
// SetHistoryDays from COMPASS_SYNC_HISTORY_DAYS; go-sync's constant was 14
// days).
var HistoryExpirationIntervalSecs int64 = DefaultHistoryDays * 24 * 60 * 60

// SetHistoryDays sets how long history stays on the server (1 to
// MaxHistoryDays). The history quota's four counting periods are a quarter
// of it each.
func SetHistoryDays(days int) error {
	if days < 1 || days > MaxHistoryDays {
		return fmt.Errorf("history days must be 1-%d, not %d", MaxHistoryDays, days)
	}
	HistoryExpirationIntervalSecs = int64(days) * 24 * 60 * 60
	return nil
}

// SyncEntity is one row of the entities table. (The dynamodbav tags are
// left from go-sync's DynamoDB store; nothing reads them now.)
type SyncEntity struct {
	ClientID               string
	ID                     string
	ParentID               *string `dynamodbav:",omitempty"`
	Version                *int64
	Mtime                  *int64
	Ctime                  *int64
	Name                   *string `dynamodbav:",omitempty"`
	NonUniqueName          *string `dynamodbav:",omitempty"`
	ServerDefinedUniqueTag *string `dynamodbav:",omitempty"`
	Deleted                *bool
	OriginatorCacheGUID    *string `dynamodbav:",omitempty"`
	OriginatorClientItemID *string `dynamodbav:",omitempty"`
	Specifics              []byte
	DataType               *int
	Folder                 *bool
	ClientDefinedUniqueTag *string `dynamodbav:",omitempty"`
	UniquePosition         []byte  `dynamodbav:",omitempty"`
	DataTypeMtime          *string
	ExpirationTime         *int64
	// Set by InsertSyncEntity when the create replaced a live entity of its
	// client tag (so the chain's item count stays). Not stored.
	ReplacedLive bool `dynamodbav:"-"`
}

// SyncEntityByClientIDID implements sort.Interface for []SyncEntity based on
// the string concatenation of ClientID and ID fields.
type SyncEntityByClientIDID []SyncEntity

func (a SyncEntityByClientIDID) Len() int      { return len(a) }
func (a SyncEntityByClientIDID) Swap(i, j int) { a[i], a[j] = a[j], a[i] }
func (a SyncEntityByClientIDID) Less(i, j int) bool {
	return a[i].ClientID+a[i].ID < a[j].ClientID+a[j].ID
}

// SyncEntityByMtime implements sort.Interface for []SyncEntity based on Mtime.
type SyncEntityByMtime []SyncEntity

func (a SyncEntityByMtime) Len() int      { return len(a) }
func (a SyncEntityByMtime) Swap(i, j int) { a[i], a[j] = a[j], a[i] }
func (a SyncEntityByMtime) Less(i, j int) bool {
	return *a[i].Mtime < *a[j].Mtime
}

// DisabledMarkerItem is used to mark sync chain as deleted in Dynamodb
type DisabledMarkerItem struct {
	ClientID string
	ID       string
	Reason   string
	Mtime    *int64
	Ctime    *int64
}

// DisabledMarkerItemQuery is used to query for disabled marker item in
// DynamoDB
type DisabledMarkerItemQuery struct {
	ClientID string
	ID       string
}

// ServerClientUniqueTagItem is used to marshal and unmarshal tag items in
// dynamoDB.
type ServerClientUniqueTagItem struct {
	ClientID string // Hash key
	ID       string // Range key
	Mtime    *int64
	Ctime    *int64
}

// ServerClientUniqueTagItemQuery is used to query for unique tag items in
// dynamoDB.
type ServerClientUniqueTagItemQuery struct {
	ClientID string // Hash key
	ID       string // Range key
}

// TagItemByClientIDID implements sort.Interface for []ServerClientUniqueTagItem
// based on the string concatenation of ClientID and ID fields.
type TagItemByClientIDID []ServerClientUniqueTagItem

func (a TagItemByClientIDID) Len() int      { return len(a) }
func (a TagItemByClientIDID) Swap(i, j int) { a[i], a[j] = a[j], a[i] }
func (a TagItemByClientIDID) Less(i, j int) bool {
	return a[i].ClientID+a[i].ID < a[j].ClientID+a[j].ID
}

// getTagPrefix is a helper method to give the proper prefix for unique tag
func getTagPrefix(isServer bool) string {
	if isServer {
		return serverTagItemPrefix
	}
	return clientTagItemPrefix
}

// NewServerClientUniqueTagItem creates a tag item which is used to ensure the
// uniqueness of server-defined or client-defined unique tags for a client.
func NewServerClientUniqueTagItem(clientID string, tag string, isServer bool) *ServerClientUniqueTagItem {
	prefix := getTagPrefix(isServer)
	now := ptr.Int64(time.Now().UnixMilli())

	return &ServerClientUniqueTagItem{
		ClientID: clientID,
		ID:       prefix + tag,
		Mtime:    now,
		Ctime:    now,
	}
}

// NewServerClientUniqueTagItemQuery creates a tag item query which is used to
// determine whether a sync entity has a unique tag item or not
func NewServerClientUniqueTagItemQuery(clientID string, tag string, isServer bool) *ServerClientUniqueTagItemQuery {
	prefix := getTagPrefix(isServer)

	return &ServerClientUniqueTagItemQuery{
		ClientID: clientID,
		ID:       prefix + tag,
	}
}

// encryptedSpecificsField is EntitySpecifics.encrypted, the one field of
// EntitySpecifics that isn't a data type.
const encryptedSpecificsField protowire.Number = 1

// DataTypeOfSpecifics returns the data type ID of |specifics|: the field
// number of its type-specific field (e.g. 32904 for bookmarks).
//
// Compass addition: a type newer than this server's protos (such as
// COMPASS_SPACES) parses as an unknown field, and go-sync's reflection on
// the oneof would panic. The server never reads specifics, so it takes the
// field number from the wire format and stores the bytes as they came.
func DataTypeOfSpecifics(specifics *sync_pb.EntitySpecifics) (int, error) {
	if specifics.SpecificsVariant != nil {
		// Use reflect to find out data type ID defined in protobuf tag.
		structField := reflect.ValueOf(specifics.SpecificsVariant).Elem().Type().Field(0)
		tag := structField.Tag.Get("protobuf")
		s := strings.Split(tag, ",")
		if len(s) < 2 {
			return 0, errors.New("malformed protobuf tag on specifics variant")
		}
		return strconv.Atoi(s[1])
	}
	unknown := specifics.ProtoReflect().GetUnknown()
	for len(unknown) > 0 {
		num, typ, n := protowire.ConsumeTag(unknown)
		if n < 0 {
			return 0, protowire.ParseError(n)
		}
		if typ == protowire.BytesType && num != encryptedSpecificsField {
			return int(num), nil
		}
		m := protowire.ConsumeFieldValue(num, typ, unknown[n:])
		if m < 0 {
			return 0, protowire.ParseError(m)
		}
		unknown = unknown[n+m:]
	}
	return 0, errors.New("specifics have no data type field")
}

func validatePBEntity(entity *sync_pb.SyncEntity) error {
	if entity == nil {
		return errors.New("validate SyncEntity error: empty SyncEntity")
	}

	if entity.IdString == nil {
		return errors.New("validate SyncEntity error: empty IdString")
	}

	if entity.Version == nil {
		return errors.New("validate SyncEntity error: empty Version")
	}

	if entity.Specifics == nil {
		return errors.New("validate SyncEntity error: nil Specifics")
	}

	return nil
}

// CreateDBSyncEntity converts a protobuf sync entity into a DB sync item.
func CreateDBSyncEntity(entity *sync_pb.SyncEntity, cacheGUID *string, clientID string) (*SyncEntity, error) {
	err := validatePBEntity(entity)
	if err != nil {
		log.Error().Err(err).Msg("Invalid sync_pb.SyncEntity received")
		return nil, fmt.Errorf("error validating protobuf sync entity to create DB sync entity: %w", err)
	}

	// Specifics are always passed and checked by validatePBEntity above.
	var specifics []byte
	specifics, err = proto.Marshal(entity.Specifics)
	if err != nil {
		log.Error().Err(err).Msg("Marshal specifics failed")
		return nil, fmt.Errorf("error marshalling specifics to create DB sync entity: %w", err)
	}

	dataType, err := DataTypeOfSpecifics(entity.Specifics)
	if err != nil {
		log.Error().Err(err).Msg("Unknown data type")
		return nil, fmt.Errorf("error finding data type to create DB sync entity: %w", err)
	}

	var uniquePosition []byte
	if entity.UniquePosition != nil {
		uniquePosition, err = proto.Marshal(entity.UniquePosition)
		if err != nil {
			log.Error().Err(err).Msg("Marshal UniquePosition failed")
			return nil, fmt.Errorf("error marshalling unique position to create DB sync entity: %w", err)
		}
	}

	id := *entity.IdString
	var originatorCacheGUID, originatorClientItemID *string
	if cacheGUID != nil {
		if *entity.Version == 0 {
			id = uuid.New().String()
		}
		originatorCacheGUID = cacheGUID
		originatorClientItemID = entity.IdString
	}

	// The client tag hash must be used as the primary key
	// for the history type.
	if dataType == HistoryTypeID {
		id = *entity.ClientTagHash
	}

	now := time.Now()

	var expirationTime *int64
	if dataType == HistoryTypeID || dataType == HistoryDeleteDirectiveTypeID {
		expirationTime = ptr.Int64(now.Unix() + HistoryExpirationIntervalSecs)
	}

	nowMillis := ptr.Int64(NextMtime()) // Compass: unique, see mtime.go.
	// ctime is only used when inserting a new entity, here we use client passed
	// ctime if it is passed, otherwise, use current server time as the creation
	// time. When updating, ctime will be ignored later in the query statement.
	cTime := nowMillis
	if entity.Ctime != nil {
		cTime = entity.Ctime
	}

	dataTypeMtime := strconv.Itoa(dataType) + "#" + strconv.FormatInt(*nowMillis, 10)

	// Set default values on Deleted and Folder attributes for new entities, the
	// default values are specified by sync.proto protocol.
	deleted := entity.Deleted
	folder := entity.Folder
	if *entity.Version == 0 {
		if entity.Deleted == nil {
			deleted = ptr.Bool(false)
		}
		if entity.Folder == nil {
			folder = ptr.Bool(false)
		}
	}

	return &SyncEntity{
		ClientID:               clientID,
		ID:                     id,
		ParentID:               entity.ParentIdString,
		Version:                entity.Version,
		Ctime:                  cTime,
		Mtime:                  nowMillis,
		Name:                   entity.Name,
		NonUniqueName:          entity.NonUniqueName,
		ServerDefinedUniqueTag: entity.ServerDefinedUniqueTag,
		Deleted:                deleted,
		OriginatorCacheGUID:    originatorCacheGUID,
		OriginatorClientItemID: originatorClientItemID,
		ClientDefinedUniqueTag: entity.ClientTagHash,
		Specifics:              specifics,
		Folder:                 folder,
		UniquePosition:         uniquePosition,
		DataType:               ptr.Int(dataType),
		DataTypeMtime:          ptr.String(dataTypeMtime),
		ExpirationTime:         expirationTime,
	}, nil
}

// CreatePBSyncEntity converts a DB sync item to a protobuf sync entity.
func CreatePBSyncEntity(entity *SyncEntity) (*sync_pb.SyncEntity, error) {
	pbEntity := &sync_pb.SyncEntity{
		IdString:               &entity.ID,
		ParentIdString:         entity.ParentID,
		Version:                entity.Version,
		Mtime:                  entity.Mtime,
		Ctime:                  entity.Ctime,
		Name:                   entity.Name,
		NonUniqueName:          entity.NonUniqueName,
		ServerDefinedUniqueTag: entity.ServerDefinedUniqueTag,
		ClientTagHash:          entity.ClientDefinedUniqueTag,
		OriginatorCacheGuid:    entity.OriginatorCacheGUID,
		OriginatorClientItemId: entity.OriginatorClientItemID,
		Deleted:                entity.Deleted,
		Folder:                 entity.Folder,
	}

	if entity.Specifics != nil {
		pbEntity.Specifics = &sync_pb.EntitySpecifics{}
		err := proto.Unmarshal(entity.Specifics, pbEntity.Specifics)
		if err != nil {
			log.Error().Err(err).Msg("Unmarshal specifics failed")
			return nil, fmt.Errorf("error unmarshalling specifics to create protobuf sync entity: %w", err)
		}
	}

	if entity.UniquePosition != nil {
		pbEntity.UniquePosition = &sync_pb.UniquePosition{}
		err := proto.Unmarshal(entity.UniquePosition, pbEntity.UniquePosition)
		if err != nil {
			log.Error().Err(err).Msg("Unmarshal UniquePosition failed")
			return nil, fmt.Errorf("error unmarshalling unique position to create protobuf sync entity: %w", err)
		}
	}

	return pbEntity, nil
}
