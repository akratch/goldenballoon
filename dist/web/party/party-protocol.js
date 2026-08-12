// Phone Party pad-state protocol v1. Mirrors platform/party/party_protocol.c.
// Dependency-free and usable from a controller page, host Worker, or Node gate.
(function (root, factory) {
  "use strict";
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  root.MDKRPartyProtocol = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const VERSION = 1;
  const TYPE_STATE = 1;
  const MAX_EDGES = 8;
  const FIXED_BYTES = 24;
  const EDGE_BYTES = 5;
  const MAX_BYTES = FIXED_BYTES + MAX_EDGES * EDGE_BYTES;
  const FLAG_PRESENT = 0x01;
  const FLAG_NEUTRAL = 0x02;
  const FLAG_HAS_EDGES = 0x04;
  const FLAG_MASK = 0x07;
  const LEGAL_BUTTONS = 0xff3f;

  class PartyProtocolError extends Error {
    constructor(code) {
      super("Phone Party pad packet rejected: " + code);
      this.name = "PartyProtocolError";
      this.code = code;
    }
  }

  function fail(code) { throw new PartyProtocolError(code); }
  function encodedSize(count) {
    return Number.isInteger(count) && count >= 0 && count <= MAX_EDGES
      ? FIXED_BYTES + count * EDGE_BYTES : 0;
  }

  function crc16(bytes, length = bytes && bytes.byteLength) {
    if (!(bytes instanceof Uint8Array) || !Number.isInteger(length) ||
        length < 0 || length > bytes.byteLength) fail("crc-input");
    let crc = 0xffff;
    for (let index = 0; index < length; index++) {
      crc ^= bytes[index] << 8;
      for (let bit = 0; bit < 8; bit++) {
        crc = (crc & 0x8000) !== 0
          ? ((crc << 1) ^ 0x1021) & 0xffff
          : (crc << 1) & 0xffff;
      }
    }
    return crc;
  }

  function finiteInteger(value, low, high, code) {
    if (!Number.isInteger(value) || value < low || value > high) fail(code);
    return value;
  }

  function sample(value, codePrefix) {
    if (!value || typeof value !== "object") fail(codePrefix + "-sample");
    const buttons = finiteInteger(value.buttons, 0, 0xffff, codePrefix + "-buttons");
    if ((buttons & ~LEGAL_BUTTONS) !== 0) fail(codePrefix + "-buttons");
    const stickX = finiteInteger(value.stickX, -80, 80, codePrefix + "-stick");
    const stickY = finiteInteger(value.stickY, -80, 80, codePrefix + "-stick");
    return {buttons, stickX, stickY};
  }

  function validate(packet) {
    if (!packet || typeof packet !== "object") fail("packet");
    const flags = finiteInteger(packet.flags, 0, 0xff, "flags");
    if ((flags & ~FLAG_MASK) !== 0) fail("flags");
    const current = sample(packet, "current");
    const edges = Array.isArray(packet.edges) ? packet.edges : [];
    if (edges.length > MAX_EDGES) fail("edge-count");
    if (((flags & FLAG_HAS_EDGES) !== 0) !== (edges.length !== 0)) fail("flags");
    if ((flags & FLAG_NEUTRAL) !== 0 &&
        (current.buttons !== 0 || current.stickX !== 0 || current.stickY !== 0)) {
      fail("neutral");
    }
    if ((flags & FLAG_PRESENT) === 0 &&
        ((flags & FLAG_NEUTRAL) === 0 || current.buttons !== 0 ||
         current.stickX !== 0 || current.stickY !== 0)) fail("neutral");
    let previous = 128;
    const checkedEdges = edges.map((edge) => {
      const sequenceDelta = finiteInteger(
        edge && edge.sequenceDelta, 1, 127, "edge-sequence");
      if (sequenceDelta >= previous) fail("edge-sequence");
      previous = sequenceDelta;
      return {sequenceDelta, ...sample(edge, "edge")};
    });
    return {
      flags,
      connectionSequence: finiteInteger(
        packet.connectionSequence, 0, 0xffffffff, "connection-sequence") >>> 0,
      sampleSequence: finiteInteger(
        packet.sampleSequence, 0, 0xffffffff, "sample-sequence") >>> 0,
      senderTimeMs: finiteInteger(
        packet.senderTimeMs, 0, 0xffffffff, "sender-time") >>> 0,
      ...current,
      edges: checkedEdges,
    };
  }

  function encode(packet) {
    const value = validate(packet);
    const bytes = new Uint8Array(encodedSize(value.edges.length));
    const view = new DataView(bytes.buffer);
    bytes[0] = 0x47; bytes[1] = 0x42;
    bytes[2] = VERSION; bytes[3] = TYPE_STATE; bytes[4] = value.flags;
    view.setUint32(5, value.connectionSequence, false);
    view.setUint32(9, value.sampleSequence, false);
    view.setUint32(13, value.senderTimeMs, false);
    view.setUint16(17, value.buttons, false);
    view.setInt8(19, value.stickX); view.setInt8(20, value.stickY);
    bytes[21] = value.edges.length;
    let offset = 22;
    for (const edge of value.edges) {
      bytes[offset++] = edge.sequenceDelta;
      view.setUint16(offset, edge.buttons, false); offset += 2;
      view.setInt8(offset++, edge.stickX);
      view.setInt8(offset++, edge.stickY);
    }
    view.setUint16(offset, crc16(bytes, offset), false);
    return bytes;
  }

  function decode(input) {
    const source = input instanceof Uint8Array
      ? input : input instanceof ArrayBuffer ? new Uint8Array(input) : null;
    if (!source) fail("input");
    // Copy before parsing: a concurrently reused DataChannel buffer cannot
    // change fields between validation and the returned immutable snapshot.
    const bytes = source.slice();
    if (bytes.byteLength < FIXED_BYTES || bytes.byteLength > MAX_BYTES) fail("length");
    if (bytes[0] !== 0x47 || bytes[1] !== 0x42) fail("magic");
    if (bytes[2] !== VERSION) fail("version");
    if (bytes[3] !== TYPE_STATE) fail("type");
    const edgeCount = bytes[21];
    if (encodedSize(edgeCount) !== bytes.byteLength) fail("edge-count");
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const checksumOffset = bytes.byteLength - 2;
    if (view.getUint16(checksumOffset, false) !== crc16(bytes, checksumOffset)) {
      fail("checksum");
    }
    let offset = 22;
    const edges = [];
    for (let index = 0; index < edgeCount; index++) {
      edges.push({
        sequenceDelta: bytes[offset++],
        buttons: view.getUint16(offset, false),
        stickX: view.getInt8(offset + 2),
        stickY: view.getInt8(offset + 3),
      });
      offset += 4;
    }
    return Object.freeze(validate({
      flags: bytes[4],
      connectionSequence: view.getUint32(5, false),
      sampleSequence: view.getUint32(9, false),
      senderTimeMs: view.getUint32(13, false),
      buttons: view.getUint16(17, false),
      stickX: view.getInt8(19),
      stickY: view.getInt8(20),
      edges,
    }));
  }

  return Object.freeze({
    VERSION, TYPE_STATE, MAX_EDGES, FIXED_BYTES, EDGE_BYTES, MAX_BYTES,
    FLAG_PRESENT, FLAG_NEUTRAL, FLAG_HAS_EDGES, LEGAL_BUTTONS,
    PartyProtocolError, encodedSize, crc16, encode, decode,
  });
});
