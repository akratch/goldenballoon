import {DurableObject} from "cloudflare:workers";
import type {Env} from "./types";
import {json} from "./security";
import {INTERNAL_API_HEADER, INTERNAL_API_VERSION,
  rejectUnsupportedInternalApi} from "./internal-api";

export const BUDGET_OPERATIONS = [
  "matchCreate", "matchLinkJoin", "matchCodeJoin", "matchControl",
  "matchRotate", "matchSocket", "matchSignalSocket", "partyCreate", "partyLinkJoin",
  "partyCodeJoin", "partyControl", "partyRotate", "partySocket",
] as const;
export type BudgetOperation = typeof BUDGET_OPERATIONS[number];
type StoredBudgetOperation = BudgetOperation | "legacy";
type ReservationCounts = Partial<Record<StoredBudgetOperation, number>>;

const BUDGET_OPERATION_SET = new Set<string>(BUDGET_OPERATIONS);
const BUDGET_OPERATION_SHAPE: Readonly<Record<BudgetOperation,
  readonly ["pairing" | "control", number]>> = Object.freeze({
  matchCreate: ["pairing", 10], matchLinkJoin: ["pairing", 2],
  matchCodeJoin: ["pairing", 3],
  matchControl: ["control", 2], matchRotate: ["control", 10],
  matchSocket: ["control", 4], matchSignalSocket: ["control", 15],
  partyCreate: ["pairing", 10],
  partyLinkJoin: ["pairing", 2], partyCodeJoin: ["pairing", 3],
  partyControl: ["control", 2],
  partyRotate: ["control", 10], partySocket: ["control", 28],
});

interface Counters {
  pairing: number;
  control: number;
  pairingRefusalObserved?: boolean;
  controlRefusalObserved?: boolean;
  reservations?: ReservationCounts;
}

/* External operations fan out to room/code objects, while hibernatable socket
 * messages consume the same free Durable Object request allowance. Keep a
 * deliberately large margin below provider headline limits. */
const SAFE_EXTERNAL_OPERATIONS = 20_000;
const BUDGET_RETENTION_MS = 32 * 24 * 60 * 60_000;

/* I-2: the global reserve is a cost cap, not an availability control — one
 * distributed abuser could drain the whole day's rooms and deny pairing to
 * everyone until UTC midnight. Room creation therefore also carries a
 * pseudonymous per-source daily allowance. The counters live in this same
 * day-named shard, so they reset at UTC midnight with the reserve and are
 * deleted by the same retention alarm. Refusal charges nothing anywhere:
 * neither the reserve nor the source counter moves. */
const CREATE_OPERATIONS: ReadonlySet<string> = new Set(
  ["partyCreate", "matchCreate"]);
const MAX_DAILY_CREATES_PER_SOURCE = 25;

export function boundedSetting(value: string | undefined, fallback: number,
                               minimum: number, maximum: number): number {
  if (value === undefined || value.trim() === "") return fallback;
  const parsed = Number(value);
  if (!Number.isInteger(parsed)) return minimum;
  return Math.max(minimum, Math.min(maximum, parsed));
}

export class PartyBudget extends DurableObject<Env> {
  private async persist(counters: Counters, firstWrite: boolean): Promise<void> {
    // A day shard is never addressed again after UTC midnight. Schedule its
    // deletion on the first durable mutation so aggregate counters cannot
    // become an unbounded historical database.
    if (firstWrite) {
      await this.ctx.storage.setAlarm(Date.now() + BUDGET_RETENTION_MS);
    }
    await this.ctx.storage.put("counters", counters);
  }

  override async alarm(): Promise<void> {
    await this.ctx.storage.deleteAll();
  }

  override async fetch(request: Request): Promise<Response> {
    const rejected = rejectUnsupportedInternalApi(request);
    if (rejected) return rejected;
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/status") {
      const counters = await this.ctx.storage.get<Counters>("counters") ||
        {pairing: 0, control: 0};
      const maxAdmissions = boundedSetting(this.env.MAX_ADMISSIONS_PER_DAY,
        10_000, 0, 12_000);
      const controlReserve = boundedSetting(this.env.CONTROL_RESERVE_PER_DAY,
        5_000, 2_000, 8_000);
      const total = counters.pairing + counters.control;
      const admissionCeiling = Math.min(maxAdmissions,
        Math.max(0, SAFE_EXTERNAL_OPERATIONS - controlReserve - counters.control));
      const admissionPercent = admissionCeiling === 0 ? 100 : Math.min(100,
        Math.floor(counters.pairing * 100 / admissionCeiling));
      return json({schemaVersion: 1,
        admitted: {pairingUnits: counters.pairing, controlUnits: counters.control},
        refusalObserved: {
          pairing: counters.pairingRefusalObserved === true,
          control: counters.controlRefusalObserved === true,
        },
        remaining: {
          admissionUnits: Math.max(0, admissionCeiling - counters.pairing),
          controlUnits: Math.max(0, SAFE_EXTERNAL_OPERATIONS - 100 - total),
        },
        admissionPercent,
        level: admissionPercent >= 90 ? "closed" : admissionPercent >= 75
          ? "freeze" : admissionPercent >= 50 ? "watch" : "normal"});
    }
    if (request.method === "GET" && url.pathname === "/health") {
      const counters = await this.ctx.storage.get<Counters>("counters") ||
        {pairing: 0, control: 0};
      const reservations = {} as Record<StoredBudgetOperation, number>;
      let trackedPairingUnits = 0;
      let trackedControlUnits = 0;
      for (const operation of [...BUDGET_OPERATIONS, "legacy"] as const) {
        const stored = counters.reservations?.[operation];
        reservations[operation] = Number.isSafeInteger(stored) && stored! >= 0 &&
          stored! <= 19_900 ? stored! : 0;
        if (operation !== "legacy") {
          const [kind, units] = BUDGET_OPERATION_SHAPE[operation];
          if (kind === "pairing") trackedPairingUnits += reservations[operation] * units;
          else trackedControlUnits += reservations[operation] * units;
        }
      }
      const tracking = trackedPairingUnits > counters.pairing ||
        trackedControlUnits > counters.control ? "invalid" :
        reservations.legacy === 0 && trackedPairingUnits === counters.pairing &&
        trackedControlUnits === counters.control ? "complete" : "partial";
      return json({schemaVersion: 2,
        reservationRequests: Object.values(reservations)
          .reduce((total, count) => total + count, 0),
        reservations,
        admitted: {pairingUnits: counters.pairing, controlUnits: counters.control},
        tracked: {pairingUnits: trackedPairingUnits,
          controlUnits: trackedControlUnits},
        tracking});
    }
    if (request.method !== "POST") return json({error: "method_not_allowed"}, 405);
    const kind = url.searchParams.get("kind");
    const units = Number(url.searchParams.get("units"));
    if (kind !== "pairing" && kind !== "control") {
      return json({error: "invalid_kind"}, 400);
    }
    if (!Number.isInteger(units) || units < 1 || units > 32) {
      return json({error: "invalid_units"}, 400);
    }
    const operationText = url.searchParams.get("operation");
    const version = request.headers.get(INTERNAL_API_HEADER);
    let operation: StoredBudgetOperation = "legacy";
    if (operationText !== null) {
      if (!BUDGET_OPERATION_SET.has(operationText)) {
        return json({error: "invalid_operation"}, 400);
      }
      operation = operationText as BudgetOperation;
    } else if (version === INTERNAL_API_VERSION) {
      return json({error: "invalid_operation"}, 400);
    }
    if (operation !== "legacy") {
      const shape = BUDGET_OPERATION_SHAPE[operation];
      if (shape[0] !== kind || shape[1] !== units) {
        return json({error: "invalid_operation"}, 400);
      }
    }
    /* Fail closed at the enforcement point: a create admission that lost its
     * source identity must never bypass the per-source cap, and no other
     * operation may smuggle one in. Legacy (pre-v1 Worker) requests never
     * carry an operation, so they never reach the create branch. */
    const source = url.searchParams.get("source");
    const createOperation = CREATE_OPERATIONS.has(operation);
    if (createOperation
        ? source === null || !/^[A-Za-z0-9_-]{43}$/.test(source)
        : source !== null) {
      return json({error: "invalid_source"}, 400);
    }
    return this.ctx.blockConcurrencyWhile(async () => {
      const counters = await this.ctx.storage.get<Counters>("counters") ||
        {pairing: 0, control: 0};
      const sourceKey = `source:${source}`;
      let sourceCreates = 0;
      if (createOperation) {
        const stored = await this.ctx.storage.get<number>(sourceKey);
        /* A corrupt stored count refuses rather than admits. */
        sourceCreates = stored === undefined ? 0
          : Number.isSafeInteger(stored) && stored >= 0 ? stored
          : MAX_DAILY_CREATES_PER_SOURCE;
        if (sourceCreates + 1 > MAX_DAILY_CREATES_PER_SOURCE) {
          return json({allowed: false, error: "create_rate_limited"}, 429);
        }
      }
      const maxAdmissions = boundedSetting(this.env.MAX_ADMISSIONS_PER_DAY,
        10_000, 0, 12_000);
      const controlReserve = boundedSetting(this.env.CONTROL_RESERVE_PER_DAY,
        5_000, 2_000, 8_000);
      const total = counters.pairing + counters.control;
      const firstWrite = total === 0 &&
        counters.pairingRefusalObserved !== true &&
        counters.controlRefusalObserved !== true;
      if (kind === "pairing" &&
          (counters.pairing + units > maxAdmissions ||
           total + units > SAFE_EXTERNAL_OPERATIONS - controlReserve)) {
        if (counters.pairingRefusalObserved !== true) {
          counters.pairingRefusalObserved = true;
          await this.persist(counters, firstWrite);
        }
        return json({allowed: false, error: "service_budget_safe"}, 503);
      }
      if (kind === "control" && total + units > SAFE_EXTERNAL_OPERATIONS - 100) {
        if (counters.controlRefusalObserved !== true) {
          counters.controlRefusalObserved = true;
          await this.persist(counters, firstWrite);
        }
        return json({allowed: false, error: "service_budget_safe"}, 503);
      }
      counters[kind] += units;
      const reservations = counters.reservations || {};
      reservations[operation] = Math.min(19_900,
        (reservations[operation] || 0) + 1);
      counters.reservations = reservations;
      /* Count the create against its source only once the global reserve has
       * also admitted it, so a refusal of either kind consumes neither. The
       * key count is bounded by the day's admitted creates, never by how
       * many addresses attempt them. */
      if (createOperation) {
        await this.ctx.storage.put(sourceKey, sourceCreates + 1);
      }
      await this.persist(counters, firstWrite);
      return json({allowed: true, remainingControlReserve:
        Math.max(0, SAFE_EXTERNAL_OPERATIONS - counters.pairing - counters.control)});
    });
  }
}
