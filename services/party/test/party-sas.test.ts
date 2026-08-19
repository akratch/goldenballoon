import {beforeAll, describe, expect, it} from "vitest";
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
  createIdentity(): Promise<{privateKey: CryptoKey; publicKey: string}>;
  phrase(privateKey: CryptoKey, peerPublicKey: string,
         transcript: Record<string, string>): Promise<string>;
  sdpFingerprint(sdp: string): string;
}

const sas = (): PageSas => (globalThis as Record<string, any>).MDKRPartySas;

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

let hostPrivateKey: CryptoKey;

beforeAll(async () => {
  const point = bytesFromBase64Url(hostPublicKey);
  const scalar = new Uint8Array(32);
  scalar[31] = 1;
  hostPrivateKey = await crypto.subtle.importKey("jwk", {
    kty: "EC", crv: "P-256",
    x: base64UrlFromBytes(point.slice(1, 33)),
    y: base64UrlFromBytes(point.slice(33, 65)),
    d: base64UrlFromBytes(scalar),
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
