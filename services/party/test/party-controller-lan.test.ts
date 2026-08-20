import {beforeAll, afterEach, describe, expect, it, vi} from "vitest";

/*
 * Task 4b page-side LAN wiring. The controller page is a DOM-coupled IIFE, so
 * these unit tests reach only its pure, hoisted seams -- the trusted-origin
 * gate, the crypto.subtle-tolerance predicate, the LAN-vs-cloud selector, and
 * the redeem-frame builder. The page exposes them without touching the DOM via
 * the exposeInternals test flag: with it set the IIFE installs the seams on a
 * global and returns before it ever calls document.getElementById.
 *
 * The full real-page-to-native proof (a real phone redeeming over ws against
 * lan_party_room.cpp) is Task 6's browser E2E lane; here roomRedeemVerdict
 * mirrors the room's handleRedeem frame validators so the builder's output is
 * checked against the exact contract the native parser enforces.
 */

const BIDI_OVERRIDE = "‮"; // right-to-left override, stripped from names.

interface ControllerInternals {
  trustedControllerLocation(): boolean;
  pairingCryptoAvailable(): boolean;
  lanControllerMode(): boolean;
  lanRedeemFrame(controllerPublicKey: string,
    invite: {capability?: string; code?: string; name?: string}): Record<string, unknown>;
}

let internals: ControllerInternals;

beforeAll(async () => {
  vi.stubGlobal("location", {
    hash: "", pathname: "/controller/", search: "",
    href: "http://192.168.1.5:8080/controller/", replace() {},
  });
  vi.stubGlobal("history", {replaceState() {}});
  (globalThis as Record<string, unknown>).__mdkrControllerTestConfig =
    {exposeInternals: true};
  await import("../../../dist/web/controller/controller.js");
  internals = (globalThis as Record<string, unknown>)
    .__mdkrControllerInternals as ControllerInternals;
  vi.unstubAllGlobals();
});

afterEach(() => vi.unstubAllGlobals());

function atLocation(href: string, secureContext: boolean): boolean {
  vi.stubGlobal("location", {href});
  vi.stubGlobal("isSecureContext", secureContext);
  return internals.trustedControllerLocation();
}

/* The room's handleRedeem frame validators, mirrored: exactly one of
 * capability|code alongside type/protocol/controllerPublicKey and an optional
 * name, protocol 2, a base64url-87 controller key, a base64url-43 capability
 * or a six-digit code, and (if present) a string name. */
function roomRedeemVerdict(frame: Record<string, unknown>): string {
  const byCapability = frame.capability !== undefined;
  const byCode = frame.code !== undefined;
  const hasName = frame.name !== undefined;
  const keys = Object.keys(frame).sort().join(",");
  let keysOk = false;
  if (byCapability !== byCode) {
    if (byCapability) {
      keysOk = keys === (hasName
        ? "capability,controllerPublicKey,name,protocol,type"
        : "capability,controllerPublicKey,protocol,type");
    } else {
      keysOk = keys === (hasName
        ? "code,controllerPublicKey,name,protocol,type"
        : "code,controllerPublicKey,protocol,type");
    }
  }
  if (!keysOk) return "invalid_invite";
  if (frame.type !== "redeem") return "invalid_invite";
  if (frame.protocol !== 2) return "protocol_update_required";
  if (typeof frame.controllerPublicKey !== "string" ||
      !/^[A-Za-z0-9_-]{87}$/.test(frame.controllerPublicKey)) {
    return "invalid_controller_key";
  }
  if (frame.name !== undefined && typeof frame.name !== "string") {
    return "invalid_invite";
  }
  if (byCode) {
    if (typeof frame.code !== "string" || !/^\d{6}$/.test(frame.code)) {
      return "invalid_code";
    }
  } else if (typeof frame.capability !== "string" ||
             !/^[A-Za-z0-9_-]{43}$/.test(frame.capability)) {
    return "invalid_invite";
  }
  return "ok";
}

const publicKey =
  "BGsX0fLhLEJH-Lzm5WOkQPJ3A32BLeszoPShOUXYmMKWT-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU";
const capability = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_ABCDE";

describe("controller-page LAN trusted-origin gate", () => {
  it("trusts the LAN/loopback origin that served the page", () => {
    for (const host of [
      "http://192.168.1.5:8080/controller/",
      "http://10.0.0.2/controller/",
      "http://172.16.5.9/controller/",
      "http://172.31.255.254/controller/",
      "http://169.254.10.20/controller/",
      "http://127.0.0.1:8080/controller/",
      "http://localhost:8080/controller/",
      "http://host.localhost/controller/",
    ]) {
      // A plain-http LAN page is served in an insecure context.
      expect(atLocation(host, false)).toBe(true);
    }
    // Cloud stays exactly as before: an https secure context.
    expect(atLocation("https://play.example.com/controller/", true)).toBe(true);
  });

  it("refuses any foreign or public http origin (fail-closed)", () => {
    for (const host of [
      "http://evil.example.com/controller/",
      "http://play.example.com/controller/",
      "http://93.184.216.34/controller/",
      "http://8.8.8.8/controller/",
      "http://172.32.0.1/controller/",
      "http://172.15.0.1/controller/",
      "http://192.169.1.1/controller/",
      "http://11.0.0.1.evil.com/controller/",
    ]) {
      expect(atLocation(host, false)).toBe(false);
    }
  });

  it("refuses https without a secure context and any embedded credentials", () => {
    expect(atLocation("https://play.example.com/controller/", false)).toBe(false);
    expect(atLocation("https://user:pass@play.example.com/controller/", true))
      .toBe(false);
    expect(atLocation("http://user:pass@192.168.1.5/controller/", false))
      .toBe(false);
  });
});

describe("controller-page crypto.subtle tolerance (boot gate)", () => {
  it("accepts crypto.subtle when present on any origin", () => {
    vi.stubGlobal("crypto", {subtle: {}, getRandomValues() {}});
    vi.stubGlobal("isSecureContext", true);
    expect(internals.pairingCryptoAvailable()).toBe(true);
  });

  it("tolerates absent subtle only on an insecure (LAN) origin with RNG", () => {
    vi.stubGlobal("crypto", {getRandomValues() {}});
    vi.stubGlobal("isSecureContext", false);
    expect(internals.pairingCryptoAvailable()).toBe(true);
  });

  it("fails closed when subtle is absent on a secure origin", () => {
    vi.stubGlobal("crypto", {getRandomValues() {}});
    vi.stubGlobal("isSecureContext", true);
    expect(internals.pairingCryptoAvailable()).toBe(false);
  });

  it("fails closed when neither subtle nor getRandomValues exist", () => {
    vi.stubGlobal("crypto", {});
    vi.stubGlobal("isSecureContext", false);
    expect(internals.pairingCryptoAvailable()).toBe(false);
  });
});

describe("controller-page LAN-vs-cloud redeem selection", () => {
  it("selects the LAN ws path on a plain-http origin", () => {
    vi.stubGlobal("location", {protocol: "http:"});
    expect(internals.lanControllerMode()).toBe(true);
  });

  it("keeps the cloud POST path on an https origin", () => {
    vi.stubGlobal("location", {protocol: "https:"});
    expect(internals.lanControllerMode()).toBe(false);
  });
});

describe("controller-page LAN redeem-frame shape", () => {
  it("builds a capability redeem frame the room accepts", () => {
    const frame = internals.lanRedeemFrame(publicKey, {capability, name: "Ada"});
    expect(frame).toEqual({
      type: "redeem", protocol: 2, controllerPublicKey: publicKey,
      name: "Ada", capability,
    });
    expect(roomRedeemVerdict(frame)).toBe("ok");
  });

  it("builds a six-digit code redeem frame the room accepts", () => {
    const frame = internals.lanRedeemFrame(publicKey, {code: "012345", name: ""});
    expect(frame).toEqual({
      type: "redeem", protocol: 2, controllerPublicKey: publicKey,
      name: "", code: "012345",
    });
    expect(roomRedeemVerdict(frame)).toBe("ok");
    // Exactly one of capability|code -- never both, which the room refuses.
    expect(frame.capability).toBeUndefined();
  });

  it("normalizes the device name (strips control/bidi, caps length)", () => {
    const noisy = BIDI_OVERRIDE + "  Ada" + "x".repeat(40);
    const frame = internals.lanRedeemFrame(publicKey, {capability, name: noisy});
    const name = frame.name as string;
    expect(typeof name).toBe("string");
    expect([...name].length).toBeLessThanOrEqual(24);
    expect(name.includes(BIDI_OVERRIDE)).toBe(false);
    expect(name.includes(" ")).toBe(false);
    expect(name.startsWith("Ada")).toBe(true);
    expect(roomRedeemVerdict(frame)).toBe("ok");
  });
});
