// mdkr64-shell.js — browser launcher for the mdkr64 WebGPU/wasm engine.
//
// Flow: feature-detect WebGPU -> let the user pick their .z64 (validated and
// byte-order-normalised by rom-id.js, which index.html loads first) ->
// instantiate the wasm module -> mount IDBFS at /rom (ROM persists across
// reloads) and /save (EEPROM persists) -> write the ROM into MEMFS/IDBFS ->
// callMain(--rom ...).
// The engine reads the whole ROM at boot (rom_io.c) and drives its own frame
// loop, suspending to requestAnimationFrame via Asyncify at each frame boundary.
//
// No ROM is distributed with this page; the user supplies their own. Everything
// stays in the browser — there is no server to upload to.

"use strict";

// ---- Visual-only: make :active feedback reliable on iOS Safari ------------
// iOS Safari only applies the CSS :active pseudo-class to a tapped element
// while at least one ancestor has a touchstart listener; without one, taps
// on the plain launcher buttons (Play, Forget, Download backup, ...) skip
// straight from default to hover-ish state with no visible press. This is
// the standard, well-documented no-op fix. It does not read or react to any
// touch data, does not call preventDefault, and is entirely independent of
// the touch-pad pointer lifecycle below (which drives its own "is-pressed"
// classes directly and never relies on :active). Zero effect on gameplay.
document.addEventListener("touchstart", () => {}, { passive: true });

// The ROM is always written here in canonical .z64 order — validateRom() below
// converts a .v64/.n64 pick in place before it is persisted, so the name is
// accurate rather than aspirational. Size and revision live in rom-id.js.
const ROM_PATH = "/rom/baserom.us.v80.z64";
const $ = (id) => document.getElementById(id);

let romBytes = null;     // freshly-picked ROM bytes (null once written to FS)
let selectedRomName = ""; // user-facing name for an unpersisted valid pick
let storedRomAvailable = false; // true only after full-image validation
let romSessionOnlyWarning = ""; // visible if a fresh pick could not reach IDBFS
let romPersistencePending = false; // a retry can promote this in-memory ROM
let romStorageMounted = false;
let romSelectionEpoch = 0; // latest user pick wins across async read/hash work

// #forget is the documented recovery control for a stored ROM this browser can
// no longer boot from. It must therefore be reachable from the moment a stored
// copy can exist — which includes the session that CREATED it, not only a
// later reload that happens to run the page-load probe. Every path that learns
// a stored copy exists (or might exist) goes through here; only the confirmed
// erase hides it again.
function revealForgetControl() {
  const forget = $("forget");
  if (forget) forget.hidden = false;
}

function setStoredRomAvailable(available) {
  storedRomAvailable = available;
  if (available) revealForgetControl();
}
let module = null;       // the instantiated engine Module
let booted = false;
let savedOnce = false;

// One plain object is shared with the wasm input sampler. Pointer callbacks only
// mutate browser state; C reads one coherent snapshot at its ordinary per-frame
// input boundary, avoiding Asyncify re-entry while main() is suspended.
const touchPadState = {
  enabled: false,
  buttons: 0,
  stickX: 0,
  stickY: 0,
  events: [],
  overflow: 0,
  sequence: 0,
};

const TOUCH_EDGE_CAPACITY = 128;
let lastTouchSnapshot = null;

function publishTouchPad() {
  const snapshot = {
    enabled: touchPadState.enabled !== false,
    buttons: Number(touchPadState.buttons) >>> 0,
    stickX: Number(touchPadState.stickX) | 0,
    stickY: Number(touchPadState.stickY) | 0,
    sequence: ++touchPadState.sequence,
  };
  const changed = !lastTouchSnapshot ||
    snapshot.enabled !== lastTouchSnapshot.enabled ||
    snapshot.buttons !== lastTouchSnapshot.buttons ||
    snapshot.stickX !== lastTouchSnapshot.stickX ||
    snapshot.stickY !== lastTouchSnapshot.stickY;
  if (changed) {
    if (touchPadState.events.length >= TOUCH_EDGE_CAPACITY) {
      // A bounded overflow must never strand an old press. Retire the backlog
      // with an explicit neutral sample; the next changed pointer state can
      // enqueue normally and the counter makes the loss observable.
      touchPadState.events.length = 0;
      touchPadState.events.push({
        enabled: snapshot.enabled,
        buttons: 0,
        stickX: 0,
        stickY: 0,
        sequence: snapshot.sequence,
        overflow: true,
      });
      touchPadState.overflow++;
      lastTouchSnapshot = {
        enabled: snapshot.enabled,
        buttons: 0,
        stickX: 0,
        stickY: 0,
      };
    } else {
      touchPadState.events.push(snapshot);
      lastTouchSnapshot = snapshot;
    }
  }
  if (module) module.__mdkrTouchPad = touchPadState;
}

function clearTouchPad() {
  touchPadState.buttons = 0;
  touchPadState.stickX = 0;
  touchPadState.stickY = 0;
  publishTouchPad();
}

// ---- Browser regression bridge -------------------------------------------
// A CDP test can install this object with Page.addScriptToEvaluateOnNewDocument
// before any page code runs. Nothing is read from the URL and no test asset is
// shipped: the fixture text remains in the test process and is written into the
// module's private MEMFS only after the user-supplied ROM has passed the normal
// picker/identity gate. Ordinary visitors never create this object, so this path
// is inert in production.
const testConfig = (() => {
  const value = globalThis.__mdkrTestConfig;
  return value && typeof value === "object" ? value : null;
})();
const testState = testConfig ? {
  phase: "shell-loaded",
  exitCode: null,
  abortReason: null,
  errors: [],
  storage: null,
  module: null,
} : null;
if (testState) globalThis.__mdkrTestState = testState;

// The engine's sole browser frame boundary calls this instead of reaching rAF
// directly. Normal visitors receive the real DOMHighResTimeStamp. The browser
// schedule gate may supply bounded synthetic deltas while still yielding once
// to the real event loop per opportunity. Hidden documents wait for visibility
// instead of doing expensive 1 Hz background replays; the C clock rebases the
// resumed interval.
const testRafDeltas = (() => {
  const values = testConfig && testConfig.rafDeltasMs;
  if (!Array.isArray(values) || !values.length || values.length > 256) return null;
  const parsed = values.map(Number);
  return parsed.every((value) => Number.isFinite(value) && value > 0 && value <= 250)
    ? parsed : null;
})();
const testRafDeltasNs = (() => {
  const values = testConfig && testConfig.rafDeltasNs;
  if (!Array.isArray(values) || !values.length || values.length > 256) return null;
  const parsed = values.map(Number);
  return parsed.every((value) => Number.isSafeInteger(value) &&
      value > 0 && value <= 250000000) ? parsed : null;
})();
let testRafIndex = 0;
let testRafNow = null;
let testActualRafLast = null;
globalThis.__mdkrActualRafDeltas = testConfig ? [] : null;
globalThis.__mdkrLastAnimationFrameDeltaNs = 0;
globalThis.__mdkrSyntheticAnimationFrameClock =
  testRafDeltas !== null || testRafDeltasNs !== null;
globalThis.__mdkrWaitAnimationFrame = async function () {
  while (document.visibilityState === "hidden") {
    await new Promise((resolve) => {
      const visible = () => {
        if (document.visibilityState !== "hidden") {
          document.removeEventListener("visibilitychange", visible);
          resolve();
        }
      };
      document.addEventListener("visibilitychange", visible);
    });
  }
  const actual = await new Promise((resolve) => requestAnimationFrame(resolve));
  if (globalThis.__mdkrActualRafDeltas) {
    if (testActualRafLast !== null) {
      globalThis.__mdkrActualRafDeltas.push(actual - testActualRafLast);
      if (globalThis.__mdkrActualRafDeltas.length > 12000) {
        globalThis.__mdkrActualRafDeltas.shift();
      }
    }
    testActualRafLast = actual;
  }
  if (!testRafDeltas && !testRafDeltasNs) return actual;
  if (testRafNow === null) testRafNow = actual;
  const index = testRafIndex++;
  const deltaNs = testRafDeltasNs
    ? testRafDeltasNs[index % testRafDeltasNs.length]
    : Math.round(testRafDeltas[index % testRafDeltas.length] * 1000000);
  globalThis.__mdkrLastAnimationFrameDeltaNs = deltaNs;
  testRafNow += deltaNs / 1000000;
  return testRafNow;
};

function testMark(phase) {
  if (testState) testState.phase = phase;
}

function testError(value) {
  if (!testState) return;
  const text = String(value == null ? "" : value);
  testState.errors.push(text.slice(0, 1000));
  if (testState.errors.length > 32) testState.errors.shift();
}

let romPersistenceNoticeTimer = null;
let romPersistenceRetryRequested = false;

function focusLauncherRecovery(target) {
  if (!target) return;
  requestAnimationFrame(() => {
    const gate = $("gate");
    if (gate && !gate.hidden && target.isConnected) {
      target.focus({ preventScroll: true });
    }
  });
}

function showRomPersistenceNotice(message, canRetry, success = false,
                                  retrying = false) {
  const banner = $("rom-session-banner");
  const text = $("rom-session-message");
  const retry = $("rom-storage-retry");
  if (!banner || !text || !retry) return;
  if (romPersistenceNoticeTimer !== null) {
    clearTimeout(romPersistenceNoticeTimer);
    romPersistenceNoticeTimer = null;
  }
  banner.hidden = false;
  banner.classList.toggle("ok", success);
  text.textContent = message;
  // Do not hide an invoked button while it owns keyboard focus. A visible,
  // disabled pending control keeps both focus and the operation's state clear.
  retry.hidden = !canRetry && !retrying;
  retry.disabled = retrying;
  retry.textContent = retrying ? "Retrying…" : "Retry browser storage";
}

function focusRomPersistenceRetry() {
  const banner = $("rom-session-banner");
  const retry = $("rom-storage-retry");
  requestAnimationFrame(() => {
    if (banner && !banner.hidden && retry && !retry.hidden &&
        !retry.disabled && retry.isConnected) {
      retry.focus({ preventScroll: true });
    }
  });
}

function focusRomPersistenceSuccess() {
  requestAnimationFrame(() => {
    const stage = $("stage");
    const canvas = $("canvas");
    if (stage && !stage.hidden && canvas && canvas.isConnected) {
      canvas.focus({ preventScroll: true });
      return;
    }
    focusLauncherRecovery($("gate-msg"));
  });
}

function clearRomSessionOnlyWarning() {
  romSessionOnlyWarning = "";
  const banner = $("rom-session-banner");
  const text = $("rom-session-message");
  const retry = $("rom-storage-retry");
  if (romPersistenceNoticeTimer !== null) {
    clearTimeout(romPersistenceNoticeTimer);
    romPersistenceNoticeTimer = null;
  }
  if (banner) {
    banner.hidden = true;
    banner.classList.remove("ok");
  }
  if (text) text.textContent = "";
  if (retry) {
    retry.hidden = true;
    retry.disabled = false;
    retry.textContent = "Retry browser storage";
  }
}

function noteRomPersistenceConfirmed() {
  if (!romPersistencePending || !romStorageMounted) return;
  romPersistencePending = false;
  storedRomAvailable = true;
  romSessionOnlyWarning = "";
  showRomPersistenceNotice("ROM saved to browser storage.", false, true);
  if (romPersistenceRetryRequested) focusRomPersistenceSuccess();
  romPersistenceRetryRequested = false;
  romPersistenceNoticeTimer = setTimeout(() => {
    clearRomSessionOnlyWarning();
  }, 4000);
}

function retryRomPersistence() {
  if (!romPersistencePending || !romStorageMounted) {
    return Promise.resolve({ storedRomAvailable, romSessionOnlyWarning });
  }
  romPersistenceRetryRequested = true;
  showRomPersistenceNotice("Retrying browser storage…", false, false, true);
  return persist({ reason: "rom-retry", urgent: true }).then(() => ({
    storedRomAvailable,
    romSessionOnlyWarning,
  })).catch((error) => {
    testError("ROM persistence retry failed: " +
      String(error && error.message ? error.message : error));
    romSessionOnlyWarning =
      "The ROM still could not be saved to browser storage. It remains " +
      "available for this session; keep the original file and try again.";
    romPersistenceRetryRequested = false;
    showRomPersistenceNotice(romSessionOnlyWarning, true);
    focusRomPersistenceRetry();
    throw error;
  });
}

function retireRomPersistenceSession() {
  romPersistencePending = false;
  romStorageMounted = false;
  romPersistenceRetryRequested = false;
  clearRomSessionOnlyWarning();
  if (testConfig) {
    delete globalThis.__mdkrTestForceSavePersistenceFailure;
    delete globalThis.__mdkrTestForceAudioOverflow;
    delete globalThis.__mdkrTestRetryRomPersistence;
  }
}

// Called synchronously by the wasm WebGPU seam when device bring-up or a live
// device fails. Keep recovery outside the renderer: expose the launcher (and
// its independent Clear Save / Forget ROM controls), hide the frozen canvas,
// and give the player a stable, actionable message instead of a black frame.
let graphicsRecoveryQueued = false;
window.mdkr64ShowError = (message) => {
  const text = String(
    message || "The graphics device stopped. Reload the page to try again."
  );
  testError(text);
  testMark("graphics-failed");
  clearTouchPad();
  $("stage").hidden = true;
  $("gate").hidden = false;
  const status = $("gate-msg");
  status.className = "status-line err";
  status.textContent = text;
  focusLauncherRecovery(status);
  const play = $("play");
  play.disabled = true;
  play.title = text;
  /*
   * Asyncify's callMain() can return at its first unwind and never deliver the
   * ordinary onExit callback for an initialization failure. Explicitly finish
   * the engine's persistence generation, then hand /save back to the
   * ROM-independent recovery module. This keeps Clear Save/import/export
   * usable on the exact screen reached when the engine cannot render.
   */
  if (!graphicsRecoveryQueued) {
    graphicsRecoveryQueued = true;
    quiesceEnginePersistence("graphics-failure", () => {
      if (globalThis.MDKRSaveUI) {
        globalThis.MDKRSaveUI.resume();
      }
    });
  }
};

// FNV-1a is not a security hash; it is a compact reload oracle for the 512-byte
// EEPROM. Never fingerprint the ROM here: the browser check only records that
// the expected private file exists and has the legal 12 MiB size.
function testFileInfo(path, includeHash, includeBytes = false) {
  if (!module || !module.FS) return { exists: false, size: 0, hash: null };
  try {
    const bytes = module.FS.readFile(path);
    let hash = null;
    if (includeHash) {
      let value = 0x811c9dc5;
      for (const byte of bytes) {
        value ^= byte;
        value = Math.imul(value, 0x01000193) >>> 0;
      }
      hash = value.toString(16).padStart(8, "0");
    }
    return {
      exists: true,
      size: bytes.length,
      hash,
      bytes: includeBytes ? Array.from(bytes) : undefined,
    };
  } catch (e) {
    return { exists: false, size: 0, hash: null };
  }
}

function testAudioInfo() {
  const audio = module && module.mdkrAudio;
  if (!audio) return null;
  return {
    ready: audio.ready === true,
    failed: audio.failed === true,
    posted: Number(audio.posted) || 0,
    ring: Number(audio.statRing) || 0,
    underflows: Number(audio.under) || 0,
    ringDroppedFrames: Number(audio.droppedTotal) || 0,
    pendingDroppedFrames: Number(audio.pendingDroppedFrames) || 0,
    recoveries: Number(audio.recoveries) || 0,
    recoveryFrames: Number(audio.recoveryFrames) || 0,
    recoverySamples: Number(audio.recoverySamples) || 0,
    completedRecoveries: Number(audio.completedRecoveries) || 0,
    contextState: audio.ctx ? String(audio.ctx.state) : "missing",
    shutdownComplete: audio.shutdownComplete === true,
  };
}

function testRefreshExitState() {
  if (!testState || !module || testState.phase !== "main-started") return;
  /*
   * __mdkrExitCode is published when a frame requests termination. That is
   * deliberately earlier than renderer/audio/platform teardown. Only the
   * post-teardown flag proves C main actually unwound through every owner.
   */
  if (Number.isInteger(module.__mdkrExitCode) &&
      module.__mdkrShutdownComplete === true) {
    testState.exitCode = module.__mdkrExitCode;
    testMark("exited");
  }
}

if (testState) {
  globalThis.__mdkrTestSnapshot = () => {
    testRefreshExitState();
    return {
      phase: testState.phase,
      exitCode: testState.exitCode,
      abortReason: testState.abortReason,
      errors: testState.errors.slice(),
      frames: module && Number.isFinite(module.__mdkrFrames)
        ? module.__mdkrFrames : 0,
      exitRequested: module && module.__mdkrExitRequested === true,
      shutdownComplete: module && module.__mdkrShutdownComplete === true,
      rom: testFileInfo(ROM_PATH, false),
      save: testFileInfo("/save/eeprom.bin", true),
      videoConfig: testFileInfo("/save/mdkr64.ini", false),
      saveRecovery: {
        previous: testFileInfo("/save/eeprom.bin.previous", true),
        automatic: [1, 2, 3].map((index) =>
          testFileInfo(`/save/eeprom.bin.autosave.${index}`, true, true)),
      },
      saveDurability: module && module.__mdkrSaveDurability
        ? { ...module.__mdkrSaveDurability } : null,
      storedRomAvailable,
      romSessionOnlyWarning,
      persistenceWait: testState.persistenceWait
        ? { ...testState.persistenceWait } : null,
      audio: testAudioInfo(),
    };
  };
}

// ---- WebGPU capability gate ------------------------------------------------
// A real adapter request (not just navigator.gpu presence): some browsers expose
// the API but have no usable GPU, which would boot to a permanently black canvas.
async function gate() {
  if (!("gpu" in navigator)) {
    return "This build needs WebGPU. Use Chrome / Edge 113+ (or a WebGPU-enabled Firefox / Safari).";
  }
  try {
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
      return "Your browser exposes WebGPU but no usable GPU adapter was found (it may be blocklisted or disabled).";
    }
  } catch (e) {
    return "WebGPU adapter request failed: " + (e && e.message ? e.message : e);
  }
  return null;
}

// ---- Client-side ROM check -------------------------------------------------
// The whole gate lives in rom-id.js, which is the browser mirror of
// platform/rom_id.c: size -> byte order (converted IN PLACE to .z64 here, so the
// copy persisted to IDBFS is canonical) -> which DKR revision this actually is.
//
// It used to be size + magic only. That accepted .v64/.n64 without converting
// anything (the engine converted, so it worked, but this side did not know it)
// and — the real hole — accepted EVERY DKR revision, because all five are 12 MB
// with the same magic. A European or Japanese cart passed and booted into
// garbage. rom-id.js says exactly which revision it is instead.
//
// Returns an error string to show the user, or null to accept. Mutates `bytes`
// into .z64 order before hashing so .z64/.v64/.n64 all compare against one
// canonical identity.
async function validateRom(bytes, name) {
  if (typeof dkrValidateRom !== "function") {
    return "rom-id.js failed to load, so this page cannot check your ROM. Reload the page.";
  }
  const res = dkrValidateRom(bytes, name);
  if (res.error) return res.error;
  if (res.order && res.order !== "z64") {
    console.info(`[ROM] .${res.order} image converted to big-endian .z64 order.`);
  }
  if (!globalThis.crypto || !globalThis.crypto.subtle ||
      typeof dkrReferenceSha256 !== "function") {
    return "This browser cannot perform the complete ROM integrity check. " +
           "Use the current HTTPS page in a supported browser.";
  }
  const expected = dkrReferenceSha256(res.id && res.id.decompBuild);
  if (!expected) {
    return `${name || "That file"} has no supported full-image identity in this build.`;
  }
  let actual;
  try {
    const digest = await globalThis.crypto.subtle.digest("SHA-256", bytes);
    actual = Array.from(new Uint8Array(digest), (value) =>
      value.toString(16).padStart(2, "0")).join("");
  } catch (error) {
    return "The browser could not complete the ROM integrity check (" +
      (error && error.message ? error.message : error) + ").";
  }
  if (actual !== expected) {
    return `${name || "That file"} identifies as ${res.id.revisionName}, but its ` +
      "complete SHA-256 does not match the supported reference image. The dump " +
      "is modified or damaged; choose a clean copy of your cartridge.";
  }
  return null;
}

// ---- Presentation preferences ---------------------------------------------
// Remembered across sessions so a chosen mode survives a reload, and reflected
// from the URL so a shared link opens on the same settings.
const PREF_MODE = "mdkr64.mode";
const PREF_SCALE = "mdkr64.scale";
const PREF_RATE = "mdkr64.rate";
const PREF_SMOOTHING = "mdkr64.smoothing";
let qualityModeDirty = false;
let qualityScaleDirty = false;
let qualityRateDirty = false;
let qualitySmoothingDirty = false;
function revealConfiguredExperimentalPresentation() {
  const disclosure = $("experimental-presentation");
  const rate = $("rate");
  const smoothing = $("smoothing");
  if (disclosure && rate && smoothing &&
      (rate.value !== "original" || smoothing.value !== "off")) {
    disclosure.open = true;
  }
}
function initQualityControls() {
  const qsp = new URLSearchParams(location.search);
  const mode = $("mode");
  const scale = $("scale");
  const rate = $("rate");
  const smoothing = $("smoothing");
  if (!mode || !scale || !rate || !smoothing) return;
  try {
    const m = qsp.get("mode") || localStorage.getItem(PREF_MODE);
    const s = qsp.get("scale") || localStorage.getItem(PREF_SCALE);
    const r = qsp.get("rate") || localStorage.getItem(PREF_RATE);
    const motion = qsp.get("smoothing") ||
      localStorage.getItem(PREF_SMOOTHING);
    if (m && [...mode.options].some((o) => o.value === m)) mode.value = m;
    if (s !== null && [...scale.options].some((o) => o.value === s)) scale.value = s;
    if (r && [...rate.options].some((o) => o.value === r)) rate.value = r;
    if (motion && [...smoothing.options].some((o) => o.value === motion)) {
      smoothing.value = motion;
    }
  } catch (_) { /* private mode: fall back to the defaults in the markup */ }
  revealConfiguredExperimentalPresentation();
  const save = (event) => {
    if (event && event.currentTarget === mode) qualityModeDirty = true;
    if (event && event.currentTarget === scale) qualityScaleDirty = true;
    if (event && event.currentTarget === rate) qualityRateDirty = true;
    if (event && event.currentTarget === smoothing) qualitySmoothingDirty = true;
    try {
      localStorage.setItem(PREF_MODE, mode.value);
      localStorage.setItem(PREF_SCALE, scale.value);
      localStorage.setItem(PREF_RATE, rate.value);
      localStorage.setItem(PREF_SMOOTHING, smoothing.value);
    } catch (_) { /* nothing to do if storage is unavailable */ }
  };
  mode.addEventListener("change", save);
  scale.addEventListener("change", save);
  rate.addEventListener("change", save);
  smoothing.addEventListener("change", save);
}

function parseIniSection(text, wantedSection) {
  const values = new Map();
  let section = "";
  for (const rawLine of String(text).split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || line.startsWith("#") || line.startsWith(";")) continue;
    const sectionMatch = /^\[([^\]]+)\]$/.exec(line);
    if (sectionMatch) {
      section = sectionMatch[1].trim().toLowerCase();
      continue;
    }
    const equals = line.indexOf("=");
    if (section !== wantedSection.toLowerCase() || equals <= 0) continue;
    values.set(
      line.slice(0, equals).trim().toLowerCase(),
      line.slice(equals + 1).trim()
    );
  }
  return values;
}

function applyStoredVideoControls(qs) {
  if (!module || !module.FS) return false;
  let text;
  try {
    text = module.FS.readFile("/save/mdkr64.ini", {encoding: "utf8"});
  } catch (_) {
    return false;
  }
  const mode = $("mode");
  const scale = $("scale");
  const rate = $("rate");
  const smoothing = $("smoothing");
  const video = parseIniSection(text, "video");
  const storedMode = (video.get("mode") || "").toLowerCase();
  const storedScale = video.get("renderscale") || "";
  const storedRate = (video.get("framelimit") || "").toLowerCase();
  const storedSmoothing =
    (video.get("motionsmoothing") || "").toLowerCase();
  if (mode && !qualityModeDirty && !qs.has("mode") &&
      /^(pure|restored|remastered)$/.test(storedMode)) {
    mode.value = storedMode;
  }
  if (scale && !qualityScaleDirty && !qs.has("scale") &&
      /^[1-4](?:\.0+)?$/.test(storedScale)) {
    scale.value = String(Math.trunc(Number(storedScale)));
  }
  if (rate && !qualityRateDirty && !qs.has("rate")) {
    if ([...rate.options].some((option) => option.value === storedRate)) {
      rate.value = storedRate;
    } else if (storedRate === "uncapped") {
      // A config shared from native remains loadable, but rAF is the effective
      // browser ceiling and the launcher must reflect that honestly.
      rate.value = "display";
    }
  }
  if (smoothing && !qualitySmoothingDirty && !qs.has("smoothing") &&
      /^(off|interpolate)$/.test(storedSmoothing)) {
    smoothing.value = storedSmoothing;
  }
  revealConfiguredExperimentalPresentation();
  return true;
}

// ---- Build identity ---------------------------------------------------------
// The publisher stamps every asset reference with ?v=<commit> so a stale
// Safari cache can never mix shell versions; the stamp is recovered from this
// script's own URL and propagated to the runtime-loaded engine assets. The
// visible build tag comes from build-info.json and answers "which build am I
// actually running?" without devtools.
const BUILD_QUERY = (() => {
  try {
    const src = document.currentScript && document.currentScript.src;
    const match = src && src.match(/\?v=([\w.-]+)$/);
    return match ? "?v=" + match[1] : "";
  } catch (_) {
    return "";
  }
})();
globalThis.__mdkrBuildQuery = BUILD_QUERY;

(async () => {
  try {
    const response = await fetch("build-info.json" + BUILD_QUERY, { cache: "no-cache" });
    if (!response.ok) return;
    const info = await response.json();
    const tag = document.getElementById("build-tag");
    const version = typeof info.version === "string" && info.version
      ? info.version : "?";
    const text = "Golden Balloon " + version + " · build " +
      (info.source_commit_short || "?") +
      (info.source_dirty ? " (dirty)" : "") +
      (info.built_utc ? " · " + info.built_utc : "");
    if (tag) {
      tag.textContent = text;
      tag.hidden = false;
    }
    console.info("[shell] " + text);
  } catch (_) {}
})();

// ---- Engine factory (loads mdkr64_web.js, which defines createMDKR64) -------
function loadEngineFactory() {
  if (window.createMDKR64) return Promise.resolve(window.createMDKR64);
  return new Promise((resolve, reject) => {
    const s = document.createElement("script");
    s.src = "mdkr64_web.js" + BUILD_QUERY;
    s.onload = () => window.createMDKR64
      ? resolve(window.createMDKR64)
      : reject(new Error("mdkr64_web.js loaded but did not define createMDKR64"));
    s.onerror = () => reject(new Error("failed to load mdkr64_web.js"));
    document.head.appendChild(s);
  });
}

// ---- Persist saves (IDBFS -> IndexedDB) ------------------------------------
// A save call now has completion semantics: its Promise resolves only after a
// sync that began after that call. The wasm EEPROM seam awaits this Promise via
// Asyncify, so a race result cannot be followed by more simulation while its
// browser transaction is merely queued. A single Promise chain also makes
// syncfs non-reentrancy structural rather than flag-dependent.
let persistRequested = 0;
let persistCommitted = 0;
let persistTail = Promise.resolve();
let enginePersistenceActive = true;
let persistenceTimer = null;
let persistenceListenersArmed = false;

// ---- Cross-tab save ownership ----------------------------------------------
// /save is one IndexedDB database shared by every tab on this origin, and each
// engine instance holds its OWN in-memory MEMFS view of it. A periodic
// syncfs(false) therefore writes that tab's whole view over whatever any other
// tab persisted since it mounted — two tabs racing on a 5s timer means the last
// writer silently wins and one player's progress disappears.
//
// So exactly one session owns writes, decided by an exclusive Web Lock held for
// the lifetime of the document (the browser releases it on close or crash — no
// stale-owner recovery to get wrong). A tab that cannot take the lock still
// boots and plays; it just never writes /save and says so.
//
// Ownership is deliberately NOT upgraded when the owner closes: this tab's MEMFS
// view was populated at ITS boot and has since diverged from what the owner
// persisted, so promoting mid-session would write exactly the stale image this
// mechanism exists to prevent. Reload to take ownership.
//
// navigator.locks is absent on older WebKit, so a heartbeat claim in
// localStorage stands in. It is strictly weaker -- two tabs can race inside one
// heartbeat, and a background tab whose timers are throttled can have its claim
// expire underneath it -- which is why it is the fallback and not the design.
const SAVE_LOCK_NAME = "mdkr64.save-session";
const SAVE_CLAIM_KEY = "mdkr64.save-owner";
const SAVE_CLAIM_TTL_MS = 15000;
const SAVE_CLAIM_BEAT_MS = 5000;
let saveOwnership = "unclaimed";   // "owner" | "spectator" | "unclaimed"
let saveClaimTimer = null;

function claimSaveOwnershipFallback() {
  let claim = null;
  try {
    claim = JSON.parse(localStorage.getItem(SAVE_CLAIM_KEY) || "null");
  } catch (_) { claim = null; }
  const now = Date.now();
  if (claim && typeof claim.at === "number" && now - claim.at < SAVE_CLAIM_TTL_MS) {
    return "spectator";
  }
  const id = String(now) + "." + Math.random().toString(36).slice(2);
  const beat = () => {
    try {
      localStorage.setItem(SAVE_CLAIM_KEY, JSON.stringify({ id, at: Date.now() }));
    } catch (_) { /* storage disabled: the claim simply expires */ }
  };
  beat();
  saveClaimTimer = setInterval(beat, SAVE_CLAIM_BEAT_MS);
  addEventListener("pagehide", () => {
    if (saveClaimTimer !== null) {
      clearInterval(saveClaimTimer);
      saveClaimTimer = null;
    }
    try {
      const held = JSON.parse(localStorage.getItem(SAVE_CLAIM_KEY) || "null");
      if (held && held.id === id) localStorage.removeItem(SAVE_CLAIM_KEY);
    } catch (_) {}
  });
  return "owner";
}

function claimSaveOwnership() {
  // One decision per document. A boot retry must not re-enter this and hand a
  // second engine instance a different answer from the first.
  if (saveOwnership !== "unclaimed") return Promise.resolve(saveOwnership);
  const locks = navigator.locks;
  if (!locks || typeof locks.request !== "function") {
    saveOwnership = claimSaveOwnershipFallback();
    return Promise.resolve(saveOwnership);
  }
  return new Promise((settle) => {
    let settled = false;
    const decide = (value) => {
      if (settled) return;
      settled = true;
      saveOwnership = value;
      settle(value);
    };
    locks.request(SAVE_LOCK_NAME, { mode: "exclusive", ifAvailable: true },
      (lock) => {
        if (!lock) {
          decide("spectator");
          return undefined;
        }
        decide("owner");
        // Never resolving is how a Web Lock is HELD. The document's destruction
        // is the release.
        return new Promise(() => {});
      }).catch((error) => {
        // A rejected request means we do not hold the lock. Refusing to write is
        // the only safe reading of that.
        console.warn("[shell] save-session lock unavailable:", error);
        decide("spectator");
      });
  });
}

// Publish the verdict for anything else on the page that writes /save. The save
// manager (mdkr-save-ui.js) mutates the SAME IndexedDB database from the
// launcher, before Play is ever pressed, so it needs the same answer the engine
// gets -- and it needs it at launcher init, which is why the claim below is now
// resolved there rather than at boot.
globalThis.MDKRSaveOwnership = () => saveOwnership;

function publishSaveOwnership(verdict) {
  if (verdict === "spectator") showSpectatorNotice();
  if (globalThis.MDKRSaveUI && globalThis.MDKRSaveUI.setOwnership) {
    globalThis.MDKRSaveUI.setOwnership(verdict);
  }
  return verdict;
}

function showSpectatorNotice() {
  const banner = $("session-banner");
  if (!banner) return;
  banner.textContent =
    "Another tab already owns saved progress. You can play here, but this tab " +
    "will not save. Close the other tab and reload to save from this one.";
  banner.hidden = false;
}

function publishDurability(error = null) {
  if (!module) return;
  module.__mdkrSaveDurability = {
    requested: persistRequested,
    committed: persistCommitted,
    pending: Math.max(0, persistRequested - persistCommitted),
    lastError: error ? String(error.message || error).slice(0, 500) : null,
  };
}

async function syncEngineFs(options) {
  if (testConfig && testConfig.holdRomPersistenceForPublicRetry &&
      romPersistencePending && (!options || options.reason !== "rom-retry")) {
    throw new Error("ROM storage held for public Retry test");
  }
  // Keep the browser-runtime custody test honest: its initial ROM sync fails
  // once, then its first explicitly requested retry fails before a second
  // retry can establish durable storage. This hook is inert outside tests.
  if (testConfig && options && options.reason === "rom-retry" &&
      Number.isInteger(testConfig.romSyncFailCount) &&
      testConfig.romSyncFailCount > 0) {
    testConfig.romSyncFailCount--;
    throw new Error("injected ROM storage sync failure");
  }
  if (testConfig && options && options.reason === "eeprom") {
    const delay = Number(testConfig.persistDelayOnceMs) || 0;
    const currentFrame = module && Number.isFinite(module.__mdkrFrames)
      ? module.__mdkrFrames : 0;
    if (delay > 0 && currentFrame > 0) {
      testConfig.persistDelayOnceMs = 0;
      const started = performance.now();
      const startFrame = currentFrame;
      await new Promise((resolve) => setTimeout(resolve, delay));
      const endFrame = module && Number.isFinite(module.__mdkrFrames)
        ? module.__mdkrFrames : 0;
      testState.persistenceWait = {
        requestedMs: delay,
        elapsedMs: performance.now() - started,
        startFrame,
        endFrame,
      };
    }
  }
  return new Promise((resolve, reject) => {
    if (!module || !module.FS) {
      resolve();
      return;
    }
    try {
      module.FS.syncfs(false, (error) => {
        if (error) reject(error);
        else {
          noteRomPersistenceConfirmed();
          resolve();
        }
      });
    } catch (error) {
      reject(error);
    }
  });
}

function persist(optionsOrDone) {
  const done = typeof optionsOrDone === "function" ? optionsOrDone : null;
  const options = optionsOrDone && typeof optionsOrDone === "object"
    ? optionsOrDone : null;
  // A spectator tab resolves the engine's durability await without touching
  // IDBFS. Reporting failure instead would spend the engine's save-failure UI
  // on a condition the player cannot fix from in-game; the session banner is
  // where this is said, once, in words.
  if (!enginePersistenceActive || saveOwnership === "spectator") {
    const settled = persistTail.catch(() => {});
    if (done) settled.then(() => done(null), (error) => done(error));
    return settled;
  }
  const generation = ++persistRequested;
  publishDurability();
  const run = persistTail.catch(() => {}).then(async () => {
    await syncEngineFs(options);
    persistCommitted = Math.max(persistCommitted, generation);
    savedOnce = true;
    const banner = $("save-banner");
    if (banner) banner.hidden = true;
    publishDurability();
    return generation;
  });
  persistTail = run;
  if (done) {
    run.then(() => done(null), (error) => done(error));
  }
  run.catch((error) => {
    const banner = $("save-banner");
    if (banner) banner.hidden = false;
    publishDurability(error);
  });
  return run;
}

function quiesceEnginePersistence(reason, done) {
  if (!enginePersistenceActive) {
    retireRomPersistenceSession();
    persistTail.then(
      () => { if (done) done(null); },
      (error) => { if (done) done(error); }
    );
    return persistTail;
  }
  if (persistenceTimer !== null) {
    clearInterval(persistenceTimer);
    persistenceTimer = null;
  }
  /* The in-memory module is being retired. It must never later promote a ROM
   * that a user has forgotten or leave a session-only warning on a returned
   * launcher. A future boot creates a new explicit persistence session. */
  retireRomPersistenceSession();
  /*
   * Enqueue one final generation while the engine still owns /save, then close
   * the producer side immediately. The Promise chain preserves every earlier
   * EEPROM generation; later timer/pagehide callbacks become read-only waits
   * and cannot reinstall stale files after the recovery module erases them.
   */
  const finalFlush = persist({reason, urgent: true});
  enginePersistenceActive = false;
  finalFlush.then(
    () => { if (done) done(null); },
    (error) => { if (done) done(error); }
  );
  return finalFlush;
}

// Retire the module a failed boot left behind. An Emscripten heap cannot be
// freed on demand, but everything that keeps it REACHABLE from this page can be
// dropped — and the IDBFS mounts must be, because two live module views of the
// same backing store is the overlap the persistence chain exists to prevent.
function retireEngineModule(previous) {
  if (!previous) return;
  for (const dir of ["/save", "/rom"]) {
    try { previous.FS.unmount(dir); } catch (_) {}
  }
  try { previous.__mdkrPersist = null; } catch (_) {}
  try { previous.__mdkrPersistFailed = null; } catch (_) {}
  try { previous.__mdkrTouchPad = null; } catch (_) {}
  try { previous.__mdkrCanvasSize = null; } catch (_) {}
  try { previous.canvas = null; } catch (_) {}
  if (testState) testState.module = null;
}

async function beginEnginePersistenceSession() {
  if (persistenceTimer !== null) {
    clearInterval(persistenceTimer);
    persistenceTimer = null;
  }
  // A fast retry can begin before the preceding final IDBFS transaction's
  // callback hands save tooling back. Never let two module views overlap.
  await persistTail.catch(() => {});
  retireEngineModule(module);
  persistRequested = 0;
  persistCommitted = 0;
  persistTail = Promise.resolve();
  enginePersistenceActive = true;
  savedOnce = false;
  graphicsRecoveryQueued = false;
  module = null;
}

function armEnginePersistenceListeners() {
  if (persistenceListenersArmed) return;
  persistenceListenersArmed = true;
  addEventListener("pagehide", () => persist());
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden") persist();
    else resumeAudio();
  });
  addEventListener("keydown", resumeAudio, true);
  addEventListener("pointerdown", resumeAudio);
}

// ---- Resume the AudioContext on a user gesture (autoplay policy) -----------
// iOS additionally mutes Web Audio while the physical ring/silent switch is
// on, because a bare AudioContext runs in the "ambient" audio session. The
// documented escape (the unmute.js technique) is to play a silent MEDIA
// element during a user gesture: media playback flips the session to
// "playback", which ignores the ringer switch — and Web Audio rides along.
let iosUnmuteElement = null;
let iosUnmuteDone = false;
function iosUnmuteKick() {
  try {
    const iosLike = /iP(hone|ad|od)/.test(navigator.userAgent) ||
      (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
    if (!iosLike) return;
    // Preferred, sanctioned path (Safari 16.4+): declare a playback audio
    // session. Web Audio then ignores the ring/silent switch with NO media
    // element involved. The legacy silent-element trick below is only the
    // fallback for older iOS — and it must NOT loop: a persistently playing
    // media element drags the whole session through the media pipeline's
    // large HAL buffers, which is exactly the "huge audio delay" failure.
    if (iosUnmuteDone) return;
    if (navigator.audioSession) {
      try {
        if (navigator.audioSession.type !== "playback") {
          navigator.audioSession.type = "playback";
        }
        // Verify the engine actually accepted it; a silently-ignored write
        // must fall through to the element fallback.
        if (navigator.audioSession.type === "playback") {
          iosUnmuteDone = true;
          return;
        }
      } catch (_) {}
    }
    if (!iosUnmuteElement) {
      iosUnmuteElement = document.createElement("audio");
      iosUnmuteElement.setAttribute("playsinline", "");
      iosUnmuteElement.loop = false;
      iosUnmuteElement.preload = "auto";
      iosUnmuteElement.src = "data:audio/wav;base64,UklGRkQDAABXQVZFZm10IBAAAAABAAEAQB8AAIA+AAACABAAZGF0YSADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==";
    }
    const attempt = iosUnmuteElement.play();
    if (attempt && attempt.then) {
      attempt.then(() => { iosUnmuteDone = true; }, () => {});
    }
  } catch (_) {}
}

let audioLatencyLogged = false;
function resumeAudio() {
  iosUnmuteKick();
  try {
    const ctx = module && module.SDL2 && module.SDL2.audioContext;
    if (ctx && ctx.state !== "running") ctx.resume().catch(() => {});
    if (ctx && ctx.state === "running" && !audioLatencyLogged) {
      audioLatencyLogged = true;
      console.info(
        "[audio] rate=" + ctx.sampleRate +
        " baseLatency=" + (ctx.baseLatency != null ? ctx.baseLatency.toFixed(3) : "?") +
        " outputLatency=" + (ctx.outputLatency != null ? ctx.outputLatency.toFixed(3) : "?") +
        (navigator.audioSession ? " session=" + navigator.audioSession.type : ""));
    }
  } catch (e) {}
}

// ---- Canvas sizing ---------------------------------------------------------
// The engine now samples the canvas backing store every frame. Fill the browser
// viewport at its real aspect, keep CSS pixels and GPU pixels separate for HiDPI,
// and resize live (including fullscreen/orientation changes). The long-edge and
// pixel-budget caps prevent a 5K/8K display from allocating several full-resolution
// WebGPU post-processing targets while retaining the exact host aspect.
const MAX_EDGE = 2560;
const MAX_PIXELS = 2560 * 1440;

function sizeCanvas() {
  const canvas = $("canvas");
  // Prefer the visual viewport: iOS (especially standalone/home-screen
  // launches) can report a stale layout innerHeight until a rotation, while
  // visualViewport tracks the real visible area. The inline pixel styles set
  // below override the stylesheet, so THIS measurement is the layout.
  // Only trust the visual viewport at rest (scale ~1): during a pinch it
  // reports the zoomed region, and resizing the engine per pinch frame would
  // churn the render for an accidental gesture.
  const vv = window.visualViewport;
  const vvAtRest = vv && Math.abs((vv.scale || 1) - 1) < 0.001;
  const rawW = (vvAtRest && vv.width) || window.innerWidth;
  const rawH = (vvAtRest && vv.height) || window.innerHeight;
  const vw = Math.max(320, Math.round(rawW));
  const vh = Math.max(240, Math.round(rawH));
  const cssW = vw, cssH = vh;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  let bw = Math.round(cssW * dpr), bh = Math.round(cssH * dpr);
  const edgeScale = Math.min(1, MAX_EDGE / Math.max(bw, bh));
  const pixelScale = Math.min(1, Math.sqrt(MAX_PIXELS / Math.max(1, bw * bh)));
  const scale = Math.min(edgeScale, pixelScale);
  // Apply one scale to both axes. Independent minimums would subtly change the
  // backing-store aspect in very wide or very short browser windows, leaving
  // CSS to stretch the otherwise-correct engine output.
  bw = Math.max(1, Math.round(bw * scale));
  bh = Math.max(1, Math.round(bh * scale));

  if (canvas.width !== bw) canvas.width = bw;
  if (canvas.height !== bh) canvas.height = bh;
  canvas.style.width = cssW + "px";
  canvas.style.height = cssH + "px";
  return { bw, bh, cssW, cssH };
}

let canvasResizeFrame = 0;
let canvasResizeObserver = null;
function scheduleCanvasResize() {
  if (canvasResizeFrame) return;
  canvasResizeFrame = requestAnimationFrame(() => {
    canvasResizeFrame = 0;
    const dim = sizeCanvas();
    if (module) {
      module.__mdkrCanvasSize = [dim.bw, dim.bh];
    }
    // A layout change moves the touch cluster (URL-bar collapse, settle,
    // rotation): re-measure the zone rects of any press in flight so slides
    // keep scoring against where the buttons actually are.
    if (globalThis.__mdkrRefreshPressRects) globalThis.__mdkrRefreshPressRects();
  });
}

// iOS can settle its real viewport (status bar, home indicator, standalone
// chrome) shortly after launch or an orientation change WITHOUT firing any
// resize event — the shipped symptom was a short canvas with the page
// background showing beneath it until a rotation forced the event chain.
// After every lifecycle edge, poll the measured viewport each frame for a
// bounded window and re-drive the resize when it moves. Cheap (a few reads
// per frame for ~2s) and categorical: no "missing event" can strand the
// layout again.
let viewportSettleUntil = 0;
let viewportSettleFrame = 0;
function viewportSignature() {
  const vv = window.visualViewport;
  return [
    window.innerWidth, window.innerHeight,
    vv ? Math.round(vv.width) : 0, vv ? Math.round(vv.height) : 0,
    window.devicePixelRatio || 1,
  ].join("x");
}
function armViewportSettleWatch(durationMs) {
  viewportSettleUntil = Math.max(
    viewportSettleUntil, performance.now() + (durationMs || 2000));
  if (viewportSettleFrame) return;
  let last = viewportSignature();
  const tick = () => {
    viewportSettleFrame = 0;
    const now = performance.now();
    const sig = viewportSignature();
    if (sig !== last) {
      last = sig;
      scheduleCanvasResize();
      // A move restarts the window: settle means "stable", not "timer ran out
      // mid-transition".
      viewportSettleUntil = Math.max(viewportSettleUntil, now + 700);
    }
    if (now < viewportSettleUntil) {
      viewportSettleFrame = requestAnimationFrame(tick);
    }
  };
  viewportSettleFrame = requestAnimationFrame(tick);
}

function wireCanvasResize() {
  addEventListener("resize", scheduleCanvasResize, { passive: true });
  addEventListener("orientationchange", () => {
    scheduleCanvasResize();
    armViewportSettleWatch(2500);
  }, { passive: true });
  document.addEventListener("fullscreenchange", () => {
    scheduleCanvasResize();
    armViewportSettleWatch(2000);
  });
  // The visual viewport fires on iOS in cases the window does not (chrome
  // collapse, standalone settle, pinch state changes).
  if (window.visualViewport) {
    window.visualViewport.addEventListener(
      "resize", scheduleCanvasResize, { passive: true });
    window.visualViewport.addEventListener(
      "scroll", scheduleCanvasResize, { passive: true });
  }
  addEventListener("pageshow", () => armViewportSettleWatch(2000));
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible") armViewportSettleWatch(1500);
  });
  if (screen.orientation && screen.orientation.addEventListener) {
    screen.orientation.addEventListener("change", () => {
      scheduleCanvasResize();
      armViewportSettleWatch(2500);
    });
  }
  if (typeof ResizeObserver !== "undefined") {
    // Retain the observer for the lifetime of the page. Relying on the browser's
    // target-registration internals to keep an otherwise-unreferenced observer
    // alive makes resize behavior GC-dependent.
    canvasResizeObserver = new ResizeObserver(scheduleCanvasResize);
    canvasResizeObserver.observe($("stage"));
  }
  // Cover the launch transient itself (standalone boots measure wrong for the
  // first few hundred ms on iOS).
  armViewportSettleWatch(3000);
}

// ---- Boot ------------------------------------------------------------------
async function boot() {
  if (booted) return;
  booted = true;
  await beginEnginePersistenceSession();
  // Decide write ownership BEFORE anything mounts /save, so a spectator tab has
  // already been switched off the persistence path by the time the engine can
  // ask for a flush.
  // Idempotent: launcher init already claimed and published. This keeps boot
  // correct on any path that reaches it without the launcher having run.
  publishSaveOwnership(await claimSaveOwnership());
  const canvas = $("canvas");
  const status = $("gate-msg");
  status.className = "status-line";
  // The tiny save-tools module and the engine each have their own in-memory FS
  // view of the same IDBFS database. Hand ownership to the engine only after
  // save tooling has flushed and stopped mutating its view.
  try {
    if (globalThis.MDKRSaveUI) {
      await globalThis.MDKRSaveUI.release();
    }
  } catch (error) {
    booted = false;
    const detail = String(error && error.message ? error.message : error);
    testError(detail);
    testMark("save-ui-release-failed");
    status.className = "status-line err";
    status.textContent =
      "Couldn't prepare browser storage (" + detail.slice(0, 300) +
      "). Try again; your stored ROM and saves were not changed.";
    const play = $("play");
    play.disabled = play.dataset.blocked === "1" ||
                    (!romBytes && !storedRomAvailable);
    quiesceEnginePersistence("save-ui-release-failure", () => {
      if (globalThis.MDKRSaveUI) globalThis.MDKRSaveUI.resume();
    });
    return;
  }
  testMark("boot-started");
  status.textContent = "Downloading engine…";

  // Size the surface first so SDL and the first WebGPU configure agree.
  const dim = sizeCanvas();
  // Diagnostic only: gated behind the same ?trace= flag that arms the engine's
  // own traces, so a normal boot writes nothing to the console.
  if (new URLSearchParams(location.search).get("trace")) {
    console.info("[shell] canvas backing store " + dim.bw + "x" + dim.bh +
                 " (css " + dim.cssW + "x" + dim.cssH + ")");
  }

  let createMDKR64;
  try {
    createMDKR64 = await loadEngineFactory();
    testMark("factory-loaded");
  } catch (e) {
    booted = false;
    testError(e && e.message ? e.message : e);
    testMark("factory-failed");
    status.className = "status-line err";
    status.textContent = "Couldn't load the engine (" +
      String(e && e.message ? e.message : e).slice(0, 300) +
      "). Try again or reload the page.";
    const play = $("play");
    play.disabled = play.dataset.blocked === "1" ||
                    (!romBytes && !storedRomAvailable);
    quiesceEnginePersistence("factory-failure", () => {
      if (globalThis.MDKRSaveUI) globalThis.MDKRSaveUI.resume();
    });
    return;
  }

  status.textContent = "Starting engine…";

  // ?trace=1 turns on the engine's own [PACE] trace, which prints the real
  // per-frame time (dtms) and the updateRate the game is using (R=). That is the
  // decisive diagnostic for "is the game running too fast?" -- a healthy 60 Hz run
  // shows dtms~16.7 with R=1. ?trace=2 adds the display-list opcode trace.
  const qs = new URLSearchParams(location.search);
  const traceLevel = qs.get("trace");
  // ?objcoll=legacy restores the pre-wave-"objcoll" behaviour, where
  // func_80017A18 returned 0 and every collision-meshed object was intangible.
  // It is the same A/B arm tests/check_door_blocks.py drives natively, exposed
  // here so a human can compare the two builds without rebuilding: with it set,
  // a locked hub door can be driven through; without it, the door blocks.
  const objCollValue = qs.get("objcoll");
  const objColl = objCollValue === "legacy" || objCollValue === "trace"
    ? objCollValue : null;
  // ?track=<levelId>[:<vehicle>] RETARGETS the next race -- it does NOT boot
  // straight in. You still navigate the menus and start a race normally (Tracks
  // or Time Trial, any track); mdkr_force_track() then rewrites gTrackIdToLoad
  // so <levelId> loads instead of the one you picked. Menu backgrounds and the
  // track-select preview are deliberately left alone.
  //
  // Useful because saves are per-ORIGIN: a test server on another port cannot
  // see your real progress, and reaching a boss legitimately costs four world
  // balloons. 38 = Tricky's volcano, 5 = Ancient Lake, 12 = Dino Domain lobby.
  // Optional vehicle: 0 = car, 1 = hovercraft, 2 = plane.
  const loadTrackValue = qs.get("track");
  const loadTrack = /^(?:[0-9]|[1-5][0-9]|6[0-5])(?::[0-2])?$/.test(
    loadTrackValue || ""
  ) ? loadTrackValue : null;
  // ?camera=modern|legacy|center-ray selects a camera policy for A/B against
  // the shipped default (observe, the authored camera). There is no setting for
  // it in the web shell; modern is how a page opts into wall correction.
  // Unrecognized values are ignored here; the engine's own fallback would land
  // on observe and announce it, which is not what a typo means.
  const cameraValue = qs.get("camera");
  const cameraPolicy = ["observe", "legacy", "center-ray", "modern"]
    .includes(cameraValue) ? cameraValue : null;

  try {
    module = await createMDKR64({
    canvas,
    noInitialRun: true,
    // Keep the wasm and any side files on the same cache-busting stamp as the
    // shell that loaded them.
    locateFile: (path, prefix) => (prefix || "") + path + BUILD_QUERY,
    // preRun runs BEFORE the createMDKR64 promise resolves, so `module` is still
    // null here — take the Module from the callback argument instead.
    preRun: [function (m) {
      if (traceLevel && m && m.ENV) {
        try { m.ENV.MDKR_TRACE = String(traceLevel); } catch (e) {}
      }
      if (objColl && m && m.ENV) {
        try { m.ENV.MDKR_OBJCOLL = objColl; } catch (e) {}
      }
      if (loadTrack && m && m.ENV) {
        try { m.ENV.MDKR_LOAD_TRACK = loadTrack; } catch (e) {}
      }
      if (cameraPolicy && m && m.ENV) {
        try { m.ENV.MDKR_CAMERA_OBSTRUCTION = cameraPolicy; } catch (e) {}
      }
      if (testConfig && testConfig.env && m && m.ENV) {
        for (const [key, value] of Object.entries(testConfig.env)) {
          if (/^MDKR_[A-Z0-9_]+$/.test(key)) {
            try { m.ENV[key] = String(value).slice(0, 256); } catch (e) {}
          }
        }
      }
    }],
    printErr: (t) => {
      testError(t);
      console.error(t);
    },
    onExit: (code) => {
      if (testState) {
        testState.exitCode = Number(code);
        testMark("exited");
      }
      // The engine's main() returns nonzero when rom_io.c refuses the ROM. The
      // stored copy stops being bootable but still OCCUPIES storage, so this
      // clears only the "can press Play" claim — #forget was revealed when the
      // copy was stored and must stay reachable on exactly this screen.
      if (code && code !== 0) {
        setStoredRomAvailable(false);
        $("gate").hidden = false;
        $("stage").hidden = true;
        $("rom-status").className = "err";
        $("rom-status").textContent =
          "The engine refused this ROM (exit " + code + "). Try a different file.";
        focusLauncherRecovery($("rom-status"));
        const play = $("play");
        play.disabled = play.dataset.blocked === "1" || !romBytes;
        booted = false;
      }
      // main() is no longer mutating IDBFS. Flush the engine view before the
      // launcher save module reacquires the store, so post-run backup/recovery
      // controls observe the exact final EEPROM.
      quiesceEnginePersistence("engine-exit", () => {
        if (globalThis.MDKRSaveUI) {
          globalThis.MDKRSaveUI.resume();
        }
      });
    },
    onAbort: (reason) => {
      if (testState) {
        testState.abortReason = String(reason == null ? "abort" : reason);
        testMark("aborted");
      }
      const text =
        "The engine crashed — reload to continue from your last persisted save.";
      status.className = "status-line err";
      status.textContent = text;
      $("stage").hidden = true;
      $("gate").hidden = false;
      $("play").disabled = true;
      focusLauncherRecovery(status);
      quiesceEnginePersistence("engine-abort", () => {
        if (globalThis.MDKRSaveUI) {
          globalThis.MDKRSaveUI.resume();
        }
      });
    },
    });
  } catch (error) {
    booted = false;
    const detail = String(error && error.message ? error.message : error);
    testError(detail);
    testMark("module-failed");
    status.className = "status-line err";
    status.textContent =
      "The engine could not start (" + detail.slice(0, 300) + "). Try again or reload the page.";
    $("stage").hidden = true;
    $("gate").hidden = false;
    const play = $("play");
    play.disabled = play.dataset.blocked === "1" ||
                    (!romBytes && !storedRomAvailable);
    focusLauncherRecovery(status);
    quiesceEnginePersistence("module-failure", () => {
      if (globalThis.MDKRSaveUI) globalThis.MDKRSaveUI.resume();
    });
    return;
  }
  publishTouchPad();
  if (testState) testState.module = module;
  testMark("module-ready");

  status.textContent = "Preparing storage…";
  // IDBFS-backed ROM + save dirs. syncfs(true) pulls any persisted copies in.
  let storageMounted = false;
  try {
    module.FS.mkdir("/rom");  module.FS.mount(module.IDBFS, {}, "/rom");
    module.FS.mkdir("/save"); module.FS.mount(module.IDBFS, {}, "/save");
    await new Promise((resolve, reject) => module.FS.syncfs(
      true, (err) => err ? reject(err) : resolve()
    ));
    storageMounted = true;
    romStorageMounted = true;
  } catch (e) {
    testError("IDBFS mount/sync failed: " + (e && e.message ? e.message : e));
    console.warn("IDBFS mount/sync failed; running from memory only:", e);
  }

  const storedRomBeforeWrite = testFileInfo(ROM_PATH, false);
  const saveBeforeMain = testFileInfo("/save/eeprom.bin", true);
  const hasStoredVideoConfig = applyStoredVideoControls(qs);

  // Write the freshly-picked ROM (if any) and persist it for next visit.
  let pickedRomWritten = false;
  if (romBytes) {
    const pickedName = selectedRomName || "The selected ROM";
    module.FS.writeFile(ROM_PATH, romBytes);
    romBytes = null;
    selectedRomName = "";
    pickedRomWritten = true;
    let romPersisted = false;
    // syncfs(false) flushes EVERY IDBFS mount this module owns, /save included.
    // A spectator tab must not perform one, so it plays from the in-memory copy
    // and leaves storing the ROM to a session that owns writes. That is policy,
    // not a storage fault, so it raises no session-only warning and offers no
    // retry: retrying would perform exactly the write ownership forbids.
    if (saveOwnership !== "spectator") {
      try {
        if (testConfig && Number.isInteger(testConfig.romSyncFailCount) &&
            testConfig.romSyncFailCount > 0) {
          testConfig.romSyncFailCount--;
          throw new Error("injected ROM storage sync failure");
        }
        if (testConfig && testConfig.romSyncFailOnce) {
          testConfig.romSyncFailOnce = false;
          throw new Error("injected ROM storage sync failure");
        }
        await new Promise((resolve, reject) => module.FS.syncfs(
          false, (err) => err ? reject(err) : resolve()
        ));
        romPersisted = true;
        romPersistencePending = false;
        clearRomSessionOnlyWarning();
      } catch (e) {
        testError("ROM persistence failed: " + (e && e.message ? e.message : e));
        romSessionOnlyWarning =
          `${pickedName} is running for this session only. Browser storage ` +
          "could not be updated, so reloading may restore an earlier ROM or " +
          "ask you to choose it again. Keep the original file available.";
        romPersistencePending = storageMounted;
      }
    }
    // The in-memory engine copy is valid even when persistence is unavailable;
    // only advertise a reusable stored copy after IDBFS was actually mounted.
    setStoredRomAvailable(storageMounted && romPersisted);
  }

  // The engine's post-EEPROM-write sync calls this, so the whole page shares one
  // in-flight sync (see the comment on persist()).
  module.__mdkrPersist = (options) => persist(options);
  module.__mdkrPersistFailed = (message) => {
    const banner = $("save-banner");
    if (banner) {
      banner.hidden = false;
      banner.textContent =
        "Couldn't save to browser storage — the game will keep retrying. " +
        String(message || "").slice(0, 300);
    }
  };
  if (testConfig) {
    /* Browser gate seam: exercise the same save-failure handler that wasm
     * invokes, alongside the independently injected ROM-sync failure. */
    globalThis.__mdkrTestForceSavePersistenceFailure = () =>
      module.__mdkrPersistFailed("injected save storage failure");
    /* Browser-gate seam: inject more PCM than the worklet's bounded ring can
     * hold. This deliberately uses the production message path, not a mock,
     * and is exposed only under __mdkrTestConfig. The worklet must report the
     * loss and start its short continuity recovery envelope. */
    globalThis.__mdkrTestForceAudioOverflow = () => {
      const audio = module.mdkrAudio;
      if (!audio || audio.ready !== true || !audio.node) return null;
      const frames = Math.max(Math.ceil((Number(audio.srcRate) || 22050) * 0.5), 12000);
      const pcm = new Int16Array(frames * 2);
      for (let index = 0; index < frames; index += 1) {
        const sample = index & 1 ? -12288 : 12288;
        pcm[index * 2] = sample;
        pcm[index * 2 + 1] = sample;
      }
      audio.node.port.postMessage({ cmd: "pcm", buf: pcm.buffer }, [pcm.buffer]);
      return {
        frames,
        capacity: Math.max(
          8192,
          Math.floor((Number(audio.srcRate) || 22050) * 0.4),
        ),
      };
    };
    globalThis.__mdkrTestRetryRomPersistence = () => retryRomPersistence();
  }

  // Arm persistence + audio-resume gestures (once). A spectator never arms the
  // timer at all: an unconditional 5s syncfs(false) from every tab IS the
  // cross-tab clobber. Browser-runtime custody tests can likewise hold automatic
  // retries while they prove the public Retry control itself owns the recovery
  // transition.
  if (saveOwnership !== "spectator" &&
      !(testConfig && testConfig.disableAutoPersistence)) {
    persistenceTimer = setInterval(persist, 5000);
  }
  armEnginePersistenceListeners();

  // Show the game view and run.
  $("gate").hidden = true;
  $("stage").hidden = false;
  const romSessionBanner = $("rom-session-banner");
  if (romSessionBanner) {
    if (romSessionOnlyWarning) {
      showRomPersistenceNotice(romSessionOnlyWarning,
                               romPersistencePending && romStorageMounted);
    } else {
      clearRomSessionOnlyWarning();
    }
  }
  armViewportSettleWatch(2500);
  canvas.focus();
  status.textContent = "";
  const mainArgs = ["--rom", ROM_PATH];

  // A stored in-game config is authoritative unless this page's controls were
  // explicitly changed or the URL names an override. First launch seeds
  // /save/mdkr64.ini from the visible picker. Launcher-rank values remain
  // editable in-game; query-only aspect/FOV flags below stay CLI-ranked.
  const MODES = ["pure", "restored", "remastered"];
  const modeSel = $("mode");
  const scaleSel = $("scale");
  const rateSel = $("rate");
  const smoothingSel = $("smoothing");
  let mode = (qs.get("mode") || (modeSel && modeSel.value) || "").toLowerCase();
  let scale = qs.get("scale") || (scaleSel && scaleSel.value) || "";
  let rate = (qs.get("rate") || (rateSel && rateSel.value) || "original").toLowerCase();
  let smoothing = (qs.get("smoothing") ||
    (smoothingSel && smoothingSel.value) || "off").toLowerCase();
  const seedMode = !hasStoredVideoConfig || qualityModeDirty || qs.has("mode");
  const seedScale = !hasStoredVideoConfig || qualityScaleDirty || qs.has("scale");
  const seedRate = !hasStoredVideoConfig || qualityRateDirty || qs.has("rate");
  const seedSmoothing = !hasStoredVideoConfig || qualitySmoothingDirty ||
    qs.has("smoothing");
  if (seedMode && MODES.includes(mode)) {
    mainArgs.push("--video-launch-mode", mode);
  }
  if (seedScale && /^[1-4]$/.test(scale)) {
    mainArgs.push("--video-launch-set", "Video.RenderScale=" + scale);
  }
  if (seedRate && /^(?:original|display|30|60|90|120|144)$/.test(rate)) {
    mainArgs.push("--video-launch-set", "Video.FrameLimit=" + rate);
  }
  if (seedSmoothing && /^(?:off|interpolate)$/.test(smoothing)) {
    mainArgs.push("--video-launch-set", "Video.MotionSmoothing=" + smoothing);
  }
  if (seedMode || seedScale || seedRate || seedSmoothing) {
    mainArgs.push("--video-launch-persist");
  }

  const aspect = qs.get("aspect");
  const fov = qs.get("fov");
  const maxHfov = qs.get("maxHfov");
  const widescreen = qs.get("widescreen");
  if (aspect) mainArgs.push("--aspect", aspect);
  if (fov) mainArgs.push("--fov", fov);
  if (maxHfov) mainArgs.push("--max-hfov", maxHfov);
  if (widescreen === "0" || widescreen === "off") mainArgs.push("--legacy-stretch");

  // The browser regression installs only in-memory fixture text. Keep limits
  // tight and fail the test loudly if its configuration is malformed.
  if (testConfig) {
    if (typeof testConfig.inputScript === "string") {
      if (testConfig.inputScript.length > 1024 * 1024) {
        throw new Error("browser test input script exceeds 1 MiB");
      }
      const inputPath = "/tmp/mdkr-browser-input.txt";
      module.FS.writeFile(inputPath, testConfig.inputScript);
      mainArgs.push("--input-script", inputPath);
    }
    const frames = Number(testConfig.headlessFrames);
    if (Number.isInteger(frames) && frames > 0 && frames <= 100000) {
      mainArgs.push("--headless-frames", String(frames));
    }
    const ticks = Number(testConfig.headlessTicks);
    if (Number.isInteger(ticks) && ticks > 0 && ticks <= 100000) {
      mainArgs.push("--headless-ticks", String(ticks));
    }
    testState.storage = {
      mounted: storageMounted,
      pickedRomWritten,
      storedRomBeforeWrite,
      romBeforeMain: testFileInfo(ROM_PATH, false),
      saveBeforeMain,
    };
  }
  testMark("main-started");
  try {
    const result = module.callMain(mainArgs);
    if (result && typeof result.then === "function") await result;
    // With Asyncify, callMain() returns when the first rAF unwinds, not when C
    // main exits. platform_frame_sync publishes the real finite-run exit below.
    testRefreshExitState();
  } catch (e) {
    testError(e && e.stack ? e.stack : e);
    testMark("main-threw");
    throw e;
  }
}

// ---- Stored data: talk to IndexedDB directly, NOT through the engine -------
// This is the recovery path, so it must work when the engine does NOT. A save
// that the engine crashes on lives in IndexedDB and comes back on every reload;
// if wiping it required booting the engine to get an FS, the one state that
// needs wiping would be the one state you could not wipe from. So these helpers
// go straight at the IDBFS backing store.
//
// IDBFS names the database after the mount point ("/rom", "/save") and keeps
// every file in one object store, "FILE_DATA", keyed by absolute path. Opening
// with no explicit version joins whatever version is already there instead of
// fighting IDBFS's own; clearing the store inside a plain readwrite transaction
// works even while the engine holds an open connection, which deleteDatabase()
// does not (it would sit in onblocked).
const IDB_STORE = "FILE_DATA";

function idbOpen(dbName) {
  return new Promise((resolve) => {
    let req;
    try { req = indexedDB.open(dbName); } catch (e) { resolve(null); return; }
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => resolve(null);
    req.onblocked = () => resolve(null);
  });
}

// Number of files IDBFS has stored for a mount (0 when the store does not exist).
// indexedDB.databases() first, so a first-ever visit does not create an empty
// database just to be told it is empty.
async function idbCount(dbName) {
  try {
    if (indexedDB.databases) {
      const names = (await indexedDB.databases()).map((d) => d.name);
      if (!names.includes(dbName)) return 0;
    }
  } catch (e) { /* fall through to open() */ }
  const db = await idbOpen(dbName);
  if (!db) return 0;
  if (!db.objectStoreNames.contains(IDB_STORE)) { db.close(); return 0; }
  return new Promise((resolve) => {
    let n = 0;
    try {
      const req = db.transaction(IDB_STORE, "readonly").objectStore(IDB_STORE).count();
      req.onsuccess = () => { n = req.result | 0; db.close(); resolve(n); };
      req.onerror = () => { db.close(); resolve(0); };
    } catch (e) { db.close(); resolve(0); }
  });
}

// Read one IDBFS file without instantiating the engine. Emscripten stores a
// record shaped as { timestamp, mode, contents } under its absolute path.
// Copy the bytes before closing the database so validation never depends on an
// implementation-owned backing buffer.
async function idbReadFile(dbName, path) {
  const db = await idbOpen(dbName);
  if (!db) return null;
  if (!db.objectStoreNames.contains(IDB_STORE)) { db.close(); return null; }
  return new Promise((resolve) => {
    try {
      const req = db.transaction(IDB_STORE, "readonly")
        .objectStore(IDB_STORE).get(path);
      req.onsuccess = () => {
        const record = req.result;
        const contents = record && record.contents;
        let copy = null;
        try {
          if (contents instanceof ArrayBuffer) {
            copy = new Uint8Array(contents.slice(0));
          } else if (ArrayBuffer.isView(contents)) {
            copy = new Uint8Array(
              contents.buffer.slice(
                contents.byteOffset, contents.byteOffset + contents.byteLength));
          }
        } catch (_) { copy = null; }
        db.close();
        resolve(copy);
      };
      req.onerror = () => { db.close(); resolve(null); };
    } catch (e) { db.close(); resolve(null); }
  });
}

async function idbClear(dbName) {
  if (testConfig && testConfig.idbClearFailOnce && dbName === "/rom") {
    testConfig.idbClearFailOnce = false;
    return false;
  }
  const db = await idbOpen(dbName);
  if (!db) return false;
  if (!db.objectStoreNames.contains(IDB_STORE)) { db.close(); return true; }
  return new Promise((resolve) => {
    try {
      const tx = db.transaction(IDB_STORE, "readwrite");
      tx.objectStore(IDB_STORE).clear();
      tx.oncomplete = () => { db.close(); resolve(true); };
      tx.onerror = () => { db.close(); resolve(false); };
      tx.onabort = () => { db.close(); resolve(false); };
    } catch (e) { db.close(); resolve(false); }
  });
}

// If a module happens to be live (a boot that bounced back to the launcher), keep
// its in-memory FS in step so it cannot write the old bytes back out.
function fsUnlinkAll(dir) {
  if (!module || !module.FS) return;
  try {
    for (const name of module.FS.readdir(dir)) {
      if (name === "." || name === "..") continue;
      try { module.FS.unlink(dir + "/" + name); } catch (e) {}
    }
  } catch (e) {}
}

// ---- ROM picker + Forget ---------------------------------------------------
function wireRomUi() {
  const input = $("rom-input");
  const play = $("play");
  const romStatus = $("rom-status");

  // One handler for both the file input and a drop, so the two paths cannot
  // drift apart.
  async function acceptFile(file) {
    if (!file) return;
    const selection = ++romSelectionEpoch;
    input.value = "";
    const hadActiveRom = romBytes !== null || storedRomAvailable;
    romStatus.className = "";
    romStatus.textContent = "Reading " + file.name + "…";
    let buf;
    try {
      buf = new Uint8Array(await file.arrayBuffer());
    } catch (e) {
      if (selection !== romSelectionEpoch) return;
      romStatus.className = "err";
      romStatus.textContent = "Couldn't read that file (" + (e.message || e) + ")." +
        (hadActiveRom ? " Your previously verified ROM is still selected." : "");
      play.disabled = play.dataset.blocked === "1" || !hadActiveRom;
      input.value = "";
      return;
    }
    if (selection !== romSelectionEpoch) return;
    const err = await validateRom(buf, file.name);
    if (selection !== romSelectionEpoch) return;
    if (err) {
      romStatus.className = "err";
      romStatus.textContent = err + (hadActiveRom
        ? " Your previously verified ROM is still selected; you can keep playing it."
        : "");
      play.disabled = play.dataset.blocked === "1" || !hadActiveRom;
      return;
    }
    romBytes = buf;
    selectedRomName = file.name || "Selected ROM";
    romStatus.className = "ok";
    if (play.dataset.blocked) {
      // Valid ROM, but this browser can't run the engine. Say so here too, since
      // this is where the user is looking.
      romStatus.textContent = "✓ " + file.name +
        " looks good — but this browser can't run WebGPU (see above).";
      return;
    }
    romStatus.textContent = "✓ " + file.name +
      " looks good — full-image integrity verified; press Play.";
    play.disabled = false;
    play.focus();
  }

  input.addEventListener("change", () => acceptFile(input.files && input.files[0]));

  // ---- drop zone: native button activation and drag-and-drop ----
  const drop = $("drop");
  if (drop) {
    drop.addEventListener("click", () => input.click());
    // dragover must be prevented or the browser navigates to the file instead.
    ["dragenter", "dragover"].forEach((ev) =>
      drop.addEventListener(ev, (e) => {
        e.preventDefault(); e.stopPropagation(); drop.classList.add("over");
      }));
    ["dragleave", "dragend"].forEach((ev) =>
      drop.addEventListener(ev, () => drop.classList.remove("over")));
    drop.addEventListener("drop", (e) => {
      e.preventDefault(); e.stopPropagation(); drop.classList.remove("over");
      const dt = e.dataTransfer;
      acceptFile(dt && dt.files && dt.files[0]);
    });
  }
  // Swallow stray drops on the page so a mis-aimed drop never navigates away
  // from a half-configured launcher.
  ["dragover", "drop"].forEach((ev) =>
    window.addEventListener(ev, (e) => { e.preventDefault(); }));

  play.addEventListener("click", () => {
    if (!romBytes && !storedRomAvailable) {
      romStatus.className = "err";
      romStatus.textContent = "Choose a clean supported ROM before playing.";
      play.disabled = true;
      return;
    }
    play.disabled = true;
    if (!romBytes) {
      // Boot from the ROM already persisted in IDBFS (checked at boot time).
      boot();
      return;
    }
    boot();
  });

  // Forget the stored ROM. This used to instantiate a SECOND engine module just to
  // get an FS, never mount IDBFS on it, and then unlink a path that did not exist
  // there — so it downloaded the whole engine and deleted nothing. It also had no
  // code path that ever un-hid the button, so it was unreachable as well as
  // ineffective. Both are fixed: it clears the "/rom" store directly.
  const forgetDialog = $("forget-rom-dialog");
  const forgetButton = $("forget");
  const forgetCancel = $("forget-rom-cancel");
  let forgetReturnFocus = null;
  let forgetPending = false;
  forgetButton.addEventListener("click", () => {
    // The invoking button is the stable focus anchor even when automation,
    // assistive technology, or a browser's pointer policy dispatches click
    // without first moving document.activeElement onto the control.
    forgetReturnFocus = forgetButton;
    if (forgetDialog && typeof forgetDialog.showModal === "function") {
      forgetDialog.showModal();
      $("forget-rom-cancel").focus();
    } else if (globalThis.confirm(
        "Forget this browser's stored ROM? Your original file and saves will not change.")) {
      $("forget-rom-confirm").click();
    }
  });
  forgetCancel.addEventListener("click", () => {
    if (!forgetPending && forgetDialog.open) forgetDialog.close();
  });
  forgetDialog.addEventListener("cancel", (event) => {
    if (forgetPending) event.preventDefault();
  });
  forgetDialog.addEventListener("close", () => {
    if (forgetReturnFocus && forgetReturnFocus.isConnected &&
        typeof forgetReturnFocus.focus === "function") {
      forgetReturnFocus.focus();
    }
    forgetReturnFocus = null;
  });
  $("forget-rom-confirm").addEventListener("click", async () => {
    const btn = $("forget");
    const confirm = $("forget-rom-confirm");
    forgetPending = true;
    btn.disabled = true;
    confirm.disabled = true;
    forgetCancel.disabled = true;
    if (!enginePersistenceActive) await persistTail.catch(() => {});
    const ok = await idbClear("/rom");
    if (ok) {
      setStoredRomAvailable(false);
      romPersistencePending = false;
      romStorageMounted = false;
      clearRomSessionOnlyWarning();
      fsUnlinkAll("/rom");
    }
    forgetPending = false;
    btn.disabled = false;
    confirm.disabled = false;
    forgetCancel.disabled = false;
    if (ok) forgetReturnFocus = null;
    if (forgetDialog.open) forgetDialog.close();
    btn.hidden = ok;
    $("rom-status").className = ok ? "" : "err";
    $("rom-status").textContent = ok
      ? (romBytes
          ? `Stored browser copy forgotten. ${selectedRomName} remains selected — press Play.`
          : "Stored browser copy forgotten. Your original ROM file and saved progress were not changed.")
      : "Couldn't clear the stored ROM. Try again; if it keeps failing, " +
        "clear this site's data in your browser settings.";
    $("play").disabled = $("play").dataset.blocked === "1" ||
                          (!romBytes && !storedRomAvailable);
    if (ok) $("drop").focus();
  });

}

// ---- Is there already a ROM in IndexedDB? ----------------------------------
// Without this the persisted ROM was unreachable: #play stayed disabled and
// #forget stayed hidden until a file was picked, so "the ROM persists across
// reloads" was true of the storage and false of the UI.
async function probeStoredRom() {
  const selection = romSelectionEpoch;
  if ((await idbCount("/rom")) === 0 || selection !== romSelectionEpoch) return;
  revealForgetControl();
  const play = $("play");
  const stored = await idbReadFile("/rom", ROM_PATH);
  if (selection !== romSelectionEpoch) return;
  if (!stored) {
    $("rom-status").className = "err";
    $("rom-status").textContent =
      "A stored ROM entry exists but could not be read. Forget it or choose a clean ROM.";
    play.disabled = true;
    return;
  }
  const error = await validateRom(stored, "Stored ROM");
  if (selection !== romSelectionEpoch) return;
  if (error) {
    $("rom-status").className = "err";
    $("rom-status").textContent = error +
      " The stored copy was not started; forget it or choose a clean ROM.";
    play.disabled = true;
    return;
  }
  setStoredRomAvailable(true);
  if (!play.dataset.blocked) {
    play.disabled = false;
    $("rom-status").className = "ok";
    $("rom-status").textContent =
      "✓ Using the ROM stored in this browser — full-image integrity verified; press Play.";
  }
}

// ---- Frame-rate readout ----------------------------------------------------
// Count images that the engine actually committed to the canvas. Numeric caps
// can deliberately yield through rAF without producing a new image, and the
// shell has unrelated rAF users for resize/touch work, so wrapping
// requestAnimationFrame would report host opportunities rather than visual FPS.
// Toggle with F3.
function wireFpsReadout() {
  const el = $("fps");
  if (!el) return;
  let lastEngineFrames = 0, since = performance.now(), shown = false;
  let timer = null;
  // The readout is off by default, so the timer only exists while it is on:
  // a hidden overlay must not cost a wakeup twice a second for the whole
  // session, least of all on a phone.
  const sample = () => {
    const now = performance.now();
    const dt = now - since;
    if (dt >= 500) {
      const engineFrames = module && Number.isFinite(module.__mdkrFrames)
        ? module.__mdkrFrames : 0;
      const presented = Math.max(0, engineFrames - lastEngineFrames);
      const fps = (presented * 1000) / dt;
      el.textContent = fps.toFixed(0) + " visual fps  ·  " + (1000 / Math.max(fps, 0.001)).toFixed(1) + " ms";
      lastEngineFrames = engineFrames;
      since = now;
    }
  };
  addEventListener("keydown", (e) => {
    if (e.key !== "F3") return;
    shown = !shown;
    el.hidden = !shown;
    if (shown) {
      // Rebase the window so the first sample measures live frames rather than
      // everything presented since the page loaded.
      lastEngineFrames = module && Number.isFinite(module.__mdkrFrames)
        ? module.__mdkrFrames : 0;
      since = performance.now();
      el.textContent = "";
      timer = setInterval(sample, 500);
    } else if (timer !== null) {
      clearInterval(timer);
      timer = null;
    }
  });
}

// ---- Mobile touch controller ----------------------------------------------
// Touch is a first-class analog P1 controller, not a collection of synthetic
// key events. It appears automatically on coarse-pointer devices, yields to a
// connected physical gamepad unless the player explicitly asks to keep it, and
// can always be hidden without leaving a held button behind.
const TOUCH_PREF = "mdkr64.touch-controls";

function wireTouchControls() {
  const controls = $("touch-controls");
  const toggle = $("touch-toggle");
  const stage = $("stage");
  const stick = $("touch-stick");
  const knob = $("touch-stick-knob");
  if (!controls || !toggle || !stage || !stick || !knob) return;

  const qs = new URLSearchParams(location.search);
  const forced = qs.get("touch");
  const coarseQuery = matchMedia("(pointer: coarse)");
  const noHoverQuery = matchMedia("(hover: none)");
  let preference = "";
  try { preference = localStorage.getItem(TOUCH_PREF) || ""; } catch (_) {}
  // A persisted explicit "shown" is an opt-in equivalent to ?touch=1: the
  // media queries can stop matching on the same physical device (a convertible
  // with a keyboard attached), and the stored choice must still revive the
  // overlay after reload.
  const mediaCapable = () => coarseQuery.matches ||
    (navigator.maxTouchPoints > 0 && noHoverQuery.matches);
  const touchCapable = forced === "1" || forced === "on" ||
    (forced !== "0" && forced !== "off" &&
     (mediaCapable() || preference === "shown"));
  if (!touchCapable) {
    if (forced === "0" || forced === "off") return;
    // A convertible can become touch-capable after load (keyboard detached,
    // pointer turns coarse). Wire the overlay the moment that happens.
    // MediaQueryList.addEventListener is missing on older engines (Safari <=13,
    // old WebViews) and this branch runs on EVERY non-touch load inside the
    // un-caught startup path — an unguarded throw here would abort bootstrap
    // before the ROM picker and the WebGPU gate message ever appear.
    if (typeof coarseQuery.addEventListener !== "function") return;
    const onCapabilityChange = () => {
      if (wireTouchControls.rewired || !mediaCapable()) return;
      wireTouchControls.rewired = true;
      coarseQuery.removeEventListener("change", onCapabilityChange);
      noHoverQuery.removeEventListener("change", onCapabilityChange);
      wireTouchControls();
    };
    try {
      coarseQuery.addEventListener("change", onCapabilityChange);
      noHoverQuery.addEventListener("change", onCapabilityChange);
    } catch (_) {}
    return;
  }
  let stickPointer = null;
  let accessibilityButtons = 0;
  const pressedPointers = new Map();
  const pressedClasses = new Set();
  const pulseTimers = new Map();
  const actionButtons = [
    ...controls.querySelectorAll("[data-touch-button]")
  ];

  const hasGamepad = () => {
    try {
      return !!(navigator.getGamepads &&
        [...navigator.getGamepads()].some((pad) => pad && pad.connected));
    } catch (_) {
      return false;
    }
  };

  function recomputeButtons() {
    let buttons = accessibilityButtons;
    pressedPointers.forEach((press) => {
      buttons |= press.bits;
    });
    touchPadState.buttons = buttons >>> 0;
    publishTouchPad();
  }

  function refreshButtonClasses() {
    actionButtons.forEach((button) => {
      const buttonBit = Number(button.dataset.touchButton) >>> 0;
      const active = (accessibilityButtons & buttonBit) !== 0 ||
        [...pressedPointers.values()].some(
          (press) => (press.bits & buttonBit) === buttonBit);
      button.classList.toggle("is-pressed", active);
      if (active) pressedClasses.add(button);
      else pressedClasses.delete(button);
    });
  }

  function releaseButtonPointer(pointerId) {
    if (!pressedPointers.delete(pointerId)) return;
    refreshButtonClasses();
    recomputeButtons();
  }

  function resetStick() {
    stickPointer = null;
    touchPadState.stickX = 0;
    touchPadState.stickY = 0;
    knob.style.transform = "translate3d(0, 0, 0)";
    stick.classList.remove("is-active");
    publishTouchPad();
  }

  function releaseAll() {
    pressedPointers.clear();
    accessibilityButtons = 0;
    pulseTimers.forEach((timer) => clearTimeout(timer));
    pulseTimers.clear();
    pressedClasses.forEach((el) => el.classList.remove("is-pressed"));
    pressedClasses.clear();
    resetStick();
    clearTouchPad();
  }

  function setVisible(visible, remember = false) {
    controls.hidden = !visible;
    toggle.hidden = false;
    toggle.setAttribute("aria-pressed", visible ? "true" : "false");
    toggle.setAttribute(
      "aria-label", visible ? "Hide touch controls" : "Show touch controls");
    toggle.querySelector(".touch-toggle-label").textContent =
      visible ? "Controls" : "Show controls";
    stage.classList.toggle("touch-ui-active", visible);
    touchPadState.enabled = visible;
    if (!visible) releaseAll();
    publishTouchPad();
    if (remember) {
      preference = visible ? "shown" : "hidden";
      try { localStorage.setItem(TOUCH_PREF, preference); } catch (_) {}
    }
  }

  function updateStick(event) {
    const rect = stick.getBoundingClientRect();
    const centerX = rect.left + rect.width / 2;
    const centerY = rect.top + rect.height / 2;
    const radius = Math.max(24, Math.min(rect.width, rect.height) * 0.31);
    let dx = event.clientX - centerX;
    let dy = event.clientY - centerY;
    const distance = Math.hypot(dx, dy);
    if (distance > radius) {
      dx = dx * radius / distance;
      dy = dy * radius / distance;
    }

    // Eight percent center deadzone, then rescale the remaining travel so the
    // outer ring still reaches the full N64 range. This prevents thumb tremor
    // from steering while retaining continuous low-speed control.
    const normalized = Math.min(1, Math.hypot(dx, dy) / radius);
    const deadzone = 0.08;
    const magnitude = normalized <= deadzone
      ? 0 : (normalized - deadzone) / (1 - deadzone);
    const angle = Math.atan2(dy, dx);
    touchPadState.stickX = Math.round(Math.cos(angle) * magnitude * 80);
    touchPadState.stickY = Math.round(-Math.sin(angle) * magnitude * 80);
    knob.style.transform =
      `translate3d(${dx.toFixed(1)}px, ${dy.toFixed(1)}px, 0)`;
    publishTouchPad();
  }

  stick.addEventListener("pointerdown", (event) => {
    if (stickPointer !== null) return;
    event.preventDefault();
    stickPointer = event.pointerId;
    // Capture is an optimization; the window-level tracking below owns the
    // lifecycle (see the pad comment — Safari can decline capture, and a
    // steering thumb that wanders off the stick must never freeze steering).
    try { stick.setPointerCapture(event.pointerId); } catch (_) {}
    stick.classList.add("is-active");
    updateStick(event);
  });
  addEventListener("pointermove", (event) => {
    if (event.pointerId !== stickPointer) return;
    event.preventDefault();
    updateStick(event);
  }, true);
  const endStick = (event) => {
    if (event.pointerId !== stickPointer) return;
    resetStick();
  };
  addEventListener("pointerup", endStick, true);
  addEventListener("pointercancel", endStick, true);

  // ---- The throttle pad: slide-to-chord ----------------------------------
  // Human-factors model: the action cluster IS the accelerator. Touching any
  // zone holds A instantly; the zone under the thumb selects the modifier, so
  // drifting or firing never requires lifting off the throttle:
  //   Go -> A         Drift -> A+R      Item -> A+Z      Look -> A+C
  //   Brake -> B      (braking is off-throttle by definition)
  // Zone changes use nearest-target hysteresis with retention: crossing a gap
  // or overshooting keeps the last chord, so a mid-corner slide can never
  // stall the kart; only lifting releases. A second finger still lands as an
  // independent tap on any zone (bits union across pointers). Pause sits
  // outside the pad and stays a plain tap without the throttle latch.
  const N64_A = 32768;
  const N64_BRAKE = 16384;
  const N64_PAUSE = 4096;
  const actions = controls.querySelector(".touch-actions");
  const clusterButtons = actions
    ? [...actions.querySelectorAll("[data-touch-button]")]
    : [];

  function chordBits(bit) {
    if (bit === N64_A || bit === N64_BRAKE) return bit;
    return bit | N64_A;
  }

  function padRects() {
    return clusterButtons.map((button) => ({
      button,
      bit: Number(button.dataset.touchButton) >>> 0,
      rect: button.getBoundingClientRect(),
    }));
  }
  globalThis.__mdkrRefreshPressRects = () => {
    pressedPointers.forEach((press) => {
      if (press.rects) press.rects = padRects();
    });
  };

  function zoneAt(rects, x, y, reach) {
    const limit = reach == null ? 14 : reach;
    let best = null;
    let bestScore = Infinity;
    for (const entry of rects) {
      const r = entry.rect;
      const dx = x < r.left ? r.left - x : (x > r.right ? x - r.right : 0);
      const dy = y < r.top ? r.top - y : (y > r.bottom ? y - r.bottom : 0);
      const outside = Math.max(dx, dy);
      if (outside > limit) continue;
      const cx = x - (r.left + r.right) / 2;
      const cy = y - (r.top + r.bottom) / 2;
      // Strictly-inside beats padded reach; ties resolve to nearest center.
      const score = outside * 1e7 + cx * cx + cy * cy;
      if (score < bestScore) {
        bestScore = score;
        best = entry;
      }
    }
    return best;
  }

  function padVibrate(ms, type) {
    try {
      if (navigator.vibrate && type !== "mouse") navigator.vibrate(ms);
    } catch (_) {}
  }

  if (actions) {
    actions.addEventListener("pointerdown", (event) => {
      const rects = padRects();
      // Any touch inside the cluster box engages the nearest zone: the box
      // IS the pad, and an inert patch would swallow the tap (the box is a
      // hit target so it never falls through to the canvas).
      const zone = zoneAt(rects, event.clientX, event.clientY, Infinity);
      if (!zone) return;
      event.preventDefault();
      // Capture is an optimization only. Release correctness must NEVER
      // depend on it: Safari declines capture in configurations Chrome
      // accepts, and a declined capture meant a thumb that slid off its
      // starting button hit-tested its pointerup into the canvas — the
      // pad's handlers never saw it and the throttle latched forever
      // (the shipped stuck-Go defect). The window-level tracking below is
      // the authoritative lifecycle.
      try { actions.setPointerCapture(event.pointerId); } catch (_) {}
      pressedPointers.set(event.pointerId, {
        rects,
        bits: chordBits(zone.bit),
        zone: zone.button,
      });
      refreshButtonClasses();
      recomputeButtons();
      padVibrate(8, event.pointerType);
    });
  }

  // Authoritative pointer lifecycle: once a pad (or pause) pointer is down,
  // its moves and its release are tracked at the WINDOW in the capture
  // phase, so no hit-testing quirk, overlay boundary, or declined capture
  // can ever eat the release. blur/pagehide/visibilitychange releaseAll
  // remains the backstop.
  addEventListener("pointermove", (event) => {
    const press = pressedPointers.get(event.pointerId);
    if (!press || !press.rects) return;
    const zone = zoneAt(press.rects, event.clientX, event.clientY);
    if (!zone || zone.button === press.zone) return;
    press.zone = zone.button;
    press.bits = chordBits(zone.bit);
    refreshButtonClasses();
    recomputeButtons();
    padVibrate(5, event.pointerType);
  }, true);
  ["pointerup", "pointercancel"].forEach((name) => {
    addEventListener(name, (event) =>
      releaseButtonPointer(event.pointerId), true);
  });

  actionButtons.forEach((button) => {
    const bit = Number(button.dataset.touchButton) >>> 0;
    if (bit === N64_PAUSE) {
      button.addEventListener("pointerdown", (event) => {
        event.preventDefault();
        try { button.setPointerCapture(event.pointerId); } catch (_) {}
        pressedPointers.set(event.pointerId, { bits: bit, zone: button });
        refreshButtonClasses();
        recomputeButtons();
        padVibrate(8, event.pointerType);
        // Release rides the window-level pointerup/pointercancel tracking.
      });
    }
    // Screen readers and switch controls may activate a button without Pointer
    // Events. Hold it long enough to cross at least one authored 30 Hz sample.
    button.addEventListener("click", (event) => {
      if (event.detail !== 0) return;
      const pulseBits = bit === N64_PAUSE ? bit : chordBits(bit);
      const priorTimer = pulseTimers.get(bit);
      if (priorTimer) clearTimeout(priorTimer);
      accessibilityButtons |= pulseBits;
      button.classList.add("is-pressed");
      recomputeButtons();
      pulseTimers.set(bit, setTimeout(() => {
        accessibilityButtons &= ~pulseBits;
        pulseTimers.delete(bit);
        refreshButtonClasses();
        recomputeButtons();
      }, 90));
    });
  });

  toggle.addEventListener("click", () => {
    setVisible(controls.hidden, true);
  });
  addEventListener("blur", releaseAll);
  addEventListener("pagehide", releaseAll);
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "visible") releaseAll();
  });
  document.addEventListener("fullscreenchange", releaseAll);

  addEventListener("gamepadconnected", () => {
    if (!preference) setVisible(false, false);
  });
  addEventListener("gamepaddisconnected", () => {
    if (!preference && !hasGamepad()) setVisible(true, false);
  });

  controls.hidden = false;
  toggle.hidden = false;
  setVisible(preference === "shown" ||
    (preference !== "hidden" && !hasGamepad()), false);
}

// ---- Fullscreen ------------------------------------------------------------
function wireFullscreen() {
  const btn = $("fullscreen");
  const stage = $("stage");
  const canvas = $("canvas");
  let transition = null;
  const stageStatus = $("stage-status");

  const clearStageStatus = () => {
    if (!stageStatus) return;
    stageStatus.hidden = true;
    stageStatus.textContent = "";
  };

  const reportFullscreenFailure = (exiting = false) => {
    if (!stageStatus) return;
    stageStatus.textContent =
      exiting
        ? "Couldn't exit fullscreen. Press Escape or use your browser's fullscreen control."
        : "Fullscreen was blocked by this browser. Continue in the current window.";
    stageStatus.hidden = false;
  };

  // Feature-detect rather than browser-sniff: current iPhone Safari supports
  // element fullscreen, while older WebKit builds and some embedded browsers
  // do not. A visible button that can only fail is worse than none.
  if (btn && stage &&
      typeof stage.requestFullscreen !== "function" &&
      typeof stage.webkitRequestFullscreen !== "function") {
    btn.hidden = true;
    // A home-screen app remains the fallback path to chromeless play when this
    // browser exposes no element-fullscreen API.
    try {
      const standalone = navigator.standalone === true ||
        (typeof matchMedia === "function" &&
         matchMedia("(display-mode: standalone), (display-mode: fullscreen)")
           .matches);
      const hint = document.getElementById("ios-fullscreen-hint");
      if (hint && !standalone) hint.hidden = false;
    } catch (_) {}
    return;
  }

  const updateButton = () => {
    if (!btn) return;
    const active = document.fullscreenElement === stage;
    btn.disabled = transition !== null;
    btn.setAttribute("aria-pressed", active ? "true" : "false");
    btn.setAttribute(
      "aria-label", active ? "Exit fullscreen" : "Enter fullscreen");
    btn.title = active ? "Exit fullscreen (F)" : "Fullscreen (F)";
  };

  const settleSurface = () => new Promise((resolve) => {
    // Fullscreen changes both the viewport and, on HiDPI displays, the canvas
    // backing store. Let those values settle before returning input to wasm.
    requestAnimationFrame(() => requestAnimationFrame(resolve));
  });

  const go = () => {
    // A second click/key-repeat while the browser's fullscreen promise is
    // pending can enqueue an immediate exit and leave resize events racing GPU
    // target creation. Serialize the transition as one state change.
    if (transition) return transition;
    transition = (async () => {
      clearStageStatus();
      try {
        if (document.fullscreenElement) {
          await document.exitFullscreen();
        } else if (document.webkitFullscreenElement &&
                   typeof document.webkitExitFullscreen === "function") {
          document.webkitExitFullscreen();
        } else {
          if (typeof stage.requestFullscreen !== "function") {
            // WebKit-prefixed engines (older iPadOS) never reach here when
            // the standard API is absent unless the prefixed form exists —
            // wireFullscreen() hides the button otherwise.
            if (typeof stage.webkitRequestFullscreen === "function") {
              stage.webkitRequestFullscreen();
              scheduleCanvasResize();
              await settleSurface();
              scheduleCanvasResize();
              canvas.focus({ preventScroll: true });
              return;
            }
            throw new Error("Fullscreen is unavailable in this browser");
          }
          try {
            await stage.requestFullscreen({ navigationUI: "hide" });
          } catch (error) {
            // Older implementations support fullscreen but reject the options
            // dictionary. Retry only if the first request did not succeed.
            if (error && error.name === "TypeError" &&
                document.fullscreenElement !== stage) {
              await stage.requestFullscreen();
            } else {
              throw error;
            }
          }
        }
        scheduleCanvasResize();
        await settleSurface();
        scheduleCanvasResize();
        canvas.focus({ preventScroll: true });
      } catch (error) {
        console.warn("[shell] fullscreen transition failed:", error);
        reportFullscreenFailure(document.fullscreenElement === stage ||
                                document.webkitFullscreenElement === stage);
      } finally {
        transition = null;
        updateButton();
      }
    })();
    updateButton();
    return transition;
  };

  document.addEventListener("fullscreenchange", () => {
    clearStageStatus();
    scheduleCanvasResize();
    updateButton();
  });
  document.addEventListener("fullscreenerror", (event) => {
    console.warn("[shell] fullscreen error:", event);
    reportFullscreenFailure(document.fullscreenElement === stage ||
                            document.webkitFullscreenElement === stage);
    updateButton();
  });
  if (btn) btn.addEventListener("click", go);
  addEventListener("keydown", (e) => {
    // A bare F only. Ctrl/Cmd/Alt+F belong to the browser (find, menus, window
    // management) and must reach it unmodified.
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key.toLowerCase() === "f") {
      // Only while playing, and never while the user is typing in a field.
      const active = document.activeElement;
      const editing = active &&
        (/^(INPUT|TEXTAREA|SELECT)$/.test(active.tagName) ||
         active.isContentEditable);
      if (!stage.hidden && !e.repeat &&
          !editing) {
        e.preventDefault();
        go();
      }
    }
  });
  updateButton();
}

// ---- Offline shell (service worker) ----------------------------------------
// Registered ONLY on a published page, which is exactly a page whose asset URLs
// carry ?v=<commit>. A dev page served from build_web.sh output has no stamp, so
// nothing installs and no worker can pin a half-edited local shell.
//
// The worker's own URL carries the same stamp, so a new build is a new script to
// the browser: it installs beside the old one and does not activate until every
// document using the old one is gone (see sw.js — no skipWaiting on purpose).
// That is what keeps the JS/wasm pair consistent: whichever worker is in charge,
// its cache holds one build and only one.
function registerServiceWorker() {
  if (!BUILD_QUERY || !navigator.serviceWorker) return;
  addEventListener("load", () => {
    navigator.serviceWorker.register("sw.js" + BUILD_QUERY, { scope: "./" })
      .catch((error) => console.warn("[shell] offline cache unavailable:", error));
  });
}

// ---- Startup ---------------------------------------------------------------
(async () => {
  registerServiceWorker();
  wireRomUi();
  const romRetry = $("rom-storage-retry");
  if (romRetry) {
    romRetry.addEventListener("click", () => {
      retryRomPersistence().catch(() => {});
    });
  }
  wireFullscreen();
  wireFpsReadout();
  wireCanvasResize();
  // A touch-subsystem failure must never block the launcher: everything after
  // this line (save UI, revealing the ROM picker, the WebGPU gate message)
  // matters on exactly the browsers most likely to lack a touch-era API.
  try { wireTouchControls(); } catch (_) {}
  if (globalThis.MDKRSaveUI) {
    // Resolve write ownership BEFORE the save manager can be used, not at Play:
    // Import/Edit/Restore/Erase write the same shared /save database the engine
    // does, and they are reachable from the launcher without ever booting. The
    // claim is per-document and idempotent, so taking it here just means the
    // tab that opened the launcher first is the tab that may write -- which is
    // the answer the save manager needs to be honest about its own buttons.
    claimSaveOwnership()
      .then(publishSaveOwnership)
      .catch(() => {});
    // Do not gate the rest of the launcher on storage availability. The save
    // panel reports its own actionable error while ROM selection remains usable.
    globalThis.MDKRSaveUI.init().catch(() => {});
  }

  // ALWAYS reveal the launcher UI. It used to be hidden behind the WebGPU gate,
  // which meant a browser without a usable adapter showed nothing but an error
  // line -- no picker, no controls, no explanation of what the page even is, and
  // no way to get as far as trying. The gate now only blocks the Play ACTION.
  $("rom-ui").hidden = false;
  initQualityControls();

  const err = await gate();
  const msg = $("gate-msg");
  if (err) {
    msg.className = "status-line err";
    msg.textContent = err;
    const play = $("play");
    play.disabled = true;
    play.dataset.blocked = "1";     // keep it disabled even after a valid ROM
    play.title = err;
    await probeStoredRom();         // still show Forget, so storage stays clearable
    return;
  }
  msg.className = "status-line";
  msg.textContent = "";
  await probeStoredRom();
})().catch((error) => {
  // Nothing above this line has a caller. An unhandled rejection here used to
  // leave "Checking your browser…" on screen forever with the reason only in
  // devtools; the gate line is where a visitor is already looking.
  const detail = String(error && error.message ? error.message : error);
  testError(detail);
  testMark("startup-failed");
  const msg = $("gate-msg");
  if (msg) {
    msg.className = "status-line err";
    msg.textContent =
      "This page failed to start (" + detail.slice(0, 300) + "). Reload to try again.";
  }
  console.error("[shell] startup failed:", error);
});
