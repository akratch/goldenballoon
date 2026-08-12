import {describe, expect, it} from "vitest";
import {blankCompatibility, MATCH_LIMITS, type MatchCommandType, type MatchCommandV1,
  type MatchCompatibilityV1, validMatchControlLog} from "../src/match/protocol";
import {createMatchLobby, dispatchMatchCommand, matchCommandFingerprint,
  validMatchLobby}
  from "../src/match/reducer";

const compatibility: MatchCompatibilityV1 = {protocolVersion: 1,
  buildId: Array.from({length: 16}, (_, i) => i + 1),
  gameplayDigest: Array.from({length: 32}, (_, i) => 128 + i),
  romRevision: 1, cadenceHz: 30};

function command(type: MatchCommandType, actor: string, id: number,
                 revision: number, value = 0, target = "0",
                 compat = blankCompatibility()): MatchCommandV1 {
  return {protocolVersion: 1, expectedRevision: revision, commandId: String(id),
    actorEndpointId: actor, type, value, targetEndpointId: target,
    compatibility: compat};
}

describe("MatchRoom protocol-v1 reducer", () => {
  it("matches the native C command fingerprint vector", () => {
    const value = command("set_character", "100", 1, 1, 0, "0");
    expect(matchCommandFingerprint(value)).toBe("187cbf8c556a134d");
  });

  it("rejects corrupt or noncontiguous persisted control history", () => {
    const lobby = createMatchLobby("1", "100", compatibility, 1)!;
    const accepted = dispatchMatchCommand(lobby,
      command("set_character", "100", 1, 1, 0, "0"));
    expect(validMatchControlLog([accepted], lobby.revision, lobby.matchEpoch)).toBe(true);
    expect(validMatchControlLog([{...accepted, revision: accepted.revision - 1}],
      lobby.revision, lobby.matchEpoch)).toBe(false);
    expect(validMatchControlLog([{...accepted, error: "capacity"}],
      lobby.revision, lobby.matchEpoch)).toBe(false);
  });

  it("completes two rounds while preserving deterministic room identity", () => {
    const lobby = createMatchLobby("42", "100", compatibility, 1)!;
    expect(dispatchMatchCommand(lobby,
      command("join", "200", 1, 1, 1, "0", compatibility)).accepted).toBe(true);
    let hostId = 1;
    let guestId = 2;
    const send = (type: MatchCommandType, actor: "100" | "200", value = 0,
                  target = "0") => dispatchMatchCommand(lobby,
      command(type, actor, actor === "100" ? hostId++ : guestId++,
        lobby.revision, value, target));
    expect(send("set_character", "100", 0, "0").accepted).toBe(true);
    expect(send("set_vehicle", "100", 0, "0").accepted).toBe(true);
    expect(send("set_character", "200", 1, "1").accepted).toBe(true);
    expect(send("set_vehicle", "200", 0, "1").accepted).toBe(true);
    expect(send("set_vote", "100", 5, "0").accepted).toBe(true);
    expect(send("set_vote", "200", 3, "1").accepted).toBe(true);
    expect(send("set_ready", "100", 1).accepted).toBe(true);
    expect(send("set_ready", "200", 1).accepted).toBe(true);
    expect(send("begin_loading", "100", 7).accepted).toBe(true);
    const firstTrack = lobby.selectedTrack;
    expect(lobby).toMatchObject({phase: "loading", matchEpoch: 1,
      selectedVehicleMask: 7});
    expect(send("ack_loaded", "100").accepted).toBe(true);
    expect(send("ack_loaded", "200").accepted).toBe(true);
    expect(send("begin_race", "100").accepted).toBe(true);
    expect(send("publish_results", "100").accepted).toBe(true);
    expect(send("rematch", "100").accepted).toBe(true);
    expect(lobby).toMatchObject({phase: "lobby", matchEpoch: 1,
      selectedTrack: null, selectedVehicleMask: 0});
    expect(lobby.members.every(item => !item.ready && !item.loaded)).toBe(true);

    send("set_vote", "100", 5, "0"); send("set_vote", "200", 3, "1");
    send("set_ready", "100", 1); send("set_ready", "200", 1);
    expect(send("begin_loading", "100", 7).accepted).toBe(true);
    expect(lobby.matchEpoch).toBe(2);
    expect([3, 5]).toContain(firstTrack);
    expect([3, 5]).toContain(lobby.selectedTrack);
    expect(validMatchLobby(lobby)).toBe(true);
  });

  it("rejects stale, conflicting, unauthorized and illegal work atomically", () => {
    const lobby = createMatchLobby("99", "100", compatibility, 1)!;
    dispatchMatchCommand(lobby, command("join", "200", 1, 1, 1, "0", compatibility));
    const before = JSON.stringify(lobby);
    expect(dispatchMatchCommand(lobby,
      command("set_character", "100", 1, lobby.revision, 0, "1")))
      .toMatchObject({accepted: false, error: "unauthorized"});
    expect(JSON.stringify(lobby)).toBe(before);

    const accepted = command("set_character", "100", 2, lobby.revision, 0, "0");
    expect(dispatchMatchCommand(lobby, accepted).accepted).toBe(true);
    const revision = lobby.revision;
    expect(dispatchMatchCommand(lobby, accepted)).toMatchObject({accepted: true,
      duplicate: true, revision});
    expect(dispatchMatchCommand(lobby, {...accepted, value: 2}))
      .toMatchObject({accepted: false, error: "command_conflict"});
    expect(lobby.revision).toBe(revision);
    expect(dispatchMatchCommand(lobby,
      command("set_vehicle", "100", 1, lobby.revision, 0, "0")))
      .toMatchObject({accepted: false, error: "stale_command"});
    expect(dispatchMatchCommand(lobby,
      command("set_vehicle", "100", 3, lobby.revision - 1, 0, "0")))
      .toMatchObject({accepted: false, error: "stale_revision"});

    dispatchMatchCommand(lobby, command("set_vehicle", "100", 3, lobby.revision, 2, "0"));
    dispatchMatchCommand(lobby, command("set_character", "200", 2, lobby.revision, 1, "1"));
    dispatchMatchCommand(lobby, command("set_vehicle", "200", 3, lobby.revision, 0, "1"));
    dispatchMatchCommand(lobby, command("set_vote", "100", 4, lobby.revision, 5, "0"));
    dispatchMatchCommand(lobby, command("set_vote", "200", 4, lobby.revision, 5, "1"));
    dispatchMatchCommand(lobby, command("set_ready", "100", 5, lobby.revision, 1));
    dispatchMatchCommand(lobby, command("set_ready", "200", 5, lobby.revision, 1));
    const ready = JSON.stringify(lobby);
    expect(dispatchMatchCommand(lobby,
      command("begin_loading", "100", 6, lobby.revision, 1)))
      .toMatchObject({accepted: false, error: "illegal_vehicle"});
    expect(JSON.stringify(lobby)).toBe(ready);
  });

  it("bounds capacity, elects deterministically, and preserves leave receipts", () => {
    const lobby = createMatchLobby("7", "400", compatibility, 1)!;
    expect(dispatchMatchCommand(lobby,
      command("join", "300", 1, lobby.revision, 1, "0", compatibility)).accepted).toBe(true);
    expect(dispatchMatchCommand(lobby,
      command("join", "200", 1, lobby.revision, 1, "0", compatibility)).accepted).toBe(true);
    expect(dispatchMatchCommand(lobby,
      command("join", "100", 1, lobby.revision, 1, "0", compatibility)).accepted).toBe(true);
    const full = JSON.stringify(lobby);
    expect(dispatchMatchCommand(lobby,
      command("join", "500", 1, lobby.revision, 1, "0", compatibility)))
      .toMatchObject({accepted: false, error: "capacity"});
    expect(JSON.stringify(lobby)).toBe(full);
    const leaving = command("leave", "400", 1, lobby.revision);
    expect(dispatchMatchCommand(lobby, leaving)).toMatchObject({accepted: true,
      leaderChanged: true, leaderEndpointId: "100"});
    const revision = lobby.revision;
    expect(dispatchMatchCommand(lobby, leaving)).toMatchObject({accepted: true,
      duplicate: true, revision});
    expect(lobby.members).toHaveLength(3);
  });

  it("remains deterministic, valid and fail-atomic under seeded command chaos", () => {
    const types: MatchCommandType[] = ["join", "leave", "disconnect", "reconnect",
      "set_ready", "set_vote", "begin_loading", "ack_loaded", "begin_race",
      "publish_results", "rematch", "transfer_leader", "close", "set_character",
      "set_vehicle", "cancel_loading"];
    const errors = new Set<string>();
    let accepted = 0;
    let rejected = 0;
    let duplicateChecks = 0;

    for (let seed = 1; seed <= 48; seed++) {
      let randomState = (0x9e3779b9 ^ seed) >>> 0;
      const random = (bound: number) => {
        randomState ^= randomState << 13;
        randomState ^= randomState >>> 17;
        randomState ^= randomState << 5;
        return (randomState >>> 0) % bound;
      };
      let lobby = createMatchLobby(String(10_000 + seed), "100", compatibility, 1)!;
      let mirror = structuredClone(lobby);
      const history: {lastAccepted: MatchCommandV1 | null} = {lastAccepted: null};

      const applyBoth = (value: MatchCommandV1) => {
        const before = JSON.stringify(lobby);
        const result = dispatchMatchCommand(lobby, value);
        const mirrored = dispatchMatchCommand(mirror, structuredClone(value));
        expect(mirrored).toEqual(result);
        expect(mirror).toEqual(lobby);
        expect(validMatchLobby(lobby)).toBe(true);
        expect(lobby.members.length).toBeLessThanOrEqual(MATCH_LIMITS.maxEndpoints);
        expect(lobby.seats.length).toBeLessThanOrEqual(MATCH_LIMITS.maxSeats);
        expect(lobby.receipts.length).toBeLessThanOrEqual(MATCH_LIMITS.receiptWindow);
        if (result.accepted && !result.duplicate) {
          accepted++;
          expect(result.revision).toBe(JSON.parse(before).revision + 1);
          expect(JSON.stringify(lobby)).not.toBe(before);
          history.lastAccepted = structuredClone(value);
        } else {
          if (!result.accepted) { rejected++; errors.add(result.error); }
          expect(JSON.stringify(lobby)).toBe(before);
        }
        return result;
      };

      // Start every seed with enough independent actors for custody, selection,
      // disconnect and leader-election collisions to be reachable.
      for (const endpoint of ["200", "300"] as const) {
        const joined = command("join", endpoint, 1, lobby.revision, 1, "0",
          compatibility);
        expect(applyBoth(joined).accepted).toBe(true);
      }

      for (let stepIndex = 0; stepIndex < 512; stepIndex++) {
        if (lobby.phase === "closed") {
          lobby = createMatchLobby(String(1_000_000 + seed * 1000 + stepIndex),
            "100", compatibility, 1)!;
          mirror = structuredClone(lobby);
          history.lastAccepted = null;
        }
        const actor = String(100 + random(6) * 100);
        const revisionMode = random(5);
        const expectedRevision = revisionMode < 3 ? lobby.revision :
          revisionMode === 3 ? Math.max(1, lobby.revision - 1) :
          Math.min(0xffff_ffff, lobby.revision + 1);
        const value: MatchCommandV1 = {
          protocolVersion: random(20) === 0 ? 2 : 1,
          expectedRevision,
          commandId: String(1 + random(20_000)),
          actorEndpointId: actor,
          type: types[random(types.length)]!,
          value: random(12) === 0 ? 0xffff_ffff : random(300),
          targetEndpointId: String(random(6)),
          compatibility: random(4) === 0 ? structuredClone(compatibility) :
            blankCompatibility(),
        };
        applyBoth(value);

        // A retained accepted receipt must be exactly idempotent and a
        // fingerprint-changing replay must be an atomic conflict.
        const retained = history.lastAccepted;
        if (retained && stepIndex % 31 === 30) {
          const before = JSON.stringify(lobby);
          const replay = applyBoth(structuredClone(retained));
          expect(replay).toMatchObject({accepted: true, duplicate: true});
          expect(JSON.stringify(lobby)).toBe(before);
          const conflict = structuredClone(retained);
          conflict.value = conflict.value === 0xffff_ffff ? 0 : conflict.value + 1;
          expect(applyBoth(conflict)).toMatchObject({accepted: false,
            error: "command_conflict"});
          duplicateChecks++;
        }

        // Simulate eviction/reconstruction frequently; the next command must
        // behave byte-for-byte like the uninterrupted actor.
        if (stepIndex % 17 === 16) mirror = structuredClone(lobby);
      }
    }

    expect(accepted).toBeGreaterThan(100);
    expect(rejected).toBeGreaterThan(20_000);
    expect(duplicateChecks).toBeGreaterThan(20);
    expect(errors.size).toBeGreaterThanOrEqual(7);
  });
});
