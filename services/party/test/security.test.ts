import {env} from "cloudflare:workers";
import {describe, expect, it} from "vitest";
import {boundCredential, utf8Exceeds, validBoundCredential} from "../src/security";
import type {Env} from "../src/types";

describe("room-bound credentials", () => {
  it("keeps the 43-character wire shape while binding role and room", async () => {
    const bindings = env as unknown as Env;
    const first = await boundCredential(bindings, "host", "room-one");
    const second = await boundCredential(bindings, "host", "room-one");
    expect(first).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(second).not.toBe(first);
    expect(await validBoundCredential(bindings, "host", "room-one", first))
      .toBe(true);
    expect(await validBoundCredential(bindings, "controller", "room-one", first))
      .toBe(false);
    expect(await validBoundCredential(bindings, "host", "room-two", first))
      .toBe(false);
    const changed = `${first.slice(0, -1)}${first.endsWith("A") ? "B" : "A"}`;
    expect(await validBoundCredential(bindings, "host", "room-one", changed))
      .toBe(false);
  });

  it("bounds UTF-8 wire length without confusing code units and bytes", () => {
    expect(utf8Exceeds("a".repeat(4096), 4096)).toBe(false);
    expect(utf8Exceeds("a".repeat(4097), 4096)).toBe(true);
    expect(utf8Exceeds("é".repeat(2048), 4096)).toBe(false);
    expect(utf8Exceeds("é".repeat(2049), 4096)).toBe(true);
    expect(utf8Exceeds("😀".repeat(1024), 4096)).toBe(false);
    expect(utf8Exceeds("😀".repeat(1025), 4096)).toBe(true);
    expect(utf8Exceeds("\ud800", 2)).toBe(true);
    expect(utf8Exceeds("\ud800", 3)).toBe(false);
  });
});
