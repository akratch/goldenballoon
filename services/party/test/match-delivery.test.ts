import {describe, expect, it} from "vitest";
import {deliverMatchSocketText} from "../src/match/match-room";
import {deliverPartySocketText} from "../src/party-room";

function socketWith(overrides: Partial<WebSocket>): WebSocket {
  return {readyState: WebSocket.OPEN, send() {}, close() {},
    ...overrides} as unknown as WebSocket;
}

describe("MatchRoom best-effort WebSocket delivery", () => {
  it("delivers to an open socket", () => {
    const sent: string[] = [];
    const socket = socketWith({send(message) { sent.push(String(message)); }});
    expect(deliverMatchSocketText(socket, "state", "state_delivery_failed")).toBe(true);
    expect(sent).toEqual(["state"]);
  });

  it("closes a failed socket without propagating its send exception", () => {
    const closed: Array<[number | undefined, string | undefined]> = [];
    const socket = socketWith({send() { throw new Error("peer disappeared"); },
      close(code, reason) { closed.push([code, reason]); }});
    expect(deliverMatchSocketText(socket, "state", "state_delivery_failed")).toBe(false);
    expect(closed).toEqual([[1011, "state_delivery_failed"]]);
  });

  it("contains both send and close failures from a broken peer", () => {
    const socket = socketWith({send() { throw new Error("send failed"); },
      close() { throw new Error("close failed"); }});
    expect(() => deliverMatchSocketText(socket, "signal",
      "signal_delivery_failed")).not.toThrow();
    expect(deliverMatchSocketText(socket, "signal",
      "signal_delivery_failed")).toBe(false);
  });

  it("does not send to a non-open socket", () => {
    let sends = 0;
    const socket = socketWith({readyState: WebSocket.CLOSED,
      send() { sends++; }});
    expect(deliverMatchSocketText(socket, "state", "state_delivery_failed")).toBe(false);
    expect(sends).toBe(0);
  });
});

describe("PartyRoom best-effort WebSocket delivery", () => {
  it("contains a stale socket after authoritative state commits", () => {
    const closed: Array<[number | undefined, string | undefined]> = [];
    const socket = socketWith({send() { throw new Error("peer disappeared"); },
      close(code, reason) { closed.push([code, reason]); }});
    expect(deliverPartySocketText(socket, "state", "state_delivery_failed"))
      .toBe(false);
    expect(closed).toEqual([[1011, "state_delivery_failed"]]);
  });

  it("contains a second failure while retiring the broken peer", () => {
    const socket = socketWith({send() { throw new Error("send failed"); },
      close() { throw new Error("close failed"); }});
    expect(() => deliverPartySocketText(socket, "state",
      "state_delivery_failed")).not.toThrow();
  });
});
