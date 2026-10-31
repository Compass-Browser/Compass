# Notice

This directory is a fork of **brave/go-sync**
(<https://github.com/brave/go-sync>), imported at commit
`31ad41afd4d33e3fd7434b4cc54c17c2f2f6f9c0` ("fix(server): remove spoofable
chiware.RealIP middleware", #454). The import is its own commit in the
Compass history ("Vendor brave/go-sync 31ad41a into server/"), so `git diff`
from it shows every Compass change.

go-sync is licensed under the Mozilla Public License 2.0 (`LICENSE` in this
directory). Files that came from go-sync, modified or not, stay under the
MPL-2.0; files Compass added here are under the MPL-2.0 as well, so the
whole directory has one license. The Go module path is still
`github.com/brave/go-sync` to keep the diff against upstream small.

What Compass changed, in short:

- Storage: DynamoDB replaced by one SQLite file (`datastore/sqlite.go`,
  pure-Go driver `modernc.org/sqlite`), Redis by an in-process cache
  (`cache/memory.go`); the AWS SDK replaced by `ptr/`.
- Strictly increasing entity mtimes (`datastore/mtime.go`) and GetUpdates
  pages that never split equal mtimes.
- Entities of data types newer than the bundled protos are stored as they
  arrive instead of failing (`datastore.DataTypeOfSpecifics`).
- A create whose client tag belongs to a deleted entity revives that
  entity under its server ID, as Chromium's servers do (go-sync stored a
  second entity with a new ID, or answered CONFLICT if the tag item was
  left over); an update that undeletes an entity takes its tag back.
- A byte quota per chain and a configurable entity quota.
- The pairing relay (`pairing/`) and a client IP resolver that trusts
  `X-Forwarded-For` only from configured proxies (`clientip/`).
- Configuration by `COMPASS_SYNC_*` environment variables, `/healthz`,
  `healthcheck` and `backup` subcommands, a distroless image and a
  docker-compose file with Caddy.
