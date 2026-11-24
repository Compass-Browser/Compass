# Compass sync server on Cloudflare (free)

The Compass sync server as a Cloudflare Worker with Durable Objects, so it
runs on a free Cloudflare account with nothing to maintain. It speaks the
same HTTP API as the Go server in `server/` (docs/design/sync.md, "Server"),
so Compass needs no changes to use it.

## How it works

- **One Durable Object per sync chain** (`src/chain.ts`) holds that chain's
  data in its own SQLite database. It is a port of the Go server's sync
  path (`command/`, `datastore/sqlite.go`) with the same rules: unique
  increasing mtimes as versions and progress tokens, soft deletes, client
  and server tags, create over a tombstone, the entity, byte and history
  quotas, the device limit, removed devices, and reset. An hourly alarm
  drops expired history and old tombstones, and clears a chain idle for
  `COMPASS_SYNC_IDLE_CHAIN_DAYS`.
- **Change notifications** (`GET /v2/compass/events`) stay Server-Sent
  Events for the client. Internally, the chain's object announces changes
  on a *hibernating* WebSocket, which costs nothing while the chain is
  quiet; the Worker holds the client's stream and writes the keep-alives
  (`src/index.ts`, `events`). An object that held the streams itself would
  stay awake, and billed, for as long as any device was connected.
- **Pairing** (`src/pairing.ts`) is one object for the server, with slots in
  memory as in Go.
- **Access** (`src/access.ts`): who may use the server (below).
- **Rooms** (shared spaces, folders and easels; sync.md, "Rooms"): one
  object per room (`src/room.ts`) with its members, records, envelopes and
  invites; the Worker's part is `src/rooms.ts`. Same routes, checks and
  answers as the Go server, except what needs company sign-in (the
  directory and email invites answer `404 directory_disabled`; members'
  `name` and `email` are `""`). A room's bytes count in its owner chain's
  quota. Each change reaches the member chains' objects, which send the
  `room` event on their change streams and keep an index of their rooms
  (`GET /v2/compass/rooms`) and the chain's identity key. A long poll
  (`GET .../changes?wait=`) is a hibernating WebSocket to the room's
  object, held by the Worker, so waiting costs no Durable Object time.
  The Worker keeps a chain's "removed device / disabled" answer for a
  minute. Clearing a chain (reset, idle expiry) deletes the rooms it owns
  and its other memberships, as on the Go server. No admin commands.
- The protobuf messages are read and written by a small codec
  (`src/proto.ts`); specifics stay the bytes the client sent.

- **Push** (`src/push.ts`) goes through the Compass push relay
  (`relay/`, below). Not ported: company sign-in (OIDC), Prometheus
  metrics, backups (Durable Objects keep their own point-in-time recovery
  for 30 days).

## Push

A server deployed into someone's own Cloudflare account can't hold
Compass's APNs key or Firebase service account, so it pushes through the
Compass push relay (`relay/README.md`), which the Compass developers run at
`https://push.compassbrowser.org`:

- `POST /v2/compass/push` (same body and checks as the Go server) has the
  relay seal the device's token into a handle bound to this server's host
  (`COMPASS_SYNC_PUBLIC_HOST`, or the host the request came to). The
  chain's object keeps the handle and a hash of the token, never the token.
  If the relay can't be reached the answer is `503`, and the device
  registers again at its next launch.
- The rules are the Go server's: a new sent tab alerts the chain's other
  devices (iPhones and Android); a change to spaces, passwords or
  extensions has its other iPhones sync in the background, at most once
  per `COMPASS_SYNC_PUSH_BACKGROUND_SECONDS` (900) each, and not while
  their change stream is open. A wait for the interval is the chain's
  alarm.
- A handle the relay calls gone or not its own is dropped; so are a
  device's on `DELETE /v2/compass/push`, on removal, and on reset.

`COMPASS_SYNC_PUSH_RELAY` sets another relay; `off` turns push off
(registrations are accepted and nothing is kept, as before version 3).
`COMPASS_SYNC_APNS_TOPICS` is the Go server's (`it.cipi1965.compass`).

## Who may use it

`COMPASS_SYNC_ALLOWED_CHAINS` lists the server's owners (Compass sets it to
its own chain when it deploys the server). Empty, the server is open to
anyone who knows its address, as the Go server without the list.

Owners invite friends; a friend is added for good when they join with an
invite code:

| Route | Who | |
|---|---|---|
| `POST /v2/compass/access/invites` `{"name": "Sam"}` | owner | `201 {"code": "...", "expires_in": 604800}`, single use, 7 days |
| `POST /v2/compass/access/join` `{"code": "..."}` | any valid token | `204` (also when already allowed), `404` unknown code, `410` used or expired |
| `GET /v2/compass/access` | owner | `{"owners": [...], "members": [{"chain", "name", "added_at", "linked"}], "invites": [{"code", "name", "expires_in"}]}` |
| `DELETE /v2/compass/access/{chain or code}` | owner | `204`: removes a friend (and their linked profiles) or revokes an invite |

Profile accounts pass with the chain they're linked to
(`POST /v2/compass/account/link`, as on the Go server). Other chains get
`401`. A removed friend is refused within a minute (the Worker caches
answers). `BLOCKED_CLIENT_IDS` refuses chains outright. Rooms follow the
Go server's private-server gate: a chain not on the list may use the rooms
it's a member of, or holds an invite to, and nothing else (`403
chain_not_allowed`); creating a room needs the list.

## Deploying by hand

```sh
cd server/cloudflare
npm install
npx wrangler login
npx wrangler deploy --var COMPASS_SYNC_ALLOWED_CHAINS:<your chain ID>
```

Your chain ID is on `chrome://compass-sync`. The server URL for Compass is
`https://compass-sync.<your subdomain>.workers.dev/v2`.

Every variable in the Go server's README table that applies here has the
same name and default: `COMPASS_SYNC_MAX_ENTITIES`, `_MAX_BYTES`,
`_MAX_HISTORY`, `_HISTORY_DAYS`, `_TOMBSTONE_DAYS`, `_IDLE_CHAIN_DAYS`,
`_POLL_SECONDS`, `_PUBLIC_HOST`, `_APNS_TOPICS`, `HIGH_DEVICE_LIMIT_CLIENT_IDS`,
`BLOCKED_CLIENT_IDS`; and its own `COMPASS_SYNC_PUSH_RELAY` and
`_PUSH_BACKGROUND_SECONDS` (Push, above).

## Versions and updates

`src/version.ts` has the server's version (`SERVER_VERSION`), reported in
`GET /v2/compass/info` as `"server": {"kind": "cloudflare", "version": N}`.
Raise it with any change existing servers should get, then `npm run
bundle` (it writes `dist/compass-sync.version` next to the bundle). Compass
offers the owner of an older server an update on its sync page, which
uploads the new bundle keeping the server's variables (`keep_bindings`) and
adding the Durable Object classes it lacks (docs/design/sync.md, "Updating
a server"). A migration needs a new tag in `wrangler.jsonc` and in
`components/sync/cloudflare_deployer.cc`, from every older tag.

## Free plan

A person's devices should fit the free plan's daily allowance (from
memory, not checked against Cloudflare's pricing page: 100k requests,
100k SQLite rows written, 5M rows read, 13k GB-s of Durable Object time;
check the current limits). What uses it:

- each sync request is one Worker and one Durable Object request;
- each stored entity writes a few rows (entity, indexes, tag, counts);
  history and open tabs commit most;
- open change streams cost Worker requests only when they connect, and
  no Durable Object time while quiet.
- a room's long poll is one Worker request and two room-object requests
  per poll (the WebSocket, then the answer; one when it times out empty),
  with no Durable Object time while it waits; each room change is a call
  to every member chain's object.

A free Worker has 10 ms of CPU per request; a long-lived change stream can
reach it after many hours and is then closed, and the device reconnects at
once and refreshes. Sync requests do their work in the chain's object,
which has a higher limit.

## Testing

```sh
npx wrangler dev --port 8797
# The Go server's end-to-end tests that take a URL:
(cd .. && COMPASS_SYNC_E2E_URL=http://127.0.0.1:8797 \
  go test ./server/ -run 'TestEndToEnd(TwoDevices|CreateOverTombstone|Auth|Pairing)$')
# The rooms tests that can run against a URL (the others skip; one uses a
# fixed room ID, so start from a fresh --persist-to directory):
(cd .. && COMPASS_SYNC_E2E_URL=http://127.0.0.1:8797 \
  go test ./server/ -run 'TestEndToEndRooms|TestRooms' -v)
# This server's own (change stream, quota, device removal, reset; rooms:
# events, long polls, resets, successors, quota, the private gate):
COMPASS_CF_URL=http://127.0.0.1:8797 node --test test/worker.test.ts
# Push, against a mock relay (starts its own wrangler dev):
node --test test/push.test.ts
# The relay itself (relay/README.md):
npm run test:relay
# The access list, against a server whose owner is a test seed's chain:
#   wrangler dev --var COMPASS_SYNC_ALLOWED_CHAINS:<that chain ID>
COMPASS_CF_URL=... COMPASS_CF_OWNER_SEED=<64 hex> npm test
```
