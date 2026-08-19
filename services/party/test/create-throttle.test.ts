import {env} from "cloudflare:workers";
import {SELF, runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import {INTERNAL_API_HEADER} from "../src/internal-api";
import {digest} from "../src/security";
import type {Env} from "../src/types";

const origin = "https://party.example.invalid";
const hostPublicKey = "H".repeat(87);
const compatibility = {protocolVersion: 1,
  buildId: Array.from({length: 16}, (_, index) => index + 1),
  gameplayDigest: Array.from({length: 32}, (_, index) => 128 + index),
  romRevision: 1, cadenceHz: 30};

const bindings = env as unknown as Env;

function budgetStub(day = new Date().toISOString().slice(0, 10)) {
  return bindings.PARTY_BUDGETS.get(bindings.PARTY_BUDGETS.idFromName(day));
}

function post(path: string, value: unknown, ip: string) {
  return SELF.fetch(`https://party.test${path}`, {
    method: "POST", headers: {origin, "content-type": "application/json",
      "cf-connecting-ip": ip},
    body: JSON.stringify(value),
  });
}

async function admittedPairingUnits(): Promise<number> {
  const response = await budgetStub().fetch("https://budget/status",
    {headers: {[INTERNAL_API_HEADER]: "1"}});
  const value = await response.json() as {admitted: {pairingUnits: number}};
  return value.admitted.pairingUnits;
}

/* I-2: the daily admission reserve is a cost cap shared by everyone, so one
 * distributed abuser could previously drain the whole day's ~1000 rooms and
 * deny pairing to every honest player until UTC midnight. Each connecting
 * address now also has its own bounded daily create allowance, enforced in
 * code rather than only by the hand-applied edge zone rule. */
describe("per-address daily create throttle", () => {
  it("refuses the 26th create from one address with a typed 429 that charges nothing",
      async () => {
    const source = await digest(bindings, "create-source", "192.0.2.61");
    await runInDurableObject(budgetStub(), async (_instance, state) => {
      await state.storage.put(`source:${source}`, 24);
    });
    /* The 25th create of the day is still admitted, through the real path,
     * proving the counter advances where admission happens. */
    const admitted = await post("/api/party/create", {hostPublicKey},
      "192.0.2.61");
    expect(admitted.status).toBe(201);

    const before = await admittedPairingUnits();
    const refusedParty = await post("/api/party/create", {hostPublicKey},
      "192.0.2.61");
    expect(refusedParty.status).toBe(429);
    expect(await refusedParty.json()).toEqual({error: "create_rate_limited"});

    // Party and match creation spend one shared allowance.
    const refusedMatch = await post("/api/match/create",
      {compatibility, seatCount: 1}, "192.0.2.61");
    expect(refusedMatch.status).toBe(429);
    expect(await refusedMatch.json()).toEqual({error: "create_rate_limited"});

    // The native originless bootstrap is the same create path.
    const refusedNative = await SELF.fetch(
      "https://party.test/api/party/native-create", {headers: {
        upgrade: "websocket", "cf-connecting-ip": "192.0.2.61",
        "sec-websocket-protocol":
          `gb-native-host-v1, gb-control-v1, gb-key.${hostPublicKey}`,
      }});
    expect(refusedNative.status).toBe(429);
    expect(await refusedNative.json()).toEqual({error: "create_rate_limited"});

    /* A throttled address charges the global daily reserve nothing, and its
     * own counter does not creep past the cap on refusals. */
    expect(await admittedPairingUnits()).toBe(before);
    const stored = await runInDurableObject(budgetStub(),
      async (_instance, state) => state.storage.get<number>(`source:${source}`));
    expect(stored).toBe(25);

    // A different address is unaffected.
    const other = await post("/api/party/create", {hostPublicKey},
      "192.0.2.62");
    expect(other.status).toBe(201);
  });

  it("resets the allowance at UTC midnight with the daily budget shard", async () => {
    const source = await digest(bindings, "create-source", "192.0.2.63");
    const today = budgetStub("2099-06-01");
    await runInDurableObject(today, async (_instance, state) => {
      await state.storage.put(`source:${source}`, 25);
    });
    const refused = await today.fetch(
      `https://budget/admit?kind=pairing&units=10&operation=partyCreate&source=${source}`,
      {method: "POST", headers: {[INTERNAL_API_HEADER]: "1"}});
    expect(refused.status).toBe(429);
    expect(await refused.json()).toEqual({allowed: false,
      error: "create_rate_limited"});
    /* The per-address counters live inside the day-named shard exactly like
     * the global reserve, so the next UTC day addresses a fresh object where
     * the same address is admitted again. */
    const tomorrow = budgetStub("2099-06-02");
    const admitted = await tomorrow.fetch(
      `https://budget/admit?kind=pairing&units=10&operation=partyCreate&source=${source}`,
      {method: "POST", headers: {[INTERNAL_API_HEADER]: "1"}});
    expect(admitted.status).toBe(200);
  });

  it("fails closed when a create admission arrives without a valid source", async () => {
    const stub = budgetStub("2099-06-03");
    for (const path of [
      // A create that lost its source identity must never bypass the cap.
      "/admit?kind=pairing&units=10&operation=partyCreate",
      "/admit?kind=pairing&units=10&operation=matchCreate",
      "/admit?kind=pairing&units=10&operation=partyCreate&source=short",
      // Non-create operations never carry one.
      `/admit?kind=pairing&units=2&operation=partyLinkJoin&source=${"A".repeat(43)}`,
    ]) {
      const response = await stub.fetch(`https://budget${path}`,
        {method: "POST", headers: {[INTERNAL_API_HEADER]: "1"}});
      expect(response.status).toBe(400);
      expect(await response.json()).toEqual({error: "invalid_source"});
    }
  });
});
