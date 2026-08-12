import {DurableObject} from "cloudflare:workers";
import {json, readJson} from "./security";
import {rejectUnsupportedInternalApi} from "./internal-api";
import type {Env} from "./types";

interface CodeEntry { roomId: string; expiresAt: number }
interface RateEntry { windowStartedAt: number; attempts: number }

const RATE_TTL_MS = 10 * 60_000;

function expiresAt(key: string, value: CodeEntry | RateEntry): number {
  return key.startsWith("rate:")
    ? (value as RateEntry).windowStartedAt + RATE_TTL_MS
    : (value as CodeEntry).expiresAt;
}

export class PartyCodeDirectory extends DurableObject<Env> {
  override async fetch(request: Request): Promise<Response> {
    const rejected = rejectUnsupportedInternalApi(request);
    if (rejected) return rejected;
    if (request.method !== "POST") return json({error: "method_not_allowed"}, 405);
    const url = new URL(request.url);
    const body = await readJson<Record<string, unknown>>(request);
    const codeDigest = typeof body.codeDigest === "string" ? body.codeDigest : "";
    if (!/^[A-Za-z0-9_-]{43}$/.test(codeDigest)) {
      return json({error: "invalid_code"}, 400);
    }
    const codeKey = `code:${codeDigest}`;
    if (url.pathname === "/register") {
      if (typeof body.roomId !== "string" ||
          !/^[A-Za-z0-9_-]{22}$/.test(body.roomId) ||
          !Number.isFinite(body.expiresAt)) return json({error: "invalid_room"}, 400);
      const existing = await this.ctx.storage.get<CodeEntry>(codeKey);
      if (existing && existing.expiresAt > Date.now() && existing.roomId !== body.roomId) {
        return json({error: "code_collision"}, 409);
      }
      await this.ctx.storage.put(codeKey, {roomId: body.roomId,
        expiresAt: Number(body.expiresAt)} satisfies CodeEntry);
      await this.scheduleCleanup();
      return json({ok: true});
    }
    if (url.pathname === "/resolve") {
      const requesterDigest = typeof body.requesterDigest === "string"
        ? body.requesterDigest : "";
      if (!/^[A-Za-z0-9_-]{43}$/.test(requesterDigest)) {
        return json({error: "invalid_requester"}, 400);
      }
      const rateKey = `rate:${requesterDigest}`;
      const now = Date.now();
      let rate = await this.ctx.storage.get<RateEntry>(rateKey) ||
        {windowStartedAt: now, attempts: 0};
      if (now - rate.windowStartedAt >= 10 * 60_000) {
        rate = {windowStartedAt: now, attempts: 0};
      }
      if (rate.attempts >= 12) return json({error: "rate_limited"}, 429);
      rate.attempts++;
      await this.ctx.storage.put(rateKey, rate);
      await this.scheduleCleanup();
      const entry = await this.ctx.storage.get<CodeEntry>(codeKey);
      if (!entry || entry.expiresAt <= Date.now()) {
        if (entry) await this.ctx.storage.delete(codeKey);
        return json({error: "invalid_code"}, 404);
      }
      return json({roomId: entry.roomId});
    }
    return json({error: "not_found"}, 404);
  }

  private async scheduleCleanup(): Promise<void> {
    const alarm = await this.ctx.storage.getAlarm();
    const desired = Date.now() + 60_000;
    if (alarm === null || alarm > desired) await this.ctx.storage.setAlarm(desired);
  }

  override async alarm(): Promise<void> {
    const now = Date.now();
    let next: number | null = null;
    const entries = await this.ctx.storage.list<CodeEntry | RateEntry>();
    const expired: string[] = [];
    for (const [key, value] of entries) {
      const expiry = expiresAt(key, value);
      if (expiry <= now) expired.push(key);
      else if (next === null || expiry < next) next = expiry;
    }
    if (expired.length) await this.ctx.storage.delete(expired);
    if (next !== null) await this.ctx.storage.setAlarm(next);
  }
}
