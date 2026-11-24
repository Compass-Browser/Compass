// One sync chain: a Durable Object holding the chain's entities in its own
// SQLite database. A port of the Go server's sync path (server/command,
// server/datastore/sqlite.go), with the same rules: unique increasing
// mtimes as versions and progress tokens, soft deletes, client and server
// tags, the entity, byte and history quotas, create over a tombstone, the
// device limit, device removal, idle-chain expiry. Change notifications go
// to hibernating WebSockets, which the Worker turns into the Server-Sent
// Events stream clients read (index.ts, events).
//
// Rooms (room.ts): the chain's object counts the bytes of the rooms the
// chain owns in its quota (addRoomBytes), keeps an index of the rooms it's
// a member of (room_index, for GET /v2/compass/rooms) and its identity key,
// sends "room" events to its streams (roomEvent), and clears its rooms when
// the chain is cleared.
//
// The Worker has authenticated the request and passes the chain ID (as the
// token spells it) in X-Chain-ID.

import { DurableObject } from "cloudflare:workers";
import { type Config, deviceRemoved, type Env, fail, json, loadConfig, text } from "./config";
import {
  BACKGROUND_TYPES,
  MAX_REGISTRATIONS,
  parseRegistration,
  relayPush,
  SEND_TAB_TYPE,
  seal,
  tokenHash,
} from "./push";
import type { RoomEvent } from "./roomwire";
import {
  type ClientToServerMessage,
  type ClientToServerResponse,
  Contents,
  decodeClientToServerMessage,
  decodeToken,
  dataTypeOfSpecifics,
  emptySpecifics,
  encodeClientToServerResponse,
  encodeToken,
  type EntryResponse,
  ErrorType,
  Origin,
  ProtoError,
  ResponseType,
  type SyncEntity,
} from "./proto";

export const BOOKMARK_TYPE = 32904;
export const NIGORI_TYPE = 47745;
export const DEVICE_INFO_TYPE = 154522;
export const HISTORY_TYPE = 963985;
export const HISTORY_DELETE_DIRECTIVE_TYPE = 150251;

const STORE_BIRTHDAY = "1";
const MAX_COMMIT_BATCH_SIZE = 90;
const MAX_GU_BATCH_SIZE = 500;
const TIE_FALLBACK_SIZE = 100;
const PAYLOAD_LIMIT = 10 * 1024 * 1024;
const MAX_ACTIVE_DEVICES = 50;
const HIGH_MAX_ACTIVE_DEVICES = 100;
const MAX_STREAMS = 20;
const MAX_CLIENT_ID = 128;
const BYTES_FULL_MARGIN = 64 << 10;
const TOUCH_INTERVAL_MS = 3600_000;
const PURGE_INTERVAL_MS = 3600_000;

// QuotaFull bits (datastore/maintenance.go).
const QUOTA_ENTITIES = 1;
const QUOTA_BYTES = 2;
const QUOTA_HISTORY = 4;

const SCHEMA = `
CREATE TABLE IF NOT EXISTS entities (
  id                        TEXT PRIMARY KEY,
  parent_id                 TEXT,
  version                   INTEGER,
  mtime                     INTEGER,
  ctime                     INTEGER,
  name                      TEXT,
  non_unique_name           TEXT,
  server_defined_unique_tag TEXT,
  deleted                   INTEGER,
  originator_cache_guid     TEXT,
  originator_client_item_id TEXT,
  specifics                 BLOB,
  data_type                 INTEGER,
  folder                    INTEGER,
  client_defined_unique_tag TEXT,
  unique_position           BLOB,
  expiration_time           INTEGER,
  size                      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS entities_by_type_mtime ON entities (data_type, mtime);
CREATE INDEX IF NOT EXISTS entities_by_client_tag ON entities (client_defined_unique_tag)
  WHERE client_defined_unique_tag IS NOT NULL;
CREATE INDEX IF NOT EXISTS entities_by_expiration ON entities (expiration_time)
  WHERE expiration_time > 0;
CREATE INDEX IF NOT EXISTS tombstones_by_mtime ON entities (mtime) WHERE deleted = 1;
CREATE TABLE IF NOT EXISTS unique_tags (
  id    TEXT PRIMARY KEY,
  mtime INTEGER,
  ctime INTEGER
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS chain (
  one                     INTEGER PRIMARY KEY CHECK (one = 1),
  chain_id                TEXT,
  bytes                   INTEGER NOT NULL DEFAULT 0,
  disabled_at             INTEGER,
  last_active             INTEGER,
  quota_full              INTEGER NOT NULL DEFAULT 0,
  quota_full_time         INTEGER,
  item_count              INTEGER NOT NULL DEFAULT 0,
  history_period1         INTEGER NOT NULL DEFAULT 0,
  history_period2         INTEGER NOT NULL DEFAULT 0,
  history_period3         INTEGER NOT NULL DEFAULT 0,
  history_period4         INTEGER NOT NULL DEFAULT 0,
  last_period_change_time INTEGER NOT NULL DEFAULT 0
);
INSERT OR IGNORE INTO chain (one) VALUES (1);
CREATE TABLE IF NOT EXISTS removed_devices (
  cache_guid TEXT PRIMARY KEY,
  removed_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS room_index (
  room_id TEXT PRIMARY KEY,
  kind    TEXT NOT NULL,
  owner   TEXT NOT NULL,
  role    TEXT NOT NULL,
  pending INTEGER NOT NULL,
  epoch   INTEGER NOT NULL,
  seq     INTEGER NOT NULL
) WITHOUT ROWID;
-- Devices' push registrations, as relay handles (push.ts): never the
-- token itself, only its hash, to find a device that registered again
-- under a new cache GUID. bg_last and bg_due space background pushes.
CREATE TABLE IF NOT EXISTS push_handles (
  cache_guid TEXT PRIMARY KEY,
  handle     TEXT NOT NULL,
  token_hash TEXT NOT NULL,
  platform   TEXT NOT NULL,
  server     TEXT NOT NULL,
  relay      TEXT NOT NULL,
  updated    INTEGER NOT NULL,
  bg_last    INTEGER NOT NULL DEFAULT 0,
  bg_due     INTEGER
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS identity (
  one     INTEGER PRIMARY KEY CHECK (one = 1),
  pub     TEXT NOT NULL,
  updated INTEGER NOT NULL
);
`;

const ENTITY_COLUMNS = `id, parent_id, version, mtime, ctime, name, non_unique_name,
  server_defined_unique_tag, deleted, originator_cache_guid, originator_client_item_id,
  specifics, data_type, folder, client_defined_unique_tag, unique_position, expiration_time`;

// A row of the entities table, as the commit path builds it
// (datastore.SyncEntity).
interface DBEntity {
  id: string;
  parentId?: string;
  version?: number;
  mtime: number;
  ctime?: number;
  name?: string;
  nonUniqueName?: string;
  serverDefinedUniqueTag?: string;
  deleted?: boolean;
  originatorCacheGuid?: string;
  originatorClientItemId?: string;
  specifics: Uint8Array;
  dataType: number;
  folder?: boolean;
  clientTag?: string;
  uniquePosition?: Uint8Array;
  expirationTime?: number;
  replacedLive?: boolean; // Set by insertEntity, not stored.
}

class QuotaExceeded extends Error {}

// InsertError is an insert that failed; conflict says the client should
// treat it as a CONFLICT.
class InsertError extends Error {
  constructor(message: string, readonly conflict: boolean) {
    super(message);
  }
}

interface Counts {
  itemCount: number;
  h1: number;
  h2: number;
  h3: number;
  h4: number;
  lastPeriodChange: number;
}

const encoder = new TextEncoder();

function utf8Len(s: string | undefined | null): number {
  return s ? encoder.encode(s).length : 0;
}

function entitySize(specifics?: Uint8Array | null, pos?: Uint8Array | null, name?: string | null, nun?: string | null): number {
  return (specifics?.length ?? 0) + (pos?.length ?? 0) + utf8Len(name) + utf8Len(nun);
}

// blob binds bytes for SQL, storing empty ones as NULL (as the Go store).
function blob(b: Uint8Array | undefined | null): ArrayBuffer | null {
  if (!b || b.length === 0) return null;
  return b.slice().buffer as ArrayBuffer;
}

function boolCol(b: boolean | undefined): number | null {
  return b === undefined ? null : b ? 1 : 0;
}

function opt<T>(v: T | null | undefined): T | undefined {
  return v === null ? undefined : v;
}

function bytesOf(v: unknown): Uint8Array | undefined {
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  if (v instanceof Uint8Array) return v;
  return undefined;
}

type Row = Record<string, SqlStorageValue>;

function rowToEntity(r: Row): SyncEntity {
  const flag = (v: SqlStorageValue) => (v === null ? undefined : Number(v) !== 0);
  return {
    idString: r.id as string,
    parentIdString: opt(r.parent_id as string | null),
    version: opt(r.version as number | null),
    mtime: opt(r.mtime as number | null),
    ctime: opt(r.ctime as number | null),
    name: opt(r.name as string | null),
    nonUniqueName: opt(r.non_unique_name as string | null),
    serverDefinedUniqueTag: opt(r.server_defined_unique_tag as string | null),
    clientTagHash: opt(r.client_defined_unique_tag as string | null),
    originatorCacheGuid: opt(r.originator_cache_guid as string | null),
    originatorClientItemId: opt(r.originator_client_item_id as string | null),
    deleted: flag(r.deleted),
    folder: flag(r.folder),
    specifics: bytesOf(r.specifics) ?? new Uint8Array(0),
    uniquePosition: bytesOf(r.unique_position),
  };
}

// clientTagHash is Chromium's client tag hash for a tag of a data type
// (datastore.ClientTagHash): base64(SHA-1(an empty specifics of the type ||
// the tag)).
export async function clientTagHash(dataType: number, tag: string): Promise<string> {
  const prefix = emptySpecifics(dataType);
  const t = encoder.encode(tag);
  const input = new Uint8Array(prefix.length + t.length);
  input.set(prefix);
  input.set(t, prefix.length);
  const sum = new Uint8Array(await crypto.subtle.digest("SHA-1", input));
  return btoa(String.fromCharCode(...sum));
}

export class Chain extends DurableObject<Env> {
  private sql: SqlStorage;
  private cfg: Config;
  private lastMtime = 0;
  private lastTouch = 0;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.cfg = loadConfig(env);
    this.sql.exec(SCHEMA);
    const row = this.sql.exec(`SELECT MAX(mtime) AS m FROM entities`).one();
    this.lastMtime = Number(row.m ?? 0);
  }

  // nextMtime: strictly increasing mtimes in ms (datastore/mtime.go), so a
  // page of updates never ends inside a run of equal mtimes.
  private nextMtime(): number {
    let now = Date.now();
    if (now <= this.lastMtime) now = this.lastMtime + 1;
    this.lastMtime = now;
    return now;
  }

  private get historySecs(): number {
    return this.cfg.historyDays * 86400;
  }

  // --- HTTP ------------------------------------------------------------------

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const chainID = request.headers.get("X-Chain-ID") ?? "";
    if (!chainID) return text(401, "Unauthorized");
    this.rememberChainID(chainID);
    const client = url.searchParams.get("client_id") ?? "";
    if (client && this.isDeviceRemoved(client)) return deviceRemoved();

    const path = url.pathname;
    if (path === "/command/" && request.method === "POST") return this.command(request, chainID);
    if (path === "/events" && request.method === "GET") return this.events(request, client);
    if (path === "/quota" && request.method === "GET") return this.quota();
    if (path.startsWith("/devices") && request.method === "DELETE") return this.removeDevice(url, client);
    if (path === "/push") return this.register(request, client);
    return text(404, "404 page not found");
  }

  private rememberChainID(chainID: string): void {
    // Kept for logs and admin listings; the object is named by its lower case.
    const row = this.sql.exec(`SELECT chain_id FROM chain`).one();
    if (row.chain_id !== chainID) this.sql.exec(`UPDATE chain SET chain_id = ?`, chainID);
  }

  private isDisabled(): boolean {
    return this.sql.exec(`SELECT disabled_at FROM chain`).one().disabled_at !== null;
  }

  // touch records that a device talked to the server (chains.last_active,
  // at most hourly) and keeps the maintenance alarm set.
  private async touch(): Promise<void> {
    const now = Date.now();
    if (now - this.lastTouch < TOUCH_INTERVAL_MS) return;
    this.lastTouch = now;
    this.sql.exec(`UPDATE chain SET last_active = ?`, Math.floor(now / 1000));
    if ((await this.ctx.storage.getAlarm()) === null) {
      this.ctx.storage.kv.put("purge_at", now + PURGE_INTERVAL_MS);
      await this.ctx.storage.setAlarm(now + PURGE_INTERVAL_MS);
    }
  }

  private async command(request: Request, chainID: string): Promise<Response> {
    if (this.isDisabled()) {
      return protoResponse({ errorCode: ErrorType.DISABLED_BY_ADMIN });
    }
    await this.touch();

    let body: Uint8Array;
    try {
      body = await readBody(request);
    } catch {
      return text(500, "Read request body error");
    }
    let msg: ClientToServerMessage;
    try {
      msg = decodeClientToServerMessage(body);
    } catch {
      return text(500, "Unmarshal error");
    }

    const rsp: ClientToServerResponse = {
      storeBirthday: STORE_BIRTHDAY,
      clientCommand: { setSyncPollInterval: this.cfg.pollSeconds, maxCommitBatchSize: MAX_COMMIT_BATCH_SIZE },
    };
    try {
      switch (msg.messageContents) {
        case Contents.GET_UPDATES:
          this.handleGetUpdates(msg, rsp, chainID);
          break;
        case Contents.COMMIT: {
          if (!msg.commit) throw new Error("nil commitMsg is received");
          const isNew = msg.commit.entries.map((e) => (e.version ?? 0) === 0 && !e.deleted);
          this.handleCommit(msg.commit.entries, msg.commit.cacheGuid, rsp);
          this.notifyCommit(msg, rsp, isNew);
          break;
        }
        case Contents.CLEAR_SERVER_DATA:
          await this.disable();
          rsp.clearServerData = true;
          rsp.errorCode = ErrorType.SUCCESS;
          break;
        default:
          return text(500, "unsupported message type of ClientToServerMessage");
      }
    } catch (e) {
      if (e instanceof CodedError) {
        rsp.errorCode = e.code;
        rsp.errorMessage = e.message;
      } else {
        console.error("command failed", e);
        return text(500, String(e instanceof Error ? e.message : e));
      }
    }
    return protoResponse(rsp);
  }

  // --- GetUpdates ------------------------------------------------------------

  private handleGetUpdates(msg: ClientToServerMessage, rsp: ClientToServerResponse, chainID: string): void {
    const gu = msg.getUpdates ?? { fromProgressMarker: [] };
    const isNewClient = gu.origin === Origin.NEW_CLIENT;
    const g: NonNullable<ClientToServerResponse["getUpdates"]> = {
      entries: [],
      changesRemaining: 0,
      newProgressMarker: [],
      encryptionKeys: [],
    };
    rsp.getUpdates = g;
    rsp.errorCode = ErrorType.SUCCESS;
    if (isNewClient) this.setupNewClient(chainID);
    if (gu.fromProgressMarker.length === 0) return;

    const fetchFolders = gu.fetchFolders ?? true;
    for (const marker of gu.fromProgressMarker) {
      const dataType = marker.dataTypeId ?? 0;
      const token = marker.token && marker.token.length > 0 ? marker.token : encodeToken(0);
      const out = { dataTypeId: dataType, token };
      g.newProgressMarker.push(out);
      if (dataType === NIGORI_TYPE && isNewClient) {
        // The client sets its own keys; go-sync sends a placeholder.
        g.encryptionKeys = [encoder.encode("1234")];
      }
      if (g.entries.length >= MAX_GU_BATCH_SIZE) continue;
      let since: number;
      try {
        since = decodeToken(token);
      } catch {
        throw new Error(`failed at decoding token value`);
      }
      const max = MAX_GU_BATCH_SIZE - g.entries.length;
      const { more, entities } = this.getUpdatesForType(dataType, since, fetchFolders, max);
      if (isNewClient && dataType === NIGORI_TYPE && since === 0 && entities.length === 0) {
        throw new CodedError(ErrorType.TRANSIENT_ERROR, "nigori root folder entity is not ready yet");
      }
      if (more) g.changesRemaining = 1; // Chromium sends 1, not the count.
      if (entities.length > 0) {
        g.entries.push(...entities);
        out.token = encodeToken(entities[entities.length - 1].mtime!);
      }
    }
  }

  // getUpdatesForType returns up to max entities of a type changed after
  // `since`, oldest first, without expired ones; a page never ends inside a
  // run of equal mtimes (datastore.SQLite.GetUpdatesForType).
  private getUpdatesForType(
    dataType: number,
    since: number,
    fetchFolders: boolean,
    max: number,
  ): { more: boolean; entities: SyncEntity[] } {
    if (max <= 0) return { more: true, entities: [] };
    let q = `SELECT ${ENTITY_COLUMNS} FROM entities
      WHERE data_type = ? AND mtime > ?
        AND (expiration_time IS NULL OR expiration_time <= 0 OR expiration_time >= ?)`;
    if (!fetchFolders) q += ` AND folder = 0`;
    q += ` ORDER BY mtime, rowid LIMIT ?`;
    const rows = this.sql.exec(q, dataType, since, Math.floor(Date.now() / 1000), max + 1).toArray();
    let entities = rows.map(rowToEntity);
    const more = entities.length >= max;
    if (entities.length > max) {
      const next = entities[max].mtime;
      entities = entities.slice(0, max);
      let k = entities.length;
      while (k > 0 && entities[k - 1].mtime === next) k--;
      if (k > 0) entities = entities.slice(0, k);
      else if (max < TIE_FALLBACK_SIZE) entities = [];
    }
    return { more, entities };
  }

  // setupNewClient refuses a new device past the device limit (THROTTLED)
  // and creates the server-defined folders on a new chain.
  private setupNewClient(chainID: string): void {
    const active = Number(
      this.sql.exec(
        `SELECT COUNT(*) AS n FROM entities
         WHERE data_type = ? AND folder = 0 AND COALESCE(deleted, 0) = 0
           AND (expiration_time IS NULL OR expiration_time <= 0 OR expiration_time >= ?)`,
        DEVICE_INFO_TYPE,
        Math.floor(Date.now() / 1000),
      ).one().n,
    );
    const limit = this.cfg.highDeviceLimit.has(chainID.toLowerCase()) ? HIGH_MAX_ACTIVE_DEVICES : MAX_ACTIVE_DEVICES;
    if (active >= limit) {
      throw new CodedError(ErrorType.THROTTLED, "exceed limit of active devices in a chain");
    }
    this.insertServerDefinedEntities();
  }

  private insertServerDefinedEntities(): void {
    const has = this.sql.exec(`SELECT 1 FROM unique_tags WHERE id = ?`, "Server#google_chrome_nigori").toArray();
    if (has.length > 0) return;
    const now = Date.now();
    const make = (name: string, tag: string, parentId: string, dataType: number): DBEntity => ({
      id: crypto.randomUUID(),
      parentId,
      version: 1,
      mtime: this.nextMtime(),
      ctime: now,
      name,
      serverDefinedUniqueTag: tag,
      deleted: false,
      folder: true,
      specifics: emptySpecifics(dataType),
      dataType,
    });
    const root = make("Bookmarks", "google_chrome_bookmarks", "0", BOOKMARK_TYPE);
    const entities = [
      make("Nigori", "google_chrome_nigori", "0", NIGORI_TYPE),
      root,
      make("Other Bookmarks", "other_bookmarks", root.id, BOOKMARK_TYPE),
      make("Synced Bookmarks", "synced_bookmarks", root.id, BOOKMARK_TYPE),
      make("Bookmark Bar", "bookmark_bar", root.id, BOOKMARK_TYPE),
    ];
    this.ctx.storage.transactionSync(() => {
      for (const e of entities) {
        if (!this.insertTag("Server#" + e.serverDefinedUniqueTag!)) {
          throw new CodedError(ErrorType.TRANSIENT_ERROR, `tag ${e.serverDefinedUniqueTag} in use`);
        }
        this.writeNewEntity(e);
        const size = entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName);
        this.sql.exec(`UPDATE chain SET bytes = bytes + ?`, size);
      }
    });
  }

  // --- Commit ----------------------------------------------------------------

  private handleCommit(entries: SyncEntity[], cacheGuid: string | undefined, rsp: ClientToServerResponse): void {
    const responses: EntryResponse[] = [];
    rsp.commit = { entryResponse: responses };
    rsp.errorCode = ErrorType.SUCCESS;
    if (entries.length === 0) return;

    const counts = this.readCounts();
    const state = {
      currentNormal: counts.itemCount,
      currentHistory: counts.h1 + counts.h2 + counts.h3 + counts.h4,
      newNormal: 0,
      newHistory: 0,
      boosted: 0,
      refused: 0,
      passed: 0,
    };
    if (state.currentHistory > this.cfg.maxHistory) {
      // Chains whose history count is over the quota from before can still
      // sync other types (go-sync's "boosted" quota).
      state.boosted = Math.min(this.cfg.maxEntities - this.cfg.maxHistory, state.currentHistory - this.cfg.maxHistory);
    }

    const idMap = new Map<string, string>();
    for (const pb of entries) {
      const r: EntryResponse = {};
      responses.push(r);
      this.commitEntry(pb, cacheGuid, r, idMap, state);
    }

    this.recordQuota(state.refused, state.passed);
    counts.itemCount += state.newNormal;
    counts.h4 += state.newHistory;
    this.writeCounts(counts);
  }

  private commitEntry(
    pb: SyncEntity,
    cacheGuid: string | undefined,
    r: EntryResponse,
    idMap: Map<string, string>,
    state: {
      currentNormal: number;
      currentHistory: number;
      newNormal: number;
      newHistory: number;
      boosted: number;
      refused: number;
      passed: number;
    },
  ): void {
    let entity: DBEntity;
    try {
      entity = this.toDBEntity(pb, cacheGuid);
    } catch (e) {
      r.responseType = ResponseType.INVALID_MESSAGE;
      r.errorMessage = `Cannot convert protobuf sync entity to DB format: ${e instanceof Error ? e.message : e}`;
      return;
    }
    if (entity.parentId !== undefined && idMap.has(entity.parentId)) {
      entity.parentId = idMap.get(entity.parentId);
    }
    const oldVersion = entity.version ?? 0;
    let isUpdate = oldVersion !== 0;
    const isHistory = entity.dataType === HISTORY_TYPE || entity.dataType === HISTORY_DELETE_DIRECTIVE_TYPE;
    entity.version = entity.mtime;
    if (entity.dataType === HISTORY_TYPE) {
      isUpdate = this.sql.exec(`SELECT 1 FROM entities WHERE id = ?`, entity.id).toArray().length > 0;
    }

    const overQuota = (refused: number) => {
      state.refused |= refused;
      r.responseType = ResponseType.OVER_QUOTA;
      r.errorMessage = "The chain is over its storage quota";
    };

    if (isUpdate) {
      let result: { conflict: boolean; deleted: boolean };
      try {
        result = this.updateEntity(entity, oldVersion);
      } catch (e) {
        if (e instanceof QuotaExceeded) return overQuota(QUOTA_BYTES);
        // As the Go server: any other error of an update, a failed
        // re-create included, is transient.
        r.responseType = ResponseType.TRANSIENT_ERROR;
        r.errorMessage = `Update sync entity failed: ${e instanceof Error ? e.message : e}`;
        return;
      }
      if (result.conflict) {
        r.responseType = ResponseType.CONFLICT;
        return;
      }
      if (result.deleted) {
        if (isHistory) state.newHistory--;
        else state.newNormal--;
      }
    } else {
      const total = state.currentNormal + state.currentHistory + state.newNormal + state.newHistory;
      if (total >= this.cfg.maxEntities + state.boosted) {
        state.refused |= QUOTA_ENTITIES;
        r.responseType = ResponseType.OVER_QUOTA;
        r.errorMessage = `There are already ${state.currentNormal + state.currentHistory} non-deleted objects in store`;
        return;
      }
      // History over its quota is dropped, and the client is told it
      // synced so its other types keep syncing; the chain remembers it.
      if (isHistory && state.currentHistory + state.newHistory >= this.cfg.maxHistory) {
        state.refused |= QUOTA_HISTORY;
      } else {
        try {
          this.insertEntity(entity);
        } catch (e) {
          if (e instanceof QuotaExceeded) return overQuota(QUOTA_BYTES);
          r.responseType = e instanceof InsertError && e.conflict ? ResponseType.CONFLICT : ResponseType.TRANSIENT_ERROR;
          r.errorMessage = `Insert sync entity failed: ${e instanceof Error ? e.message : e}`;
          return;
        }
        if (entity.originatorClientItemId !== undefined) idMap.set(entity.originatorClientItemId, entity.id);
        state.passed |= isHistory ? QUOTA_HISTORY : QUOTA_ENTITIES | QUOTA_BYTES;
        if (!entity.replacedLive) {
          if (isHistory) state.newHistory++;
          else state.newNormal++;
        }
      }
    }
    r.responseType = ResponseType.SUCCESS;
    r.idString = entity.id;
    r.version = entity.version;
    r.mtime = entity.mtime;
  }

  // toDBEntity converts a committed entity (datastore.CreateDBSyncEntity).
  private toDBEntity(e: SyncEntity, cacheGuid: string | undefined): DBEntity {
    if (e.idString === undefined) throw new Error("validate SyncEntity error: empty IdString");
    if (e.version === undefined) throw new Error("validate SyncEntity error: empty Version");
    if (e.specifics === undefined) throw new Error("validate SyncEntity error: nil Specifics");
    let dataType: number;
    try {
      dataType = dataTypeOfSpecifics(e.specifics);
    } catch (err) {
      throw new Error(`error finding data type to create DB sync entity: ${err instanceof ProtoError ? err.message : err}`);
    }
    let id = e.idString;
    let originatorCacheGuid: string | undefined;
    let originatorClientItemId: string | undefined;
    if (cacheGuid !== undefined) {
      if (e.version === 0) id = crypto.randomUUID();
      originatorCacheGuid = cacheGuid;
      originatorClientItemId = e.idString;
    }
    if (dataType === HISTORY_TYPE) {
      // History is keyed by its client tag hash.
      if (e.clientTagHash === undefined) throw new Error("history entity without a client tag");
      id = e.clientTagHash;
    }
    let expirationTime: number | undefined;
    if (dataType === HISTORY_TYPE || dataType === HISTORY_DELETE_DIRECTIVE_TYPE) {
      expirationTime = Math.floor(Date.now() / 1000) + this.historySecs;
    }
    const mtime = this.nextMtime();
    let deleted = e.deleted;
    let folder = e.folder;
    if (e.version === 0) {
      deleted ??= false;
      folder ??= false;
    }
    return {
      id,
      parentId: e.parentIdString,
      version: e.version,
      mtime,
      ctime: e.ctime ?? mtime,
      name: e.name,
      nonUniqueName: e.nonUniqueName,
      serverDefinedUniqueTag: e.serverDefinedUniqueTag,
      deleted,
      originatorCacheGuid,
      originatorClientItemId,
      specifics: e.specifics,
      dataType,
      folder,
      clientTag: e.clientTagHash,
      uniquePosition: e.uniquePosition,
      expirationTime,
    };
  }

  // addBytes adds to the chain's stored bytes, refusing growth past the cap.
  private addBytes(delta: number): void {
    if (delta > 0 && this.cfg.maxBytes > 0) {
      const current = Number(this.sql.exec(`SELECT bytes FROM chain`).one().bytes);
      if (current + delta > this.cfg.maxBytes) throw new QuotaExceeded("chain storage quota exceeded");
    }
    if (delta !== 0) this.sql.exec(`UPDATE chain SET bytes = MAX(bytes + ?, 0)`, delta);
  }

  private insertTag(id: string): boolean {
    const now = Date.now();
    const c = this.sql.exec(`INSERT INTO unique_tags (id, mtime, ctime) VALUES (?, ?, ?) ON CONFLICT DO NOTHING`, id, now, now);
    return c.rowsWritten > 0;
  }

  private writeNewEntity(e: DBEntity): boolean {
    const size = entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName);
    const c = this.sql.exec(
      `INSERT INTO entities (${ENTITY_COLUMNS}, size)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT DO NOTHING`,
      e.id, e.parentId ?? null, e.version ?? null, e.mtime, e.ctime ?? null, e.name ?? null,
      e.nonUniqueName ?? null, e.serverDefinedUniqueTag ?? null, boolCol(e.deleted),
      e.originatorCacheGuid ?? null, e.originatorClientItemId ?? null, blob(e.specifics),
      e.dataType, boolCol(e.folder), e.clientTag ?? null, blob(e.uniquePosition),
      e.expirationTime ?? null, size,
    );
    return c.rowsWritten > 0;
  }

  // insertEntity stores a new entity (datastore.SQLite.InsertSyncEntity). A
  // create whose client tag belongs to an entity of the same type replaces
  // it under its ID, deleted or live (create over a tombstone).
  private insertEntity(e: DBEntity): void {
    this.ctx.storage.transactionSync(() => {
      const withTag = e.clientTag !== undefined && e.dataType !== HISTORY_TYPE;
      if (withTag) {
        const old = this.sql.exec(
          `SELECT id, deleted, size FROM entities
           WHERE client_defined_unique_tag = ? AND data_type = ?
           ORDER BY mtime DESC, rowid DESC LIMIT 1`,
          e.clientTag!, e.dataType,
        ).toArray()[0];
        if (old) {
          const oldDeleted = Number(old.deleted ?? 0) !== 0;
          if (!oldDeleted && e.deleted) {
            throw new InsertError("error inserting sync item with client tag: tag in use", true);
          }
          if (!e.deleted) {
            this.reviveEntity(e, old.id as string, Number(old.size));
            e.replacedLive = !oldDeleted;
            return;
          }
        }
      }
      this.addBytes(entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName));
      if (withTag && !this.insertTag("Client#" + e.clientTag)) {
        throw new InsertError("error inserting sync item with client tag: tag in use", true);
      }
      if (!this.writeNewEntity(e)) {
        throw new InsertError("error inserting sync item: ID in use", withTag);
      }
    });
  }

  private reviveEntity(e: DBEntity, oldID: string, oldSize: number): void {
    const size = entitySize(e.specifics, e.uniquePosition, e.name, e.nonUniqueName);
    this.addBytes(size - oldSize);
    this.sql.exec(
      `UPDATE entities SET
         parent_id = ?, version = ?, mtime = ?, ctime = ?, name = ?, non_unique_name = ?,
         server_defined_unique_tag = ?, deleted = ?, originator_cache_guid = ?,
         originator_client_item_id = ?, specifics = ?, folder = ?, unique_position = ?,
         expiration_time = ?, size = ?
       WHERE id = ?`,
      e.parentId ?? null, e.version ?? null, e.mtime, e.ctime ?? null, e.name ?? null,
      e.nonUniqueName ?? null, e.serverDefinedUniqueTag ?? null, boolCol(e.deleted),
      e.originatorCacheGuid ?? null, e.originatorClientItemId ?? null, blob(e.specifics),
      boolCol(e.folder), blob(e.uniquePosition), e.expirationTime ?? null, size, oldID,
    );
    this.insertTag("Client#" + e.clientTag);
    e.id = oldID;
  }

  // updateEntity updates an entity at oldVersion (any version for history)
  // (datastore.SQLite.UpdateSyncEntity).
  private updateEntity(e: DBEntity, oldVersion: number): { conflict: boolean; deleted: boolean } {
    const old = this.sql.exec(
      `SELECT version, deleted, size, unique_position, name, non_unique_name FROM entities WHERE id = ?`,
      e.id,
    ).toArray()[0];
    if (!old) {
      // A purged tombstone or an expired directive: deleting it again is
      // done; an update re-creates it if it has a client tag (the local
      // edit wins over the remote delete).
      if (e.deleted) return { conflict: false, deleted: false };
      if (e.clientTag !== undefined && e.dataType !== HISTORY_TYPE) {
        e.ctime ??= e.mtime;
        e.deleted ??= false;
        e.folder ??= false;
        this.insertEntity(e);
        return { conflict: false, deleted: false };
      }
      return { conflict: true, deleted: false };
    }
    if (e.dataType !== HISTORY_TYPE && (old.version === null || Number(old.version) !== oldVersion)) {
      return { conflict: true, deleted: false };
    }
    let deleted = false;
    this.ctx.storage.transactionSync(() => {
      const pos = e.uniquePosition ?? bytesOf(old.unique_position);
      const name = e.name ?? opt(old.name as string | null);
      const nun = e.nonUniqueName ?? opt(old.non_unique_name as string | null);
      const newSize = entitySize(e.specifics, pos, name, nun);
      this.addBytes(newSize - Number(old.size ?? 0));
      this.sql.exec(
        `UPDATE entities SET
           version = ?, mtime = ?, specifics = ?, size = ?,
           unique_position = COALESCE(?, unique_position),
           parent_id = COALESCE(?, parent_id),
           name = COALESCE(?, name),
           non_unique_name = COALESCE(?, non_unique_name),
           deleted = COALESCE(?, deleted),
           folder = COALESCE(?, folder),
           expiration_time = COALESCE(?, expiration_time)
         WHERE id = ?`,
        e.version ?? null, e.mtime, blob(e.specifics), newSize, blob(e.uniquePosition),
        e.parentId ?? null, e.name ?? null, e.nonUniqueName ?? null, boolCol(e.deleted),
        boolCol(e.folder), e.expirationTime ?? null, e.id,
      );
      const oldDeleted = old.deleted === null ? undefined : Number(old.deleted) !== 0;
      const tagged = e.clientTag !== undefined && e.dataType !== HISTORY_TYPE;
      if (e.deleted === true && tagged) {
        // A soft delete frees the client tag.
        this.sql.exec(`DELETE FROM unique_tags WHERE id = ?`, "Client#" + e.clientTag);
        deleted = true;
      } else {
        if (oldDeleted === true && e.deleted === false && tagged) {
          // Undeleting takes the tag back.
          this.insertTag("Client#" + e.clientTag);
        }
        if (e.deleted === undefined) deleted = false;
        else if (oldDeleted === undefined) deleted = e.deleted;
        else deleted = !oldDeleted && e.deleted;
      }
    });
    return { conflict: false, deleted };
  }

  // --- Counts and quota ------------------------------------------------------

  private readCounts(): Counts {
    const r = this.sql.exec(
      `SELECT item_count, history_period1, history_period2, history_period3, history_period4,
              last_period_change_time FROM chain`,
    ).one();
    const c: Counts = {
      itemCount: Number(r.item_count),
      h1: Number(r.history_period1),
      h2: Number(r.history_period2),
      h3: Number(r.history_period3),
      h4: Number(r.history_period4),
      lastPeriodChange: Number(r.last_period_change_time),
    };
    // History is counted in four periods of a quarter of its retention; the
    // oldest drops out as its entities expire.
    const now = Math.floor(Date.now() / 1000);
    if (c.lastPeriodChange === 0) {
      c.lastPeriodChange = now;
      return c;
    }
    const period = Math.max(Math.floor(this.historySecs / 4), 1);
    const changes = Math.floor((now - c.lastPeriodChange) / period);
    for (let i = 0; i < Math.min(changes, 4); i++) {
      c.h1 = c.h2;
      c.h2 = c.h3;
      c.h3 = c.h4;
      c.h4 = 0;
    }
    if (changes > 0) c.lastPeriodChange += period * changes;
    return c;
  }

  private writeCounts(c: Counts): void {
    this.sql.exec(
      `UPDATE chain SET item_count = ?, history_period1 = ?, history_period2 = ?,
         history_period3 = ?, history_period4 = ?, last_period_change_time = ?`,
      Math.max(c.itemCount, 0), c.h1, c.h2, c.h3, Math.max(c.h4, 0), c.lastPeriodChange,
    );
  }

  // recordQuota remembers which limits refused this commit's entries, and
  // forgets those an entry got past (datastore.SQLite.RecordQuota).
  private recordQuota(full: number, cleared: number): void {
    cleared &= ~full;
    if (full) {
      this.sql.exec(
        `UPDATE chain SET quota_full = (quota_full | ?) & ~?, quota_full_time = ?`,
        full, cleared, Math.floor(Date.now() / 1000),
      );
    } else if (cleared) {
      this.sql.exec(`UPDATE chain SET quota_full = quota_full & ~?`, cleared);
    }
  }

  // quota answers GET /v2/compass/quota (server/quota).
  private quota(): Response {
    if (this.isDisabled()) return text(403, "chain disabled");
    const c = this.readCounts();
    const r = this.sql.exec(`SELECT bytes, quota_full, quota_full_time FROM chain`).one();
    const history = c.h1 + c.h2 + c.h3 + c.h4;
    const bytes = Number(r.bytes);
    const refused = Number(r.quota_full);
    const st = {
      entities: c.itemCount + history,
      max_entities: this.cfg.maxEntities,
      bytes,
      max_bytes: this.cfg.maxBytes,
      history,
      max_history: this.cfg.maxHistory,
      history_days: this.cfg.historyDays,
      full: [] as string[],
      full_since: r.quota_full_time === null ? 0 : Number(r.quota_full_time),
    };
    if (refused & QUOTA_ENTITIES || (st.max_entities > 0 && st.entities >= st.max_entities)) st.full.push("entities");
    if (refused & QUOTA_BYTES || (st.max_bytes > 0 && bytes + BYTES_FULL_MARGIN > st.max_bytes)) st.full.push("bytes");
    if (refused & QUOTA_HISTORY || (st.max_history > 0 && history >= st.max_history)) st.full.push("history");
    return json(200, st);
  }

  // --- Devices ---------------------------------------------------------------

  private isDeviceRemoved(cacheGuid: string): boolean {
    return this.sql.exec(`SELECT 1 FROM removed_devices WHERE cache_guid = ?`, cacheGuid).toArray().length > 0;
  }

  // removeDevice answers DELETE /v2/compass/devices/{cache_guid}: the
  // device's DEVICE_INFO is deleted for everyone and the server refuses it
  // from then on (sync.md, "Identity").
  private async removeDevice(url: URL, self: string): Promise<Response> {
    const raw = url.pathname.replace(/^\/devices\/?/, "");
    let guid: string;
    try {
      guid = decodeURIComponent(raw);
    } catch {
      return fail(400, "bad_request");
    }
    if (!guid || guid.length > MAX_CLIENT_ID || !self || self.length > MAX_CLIENT_ID) return fail(400, "bad_request");
    if (guid === self) return fail(409, "this_device");
    if (this.isDisabled()) return fail(403, "chain_disabled");
    this.sql.exec(
      `INSERT INTO removed_devices (cache_guid, removed_at) VALUES (?, ?) ON CONFLICT DO NOTHING`,
      guid, Math.floor(Date.now() / 1000),
    );
    const tag = await clientTagHash(DEVICE_INFO_TYPE, "DeviceInfo_" + guid);
    const row = this.sql.exec(
      `SELECT id, version, deleted FROM entities
       WHERE client_defined_unique_tag = ? AND data_type = ?
       ORDER BY mtime DESC, rowid DESC LIMIT 1`,
      tag, DEVICE_INFO_TYPE,
    ).toArray()[0];
    if (row && row.version !== null && Number(row.deleted ?? 0) === 0) {
      const rsp: ClientToServerResponse = {};
      this.handleCommit(
        [{
          idString: row.id as string,
          version: Number(row.version),
          deleted: true,
          clientTagHash: tag,
          specifics: emptySpecifics(DEVICE_INFO_TYPE),
        }],
        undefined,
        rsp,
      );
      const entry = rsp.commit!.entryResponse[0];
      if (entry.responseType !== ResponseType.SUCCESS) {
        console.error("deleting device info", entry);
        return fail(500, "unable to complete request");
      }
    }
    for (const ws of this.ctx.getWebSockets("c:" + guid)) ws.close(1000, "device removed");
    this.sql.exec(`DELETE FROM push_handles WHERE cache_guid = ?`, guid);
    this.notify(self, [DEVICE_INFO_TYPE]);
    return new Response(null, { status: 204, headers: { "Cache-Control": "no-store" } });
  }

  // --- Change notifications --------------------------------------------------

  private events(request: Request, client: string): Response {
    if (request.headers.get("Upgrade") !== "websocket") return text(426, "websocket required");
    if (this.isDisabled()) return fail(403, "sync chain disabled");
    if (client.length > MAX_CLIENT_ID) return fail(400, "client_id too long");
    if (this.ctx.getWebSockets().length >= MAX_STREAMS) {
      return fail(429, "too many streams for this chain", { "Retry-After": "60" });
    }
    const pair = new WebSocketPair();
    const [outside, inside] = [pair[0], pair[1]];
    this.ctx.acceptWebSocket(inside, client ? ["c:" + client] : []);
    inside.serializeAttachment({ client });
    return new Response(null, { status: 101, webSocket: outside });
  }

  // notify tells every stream but `except`'s which types changed.
  private notify(except: string, types: number[]): void {
    if (types.length === 0) return;
    const msg = `event: changed\ndata: ${JSON.stringify({ types })}\n\n`;
    for (const ws of this.ctx.getWebSockets()) {
      const att = ws.deserializeAttachment() as { client?: string } | null;
      if (except && att?.client === except) continue;
      try {
        ws.send(msg);
      } catch {
        // Closing; the Worker reconnects the device.
      }
    }
  }

  private notifyCommit(msg: ClientToServerMessage, rsp: ClientToServerResponse, isNew: boolean[]): void {
    const entries = msg.commit?.entries ?? [];
    const results = rsp.commit?.entryResponse ?? [];
    const types = new Set<number>();
    let sentTab = false;
    entries.forEach((e, i) => {
      if (results[i]?.responseType !== ResponseType.SUCCESS || !e.specifics) return;
      try {
        const type = dataTypeOfSpecifics(e.specifics);
        types.add(type);
        if (type === SEND_TAB_TYPE && isNew[i]) sentTab = true;
      } catch {
        // Not a type: nothing to announce.
      }
    });
    const from = msg.commit?.cacheGuid ?? "";
    this.notify(from, [...types].sort((a, b) => a - b));
    const background = [...types].some((t) => BACKGROUND_TYPES.has(t));
    if ((sentTab || background) && this.cfg.pushRelay) {
      this.ctx.waitUntil(this.pushCommit(from, sentTab, background));
    }
  }

  // --- Push (push.ts) ---------------------------------------------------------

  // register answers POST and DELETE /v2/compass/push?client_id=<cache GUID>
  // as push.go does, keeping a relay handle instead of the token. Without a
  // relay, registrations are accepted and nothing is kept.
  private async register(request: Request, guid: string): Promise<Response> {
    if (this.isDisabled()) return text(403, "sync chain disabled");
    if (!guid || guid.length > MAX_CLIENT_ID) return text(400, "client_id required (the device's cache GUID)");
    if (request.method === "DELETE") {
      this.sql.exec(`DELETE FROM push_handles WHERE cache_guid = ?`, guid);
      return new Response(null, { status: 204 });
    }
    let raw: string;
    try {
      raw = new TextDecoder().decode(await readBody(request));
    } catch {
      return text(400, "JSON body required");
    }
    if (raw.length > 8192) return text(400, "JSON body required");
    const reg = parseRegistration(raw, this.cfg);
    if (typeof reg === "string") return text(400, reg);
    const relay = this.cfg.pushRelay;
    if (!relay) return new Response(null, { status: 204 });
    const server = request.headers.get("X-Host") ?? "";
    const handle = await seal(relay, server, reg);
    if (handle === "refused") return text(400, "the push relay refused this registration");
    if (handle === null) return text(503, "the push relay can't be reached; register again later");
    const hash = await tokenHash(reg.platform, reg.token);
    const now = Math.floor(Date.now() / 1000);
    this.ctx.storage.transactionSync(() => {
      // The same device under another cache GUID (it rejoined the chain).
      this.sql.exec(`DELETE FROM push_handles WHERE token_hash = ? AND cache_guid <> ?`, hash, guid);
      this.sql.exec(
        `INSERT INTO push_handles (cache_guid, handle, token_hash, platform, server, relay, updated)
         VALUES (?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT (cache_guid) DO UPDATE SET handle = excluded.handle, token_hash = excluded.token_hash,
           platform = excluded.platform, server = excluded.server, relay = excluded.relay,
           updated = excluded.updated`,
        guid, handle, hash, reg.platform, server, relay, now,
      );
      this.sql.exec(
        `DELETE FROM push_handles WHERE cache_guid NOT IN (
           SELECT cache_guid FROM push_handles ORDER BY updated DESC, cache_guid LIMIT ?)`,
        MAX_REGISTRATIONS,
      );
    });
    return new Response(null, { status: 204 });
  }

  // The registrations made at this server's current relay (a handle from
  // another relay can't be opened by this one; its device registers again
  // at its next launch).
  private handles(): { guid: string; handle: string; platform: string; server: string }[] {
    return this.sql
      .exec(`SELECT cache_guid, handle, platform, server FROM push_handles WHERE relay = ?`, this.cfg.pushRelay)
      .toArray()
      .map((r) => ({
        guid: r.cache_guid as string,
        handle: r.handle as string,
        platform: r.platform as string,
        server: r.server as string,
      }));
  }

  private streaming(guid: string): boolean {
    return this.ctx.getWebSockets("c:" + guid).length > 0;
  }

  // pushCommit: a new sent tab alerts every other device; a background
  // type has every other iPhone sync, spaced (push.go, OnCommit).
  private async pushCommit(from: string, sentTab: boolean, background: boolean): Promise<void> {
    const now = Date.now();
    const sends: Promise<void>[] = [];
    for (const h of this.handles()) {
      if (h.guid === from) continue;
      if (sentTab) sends.push(this.send(h, "send-tab"));
      if (!background || h.platform !== "apns" || this.streaming(h.guid)) continue;
      const row = this.sql
        .exec(`SELECT bg_last, bg_due FROM push_handles WHERE cache_guid = ?`, h.guid)
        .one();
      if (row.bg_due !== null) continue; // One is waiting; it covers this change too.
      const last = Number(row.bg_last);
      if (last === 0 || now - last >= this.cfg.backgroundIntervalMs) {
        this.sql.exec(`UPDATE push_handles SET bg_last = ? WHERE cache_guid = ?`, now, h.guid);
        sends.push(this.send(h, "sync"));
      } else {
        const due = last + this.cfg.backgroundIntervalMs;
        this.sql.exec(`UPDATE push_handles SET bg_due = ? WHERE cache_guid = ?`, due, h.guid);
        const alarm = await this.ctx.storage.getAlarm();
        if (alarm === null || alarm > due) await this.ctx.storage.setAlarm(due);
      }
    }
    await Promise.all(sends);
  }

  // sendDueBackground sends the background pushes whose interval has passed
  // (from the alarm), unless Compass is open there by now.
  private async sendDueBackground(now: number): Promise<void> {
    const due = this.sql
      .exec(`SELECT cache_guid FROM push_handles WHERE bg_due IS NOT NULL AND bg_due <= ?`, now)
      .toArray()
      .map((r) => r.cache_guid as string);
    if (due.length === 0 || !this.cfg.pushRelay) {
      this.sql.exec(`UPDATE push_handles SET bg_due = NULL WHERE bg_due IS NOT NULL AND bg_due <= ?`, now);
      return;
    }
    const byGuid = new Map(this.handles().map((h) => [h.guid, h]));
    const sends: Promise<void>[] = [];
    for (const guid of due) {
      this.sql.exec(`UPDATE push_handles SET bg_due = NULL, bg_last = ? WHERE cache_guid = ?`, now, guid);
      const h = byGuid.get(guid);
      if (h && !this.streaming(guid)) sends.push(this.send(h, "sync"));
    }
    await Promise.all(sends);
  }

  private async send(h: { guid: string; handle: string; server: string }, type: "send-tab" | "sync"): Promise<void> {
    const result = await relayPush(this.cfg.pushRelay, h.server, h.handle, type);
    if (result === "drop") {
      this.sql.exec(`DELETE FROM push_handles WHERE cache_guid = ? AND handle = ?`, h.guid, h.handle);
    }
  }

  async webSocketMessage(): Promise<void> {
    // Streams only listen.
  }

  async webSocketClose(ws: WebSocket, code: number): Promise<void> {
    try {
      ws.close(code === 1005 ? 1000 : code, "closed");
    } catch {
      // Already closed.
    }
  }

  // --- Rooms (calls from the Worker and room objects) -----------------------

  // roomGate is the Worker's check before a rooms request (rooms.ts): is the
  // device removed, is the chain disabled. A chain that passes is marked
  // active, as a sync request does.
  async roomGate(chainID: string, client: string): Promise<{ disabled: boolean; removed: boolean }> {
    this.rememberChainID(chainID);
    const removed = client !== "" && this.isDeviceRemoved(client);
    const disabled = this.isDisabled();
    if (!removed && !disabled) await this.touch();
    return { disabled, removed };
  }

  status(): { disabled: boolean } {
    return { disabled: this.isDisabled() };
  }

  // addRoomBytes counts a change of a room's size (it owns the room) in the
  // chain's bytes: false, and nothing counted, when growth wouldn't fit
  // COMPASS_SYNC_MAX_BYTES. Shrinking always works.
  addRoomBytes(delta: number): boolean {
    try {
      this.addBytes(delta);
      return true;
    } catch (e) {
      if (e instanceof QuotaExceeded) return false;
      throw e;
    }
  }

  // roomEvent is a room's object telling of a change: the chain's index of
  // rooms takes the membership as it now is (none: not a member any more),
  // and every stream but the calling device's gets a "room" event.
  roomEvent(ev: RoomEvent): void {
    if (this.sql.exec(`SELECT chain_id FROM chain`).one().chain_id === null) this.rememberChainID(ev.chain);
    if (!this.isDisabled()) {
      this.ctx.storage.transactionSync(() => {
        if (ev.identity) this.storeIdentity(ev.identity);
        const m = ev.membership;
        if (m) {
          this.sql.exec(
            `INSERT INTO room_index (room_id, kind, owner, role, pending, epoch, seq) VALUES (?, ?, ?, ?, ?, ?, ?)
             ON CONFLICT (room_id) DO UPDATE SET kind = excluded.kind, owner = excluded.owner,
               role = excluded.role, pending = excluded.pending, epoch = excluded.epoch, seq = excluded.seq`,
            ev.room, m.kind, m.owner, m.role, m.pending ? 1 : 0, m.epoch, m.seq,
          );
        } else {
          this.sql.exec(`DELETE FROM room_index WHERE room_id = ?`, ev.room);
        }
      });
    }
    if (ev.silent) return;
    const msg = `event: room\ndata: ${JSON.stringify({ room_id: ev.room, seq: ev.seq, changes: ev.changes })}\n\n`;
    for (const ws of this.ctx.getWebSockets()) {
      const att = ws.deserializeAttachment() as { client?: string } | null;
      if (ev.skipClient && att?.client === ev.skipClient) continue;
      try {
        ws.send(msg);
      } catch {
        // Closing; the Worker reconnects the device.
      }
    }
  }

  // listRooms answers GET /v2/compass/rooms: the chain's current
  // memberships, by room ID; null for a chain the private server doesn't
  // list and that is in no room.
  listRooms(listed: boolean): { rooms: Record<string, unknown>[] } | null {
    const rooms = this.sql
      .exec(`SELECT room_id, kind, owner, role, pending, epoch, seq FROM room_index ORDER BY room_id`)
      .toArray()
      .map((r) => ({
        id: r.room_id,
        kind: r.kind,
        owner: r.owner,
        role: r.role,
        pending: Number(r.pending) !== 0,
        epoch: Number(r.epoch),
        seq: Number(r.seq),
      }));
    if (!listed && rooms.length === 0) return null;
    return { rooms };
  }

  hasRooms(): boolean {
    return this.sql.exec(`SELECT 1 FROM room_index LIMIT 1`).toArray().length > 0;
  }

  ownedRoomCount(): number {
    return Number(this.sql.exec(`SELECT COUNT(*) AS n FROM room_index WHERE role = 'owner'`).one().n);
  }

  // identityKey is the chain's X25519 identity key (base64), or null.
  identityKey(): string | null {
    const r = this.sql.exec(`SELECT pub FROM identity`).toArray()[0];
    return r ? (r.pub as string) : null;
  }

  // setIdentityKey records the identity key and returns the rooms the chain
  // is a current member of, whose memberships take it too.
  setIdentityKey(pub: string): string[] {
    this.storeIdentity(pub);
    return this.sql.exec(`SELECT room_id FROM room_index ORDER BY room_id`).toArray().map((r) => r.room_id as string);
  }

  private storeIdentity(pub: string): void {
    this.sql.exec(
      `INSERT INTO identity (one, pub, updated) VALUES (1, ?, ?)
       ON CONFLICT (one) DO UPDATE SET pub = excluded.pub, updated = excluded.updated WHERE identity.pub IS NOT excluded.pub`,
      pub, Date.now(),
    );
  }

  // --- Maintenance -----------------------------------------------------------

  // disable clears the chain's data and refuses it from now on ("reset
  // sync", and idle chains): ClearServerData. Its rooms go too (sync.md,
  // "Going away"): those it owns are deleted, its other memberships and
  // envelopes and its identity key go; nothing is announced.
  private async disable(): Promise<void> {
    const chain = String(this.sql.exec(`SELECT chain_id FROM chain`).one().chain_id ?? "").toLowerCase();
    const rooms = this.sql.exec(`SELECT room_id FROM room_index`).toArray().map((r) => r.room_id as string);
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`DELETE FROM entities`);
      this.sql.exec(`DELETE FROM unique_tags`);
      this.sql.exec(`DELETE FROM removed_devices`);
      this.sql.exec(`DELETE FROM room_index`);
      this.sql.exec(`DELETE FROM identity`);
      this.sql.exec(`DELETE FROM push_handles`);
      this.sql.exec(
        `UPDATE chain SET disabled_at = ?, bytes = 0, item_count = 0, history_period1 = 0,
           history_period2 = 0, history_period3 = 0, history_period4 = 0, quota_full = 0`,
        Date.now(),
      );
    });
    for (const ws of this.ctx.getWebSockets()) ws.close(1000, "chain disabled");
    if (!chain) return;
    for (let i = 0; i < rooms.length; i += 50) {
      await Promise.all(
        rooms.slice(i, i + 50).map((room) =>
          this.env.ROOM.get(this.env.ROOM.idFromName(room))
            .chainGone(room, chain)
            .catch((e: unknown) => console.error("clearing a room of a cleared chain failed", e)),
        ),
      );
    }
  }

  // alarm runs hourly while the chain is in use: expired history and old
  // tombstones go, and an idle chain is cleared and disabled. It also sends
  // background pushes that waited for their interval (pushCommit), so it
  // runs at the earlier of the two.
  async alarm(): Promise<void> {
    if (this.isDisabled()) return;
    const now = Date.now();
    await this.sendDueBackground(now);
    const purgeAt = this.ctx.storage.kv.get<number>("purge_at") ?? 0;
    if (now < purgeAt) {
      await this.setNextAlarm(purgeAt);
      return;
    }
    const lastActive = this.sql.exec(`SELECT last_active FROM chain`).one().last_active;
    if (
      this.cfg.idleChainDays > 0 &&
      lastActive !== null &&
      Number(lastActive) < Math.floor(now / 1000) - this.cfg.idleChainDays * 86400
    ) {
      await this.disable();
      return;
    }
    this.purge(now);
    this.ctx.storage.kv.put("purge_at", now + PURGE_INTERVAL_MS);
    await this.setNextAlarm(now + PURGE_INTERVAL_MS);
  }

  private async setNextAlarm(purgeAt: number): Promise<void> {
    const due = this.sql.exec(`SELECT MIN(bg_due) AS d FROM push_handles`).one().d;
    await this.ctx.storage.setAlarm(due === null ? purgeAt : Math.min(purgeAt, Number(due)));
  }

  private purge(now: number): void {
    const cutoff = Math.floor(now / 1000);
    this.ctx.storage.transactionSync(() => {
      const expired = `expiration_time > 0 AND expiration_time < ?`;
      this.sql.exec(
        `DELETE FROM unique_tags WHERE id IN (
           SELECT 'Client#' || client_defined_unique_tag FROM entities
           WHERE ${expired} AND client_defined_unique_tag IS NOT NULL AND data_type != ?)`,
        cutoff, HISTORY_TYPE,
      );
      const gone = Number(
        this.sql.exec(`SELECT COALESCE(SUM(size), 0) AS s FROM entities WHERE ${expired}`, cutoff).one().s,
      );
      this.sql.exec(`DELETE FROM entities WHERE ${expired}`, cutoff);
      if (this.cfg.tombstoneDays > 0) {
        const before = now - this.cfg.tombstoneDays * 86400_000;
        const tomb = Number(
          this.sql.exec(`SELECT COALESCE(SUM(size), 0) AS s FROM entities WHERE deleted = 1 AND mtime < ?`, before)
            .one().s,
        );
        this.sql.exec(`DELETE FROM entities WHERE deleted = 1 AND mtime < ?`, before);
        this.sql.exec(`UPDATE chain SET bytes = MAX(0, bytes - ?)`, gone + tomb);
      } else {
        this.sql.exec(`UPDATE chain SET bytes = MAX(0, bytes - ?)`, gone);
      }
    });
  }
}

class CodedError extends Error {
  constructor(readonly code: number, message: string) {
    super(message);
  }
}

function protoResponse(rsp: ClientToServerResponse): Response {
  return new Response(encodeClientToServerResponse(rsp), {
    status: 200,
    headers: { "Content-Type": "application/x-protobuf" },
  });
}

async function readBody(request: Request): Promise<Uint8Array> {
  let stream = request.body;
  if (!stream) return new Uint8Array(0);
  if (request.headers.get("Content-Encoding") === "gzip") {
    stream = stream.pipeThrough(new DecompressionStream("gzip"));
  }
  const reader = stream.getReader();
  const parts: Uint8Array[] = [];
  let size = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    const take = Math.min(value.length, PAYLOAD_LIMIT - size);
    if (take > 0) parts.push(value.subarray(0, take));
    size += take;
    if (size >= PAYLOAD_LIMIT) {
      await reader.cancel();
      break;
    }
  }
  const out = new Uint8Array(size);
  let pos = 0;
  for (const p of parts) {
    out.set(p, pos);
    pos += p.length;
  }
  return out;
}
