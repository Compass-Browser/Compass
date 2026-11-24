// Configuration: Worker variables with the Go server's names and defaults
// (server/README.md, "Configuration").

import type { Access } from "./access";
import type { Chain } from "./chain";
import type { Pairing } from "./pairing";
import type { Room } from "./room";

export interface Env {
  CHAIN: DurableObjectNamespace<Chain>;
  PAIRING: DurableObjectNamespace<Pairing>;
  ACCESS: DurableObjectNamespace<Access>;
  ROOM: DurableObjectNamespace<Room>;
  COMPASS_SYNC_ALLOWED_CHAINS?: string;
  BLOCKED_CLIENT_IDS?: string;
  COMPASS_SYNC_MAX_ENTITIES?: string;
  COMPASS_SYNC_MAX_BYTES?: string;
  COMPASS_SYNC_MAX_HISTORY?: string;
  COMPASS_SYNC_HISTORY_DAYS?: string;
  COMPASS_SYNC_TOMBSTONE_DAYS?: string;
  COMPASS_SYNC_IDLE_CHAIN_DAYS?: string;
  COMPASS_SYNC_POLL_SECONDS?: string;
  COMPASS_SYNC_PUBLIC_HOST?: string;
  HIGH_DEVICE_LIMIT_CLIENT_IDS?: string;
  COMPASS_SYNC_PUSH_RELAY?: string;
  COMPASS_SYNC_APNS_TOPICS?: string;
  COMPASS_SYNC_PUSH_BACKGROUND_SECONDS?: string;
}

// The Compass push relay (relay/README.md), used unless
// COMPASS_SYNC_PUSH_RELAY says otherwise ("off" or "": no push).
export const DEFAULT_PUSH_RELAY = "https://push.compassbrowser.org";

export interface Config {
  maxEntities: number;
  maxBytes: number; // 0 = no limit
  maxHistory: number;
  historyDays: number;
  tombstoneDays: number; // 0 = keep
  idleChainDays: number; // 0 = keep
  pollSeconds: number;
  highDeviceLimit: Set<string>;
  pushRelay: string; // "" = no push
  pushTopics: string[];
  backgroundIntervalMs: number;
}

function num(v: string | undefined, def: number, ok: (n: number) => boolean): number {
  if (v === undefined || v.trim() === "") return def;
  const n = Number(v);
  return Number.isInteger(n) && ok(n) ? n : def;
}

export function loadConfig(env: Env): Config {
  const maxEntities = num(env.COMPASS_SYNC_MAX_ENTITIES, 100000, (n) => n > 0);
  return {
    maxEntities,
    maxBytes: num(env.COMPASS_SYNC_MAX_BYTES, 200 * 1024 * 1024, (n) => n >= 0),
    // The history quota can't be larger than the whole.
    maxHistory: Math.min(num(env.COMPASS_SYNC_MAX_HISTORY, 30000, (n) => n > 0), maxEntities),
    historyDays: num(env.COMPASS_SYNC_HISTORY_DAYS, 14, (n) => n >= 1 && n <= 30),
    tombstoneDays: num(env.COMPASS_SYNC_TOMBSTONE_DAYS, 30, (n) => n >= 0),
    idleChainDays: num(env.COMPASS_SYNC_IDLE_CHAIN_DAYS, 180, (n) => n >= 0),
    pollSeconds: num(env.COMPASS_SYNC_POLL_SECONDS, 900, (n) => n >= 30 && n <= 86400),
    highDeviceLimit: new Set(
      (env.HIGH_DEVICE_LIMIT_CLIENT_IDS ?? "").split(",").map((s) => s.trim().toLowerCase()).filter(Boolean),
    ),
    pushRelay: pushRelay(env.COMPASS_SYNC_PUSH_RELAY),
    backgroundIntervalMs: num(env.COMPASS_SYNC_PUSH_BACKGROUND_SECONDS, 900, (n) => n >= 1) * 1000,
    pushTopics: (env.COMPASS_SYNC_APNS_TOPICS || "it.cipi1965.compass").split(",").map((s) => s.trim()).filter(Boolean),
  };
}

function pushRelay(v: string | undefined): string {
  if (v === undefined) return DEFAULT_PUSH_RELAY;
  const t = v.trim().replace(/\/+$/, "");
  return t === "" || t === "off" || !/^https?:\/\//.test(t) ? "" : t;
}

export function json(status: number, body: unknown, headers: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body) + "\n", {
    status,
    headers: { "Content-Type": "application/json", "Cache-Control": "no-store", ...headers },
  });
}

export function fail(status: number, error: string, headers: Record<string, string> = {}): Response {
  return json(status, { error }, headers);
}

// text answers as Go's http.Error does.
export function text(status: number, body: string): Response {
  return new Response(body + "\n", {
    status,
    headers: { "Content-Type": "text/plain; charset=utf-8", "X-Content-Type-Options": "nosniff" },
  });
}

// deviceRemoved is the answer to a request from a removed device (sync.md,
// "Identity").
export function deviceRemoved(): Response {
  return fail(403, "device_removed", { "X-Compass-Device": "removed" });
}
