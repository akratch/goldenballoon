import {afterEach, beforeAll, describe, expect, it, vi} from "vitest";
// Side-effect import: the shipped page script installs globalThis.MDKRPartySas.
import "../../../dist/web/party/party-sas.js";

/* SAS v2 oracle vectors, pinned byte-for-byte against the native half in
 * tests/test_native_party_sas.cpp. The host private scalar is 1, so the host
 * public key is the P-256 generator and the ECDH secret is the x-coordinate
 * of the controller public key. Both implementations must agree on the full
 * transcript:
 *   ECDH_secret ‖ "golden-balloon-party-sas-v2" ‖ 0x00 ‖ roomId ‖ 0x00 ‖
 *   hostPublicKey ‖ 0x00 ‖ controllerPublicKey ‖ 0x00 ‖ hostFingerprint ‖
 *   0x00 ‖ controllerFingerprint
 */
const roomId = "AAAAAAAAAAAAAAAAAAAAAA";
const hostPublicKey =
  "BGsX0fLhLEJH-Lzm5WOkQPJ3A32BLeszoPShOUXYmMKWT-NC4v4af5uO5-tKfA-eFivOM1drMV7Oy7ZAaDe_UfU";
const controllerPublicKey =
  "BHzyexiNA09-ilI4AwS1GsPAiWnid_IbNaYLSPxHZpl4B3dVENuO0EApPZrGn3Qw27p9reY86YIpngS3nSJ4c9E";
const hostFingerprint = "sha-256 " + Array.from({length: 32}, (_, index) =>
  index.toString(16).padStart(2, "0").toUpperCase()).join(":");
const controllerFingerprint = "sha-256 " + Array.from({length: 32}, (_, index) =>
  (0xe0 + index).toString(16).padStart(2, "0").toUpperCase()).join(":");

interface PageSas {
  createIdentity(): Promise<{privateKey: unknown; publicKey: string}>;
  phrase(privateKey: unknown, peerPublicKey: string,
         transcript: Record<string, string>): Promise<string>;
  sdpFingerprint(sdp: string): string;
  fallback: {
    sha256(bytes: Uint8Array): Uint8Array;
    ecdh(scalar: Uint8Array, peerPublicKey: Uint8Array): Uint8Array;
  };
}

const sas = (): PageSas => (globalThis as Record<string, any>).MDKRPartySas;

// The real Web Crypto, captured before any test hides it from the page code.
const realCrypto = globalThis.crypto;

function bytesFromBase64Url(value: string): Uint8Array {
  const padded = value.replace(/-/g, "+").replace(/_/g, "/") +
    "=".repeat((4 - (value.length % 4)) % 4);
  return Uint8Array.from(atob(padded), character => character.charCodeAt(0));
}

function base64UrlFromBytes(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function hexToBytes(hex: string): Uint8Array {
  return Uint8Array.from(hex.match(/../g)!.map(pair => parseInt(pair, 16)));
}

function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes, byte => byte.toString(16).padStart(2, "0")).join("");
}

// Scalar 1 as a 32-byte big-endian private key (the fallback's private form).
const hostScalarOne = (() => {
  const scalar = new Uint8Array(32);
  scalar[31] = 1;
  return scalar;
})();

/* Hide crypto.subtle from the page for the duration of `body`, so its phrase()
 * and createIdentity() take the pure-JS path. crypto.getRandomValues stays --
 * it is exposed on insecure origins, unlike subtle -- so key generation still
 * works. afterEach unstubs, restoring the oracle for the next test. */
async function withFallback<T>(body: () => Promise<T>): Promise<T> {
  vi.stubGlobal("crypto", {
    getRandomValues: (array: Uint8Array) => realCrypto.getRandomValues(array),
  });
  try {
    return await body();
  } finally {
    vi.unstubAllGlobals();
  }
}

afterEach(() => vi.unstubAllGlobals());

let hostPrivateKey: CryptoKey;

beforeAll(async () => {
  const point = bytesFromBase64Url(hostPublicKey);
  hostPrivateKey = await realCrypto.subtle.importKey("jwk", {
    kty: "EC", crv: "P-256",
    x: base64UrlFromBytes(point.slice(1, 33)),
    y: base64UrlFromBytes(point.slice(33, 65)),
    d: base64UrlFromBytes(hostScalarOne),
  }, {name: "ECDH", namedCurve: "P-256"}, false, ["deriveBits"]);
});

function transcript(overrides: Record<string, string> = {}) {
  return {roomId, hostPublicKey, controllerPublicKey,
    hostFingerprint, controllerFingerprint, ...overrides};
}

describe("controller-page SAS v2 (dist/web/party/party-sas.js)", () => {
  it("derives the pinned v2 phrase from the shared oracle transcript", async () => {
    await expect(sas().phrase(hostPrivateKey, controllerPublicKey, transcript()))
      .resolves.toBe("Gentle-Star Royal-Pilot");
  });

  it("moves the words when the two fingerprints swap roles", async () => {
    await expect(sas().phrase(hostPrivateKey, controllerPublicKey, transcript({
      hostFingerprint: controllerFingerprint,
      controllerFingerprint: hostFingerprint,
    }))).resolves.toBe("Mighty-Kite Wild-Kite");
  });

  it("fails closed instead of deriving without both fingerprints", async () => {
    for (const overrides of [
      {hostFingerprint: ""}, {controllerFingerprint: ""},
      {hostFingerprint: "", controllerFingerprint: ""},
    ]) {
      await expect(sas().phrase(hostPrivateKey, controllerPublicKey,
        transcript(overrides))).rejects.toThrow();
    }
  });

  it("canonicalizes SDP fingerprints exactly like the native transport", () => {
    const canonical = sas().sdpFingerprint;
    const lower = hostFingerprint.replace(/([0-9A-F]{2})/g,
      match => match.toLowerCase());
    const sdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\n" +
      `a=fingerprint:${lower}\r\n` +
      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n" +
      `a=fingerprint:${lower}\r\n`;
    // Hex uppercased, algorithm token verbatim, agreeing repeats collapse.
    expect(canonical(sdp)).toBe(hostFingerprint);
    // The algorithm token is never case-folded.
    expect(canonical("a=fingerprint:sHa-256 ab:cd\n")).toBe("sHa-256 AB:CD");
    // Token runs of spaces and tabs re-join with a single space.
    expect(canonical("a=fingerprint:sha-256 \t ab:cd\n")).toBe("sha-256 AB:CD");
    // Lines that do not BEGIN with the attribute do not count.
    expect(canonical("  a=fingerprint:sha-256 ab:cd\n")).toBe("");
    // Refusals: missing, valueless, malformed hex, extra token, disagreement.
    expect(canonical("v=0\r\n")).toBe("");
    expect(canonical("a=fingerprint:sha-256\n")).toBe("");
    expect(canonical("a=fingerprint:sha-256 ab:c\n")).toBe("");
    expect(canonical("a=fingerprint:sha-256 ab:cg\n")).toBe("");
    expect(canonical("a=fingerprint:sha-256 ab::cd\n")).toBe("");
    expect(canonical("a=fingerprint:sha-256 ab:cd extra\n")).toBe("");
    expect(canonical(
      "a=fingerprint:sha-256 ab:cd\na=fingerprint:sha-256 ab:ce\n")).toBe("");
    // One refusing line poisons the whole description even beside a good one.
    expect(canonical(
      "a=fingerprint:sha-256 ab:cd\na=fingerprint:sha-256 ab:cg\n")).toBe("");
  });
});

/* The insecure-origin path: crypto.subtle is absent on a plain-http LAN page,
 * so party-sas.js falls back to its own SHA-256 + P-256 ECDH. That fallback
 * must produce byte-identical phrases to the native mbedTLS host, or every
 * pairing looks like a MITM. These tests hide crypto.subtle to force it. */
describe("insecure-origin JS fallback", () => {
  it("SHA-256 reproduces the FIPS 180-4 vectors", () => {
    const encoder = new TextEncoder();
    expect(bytesToHex(sas().fallback.sha256(encoder.encode("abc")))).toBe(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect(bytesToHex(sas().fallback.sha256(new Uint8Array(0)))).toBe(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // A message that straddles the 64-byte block boundary (padding path).
    expect(bytesToHex(sas().fallback.sha256(encoder.encode(
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))).toBe(
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  });

  it("P-256 ECDH reproduces the published NIST CAVP vector", () => {
    // NIST CAVP ECC CDH primitive, P-256 test case #1.
    const scalar = hexToBytes(
      "7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534");
    const peer = new Uint8Array(65);
    peer[0] = 4;
    peer.set(hexToBytes(
      "700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287"), 1);
    peer.set(hexToBytes(
      "db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac"), 33);
    expect(bytesToHex(sas().fallback.ecdh(scalar, peer))).toBe(
      "46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b");
  });

  it("reproduces the 5a oracle vectors without crypto.subtle", async () => {
    await withFallback(async () => {
      expect(globalThis.crypto.subtle).toBeUndefined();
      await expect(sas().phrase(hostScalarOne, controllerPublicKey, transcript()))
        .resolves.toBe("Gentle-Star Royal-Pilot");
      await expect(sas().phrase(hostScalarOne, controllerPublicKey, transcript({
        hostFingerprint: controllerFingerprint,
        controllerFingerprint: hostFingerprint,
      }))).resolves.toBe("Mighty-Kite Wild-Kite");
    });
  });

  it("fails closed without both fingerprints on the fallback path too", async () => {
    await withFallback(async () => {
      for (const overrides of [
        {hostFingerprint: ""}, {controllerFingerprint: ""},
        {hostFingerprint: "", controllerFingerprint: ""},
      ]) {
        await expect(sas().phrase(hostScalarOne, controllerPublicKey,
          transcript(overrides))).rejects.toThrow();
      }
    });
  });

  it("refuses a peer key that is not a point on the curve", async () => {
    // Flip a byte of the on-curve controller key: no longer satisfies the
    // curve equation, so the fallback ECDH must refuse rather than derive.
    const bad = bytesFromBase64Url(controllerPublicKey);
    bad[40] ^= 0x01;
    const badKey = base64UrlFromBytes(bad);
    await withFallback(async () => {
      await expect(sas().phrase(hostScalarOne, badKey, transcript({
        controllerPublicKey: badKey,
      }))).rejects.toThrow();
    });
  });

  it("equals the crypto.subtle path on random keys (dual-implementation)", async () => {
    for (let round = 0; round < 24; round++) {
      // Random host and controller keypairs from the vetted engine crypto.
      const hostPair = await realCrypto.subtle.generateKey(
        {name: "ECDH", namedCurve: "P-256"}, true, ["deriveBits"]);
      const controllerPair = await realCrypto.subtle.generateKey(
        {name: "ECDH", namedCurve: "P-256"}, true, ["deriveBits"]);
      const hostPubBytes = new Uint8Array(
        await realCrypto.subtle.exportKey("raw", hostPair.publicKey));
      const controllerPubBytes = new Uint8Array(
        await realCrypto.subtle.exportKey("raw", controllerPair.publicKey));
      const hostPub = base64UrlFromBytes(hostPubBytes);
      const controllerPub = base64UrlFromBytes(controllerPubBytes);
      const hostJwk = await realCrypto.subtle.exportKey("jwk", hostPair.privateKey);
      const hostScalar = new Uint8Array(32);
      const rawScalar = bytesFromBase64Url(hostJwk.d as string);
      hostScalar.set(rawScalar, 32 - rawScalar.length);

      const roundTranscript = transcript({
        roomId: base64UrlFromBytes(realCrypto.getRandomValues(new Uint8Array(16))),
        hostPublicKey: hostPub,
        controllerPublicKey: controllerPub,
        // Fresh, distinct fingerprints so the transcript is fully exercised.
        hostFingerprint: "sha-256 " + bytesToHex(
          realCrypto.getRandomValues(new Uint8Array(32))).toUpperCase()
          .replace(/(..)(?=.)/g, "$1:"),
        controllerFingerprint: "sha-256 " + bytesToHex(
          realCrypto.getRandomValues(new Uint8Array(32))).toUpperCase()
          .replace(/(..)(?=.)/g, "$1:"),
      });

      // Oracle: the secure path, deriving from the host CryptoKey.
      const secure = await sas().phrase(
        hostPair.privateKey, controllerPub, roundTranscript);
      // Fallback: the same math with subtle hidden, deriving from the scalar.
      const fallback = await withFallback(() =>
        sas().phrase(hostScalar, controllerPub, roundTranscript));
      expect(fallback).toBe(secure);
    }
  });

  it("mints a usable identity (public key + working phrase) without subtle",
    async () => {
    await withFallback(async () => {
      const identity = await sas().createIdentity();
      expect(identity.publicKey).toMatch(/^[A-Za-z0-9_-]{87}$/);
      // The minted key must ride the transcript and produce a stable phrase.
      const words = await sas().phrase(identity.privateKey, controllerPublicKey,
        transcript({hostPublicKey: identity.publicKey}));
      expect(words).toMatch(/^[A-Za-z]+-[A-Za-z]+ [A-Za-z]+-[A-Za-z]+$/);
    });
  });
});
