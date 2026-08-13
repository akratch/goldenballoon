import {env} from "cloudflare:workers";
import {describe, expect, it} from "vitest";
import {allowedOrigin, boundCredential, normalizeName, readJson, readRequestBytes, utf8Exceeds,
  validBoundCredential, validPartyOrigin} from "../src/security";
import type {Env} from "../src/types";

describe("room-bound credentials", () => {
  const jsonHeaders = {"content-type": "application/json"};

  it("normalizes controller names without visual-order controls", () => {
    expect(normalizeName(`  ${"\u202e".repeat(30)}Sam\u2066's phone  `))
      .toBe("Sam's phone");
    expect([...normalizeName("🎈".repeat(30))]).toHaveLength(24);
  });

  it("accepts only a canonical HTTPS or loopback Party origin", () => {
    for (const origin of [
      "https://party.example.test",
      "https://party.example.test:8443",
      "http://localhost:8787",
      "http://phone.localhost:8787",
      "http://127.0.0.1:8787",
      "http://[::1]:8787",
    ]) expect(validPartyOrigin(origin)).toBe(true);
    for (const origin of [
      "http://party.example.test",
      "https://party.example.test/",
      "https://party.example.test/controller/",
      "https://party.example.test?preview=1",
      "https://party.example.test#fragment",
      "https://user@party.example.test",
      "//party.example.test",
      "javascript:alert(1)",
      "",
    ]) expect(validPartyOrigin(origin)).toBe(false);
    const bindings = {PARTY_ORIGIN: "https://party.example.test"} as Env;
    expect(allowedOrigin(new Request("https://worker.test", {
      headers: {origin: bindings.PARTY_ORIGIN},
    }), bindings)).toBe(true);
    bindings.PARTY_ORIGIN = "https://party.example.test/";
    expect(allowedOrigin(new Request("https://worker.test", {
      headers: {origin: bindings.PARTY_ORIGIN},
    }), bindings)).toBe(false);
  });

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

  it("admits only object-shaped JSON protocol messages", async () => {
    await expect(readJson<Record<string, unknown>>(new Request("https://test/", {
      method: "POST", headers: jsonHeaders, body: "{\"protocolVersion\":1}",
    }))).resolves.toEqual({protocolVersion: 1});
    for (const body of ["null", "[]", "1", "\"text\"", "not-json"]) {
      try {
        await readJson(new Request("https://test/", {
          method: "POST", headers: jsonHeaders, body,
        }));
        throw new Error(`unexpectedly accepted ${body}`);
      } catch (error) {
        expect(error).toBeInstanceOf(Response);
        expect((error as Response).status).toBe(400);
        expect(await (error as Response).text()).toBe("invalid_json");
      }
    }
  });

  it("cancels chunked overflow before aggregate allocation", async () => {
    const request = new Request("https://test/", {method: "POST",
      body: new Uint8Array(17)});
    try {
      await readRequestBytes(request, 16);
      throw new Error("unexpectedly accepted oversized chunked body");
    } catch (error) {
      expect(error).toBeInstanceOf(Response);
      expect((error as Response).status).toBe(413);
      expect(await (error as Response).text()).toBe("request_too_large");
    }
  });

  it("rejects malformed UTF-8 instead of decoding replacement text", async () => {
    const bytes = new Uint8Array([
      0x7b, 0x22, 0x78, 0x22, 0x3a, 0x22, 0xff, 0x22, 0x7d,
    ]);
    try {
      await readJson(new Request("https://test/", {
        method: "POST", headers: jsonHeaders, body: bytes,
      }));
      throw new Error("unexpectedly accepted invalid UTF-8");
    } catch (error) {
      expect(error).toBeInstanceOf(Response);
      expect((error as Response).status).toBe(400);
      expect(await (error as Response).text()).toBe("invalid_json");
    }
  });

  it("rejects JSON bodies without the JSON media type before parsing", async () => {
    for (const contentType of [null, "text/plain", "application/jsonp"]) {
      const headers = new Headers();
      if (contentType) headers.set("content-type", contentType);
      try {
        await readJson(new Request("https://test/", {
          method: "POST", headers, body: "{}",
        }));
        throw new Error(`unexpectedly accepted ${contentType || "missing type"}`);
      } catch (error) {
        expect(error).toBeInstanceOf(Response);
        expect((error as Response).status).toBe(415);
        expect(await (error as Response).text()).toBe("unsupported_media_type");
      }
    }
  });
});
