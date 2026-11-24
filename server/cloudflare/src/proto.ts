// The parts of Chromium's sync protocol (sync.proto) the server reads and
// writes, on a small protobuf wire codec. Specifics and unique positions
// stay as the bytes the client sent: the server never reads them (they are
// encrypted), so types newer than any proto file sync as they are.

const WIRE_VARINT = 0;
const WIRE_I64 = 1;
const WIRE_LEN = 2;
const WIRE_START_GROUP = 3;
const WIRE_END_GROUP = 4;
const WIRE_I32 = 5;

export class ProtoError extends Error {}

// --- Reading -----------------------------------------------------------------

export interface Field {
  num: number;
  wire: number;
  // A varint's value (int64 values used here fit in a double), or the
  // bytes of a length-delimited field.
  int: number;
  bytes: Uint8Array;
}

const EMPTY = new Uint8Array(0);

function readVarint(buf: Uint8Array, pos: number): [number, number] {
  let value = 0;
  let mul = 1;
  for (let i = 0; i < 10; i++) {
    if (pos >= buf.length) throw new ProtoError("truncated varint");
    const b = buf[pos++];
    value += (b & 0x7f) * mul;
    if (b < 0x80) return [value, pos];
    mul *= 128;
  }
  throw new ProtoError("varint too long");
}

// fields walks a message's fields in order. Groups are returned as one
// field (wire START_GROUP) whose bytes are the group's contents.
export function* fields(buf: Uint8Array): Generator<Field> {
  let pos = 0;
  while (pos < buf.length) {
    let tag: number;
    [tag, pos] = readVarint(buf, pos);
    const num = Math.floor(tag / 8);
    const wire = tag & 7;
    if (num < 1) throw new ProtoError("bad field number");
    switch (wire) {
      case WIRE_VARINT: {
        let v: number;
        [v, pos] = readVarint(buf, pos);
        yield { num, wire, int: v, bytes: EMPTY };
        break;
      }
      case WIRE_I64:
        if (pos + 8 > buf.length) throw new ProtoError("truncated fixed64");
        yield { num, wire, int: 0, bytes: buf.subarray(pos, pos + 8) };
        pos += 8;
        break;
      case WIRE_I32:
        if (pos + 4 > buf.length) throw new ProtoError("truncated fixed32");
        yield { num, wire, int: 0, bytes: buf.subarray(pos, pos + 4) };
        pos += 4;
        break;
      case WIRE_LEN: {
        let len: number;
        [len, pos] = readVarint(buf, pos);
        if (pos + len > buf.length) throw new ProtoError("truncated bytes");
        yield { num, wire, int: 0, bytes: buf.subarray(pos, pos + len) };
        pos += len;
        break;
      }
      case WIRE_START_GROUP: {
        const start = pos;
        const end = skipGroup(buf, pos, num);
        yield { num, wire, int: 0, bytes: buf.subarray(start, end.contentEnd) };
        pos = end.next;
        break;
      }
      default:
        throw new ProtoError(`unexpected wire type ${wire}`);
    }
  }
}

function skipGroup(buf: Uint8Array, pos: number, num: number): { contentEnd: number; next: number } {
  for (;;) {
    const before = pos;
    let tag: number;
    [tag, pos] = readVarint(buf, pos);
    const wire = tag & 7;
    const n = Math.floor(tag / 8);
    switch (wire) {
      case WIRE_END_GROUP:
        if (n !== num) throw new ProtoError("mismatched group end");
        return { contentEnd: before, next: pos };
      case WIRE_VARINT:
        [, pos] = readVarint(buf, pos);
        break;
      case WIRE_I64:
        pos += 8;
        break;
      case WIRE_I32:
        pos += 4;
        break;
      case WIRE_LEN: {
        let len: number;
        [len, pos] = readVarint(buf, pos);
        pos += len;
        break;
      }
      case WIRE_START_GROUP:
        pos = skipGroup(buf, pos, n).next;
        break;
      default:
        throw new ProtoError(`unexpected wire type ${wire}`);
    }
    if (pos > buf.length) throw new ProtoError("truncated group");
  }
}

const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const encoder = new TextEncoder();

function str(f: Field): string {
  if (f.wire !== WIRE_LEN) throw new ProtoError(`field ${f.num} is not a string`);
  try {
    return decoder.decode(f.bytes);
  } catch {
    throw new ProtoError(`field ${f.num} is not UTF-8`);
  }
}

function int(f: Field): number {
  if (f.wire !== WIRE_VARINT) throw new ProtoError(`field ${f.num} is not a varint`);
  // int32/int64 fields: negative values arrive as 10-byte varints.
  return f.int >= 2 ** 63 ? f.int - 2 ** 64 : f.int;
}

function bool(f: Field): boolean {
  return int(f) !== 0;
}

function len(f: Field): Uint8Array {
  if (f.wire !== WIRE_LEN) throw new ProtoError(`field ${f.num} is not length-delimited`);
  return f.bytes;
}

// --- Writing -----------------------------------------------------------------

export class Writer {
  // Small values collect in `small`; byte arrays are kept as they are, so a
  // response of large specifics isn't copied byte by byte.
  private parts: Uint8Array[] = [];
  private small: number[] = [];
  private size = 0;

  private flush(): void {
    if (this.small.length) {
      this.parts.push(Uint8Array.from(this.small));
      this.small = [];
    }
  }

  private raw(v: Uint8Array): void {
    this.flush();
    this.parts.push(v);
    this.size += v.length;
  }

  private varintRaw(v: number): void {
    if (v < 0) {
      // Two's complement over 64 bits, as protobuf encodes negative ints.
      let big = BigInt.asUintN(64, BigInt(Math.trunc(v)));
      while (big >= 0x80n) {
        this.small.push(Number(big & 0x7fn) | 0x80);
        this.size++;
        big >>= 7n;
      }
      this.small.push(Number(big));
      this.size++;
      return;
    }
    while (v >= 0x80) {
      this.small.push((v % 128) | 0x80);
      this.size++;
      v = Math.floor(v / 128);
    }
    this.small.push(v);
    this.size++;
  }

  private tag(num: number, wire: number): void {
    this.varintRaw(num * 8 + wire);
  }

  int(num: number, v: number | undefined | null): this {
    if (v === undefined || v === null) return this;
    this.tag(num, WIRE_VARINT);
    this.varintRaw(v);
    return this;
  }

  bool(num: number, v: boolean | undefined | null): this {
    if (v === undefined || v === null) return this;
    return this.int(num, v ? 1 : 0);
  }

  bytes(num: number, v: Uint8Array | undefined | null): this {
    if (v === undefined || v === null) return this;
    this.tag(num, WIRE_LEN);
    this.varintRaw(v.length);
    this.raw(v);
    return this;
  }

  string(num: number, v: string | undefined | null): this {
    if (v === undefined || v === null) return this;
    return this.bytes(num, encoder.encode(v));
  }

  message(num: number, w: Writer): this {
    return this.bytes(num, w.finish());
  }

  group(num: number, w: Writer): this {
    this.tag(num, WIRE_START_GROUP);
    this.raw(w.finish());
    this.tag(num, WIRE_END_GROUP);
    return this;
  }

  finish(): Uint8Array {
    this.flush();
    if (this.parts.length === 1) return this.parts[0];
    const out = new Uint8Array(this.size);
    let pos = 0;
    for (const p of this.parts) {
      out.set(p, pos);
      pos += p.length;
    }
    return out;
  }
}

// --- Messages ----------------------------------------------------------------

// ClientToServerMessage.Contents
export const Contents = { COMMIT: 1, GET_UPDATES: 2, CLEAR_SERVER_DATA: 5 } as const;

// SyncEnums.ErrorType
export const ErrorType = {
  SUCCESS: 0,
  THROTTLED: 3,
  TRANSIENT_ERROR: 8,
  DISABLED_BY_ADMIN: 10,
} as const;

// SyncEnums.GetUpdatesOrigin
export const Origin = { PERIODIC: 4, NEW_CLIENT: 9 } as const;

// CommitResponse.ResponseType
export const ResponseType = {
  SUCCESS: 1,
  CONFLICT: 2,
  INVALID_MESSAGE: 4,
  OVER_QUOTA: 5,
  TRANSIENT_ERROR: 6,
} as const;

export interface SyncEntity {
  idString?: string;
  parentIdString?: string;
  version?: number;
  mtime?: number;
  ctime?: number;
  name?: string;
  nonUniqueName?: string;
  serverDefinedUniqueTag?: string;
  deleted?: boolean;
  originatorCacheGuid?: string;
  originatorClientItemId?: string;
  specifics?: Uint8Array;
  folder?: boolean;
  clientTagHash?: string;
  uniquePosition?: Uint8Array;
}

export interface ProgressMarker {
  dataTypeId?: number;
  token?: Uint8Array;
}

export interface GetUpdatesMessage {
  fetchFolders?: boolean;
  fromProgressMarker: ProgressMarker[];
  origin?: number;
}

export interface CommitMessage {
  entries: SyncEntity[];
  cacheGuid?: string;
}

export interface ClientToServerMessage {
  messageContents?: number;
  commit?: CommitMessage;
  getUpdates?: GetUpdatesMessage;
}

export function decodeSyncEntity(buf: Uint8Array): SyncEntity {
  const e: SyncEntity = {};
  for (const f of fields(buf)) {
    switch (f.num) {
      case 1: e.idString = str(f); break;
      case 2: e.parentIdString = str(f); break;
      case 4: e.version = int(f); break;
      case 5: e.mtime = int(f); break;
      case 6: e.ctime = int(f); break;
      case 7: e.name = str(f); break;
      case 8: e.nonUniqueName = str(f); break;
      case 10: e.serverDefinedUniqueTag = str(f); break;
      case 18: e.deleted = bool(f); break;
      case 19: e.originatorCacheGuid = str(f); break;
      case 20: e.originatorClientItemId = str(f); break;
      case 21: e.specifics = len(f); break;
      case 22: e.folder = bool(f); break;
      case 23: e.clientTagHash = str(f); break;
      case 25: e.uniquePosition = len(f); break;
    }
  }
  return e;
}

export function encodeSyncEntity(e: SyncEntity): Writer {
  return new Writer()
    .string(1, e.idString)
    .string(2, e.parentIdString)
    .int(4, e.version)
    .int(5, e.mtime)
    .int(6, e.ctime)
    .string(7, e.name)
    .string(8, e.nonUniqueName)
    .string(10, e.serverDefinedUniqueTag)
    .bool(18, e.deleted)
    .string(19, e.originatorCacheGuid)
    .string(20, e.originatorClientItemId)
    .bytes(21, e.specifics)
    .bool(22, e.folder)
    .string(23, e.clientTagHash)
    .bytes(25, e.uniquePosition);
}

function decodeProgressMarker(buf: Uint8Array): ProgressMarker {
  const m: ProgressMarker = {};
  for (const f of fields(buf)) {
    if (f.num === 1) m.dataTypeId = int(f);
    else if (f.num === 2) m.token = len(f);
  }
  return m;
}

function decodeGetUpdates(buf: Uint8Array): GetUpdatesMessage {
  const m: GetUpdatesMessage = { fromProgressMarker: [] };
  for (const f of fields(buf)) {
    if (f.num === 3) m.fetchFolders = bool(f);
    else if (f.num === 6) m.fromProgressMarker.push(decodeProgressMarker(len(f)));
    else if (f.num === 9) m.origin = int(f);
  }
  return m;
}

function decodeCommit(buf: Uint8Array): CommitMessage {
  const m: CommitMessage = { entries: [] };
  for (const f of fields(buf)) {
    if (f.num === 1) m.entries.push(decodeSyncEntity(len(f)));
    else if (f.num === 2) m.cacheGuid = str(f);
  }
  return m;
}

export function decodeClientToServerMessage(buf: Uint8Array): ClientToServerMessage {
  const m: ClientToServerMessage = {};
  for (const f of fields(buf)) {
    if (f.num === 3) m.messageContents = int(f);
    else if (f.num === 4) m.commit = decodeCommit(len(f));
    else if (f.num === 5) m.getUpdates = decodeGetUpdates(len(f));
  }
  return m;
}

export interface EntryResponse {
  responseType?: number;
  idString?: string;
  version?: number;
  errorMessage?: string;
  mtime?: number;
}

export interface ClientToServerResponse {
  commit?: { entryResponse: EntryResponse[] };
  getUpdates?: {
    entries: SyncEntity[];
    changesRemaining?: number;
    newProgressMarker: ProgressMarker[];
    encryptionKeys: Uint8Array[];
  };
  clearServerData?: boolean;
  errorCode?: number;
  errorMessage?: string;
  storeBirthday?: string;
  clientCommand?: { setSyncPollInterval: number; maxCommitBatchSize: number };
}

export function encodeClientToServerResponse(r: ClientToServerResponse): Uint8Array {
  const w = new Writer();
  if (r.commit) {
    const c = new Writer();
    for (const e of r.commit.entryResponse) {
      c.group(1, new Writer()
        .int(2, e.responseType)
        .string(3, e.idString)
        .int(6, e.version)
        .string(9, e.errorMessage)
        .int(10, e.mtime));
    }
    w.message(1, c);
  }
  if (r.getUpdates) {
    const g = new Writer();
    for (const e of r.getUpdates.entries) g.message(1, encodeSyncEntity(e));
    g.int(4, r.getUpdates.changesRemaining);
    for (const m of r.getUpdates.newProgressMarker) {
      g.message(5, new Writer().int(1, m.dataTypeId).bytes(2, m.token));
    }
    for (const k of r.getUpdates.encryptionKeys) g.bytes(6, k);
    w.message(2, g);
  }
  w.int(4, r.errorCode);
  w.string(5, r.errorMessage);
  w.string(6, r.storeBirthday);
  if (r.clientCommand) {
    w.message(7, new Writer()
      .int(1, r.clientCommand.setSyncPollInterval)
      .int(3, r.clientCommand.maxCommitBatchSize));
  }
  if (r.clearServerData) w.message(15, new Writer());
  return w.finish();
}

// dataTypeOfSpecifics is the data type of an EntitySpecifics: the field
// number of its type field (32904 for bookmarks), the first
// length-delimited field that isn't `encrypted` (1).
export function dataTypeOfSpecifics(specifics: Uint8Array): number {
  for (const f of fields(specifics)) {
    if (f.wire === WIRE_LEN && f.num !== 1) return f.num;
  }
  throw new ProtoError("specifics have no data type field");
}

// emptySpecifics is an EntitySpecifics holding an empty message of a type.
export function emptySpecifics(dataType: number): Uint8Array {
  return new Writer().bytes(dataType, EMPTY).finish();
}

// Progress tokens are Go's binary.PutVarint (zig-zag) of the last mtime
// returned, in a 10-byte buffer, as go-sync makes them.
export function encodeToken(mtime: number): Uint8Array {
  const out = new Uint8Array(10);
  let u = BigInt(mtime) >= 0n ? BigInt(mtime) * 2n : -BigInt(mtime) * 2n - 1n;
  let i = 0;
  while (u >= 0x80n) {
    out[i++] = Number(u & 0x7fn) | 0x80;
    u >>= 7n;
  }
  out[i] = Number(u);
  return out;
}

export function decodeToken(token: Uint8Array): number {
  let u = 0n;
  let shift = 0n;
  for (let i = 0; i < token.length && i < 10; i++) {
    const b = BigInt(token[i]);
    u |= (b & 0x7fn) << shift;
    if (b < 0x80n) {
      const v = u & 1n ? -(u >> 1n) - 1n : u >> 1n;
      return Number(v);
    }
    shift += 7n;
  }
  throw new ProtoError("bad progress token");
}
