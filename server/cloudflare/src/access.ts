// Who may use the server: one Durable Object for the whole server.
//
// The owners are COMPASS_SYNC_ALLOWED_CHAINS (Compass sets it to its own
// chain when it deploys the server). With no owner the server is open, as
// the Go server without the list. Owners invite friends with single-use
// codes; a friend who joins with one is a member. A chain's profile
// accounts are linked to it (POST /v2/compass/account/link, as on the Go
// server) and pass with it. Blocked chains (BLOCKED_CLIENT_IDS) are refused
// by the Worker before it asks here.

import { DurableObject } from "cloudflare:workers";
import { idList } from "./auth";
import { type Env, fail, json } from "./config";

const MAX_LINKS_PER_CHAIN = 64;
const INVITE_TTL_MS = 7 * 86400_000;
const MAX_OPEN_INVITES = 50;
const MAX_MEMBERS = 500;

const SCHEMA = `
CREATE TABLE IF NOT EXISTS members (
  chain      TEXT PRIMARY KEY,
  added_at   INTEGER NOT NULL,
  invited_by TEXT NOT NULL,
  name       TEXT NOT NULL DEFAULT ''
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS links (
  chain   TEXT PRIMARY KEY,
  root    TEXT NOT NULL,
  created INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS links_by_root ON links (root);
CREATE TABLE IF NOT EXISTS invites (
  code       TEXT PRIMARY KEY,
  created_by TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  name       TEXT NOT NULL DEFAULT '',
  used_by    TEXT,
  used_at    INTEGER
) WITHOUT ROWID;
`;

export interface AccessState {
  allowed: boolean;
  owner: boolean;
}

function randomCode(): string {
  // 128 bits, in Crockford-free lower-case base32 for links.
  const alphabet = "abcdefghijklmnopqrstuvwxyz234567";
  const bytes = crypto.getRandomValues(new Uint8Array(26));
  return Array.from(bytes, (b) => alphabet[b & 31]).join("");
}

export class Access extends DurableObject<Env> {
  private sql: SqlStorage;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(SCHEMA);
  }

  private owners(): string[] {
    return idList(this.env.COMPASS_SYNC_ALLOWED_CHAINS);
  }

  private root(chain: string): string {
    const r = this.sql.exec(`SELECT root FROM links WHERE chain = ?`, chain).toArray()[0];
    return r ? (r.root as string) : "";
  }

  private listed(chain: string): { allowed: boolean; owner: boolean } {
    const owners = this.owners();
    if (owners.length === 0) return { allowed: true, owner: false };
    if (owners.includes(chain)) return { allowed: true, owner: true };
    const member = this.sql.exec(`SELECT 1 FROM members WHERE chain = ?`, chain).toArray().length > 0;
    return { allowed: member, owner: false };
  }

  // state says whether a chain may sync, directly or through the chain it
  // is linked to, and whether it owns the server.
  state(chainID: string): AccessState {
    const chain = chainID.toLowerCase();
    const own = this.listed(chain);
    if (own.allowed) return own;
    const root = this.root(chain);
    if (!root) return { allowed: false, owner: false };
    const blocked = idList(this.env.BLOCKED_CLIENT_IDS);
    if (blocked.includes(root)) return { allowed: false, owner: false };
    return this.listed(root);
  }

  // link links `other` (a profile account) to `from`'s root (Go:
  // LinkChainToRoot). `from` must be allowed (the Worker checked).
  link(fromID: string, otherID: string): Response {
    const from = fromID.toLowerCase();
    const other = otherID.toLowerCase();
    const root = this.root(from) || from;
    if (other === root || other === from) return new Response(null, { status: 204 });
    if (this.root(other) === root) return new Response(null, { status: 204 });
    const n = Number(this.sql.exec(`SELECT COUNT(*) AS n FROM links WHERE root = ?`, root).one().n);
    if (n >= MAX_LINKS_PER_CHAIN) return fail(429, "too many linked chains");
    this.sql.exec(
      `INSERT INTO links (chain, root, created) VALUES (?, ?, ?)
       ON CONFLICT (chain) DO UPDATE SET root = excluded.root, created = excluded.created`,
      other, root, Date.now(),
    );
    return new Response(null, { status: 204 });
  }

  // createInvite makes a single-use invite code (owners only).
  createInvite(byID: string, name: string, origin: string): Response {
    const by = byID.toLowerCase();
    if (!this.state(by).owner) return fail(403, "owner_only");
    const now = Date.now();
    this.sql.exec(`DELETE FROM invites WHERE expires_at < ? OR used_by IS NOT NULL`, now);
    const open = Number(this.sql.exec(`SELECT COUNT(*) AS n FROM invites`).one().n);
    if (open >= MAX_OPEN_INVITES) return fail(429, "too many open invites");
    const code = randomCode();
    this.sql.exec(
      `INSERT INTO invites (code, created_by, created_at, expires_at, name) VALUES (?, ?, ?, ?, ?)`,
      code, by, now, now + INVITE_TTL_MS, name.slice(0, 100),
    );
    // The link a friend pastes into Compass, or opens (invitePage()); the
    // code is in the fragment, so it never reaches the server's logs.
    return json(201, { code, link: `${origin}/invite#${code}`, expires_in: Math.floor(INVITE_TTL_MS / 1000) });
  }

  // join adds the caller with an invite code.
  join(chainID: string, code: string): Response {
    const chain = chainID.toLowerCase();
    if (this.state(chain).allowed) return new Response(null, { status: 204 });
    const now = Date.now();
    const inv = this.sql.exec(`SELECT * FROM invites WHERE code = ?`, code).toArray()[0];
    if (!inv) return fail(404, "no_such_invite");
    if (inv.used_by !== null || Number(inv.expires_at) < now) return fail(410, "invite_used_or_expired");
    if (Number(this.sql.exec(`SELECT COUNT(*) AS n FROM members`).one().n) >= MAX_MEMBERS) {
      return fail(429, "too many members");
    }
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`UPDATE invites SET used_by = ?, used_at = ? WHERE code = ?`, chain, now, code);
      this.sql.exec(
        `INSERT INTO members (chain, added_at, invited_by, name) VALUES (?, ?, ?, ?)
         ON CONFLICT (chain) DO NOTHING`,
        chain, now, inv.created_by as string, inv.name as string,
      );
    });
    return new Response(null, { status: 204 });
  }

  // members lists the friends and open invites (owners only).
  members(byID: string): Response {
    if (!this.state(byID).owner) return fail(403, "owner_only");
    const now = Date.now();
    const members = this.sql
      .exec(`SELECT chain, added_at, name FROM members ORDER BY added_at`)
      .toArray()
      .map((r) => ({
        chain: r.chain,
        name: r.name,
        added_at: Math.floor(Number(r.added_at) / 1000),
        linked: Number(this.sql.exec(`SELECT COUNT(*) AS n FROM links WHERE root = ?`, r.chain).one().n),
      }));
    const invites = this.sql
      .exec(`SELECT code, name, expires_at FROM invites WHERE used_by IS NULL AND expires_at >= ? ORDER BY created_at`, now)
      .toArray()
      .map((r) => ({ code: r.code, name: r.name, expires_in: Math.floor((Number(r.expires_at) - now) / 1000) }));
    return json(200, { owners: this.owners(), members, invites });
  }

  // remove takes a friend off the list (their linked profiles go with
  // them), or revokes an open invite.
  remove(byID: string, target: string): Response {
    if (!this.state(byID).owner) return fail(403, "owner_only");
    const t = target.toLowerCase();
    this.ctx.storage.transactionSync(() => {
      this.sql.exec(`DELETE FROM members WHERE chain = ?`, t);
      this.sql.exec(`DELETE FROM links WHERE root = ?`, t);
      this.sql.exec(`DELETE FROM invites WHERE code = ? AND used_by IS NULL`, target);
    });
    return new Response(null, { status: 204 });
  }
}
