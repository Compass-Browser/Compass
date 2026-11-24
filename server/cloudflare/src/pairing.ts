// The pairing relay (docs/design/sync.md, "Pairing API"), a port of
// server/pairing: one Durable Object for the server, slots in memory only
// (they live 5 minutes; losing them on a restart is as on the Go server).
//
// The Worker passes, after checking them: X-Chain-ID, the caller's chain
// when it sent a valid token of an allowed chain (empty otherwise),
// X-Client-IP and X-Host (the Host the request came to).

import { DurableObject } from "cloudflare:workers";
import { type Env, fail, json } from "./config";

const SLOT_TTL_MS = 300_000;
const POLL_TIMEOUT_MS = 25_000;
const MAX_MESSAGES_PER_SIDE = 8;
const MAX_MESSAGE_BYTES = 4096;
const JOINS_PER_IP = 10;
const OPENS_PER_IP = 30;
const WINDOW_MS = 600_000;
const MAX_OPEN_SLOTS = 10000;
const CLOSED_TTL_MS = 300_000;
const PAIR_TOKEN_HEADER = "X-Compass-Pair-Token";

type Reason = "mismatch" | "cancel" | "deny" | "expired";

interface Slot {
  num: number;
  chain: string;
  server: string;
  expires: number;
  bToken: string;
  msgs: [string[], string[]]; // Base64 data by sender: a, b.
  waiters: Set<() => void>;
}

export class Pairing extends DurableObject<Env> {
  private slots = new Map<number, Slot>();
  private closed = new Map<number, { reason: Reason; until: number }>();
  private byChain = new Map<string, number>();
  private joins = new Map<string, number[]>();
  private opens = new Map<string, number[]>();
  private lastSweep = 0;

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean); // after /pair
    const m = request.method;
    if (parts.length === 0 && m === "POST") return this.open(request);
    if (parts.length === 0) return fail(405, "method not allowed");
    const num = /^\d+$/.test(parts[0]) ? Number(parts[0]) : 0;
    if (parts.length === 2 && parts[1] === "join" && m === "POST") return this.join(request, num);
    if (parts.length === 2 && parts[1] === "msg" && m === "POST") return this.post(request, num);
    if (parts.length === 2 && parts[1] === "msg" && m === "GET") return this.poll(request, url, num);
    if (parts.length === 1 && m === "DELETE") return this.close(request, url, num);
    return fail(404, "not found");
  }

  private closeSlot(sl: Slot, reason: Reason, at: number): void {
    this.slots.delete(sl.num);
    this.closed.set(sl.num, { reason, until: at + CLOSED_TTL_MS });
    if (this.byChain.get(sl.chain) === sl.num) this.byChain.delete(sl.chain);
    for (const w of sl.waiters) w();
    sl.waiters.clear();
  }

  private lookup(num: number, now: number): { sl?: Slot; reason?: Reason } {
    let sl = this.slots.get(num);
    if (sl && now >= sl.expires) {
      this.closeSlot(sl, "expired", sl.expires);
      sl = undefined;
    }
    if (sl) return { sl };
    const c = this.closed.get(num);
    if (!c) return {};
    if (now >= c.until) {
      this.closed.delete(num);
      return {};
    }
    return { reason: c.reason };
  }

  private sweep(now: number): void {
    if (now - this.lastSweep < 60_000) return;
    this.lastSweep = now;
    for (const sl of [...this.slots.values()]) if (now >= sl.expires) this.closeSlot(sl, "expired", sl.expires);
    for (const [n, c] of this.closed) if (now >= c.until) this.closed.delete(n);
    for (const m of [this.joins, this.opens]) {
      for (const [ip, times] of m) {
        const kept = times.filter((t) => t > now - WINDOW_MS);
        if (kept.length) m.set(ip, kept);
        else m.delete(ip);
      }
    }
  }

  private allow(m: Map<string, number[]>, ip: string, limit: number, now: number): boolean {
    const times = (m.get(ip) ?? []).filter((t) => t > now - WINDOW_MS);
    if (times.length >= limit) {
      m.set(ip, times);
      return false;
    }
    times.push(now);
    m.set(ip, times);
    return true;
  }

  private canonicalHost(request: Request): string {
    let h = this.env.COMPASS_SYNC_PUBLIC_HOST || request.headers.get("X-Host") || "";
    if (h.startsWith("[")) h = h.slice(0, h.indexOf("]") + 1);
    else if (h.split(":").length === 2) h = h.split(":")[0];
    return h.toLowerCase();
  }

  private open(request: Request): Response {
    const chain = (request.headers.get("X-Chain-ID") ?? "").toLowerCase();
    if (!chain) return fail(401, "invalid sync token");
    const ip = request.headers.get("X-Client-IP") ?? "";
    const now = Date.now();
    this.sweep(now);
    if (!this.allow(this.opens, ip, OPENS_PER_IP, now)) return fail(429, "too many pairing slots opened");
    const oldNum = this.byChain.get(chain);
    const old = oldNum === undefined ? undefined : this.slots.get(oldNum);
    if (old && old.chain === chain) {
      if (now >= old.expires) this.closeSlot(old, "expired", old.expires);
      else this.closeSlot(old, "cancel", now);
    }
    if (this.slots.size >= MAX_OPEN_SLOTS) return fail(503, "too many open pairing slots");
    let num = 1;
    for (;;) {
      const { sl, reason } = this.lookup(num, now);
      if (!sl && !reason) break;
      num++;
    }
    const sl: Slot = {
      num,
      chain,
      server: this.canonicalHost(request),
      expires: now + SLOT_TTL_MS,
      bToken: "",
      msgs: [[], []],
      waiters: new Set(),
    };
    this.slots.set(num, sl);
    this.byChain.set(chain, num);
    return json(201, { slot: num, expires_in: SLOT_TTL_MS / 1000, server: sl.server });
  }

  private gone(reason?: Reason): Response {
    if (reason) return json(410, { error: "pairing slot closed", reason });
    return fail(404, "no such pairing slot");
  }

  private join(request: Request, num: number): Response {
    const ip = request.headers.get("X-Client-IP") ?? "";
    const now = Date.now();
    this.sweep(now);
    if (!this.allow(this.joins, ip, JOINS_PER_IP, now)) return fail(429, "too many join attempts");
    const { sl, reason } = num >= 1 ? this.lookup(num, now) : {};
    if (!sl) return this.gone(reason);
    if (sl.bToken) return fail(409, "pairing slot already joined");
    const b = crypto.getRandomValues(new Uint8Array(16));
    sl.bToken = Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
    return json(200, {
      b_token: sl.bToken,
      server: sl.server,
      expires_in: Math.ceil((sl.expires - now) / 1000),
    });
  }

  // authorize: 0 if the caller may act as `side` of the slot, else a status.
  private authorize(sl: Slot, side: 0 | 1, request: Request): number {
    if (side === 0) {
      const chain = (request.headers.get("X-Chain-ID") ?? "").toLowerCase();
      if (!chain) return 401;
      return chain === sl.chain ? 0 : 403;
    }
    const t = request.headers.get(PAIR_TOKEN_HEADER) ?? "";
    if (!t) return 401;
    if (!sl.bToken || !timingSafeEqual(t, sl.bToken)) return 403;
    return 0;
  }

  private async post(request: Request, num: number): Promise<Response> {
    if (num < 1) return fail(404, "no such pairing slot");
    const limit = Math.ceil(MAX_MESSAGE_BYTES / 3) * 4 + 1024;
    const raw = await request.text();
    if (raw.length > limit) return fail(413, "message too large");
    let body: { from?: unknown; data?: unknown };
    try {
      body = JSON.parse(raw);
    } catch {
      return fail(400, "body must be JSON {from, data}");
    }
    const sideN = body.from === "a" ? 0 : body.from === "b" ? 1 : -1;
    if (sideN < 0) return fail(400, `"from" must be "a" or "b"`);
    const side = sideN as 0 | 1;
    let data: Uint8Array;
    try {
      if (typeof body.data !== "string") throw new Error();
      data = Uint8Array.from(atob(body.data), (c) => c.charCodeAt(0));
    } catch {
      return fail(400, `"data" must be non-empty standard base64`);
    }
    if (data.length === 0) return fail(400, `"data" must be non-empty standard base64`);
    if (data.length > MAX_MESSAGE_BYTES) return fail(413, "message too large");
    const { sl, reason } = this.lookup(num, Date.now());
    if (!sl) return this.gone(reason);
    const status = this.authorize(sl, side, request);
    if (status) return fail(status, `not allowed to post as ${body.from}`);
    if (sl.msgs[side].length >= MAX_MESSAGES_PER_SIDE) return fail(429, "too many messages");
    sl.msgs[side].push(btoa(String.fromCharCode(...data)));
    for (const w of sl.waiters) w();
    sl.waiters.clear();
    return json(200, { seq: sl.msgs[side].length });
  }

  private async poll(request: Request, url: URL, num: number): Promise<Response> {
    if (num < 1) return fail(404, "no such pairing slot");
    const forSide = url.searchParams.get("for");
    const reader = forSide === "a" ? 0 : forSide === "b" ? 1 : -1;
    if (reader < 0) return fail(400, `"for" must be "a" or "b"`);
    let after = 0;
    const a = url.searchParams.get("after");
    if (a !== null && a !== "") {
      if (!/^\d+$/.test(a)) return fail(400, `"after" must be a sequence number`);
      after = Number(a);
    }
    const sender = reader === 1 ? 0 : 1;
    const deadline = Date.now() + POLL_TIMEOUT_MS;
    for (;;) {
      const now = Date.now();
      const { sl, reason } = this.lookup(num, now);
      if (!sl) return this.gone(reason);
      const status = this.authorize(sl, reader as 0 | 1, request);
      if (status) return fail(status, "not allowed to read");
      const msgs = sl.msgs[sender].slice(after).map((data, i) => ({ seq: after + i + 1, data }));
      if (msgs.length > 0) return json(200, { messages: msgs });
      if (now >= deadline) return json(200, { messages: [] });
      const wait = Math.min(deadline - now, sl.expires - now);
      await new Promise<void>((resolve) => {
        const done = () => {
          clearTimeout(timer);
          sl.waiters.delete(done);
          resolve();
        };
        const timer = setTimeout(done, Math.max(wait, 0));
        sl.waiters.add(done);
        request.signal?.addEventListener?.("abort", done);
      });
      if (request.signal?.aborted) return json(200, { messages: [] });
    }
  }

  private close(request: Request, url: URL, num: number): Response {
    if (num < 1) return fail(404, "no such pairing slot");
    let why = url.searchParams.get("reason") ?? "";
    if (why === "") why = "cancel";
    if (why !== "mismatch" && why !== "cancel" && why !== "deny") {
      return fail(400, `"reason" must be "mismatch", "cancel" or "deny"`);
    }
    const now = Date.now();
    const { sl, reason } = this.lookup(num, now);
    if (!sl) return this.gone(reason);
    const sa = this.authorize(sl, 0, request);
    const sb = this.authorize(sl, 1, request);
    if (sa && sb) return fail(Math.max(sa, sb), "not allowed to close");
    this.closeSlot(sl, why as Reason, now);
    return new Response(null, { status: 204 });
  }
}

function timingSafeEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}
