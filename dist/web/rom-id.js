// rom-id.js — WHICH Diddy Kong Racing ROM did the user pick, and can this build
// run it? The browser-side MIRROR of platform/rom_id.c.
//
// This is a standalone script (no imports, no DOM) for two reasons:
//   1. the shell loads it with a plain <script> tag before mdkr64-shell.js, and
//   2. tests/check_rom_revision.py loads it under node and runs the SAME
//      synthetic ROM headers through it and through the native binary, then
//      requires identical verdicts. That check is what keeps this file and
//      rom_id.c from drifting; "mirrors rom_id.c" is otherwise just a comment
//      that rots. Edit both files or neither.
//
// Read platform/rom_id.c for the full reasoning: what identifies a revision, why
// the CRC pair beats the header fields, and why two of the five released revisions
// are accepted and three refused. The short version is that all five DKR revisions
// pass a size + magic + "title contains diddy" gate while keeping their asset
// lookup table at a different ROM offset, so that gate let a European or Japanese
// cart through into a boot that SIGSEGV'd before the first frame.
// docs/ROM_REVISIONS.md has the measured per-revision behaviour.

"use strict";

const DKR_ROM_SIZE = 12 * 1024 * 1024; // 0xC00000 — every DKR revision

// The five released revisions.
//   name/country/revision/crc  <- the decomp's own src/hasm/header.s, which emits
//                                 them per VERSION_*. Verified against all five
//                                 real ROMs (each sha1 also matches the decomp's
//                                 ver/verification/dkr.<build>.sha1).
//   lutStart/lutEnd            <- the decomp's ver/splat/dkr.<build>.yaml
//                                 `assets.lut` and `assets` segment starts. Every
//                                 revision puts the asset table somewhere else,
//                                 which is the whole reason identification matters.
// Decompilation metadata, not ROM content: nothing here comes from a ROM body.
// MIRRORS the kDkrRevisions table in platform/rom_id.c — keep the two identical;
// tests/check_rom_revision.py fails if they drift.
const DKR_REVISIONS = [
  { name: "US 1.0 (NTSC-U, Rev 0)",    build: "us.v77",  country: "E", revision: 0x0, crc1: 0x53D440E7, crc2: 0x7519B011, lutStart: 0x0ECB60, lutEnd: 0x0ECC30, romEnd: 0xAC9630, supported: false },
  { name: "European (PAL, Rev 0)",     build: "pal.v77", country: "P", revision: 0x0, crc1: 0xFD73F775, crc2: 0x9724755A, lutStart: 0x0ECBF0, lutEnd: 0x0ECCC0, romEnd: 0xAC96C0, supported: false },
  { name: "Japanese (NTSC-J)",         build: "jpn.v79", country: "J", revision: 0x0, crc1: 0x7435C9BB, crc2: 0x39763CF4, lutStart: 0x0EE5D0, lutEnd: 0x0EE6A0, romEnd: 0xB8E4E0, supported: false },
  { name: "US 1.1 (NTSC-U, Rev 1)",    build: "us.v80",  country: "E", revision: 0x1, crc1: 0xE402430D, crc2: 0xD2FCFC9D, lutStart: 0x0ED0E0, lutEnd: 0x0ED1B0, romEnd: 0xB8CFD0, supported: true  },
  { name: "European 1.1 (PAL, Rev 1)", build: "pal.v80", country: "P", revision: 0x1, crc1: 0x596E145B, crc2: 0xF7D9879F, lutStart: 0x0ED170, lutEnd: 0x0ED240, romEnd: 0xB8D060, supported: true  },
];

// "US 1.1 (NTSC-U, Rev 1) (us.v80) and European 1.1 (PAL, Rev 1) (pal.v80)"
function dkrSupportedList() {
  return DKR_REVISIONS.filter((r) => r.supported)
                      .map((r) => `${r.name} (${r.build})`).join(" and ");
}

// Header field offsets (N64 ROM header, big-endian).
const HDR_CRC1 = 0x10, HDR_CRC2 = 0x14;
const HDR_TITLE = 0x20, HDR_TITLE_LEN = 20;
const HDR_MEDIA = 0x3b, HDR_CART_ID = 0x3c, HDR_COUNTRY = 0x3e, HDR_VERSION = 0x3f;

function rdBe32(b, off) {
  // >>> 0 keeps it unsigned: b[off] << 24 is negative for a high bit set.
  return ((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]) >>> 0;
}

function hex8(v) {
  return (v >>> 0).toString(16).toUpperCase().padStart(8, "0");
}

// Convert a .v64 (16-bit byteswapped) or .n64 (32-bit little-endian) image IN
// PLACE to .z64 (big-endian) order. Returns "z64"/"v64"/"n64", or null when the
// magic is not an N64 ROM in any of the three orders.
//
// The engine (platform/rom_io.c) does this too, so this is belt-and-braces —
// but doing it here means the copy PERSISTED to IDBFS is canonical .z64, and it
// is what lets the code below read the header fields at all.
function dkrNormalizeByteOrder(bytes) {
  if (!bytes || bytes.length < 0x40) return null;
  const b0 = bytes[0], b1 = bytes[1], b2 = bytes[2], b3 = bytes[3];
  let order = "z64";
  if (b0 === 0x37 && b1 === 0x80 && b2 === 0x40 && b3 === 0x12) {
    for (let i = 0; i + 1 < bytes.length; i += 2) {
      const t = bytes[i]; bytes[i] = bytes[i + 1]; bytes[i + 1] = t;
    }
    order = "v64";
  } else if (b0 === 0x40 && b1 === 0x12 && b2 === 0x37 && b3 === 0x80) {
    for (let i = 0; i + 3 < bytes.length; i += 4) {
      const a = bytes[i], b = bytes[i + 1], c = bytes[i + 2], d = bytes[i + 3];
      bytes[i] = d; bytes[i + 1] = c; bytes[i + 2] = b; bytes[i + 3] = a;
    }
    order = "n64";
  }
  if (bytes[0] !== 0x80 || bytes[1] !== 0x37 || bytes[2] !== 0x12 || bytes[3] !== 0x40) {
    return null;
  }
  return order;
}

// Classify a ROM from its header. `bytes` must already be in .z64 order.
// Verdicts mirror DkrRomVerdict in platform/rom_id.h.
function dkrIdentifyRom(bytes) {
  const id = {
    verdict: "not-dkr", revisionName: null, decompBuild: null,
    refCrc1: 0, refCrc2: 0, romEnd: 0,
    gameCode: "", title: "", version: 0, crc1: 0, crc2: 0, matchedByCrc: false,
  };
  if (!bytes || bytes.length < 0x40) return id;

  id.crc1 = rdBe32(bytes, HDR_CRC1);
  id.crc2 = rdBe32(bytes, HDR_CRC2);
  id.version = bytes[HDR_VERSION];
  id.gameCode = String.fromCharCode(bytes[HDR_MEDIA], bytes[HDR_CART_ID],
                                    bytes[HDR_CART_ID + 1], bytes[HDR_COUNTRY]);
  let title = "";
  for (let i = 0; i < HDR_TITLE_LEN; i++) {
    const c = bytes[HDR_TITLE + i];
    // Keep it printable: a non-DKR ROM's title is arbitrary bytes and this
    // string is shown straight back to the user.
    title += (c >= 0x20 && c < 0x7f) ? String.fromCharCode(c) : "?";
  }
  id.title = title;

  // 1. CRC pair — decisive when it matches, because it covers the ROM body.
  let row = DKR_REVISIONS.find((r) => r.crc1 === id.crc1 && r.crc2 === id.crc2) || null;
  if (row) {
    id.matchedByCrc = true;
  } else {
    // 2. Header fields, for a modified or self-built ROM whose CRCs differ.
    if (bytes[HDR_MEDIA] !== 0x4e /* 'N' */ ||
        bytes[HDR_CART_ID] !== 0x44 /* 'D' */ ||
        bytes[HDR_CART_ID + 1] !== 0x59 /* 'Y' */) {
      id.verdict = "not-dkr";
      return id;
    }
    const country = String.fromCharCode(bytes[HDR_COUNTRY]);
    const revision = bytes[HDR_VERSION] & 0x0f; // high nibble is savetype
    row = DKR_REVISIONS.find((r) => r.country === country && r.revision === revision) || null;
    if (!row) {
      id.verdict = "unknown-revision";
      return id;
    }
  }

  id.revisionName = row.name;
  id.decompBuild = row.build;
  id.refCrc1 = row.crc1;
  id.refCrc2 = row.crc2;
  id.romEnd = row.romEnd;
  id.verdict = row.supported ? "supported" : "other-revision";
  return id;
}

// The user-facing sentence for `id`. Mirrors dkr_rom_describe() in rom_id.c
// character for character — tests/check_rom_revision.py compares the two.
//
// "Character for character" includes the punctuation: the native strings use an
// ASCII hyphen, not U+2014, because U+2014 mojibakes on a Windows console. Any
// dash inside a sentence BELOW that also exists in rom_id.c/rom_io.c must stay
// ASCII, or the two sides differ by one byte and the gate fails. Em-dashes in
// the comments and in browser-only DOM strings are fine — this rule is only
// about text that has a native twin.
function dkrDescribeRom(id, name) {
  const p = name || "that file";
  const supported = dkrSupportedList();
  switch (id.verdict) {
    case "supported":
      if (id.matchedByCrc) {
        return `${p} is ${id.revisionName} (${id.decompBuild}) - a revision this build supports.`;
      }
      return `${p} has a ${id.revisionName} header (${id.decompBuild}) but its CRC1/CRC2 ` +
             `(0x${hex8(id.crc1)} / 0x${hex8(id.crc2)}) do not match that revision's reference ` +
             `pair (0x${hex8(id.refCrc1)} / 0x${hex8(id.refCrc2)}) - a modified or imperfect ` +
             `dump. Continuing anyway.`;
    case "other-revision":
      return `${p} is the ${id.revisionName} release of Diddy Kong Racing (decomp build ` +
             `${id.decompBuild}), which this build does not support: it is compiled for ` +
             `${supported}. Other revisions are refused rather than run because they are ` +
             `unvalidated against this build's asset assumptions - see docs/ROM_REVISIONS.md.`;
    case "unknown-revision":
      return `${p} is a Diddy Kong Racing cartridge (game code "${id.gameCode}", version ` +
             `0x${id.version.toString(16).toUpperCase().padStart(2, "0")}) but not a revision ` +
             `this build recognises; it supports ${supported}.`;
    default:
      return `${p} is an N64 ROM but not Diddy Kong Racing (game code "${id.gameCode}", ` +
             `internal title "${id.title}"); this build supports ${supported}.`;
  }
}

// The whole gate, in the same order as platform/rom_io.c platformInitRom():
// size -> byte order (converted in place) -> revision.
//
// Returns { error, warning, order, id }. `error` non-null means refuse; the
// caller must not boot. `warning` non-null means accept and say so.
function dkrValidateRom(bytes, name) {
  const p = name || "that file";
  if (!bytes || bytes.length !== DKR_ROM_SIZE) {
    const mb = bytes ? (bytes.length / 1048576).toFixed(1) : 0;
    return {
      error: `That file is ${mb} MB; a Diddy Kong Racing ROM must be exactly 12 MB ` +
             `(.z64, .v64 or .n64). Wrong game, headered dump, or truncated file.`,
      warning: null, order: null, id: null,
    };
  }
  const order = dkrNormalizeByteOrder(bytes);
  if (order === null) {
    const m = [0, 1, 2, 3].map((i) => bytes[i].toString(16).padStart(2, "0")).join(" ");
    return {
      error: `${p} is not an N64 ROM - its first four bytes are ${m}, which is none of ` +
             `.z64 (80 37 12 40), .v64 (37 80 40 12) or .n64 (40 12 37 80).`,
      warning: null, order: null, id: null,
    };
  }
  const id = dkrIdentifyRom(bytes);
  const msg = dkrDescribeRom(id, p);
  if (id.verdict !== "supported") {
    return { error: msg, warning: null, order, id };
  }
  return { error: null, warning: id.matchedByCrc ? null : msg, order, id };
}

// Exported for tests/check_rom_revision.py, which runs this file under node and
// compares its verdicts against the native binary's. Harmless in a browser.
if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    DKR_REVISIONS, DKR_ROM_SIZE, dkrSupportedList,
    dkrNormalizeByteOrder, dkrIdentifyRom, dkrDescribeRom, dkrValidateRom,
  };
}
