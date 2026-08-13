import {describe, expect, it} from "vitest";
import {admitMatchSignalMessage, parseMatchSignalMessage} from
  "../src/match/signaling";
import {MATCH_LIMITS} from "../src/match/protocol";

function publicKey(): string {
  const raw = new Uint8Array(65);
  raw[0] = 4;
  raw[64] = 1;
  let binary = "";
  for (const byte of raw) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function base(type: string, extra: Record<string, unknown> = {}) {
  return {protocolVersion: 1, type, sequence: 1, toEndpointId: "2",
    toConnectionGeneration: 3, ...extra};
}

describe("MatchRoom signaling protocol", () => {
  it("accepts only exact bounded sender-free message shapes", () => {
    expect(parseMatchSignalMessage(base("peer_hello", {publicKey: publicKey()}), "1"))
      .toEqual(base("peer_hello", {publicKey: publicKey()}));
    expect(parseMatchSignalMessage(base("webrtc_offer", {sdp: "v=0\r\n"}), "1"))
      .toEqual(base("webrtc_offer", {sdp: "v=0\r\n"}));
    expect(parseMatchSignalMessage(base("webrtc_answer", {sdp: "v=0\r\n"}), "1"))
      .not.toBeNull();
    expect(parseMatchSignalMessage(base("webrtc_ice", {candidate: "candidate:1",
      sdpMid: "0", sdpMLineIndex: 0, usernameFragment: null}), "1")).not.toBeNull();
    expect(parseMatchSignalMessage(base("peer_end", {reason: "restart"}), "1"))
      .not.toBeNull();

    for (const invalid of [
      null, [], base("unknown"),
      {...base("peer_hello", {publicKey: publicKey()}), fromEndpointId: "9"},
      {...base("peer_hello", {publicKey: publicKey()}), extra: true},
      {...base("peer_hello", {publicKey: publicKey()}), toEndpointId: "1"},
      {...base("peer_hello", {publicKey: publicKey()}), sequence: 0},
      {...base("peer_hello", {publicKey: publicKey()}), toConnectionGeneration: 0},
      base("peer_hello", {publicKey: "A".repeat(87)}),
      base("webrtc_offer", {sdp: "v=0\0"}),
      base("webrtc_offer", {sdp: "é".repeat(
        Math.floor(MATCH_LIMITS.maxSignalSdpBytes / 2) + 1)}),
      base("webrtc_ice", {candidate: "candidate:1", sdpMid: "0",
        sdpMLineIndex: 256, usernameFragment: null}),
      base("webrtc_ice", {candidate: "x".repeat(MATCH_LIMITS.maxSignalIceBytes + 1),
        sdpMid: null, sdpMLineIndex: null, usernameFragment: null}),
      base("peer_end", {reason: "retry"}),
    ]) expect(parseMatchSignalMessage(invalid, "1")).toBeNull();
  });

  it("enforces burst, lifetime, malformed-state and clock-regression caps", () => {
    const burst = {messages: 0, windowStartedAt: 100, lifetimeMessages: 0};
    for (let index = 0; index < MATCH_LIMITS.maxSignalMessagesPerWindow; index++) {
      expect(admitMatchSignalMessage(burst, 100)).toBe(true);
    }
    expect(admitMatchSignalMessage(burst, 100)).toBe(false);
    expect(admitMatchSignalMessage(burst,
      100 + MATCH_LIMITS.signalWindowMs)).toBe(true);
    expect(admitMatchSignalMessage(burst, 99)).toBe(false);

    const lifetime = {messages: 0, windowStartedAt: 0, lifetimeMessages: 0};
    for (let index = 0; index < MATCH_LIMITS.maxSignalMessagesPerSocket; index++) {
      const now = Math.floor(index / MATCH_LIMITS.maxSignalMessagesPerWindow) *
        MATCH_LIMITS.signalWindowMs;
      expect(admitMatchSignalMessage(lifetime, now)).toBe(true);
    }
    expect(admitMatchSignalMessage(lifetime, 100_000)).toBe(false);
    expect(admitMatchSignalMessage({messages: -1, windowStartedAt: 0,
      lifetimeMessages: 0}, 0)).toBe(false);
  });
});
