// One Durable Object per device token (named by its hash): how many pushes
// it got in the last hour, and whether Apple or Google said it's gone. It
// holds no token, and forgets the device two hours after its last push
// (thirty days once gone, so a gone token isn't sent to again).

import { DurableObject } from "cloudflare:workers";
import type { Env, PushType } from "./env";

// Per device and hour. A server sends a background push at most every 15
// minutes (server/push/push.go, DefaultBackgroundInterval).
export const LIMITS: Record<PushType, number> = { "send-tab": 30, sync: 6 };
const HOUR = 3600_000;
const FORGET = 2 * HOUR;
const FORGET_GONE = 30 * 24 * HOUR;

export type Admit = "ok" | "limited" | "gone";

export class Device extends DurableObject<Env> {
  // admit counts a push of |type| if the device may have it.
  async admit(type: PushType): Promise<Admit> {
    const kv = this.ctx.storage.kv;
    if (kv.get<boolean>("gone")) return "gone";
    const now = Date.now();
    const sent = (kv.get<number[]>(type) ?? []).filter((t) => now - t < HOUR);
    if (sent.length >= LIMITS[type]) return "limited";
    sent.push(now);
    kv.put(type, sent);
    await this.ctx.storage.setAlarm(now + FORGET);
    return "ok";
  }

  async gone(): Promise<void> {
    this.ctx.storage.kv.put("gone", true);
    await this.ctx.storage.setAlarm(Date.now() + FORGET_GONE);
  }

  async alarm(): Promise<void> {
    await this.ctx.storage.deleteAll();
  }
}

export async function deviceName(platform: string, token: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${platform}:${token}`));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
