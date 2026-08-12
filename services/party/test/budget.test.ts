import {env} from "cloudflare:workers";
import {runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import {boundedSetting} from "../src/party-budget";
import {INTERNAL_API_HEADER} from "../src/internal-api";
import type {Env} from "../src/types";

describe("zero-cost budget settings", () => {
  it("honors the literal zero admission kill switch", () => {
    expect(boundedSetting("0", 10_000, 0, 12_000)).toBe(0);
  });

  it("bounds operator input and fails malformed text to the safest bound", () => {
    expect(boundedSetting("99999", 10_000, 0, 12_000)).toBe(12_000);
    expect(boundedSetting("-1", 10_000, 0, 12_000)).toBe(0);
    expect(boundedSetting("not-a-number", 10_000, 0, 12_000)).toBe(0);
    expect(boundedSetting("0", 5_000, 2_000, 8_000)).toBe(2_000);
  });

  it("reports only bounded aggregate state and latches refusal once", async () => {
    const bindings = env as unknown as Env;
    const stub = bindings.PARTY_BUDGETS.get(
      bindings.PARTY_BUDGETS.idFromName("budget-status-unit"));
    await runInDurableObject(stub, async (_instance, state) => {
      await state.storage.put("counters", {pairing: 10_000, control: 4});
    });
    for (let attempt = 0; attempt < 3; attempt++) {
      const refused = await stub.fetch(
        "https://budget/admit?kind=pairing&units=1", {method: "POST"});
      expect(refused.status).toBe(503);
      expect(await refused.json()).toEqual(
        {allowed: false, error: "service_budget_safe"});
    }
    const stored = await runInDurableObject(stub, async (_instance, state) =>
      state.storage.get<Record<string, unknown>>("counters"));
    expect(stored).toEqual({pairing: 10_000, control: 4,
      pairingRefusalObserved: true});
    const response = await stub.fetch("https://budget/status");
    expect(response.headers.get("cache-control")).toBe("no-store");
    expect(await response.json()).toEqual({
      schemaVersion: 1,
      admitted: {pairingUnits: 10_000, controlUnits: 4},
      refusalObserved: {pairing: true, control: false},
      remaining: {admissionUnits: 0, controlUnits: 9_896},
      admissionPercent: 100,
      level: "closed",
    });

    await runInDurableObject(stub, async (_instance, state) => {
      await state.storage.put("counters", {pairing: 10_000, control: 9_900,
        pairingRefusalObserved: true});
    });
    for (let attempt = 0; attempt < 3; attempt++) {
      const refused = await stub.fetch(
        "https://budget/admit?kind=control&units=1", {method: "POST"});
      expect(refused.status).toBe(503);
    }
    const controlLatch = await runInDurableObject(stub,
      async (_instance, state) =>
        state.storage.get<Record<string, unknown>>("counters"));
    expect(controlLatch).toEqual({pairing: 10_000, control: 9_900,
      pairingRefusalObserved: true, controlRefusalObserved: true});
  });

  it("maps exact utilization boundaries to stable operator levels", async () => {
    const bindings = env as unknown as Env;
    const stub = bindings.PARTY_BUDGETS.get(
      bindings.PARTY_BUDGETS.idFromName("budget-level-unit"));
    for (const [pairing, percent, level] of [
      [4_999, 49, "normal"], [5_000, 50, "watch"],
      [7_500, 75, "freeze"], [9_000, 90, "closed"],
    ] as const) {
      await runInDurableObject(stub, async (_instance, state) => {
        await state.storage.put("counters", {pairing, control: 0});
      });
      const response = await stub.fetch("https://budget/status");
      expect(await response.json()).toMatchObject({admissionPercent: percent,
        level});
    }
  });

  it("schedules finite daily-shard retention and deletes on alarm", async () => {
    const bindings = env as unknown as Env;
    const stub = bindings.PARTY_BUDGETS.get(
      bindings.PARTY_BUDGETS.idFromName("budget-retention-unit"));
    const admitted = await stub.fetch(
      "https://budget/admit?kind=pairing&units=1", {method: "POST"});
    expect(admitted.status).toBe(200);
    const alarm = await runInDurableObject(stub, async (_instance, state) =>
      state.storage.getAlarm());
    expect(alarm).not.toBeNull();
    expect(alarm!).toBeGreaterThan(Date.now() + 31 * 24 * 60 * 60_000);
    await runInDurableObject(stub, async (instance) => instance.alarm());
    const stored = await runInDurableObject(stub, async (_instance, state) =>
      state.storage.list());
    expect(stored.size).toBe(0);
  });

  it("accepts only versioned fixed operation shapes and reports bounded aggregates",
      async () => {
    const bindings = env as unknown as Env;
    const stub = bindings.PARTY_BUDGETS.get(
      bindings.PARTY_BUDGETS.idFromName("budget-health-unit"));
    const headers = {[INTERNAL_API_HEADER]: "1"};
    for (const path of [
      "/admit?kind=pairing&units=3",
      "/admit?kind=pairing&units=3&operation=unknown",
      "/admit?kind=pairing&units=2&operation=matchCodeJoin",
      "/admit?kind=control&units=28&operation=partyControl",
    ]) {
      const response = await stub.fetch(`https://budget${path}`,
        {method: "POST", headers});
      expect(response.status).toBe(400);
      expect(await response.json()).toEqual({error: "invalid_operation"});
    }
    expect((await stub.fetch(
      "https://budget/admit?kind=pairing&units=3&operation=matchCodeJoin",
      {method: "POST", headers})).status).toBe(200);
    expect((await stub.fetch(
      "https://budget/admit?kind=control&units=28&operation=partySocket",
      {method: "POST", headers})).status).toBe(200);
    // Missing operation remains readable only for the frozen pre-v1 Worker.
    expect((await stub.fetch("https://budget/admit?kind=pairing&units=1",
      {method: "POST"})).status).toBe(200);
    const health = await stub.fetch("https://budget/health", {headers});
    expect(health.headers.get("cache-control")).toBe("no-store");
    expect(await health.json()).toEqual({schemaVersion: 1,
      reservationRequests: 3,
      reservations: {
        matchCreate: 0, matchLinkJoin: 0, matchCodeJoin: 1,
        matchControl: 0, matchRotate: 0, matchSocket: 0,
        partyCreate: 0, partyLinkJoin: 0, partyCodeJoin: 0,
        partyControl: 0, partyRotate: 0, partySocket: 1, legacy: 1,
      },
      admitted: {pairingUnits: 4, controlUnits: 28},
      tracked: {pairingUnits: 3, controlUnits: 28},
      tracking: "partial",
    });
  });
});
