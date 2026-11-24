// The server's version, which Compass compares with the one it ships to
// offer the owner an update (docs/design/sync.md, "Updating a server").
// Raise it with every change Compass should push to existing servers;
// `npm run bundle` writes it next to the bundle (dist/compass-sync.version).
//
// 1: the first release (servers deployed before this file existed report
//    no version, and count as 1). 2: rooms.
export const SERVER_VERSION = 2;
