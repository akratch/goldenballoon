import {describe, expect, it} from "vitest";
import partyRoom from "../src/party-room.ts?raw";
import matchRoom from "../src/match/match-room.ts?raw";
import partyBudget from "../src/party-budget.ts?raw";
import codeDirectory from "../src/party-code-directory.ts?raw";

const durableObjectSources = [
  ["party-room.ts", partyRoom], ["match-room.ts", matchRoom],
  ["party-budget.ts", partyBudget], ["party-code-directory.ts", codeDirectory],
] as const;

describe("Workers Free duration contract", () => {
  it("keeps every Durable Object eligible for idle hibernation", () => {
    for (const [name, text] of durableObjectSources) {
      expect(text, name).not.toMatch(/\bset(?:Timeout|Interval)\s*\(/);
      expect(text, name).not.toMatch(/\.accept\s*\(/);
      expect(text, name).not.toMatch(/\.addEventListener\s*\(/);
      expect(text, name).not.toMatch(/new\s+WebSocket\s*\(/);
    }
    for (const [name, text] of [["party-room.ts", partyRoom],
      ["match-room.ts", matchRoom]] as const) {
      expect(text, name).toContain("this.ctx.acceptWebSocket(");
      expect(text, name).toContain("serializeAttachment(");
      expect(text, name).toMatch(/override async webSocketMessage\s*\(/);
    }
  });

  it("serializes signaling close custody with replacement upgrades", () => {
    const closeHandler = matchRoom.split(
      "override async webSocketClose(socket: WebSocket): Promise<void> {", 2)[1]
      ?.split("override webSocketError", 1)[0] || "";
    expect(closeHandler).toContain("await this.ctx.blockConcurrencyWhile(async () => {");
    expect(closeHandler).toContain("signal-sequence:${attachment.endpointId}");
    expect(closeHandler).toContain("current === attachment.connectionGeneration");
    expect(closeHandler.indexOf("blockConcurrencyWhile")).toBeLessThan(
      closeHandler.indexOf("sendSignalPresence"));
  });

  it("serializes Phone Party native mutations with HTTP authority", () => {
    const enqueue = partyRoom.split(
      "private enqueueNativeHostCommand(", 2)[1]
      ?.split("private async reserveNativeCommand", 1)[0] || "";
    expect(enqueue).toContain("this.nativeCommandTail.then(() =>");
    expect(enqueue).toContain("this.ctx.blockConcurrencyWhile(async () => {");
    expect(enqueue.indexOf("blockConcurrencyWhile")).toBeLessThan(
      enqueue.indexOf("applyNativeHostCommand"));
  });

  it("contains broken socket delivery after authoritative state commits", () => {
    expect(matchRoom).toContain("export function deliverMatchSocketText(");
    expect(matchRoom).toContain("closeSocket(socket, 1011, failureReason);");
    expect(matchRoom.match(/\bsocket\.close\(/g)).toHaveLength(1);
    expect(matchRoom).toContain(
      'catch { closeSocket(socket, 1011, "attachment_update_failed"); return; }');
    const broadcast = matchRoom.split(
      "private broadcast(record: StoredMatchRoomV1): void {", 2)[1]
      ?.split("override async alarm", 1)[0] || "";
    expect(broadcast).toContain("deliverMatchSocketText(socket, message, \"state_delivery_failed\")");
    expect(broadcast).not.toContain("socket.send(");
    const signalUpgrade = matchRoom.split(
      "private async upgradeSignal(request: Request,", 2)[1]
      ?.split("override async webSocketMessage", 1)[0] || "";
    const welcome = signalUpgrade.indexOf("deliverMatchSocketText(server,");
    const persist = signalUpgrade.indexOf("await this.ctx.storage.put(key");
    const replace = signalUpgrade.indexOf("connection_replaced");
    expect(welcome).toBeGreaterThanOrEqual(0);
    expect(persist).toBeGreaterThanOrEqual(0);
    expect(replace).toBeGreaterThanOrEqual(0);
    expect(welcome).toBeLessThan(persist);
    expect(persist).toBeLessThan(replace);
    expect(signalUpgrade).toContain(
      'headers: {"sec-websocket-protocol": "gb-match-signal-v1"}');

    expect(partyRoom).toContain("export function deliverPartySocketText(");
    const partyBroadcast = partyRoom.split(
      "private broadcastHost(value: unknown): void {", 2)[1]
      ?.split("private broadcastControllerState", 1)[0] || "";
    expect(partyBroadcast).toContain(
      "deliverPartySocketText(socket, message, \"state_delivery_failed\")");
    expect(partyBroadcast).not.toContain("socket.send(");
  });
});
