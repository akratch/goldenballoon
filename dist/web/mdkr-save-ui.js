// ROM-free browser save ownership for Golden Balloon.
//
// This file deliberately contains no EEPROM offsets or checksum logic. All
// binary interpretation and mutation goes through mdkr-save-tools.wasm, built
// from the same C codec used by the native engine and mdkr-save CLI.

"use strict";

globalThis.MDKRSaveUI = (() => {
  const SAVE_PATH = "/save/eeprom.bin";
  const VIDEO_CONFIG_PATH = "/save/mdkr64.ini";
  const STAGING_PATH = "/save/eeprom.bin.importing";
  const PREVIOUS_PATH = "/save/eeprom.bin.previous";
  const AUTOSAVE_PATHS = Object.freeze([
    "/save/eeprom.bin.autosave.1",
    "/save/eeprom.bin.autosave.2",
    "/save/eeprom.bin.autosave.3",
  ]);
  const CONTROLLER_PAK_PATHS = Object.freeze(
    [1, 2, 3, 4].map((port) => `/save/controller-pak-${port}.mdp`));
  const CONTROLLER_PAK_STAGING_PATHS = Object.freeze(
    CONTROLLER_PAK_PATHS.map((path) => path + ".importing"));
  const CONTROLLER_PAK_PREVIOUS_PATHS = Object.freeze(
    CONTROLLER_PAK_PATHS.map((path) => path + ".previous"));
  const CONTROLLER_PAK_IMAGE_SIZE = 32192;
  const MAX_PAK_INPUT = 256 * 1024;
  const MAX_INPUT = 64 * 1024;
  const IMAGE_SIZE = 512;
  const SAVE_FAULT_POINTS = Object.freeze([
    "after-stage-write",
    "after-stage-verify",
    "after-backup",
    "after-install",
    "after-persist",
    "after-reload",
  ]);
  const PAK_FAULT_POINTS = Object.freeze([
    "pak-after-stage",
    "pak-after-backup",
    "pak-after-install",
    "pak-after-persist",
    "pak-after-reload",
  ]);
  const textEncoder = new TextEncoder();
  const textDecoder = new TextDecoder("utf-8", { fatal: true });
  const byId = (id) => document.getElementById(id);

  const courseNames = [
    "Bluey I", "Fossil Canyon", "Pirate Lagoon", "Ancient Lake",
    "Walrus Cove", "Hot Top Volcano", "Whale Bay", "Snowball Valley",
    "Crescent Island", "Fire Mountain", "Everfrost Peak", "Spaceport Alpha",
    "Spacedust Alley", "Greenwood Village", "Boulder Canyon",
    "Windmill Plains", "Smokey Castle", "Darkwater Beach",
    "Icicle Pyramid", "Frosty Village", "Jungle Falls", "Treasure Caves",
    "Haunted Woods", "Darkmoon Caverns", "Star City", "Wizpig I",
    "Tricky I", "Bubbler I", "Smokey I", "Tricky II", "Bluey II",
    "Bubbler II", "Smokey II", "Wizpig II",
  ];
  const worldNames = [
    "Total", "Dino Domain", "Sherbet Island", "Snowflake Mountain",
    "Dragon Forest", "Future Fun Land",
  ];
  const ttCourseNames = [
    "Ancient Lake", "Fossil Canyon", "Jungle Falls", "Hot Top Volcano",
    "Whale Bay", "Crescent Island", "Pirate Lagoon", "Treasure Caves",
    "Everfrost Peak", "Walrus Cove", "Snowball Valley", "Frosty Village",
    "Boulder Canyon", "Greenwood Village", "Windmill Plains",
    "Haunted Woods", "Spacedust Alley", "Darkmoon Caverns", "Star City",
    "Spaceport Alpha",
  ];
  const normalRaceCourseIndices = [
    1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 19, 20, 21, 22, 23, 24,
  ];
  const statusLabels = {
    empty: "Empty",
    valid: "Valid",
    noncanonical: "Safe, unusual",
    corrupt: "Corrupt",
  };
  const blockLabels = [
    "Adventure file 1",
    "Adventure file 2",
    "Adventure file 3",
    "Global unlocks and options",
    "Fastest-lap records",
    "Course-time records",
  ];

  let saveModule = null;
  let repository = null;
  let readyPromise = null;
  let released = false;
  let operation = Promise.resolve();
  let candidateFileName = "";
  let dialogReturnFocus = null;

  function setStatus(message, kind = "") {
    const status = byId("save-status");
    if (!status) return;
    status.className = kind;
    status.textContent = message;
  }

  function setControlsDisabled(disabled) {
    for (const id of [
      "download-save", "download-save-raw", "import-save-button",
      "download-paks", "import-paks-button", "edit-save",
      "restore-save-snapshot", "clear-save",
    ]) {
      const element = byId(id);
      if (element) element.disabled = disabled;
    }
  }

  function loadFactory() {
    if (globalThis.createMDKRSaveTools) {
      return Promise.resolve(globalThis.createMDKRSaveTools);
    }
    return new Promise((resolve, reject) => {
      const script = document.createElement("script");
      // Ride the same cache-busting stamp as the shell (set there from its
      // own script URL): a stale save-tools wasm against a fresh shell is
      // exactly the mix the stamp exists to prevent, on the one module that
      // performs binary EEPROM interpretation.
      script.src = "mdkr-save-tools.js" + (globalThis.__mdkrBuildQuery || "");
      script.onload = () => globalThis.createMDKRSaveTools
        ? resolve(globalThis.createMDKRSaveTools)
        : reject(new Error("save-tools loader did not define its module factory"));
      script.onerror = () => reject(new Error("could not load mdkr-save-tools.js"));
      document.head.appendChild(script);
    });
  }

  function syncFs(populate) {
    return new Promise((resolve, reject) => {
      try {
        saveModule.FS.syncfs(populate, (error) =>
          error ? reject(error) : resolve());
      } catch (error) {
        reject(error);
      }
    });
  }

  function fileOrNull(path) {
    try {
      return new Uint8Array(saveModule.FS.readFile(path));
    } catch (_) {
      return null;
    }
  }

  function bytesEqual(left, right) {
    if (left === null || right === null) return left === right;
    if (!left || !right || left.length !== right.length) return false;
    let difference = 0;
    for (let i = 0; i < left.length; i++) difference |= left[i] ^ right[i];
    return difference === 0;
  }

  function injectFault(point) {
    const config = globalThis.__mdkrTestConfig;
    if (!config || config.saveFault !== point) return;
    config.saveFault = null;
    throw new Error("injected save transaction failure at " + point);
  }

  function serialize(action) {
    const run = operation.then(action, action);
    operation = run.catch(() => {});
    return run;
  }

  async function createRepository() {
    const factory = await loadFactory();
    saveModule = await factory({
      noInitialRun: true,
      locateFile: (path, prefix) =>
        (prefix || "") + path + (globalThis.__mdkrBuildQuery || ""),
    });
    try { saveModule.FS.mkdir("/save"); } catch (_) {}
    saveModule.FS.mount(saveModule.IDBFS, {}, "/save");
    await syncFs(true);

    return {
      async snapshot() {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          await syncFs(true);
          const bytes = fileOrNull(SAVE_PATH);
          if (bytes && bytes.length !== IMAGE_SIZE) {
            throw new Error(
              `Stored EEPROM is ${bytes.length} bytes; expected ${IMAGE_SIZE}. ` +
              "Export it as forensic data before replacing it.");
          }
          return bytes;
        });
      },

      async autosaves() {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          await syncFs(true);
          return AUTOSAVE_PATHS.map((path, index) => {
            const bytes = fileOrNull(path);
            if (!bytes) return null;
            if (bytes.length !== IMAGE_SIZE) {
              throw new Error(
                `Automatic snapshot ${index + 1} is ${bytes.length} bytes; ` +
                `expected ${IMAGE_SIZE}.`);
            }
            return bytes;
          });
        });
      },

      async previous() {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          await syncFs(true);
          const bytes = fileOrNull(PREVIOUS_PATH);
          if (bytes && bytes.length !== IMAGE_SIZE) {
            throw new Error(
              `The import/edit rollback is ${bytes.length} bytes; ` +
              `expected ${IMAGE_SIZE}.`);
          }
          return bytes;
        });
      },

      async hasAnyData() {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          await syncFs(true);
          return saveModule.FS.readdir("/save").some(
            (name) => name !== "." && name !== ".." &&
                      "/save/" + name !== VIDEO_CONFIG_PATH);
        });
      },

      async controllerPaks() {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          await syncFs(true);
          return CONTROLLER_PAK_PATHS.map((path, index) => {
            const bytes = fileOrNull(path);
            if (!bytes) return null;
            if (bytes.length !== CONTROLLER_PAK_IMAGE_SIZE) {
              throw new Error(
                `Controller Pak ${index + 1} is ${bytes.length} bytes; ` +
                `expected ${CONTROLLER_PAK_IMAGE_SIZE}.`);
            }
            return { port: index + 1, bytes };
          }).filter(Boolean);
        });
      },

      async replaceControllerPaks(candidates) {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          if (!Array.isArray(candidates) ||
              candidates.length !== CONTROLLER_PAK_PATHS.length ||
              candidates.some((bytes) =>
                bytes !== null &&
                (!(bytes instanceof Uint8Array) ||
                 bytes.length !== CONTROLLER_PAK_IMAGE_SIZE))) {
            throw new Error("invalid Controller Pak replacement set");
          }
          await syncFs(true);
          const before = CONTROLLER_PAK_PATHS.map(fileOrNull);
          const priorPrevious = CONTROLLER_PAK_PREVIOUS_PATHS.map(fileOrNull);
          let installed = false;
          try {
            for (let i = 0; i < candidates.length; i++) {
              try { saveModule.FS.unlink(CONTROLLER_PAK_STAGING_PATHS[i]); }
              catch (_) {}
              if (candidates[i]) {
                saveModule.FS.writeFile(
                  CONTROLLER_PAK_STAGING_PATHS[i], candidates[i]);
                if (!bytesEqual(
                      fileOrNull(CONTROLLER_PAK_STAGING_PATHS[i]),
                      candidates[i])) {
                  throw new Error(`Controller Pak ${i + 1} staging failed`);
                }
              }
            }
            injectFault("pak-after-stage");
            for (let i = 0; i < candidates.length; i++) {
              try { saveModule.FS.unlink(CONTROLLER_PAK_PREVIOUS_PATHS[i]); }
              catch (_) {}
              if (before[i]) {
                saveModule.FS.rename(
                  CONTROLLER_PAK_PATHS[i],
                  CONTROLLER_PAK_PREVIOUS_PATHS[i]);
              }
            }
            injectFault("pak-after-backup");
            for (let i = 0; i < candidates.length; i++) {
              if (candidates[i]) {
                saveModule.FS.rename(
                  CONTROLLER_PAK_STAGING_PATHS[i],
                  CONTROLLER_PAK_PATHS[i]);
              }
            }
            injectFault("pak-after-install");
            installed = true;
            await syncFs(false);
            injectFault("pak-after-persist");
            await syncFs(true);
            injectFault("pak-after-reload");
            for (let i = 0; i < candidates.length; i++) {
              if (!bytesEqual(
                    fileOrNull(CONTROLLER_PAK_PATHS[i]), candidates[i])) {
                throw new Error(
                  `Controller Pak ${i + 1} persisted-byte verification failed`);
              }
            }
          } catch (error) {
            try {
              for (let i = 0; i < candidates.length; i++) {
                try { saveModule.FS.unlink(CONTROLLER_PAK_PATHS[i]); }
                catch (_) {}
                try { saveModule.FS.unlink(CONTROLLER_PAK_STAGING_PATHS[i]); }
                catch (_) {}
                if (before[i]) {
                  const rollback = fileOrNull(
                    CONTROLLER_PAK_PREVIOUS_PATHS[i]);
                  if (rollback && bytesEqual(rollback, before[i])) {
                    saveModule.FS.rename(
                      CONTROLLER_PAK_PREVIOUS_PATHS[i],
                      CONTROLLER_PAK_PATHS[i]);
                  } else {
                    saveModule.FS.writeFile(
                      CONTROLLER_PAK_PATHS[i], before[i]);
                  }
                }
                try { saveModule.FS.unlink(CONTROLLER_PAK_PREVIOUS_PATHS[i]); }
                catch (_) {}
                if (priorPrevious[i]) {
                  saveModule.FS.writeFile(
                    CONTROLLER_PAK_PREVIOUS_PATHS[i], priorPrevious[i]);
                }
              }
              await syncFs(false);
              await syncFs(true);
              for (let i = 0; i < before.length; i++) {
                if (!bytesEqual(
                      fileOrNull(CONTROLLER_PAK_PATHS[i]), before[i])) {
                  throw new Error(
                    `Controller Pak ${i + 1} rollback verification failed`);
                }
              }
            } catch (rollbackError) {
              throw new Error(
                `${error.message || error}; rollback also failed: ` +
                (rollbackError.message || rollbackError));
            }
            throw error;
          }
          if (!installed) {
            throw new Error("Controller Pak install did not complete");
          }
        });
      },

      async seedAutosaveForTest(index, candidate) {
        return serialize(async () => {
          if (!globalThis.__mdkrTestConfig) {
            throw new Error("automatic snapshot seeding is test-only");
          }
          if (released) throw new Error("save manager was released to the game");
          if (!Number.isInteger(index) || index < 0 ||
              index >= AUTOSAVE_PATHS.length ||
              !(candidate instanceof Uint8Array) ||
              candidate.length !== IMAGE_SIZE) {
            throw new Error("invalid automatic snapshot fixture");
          }
          saveModule.FS.writeFile(AUTOSAVE_PATHS[index], candidate);
          await syncFs(false);
        });
      },

      async replace(candidate) {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          if (!(candidate instanceof Uint8Array) ||
              candidate.length !== IMAGE_SIZE) {
            throw new Error("candidate EEPROM is not exactly 512 bytes");
          }
          await syncFs(true);
          const before = fileOrNull(SAVE_PATH);
          const priorRollback = fileOrNull(PREVIOUS_PATH);
          let movedCurrent = false;
          let installed = false;
          try {
            try { saveModule.FS.unlink(STAGING_PATH); } catch (_) {}
            saveModule.FS.writeFile(STAGING_PATH, candidate);
            injectFault("after-stage-write");
            if (!bytesEqual(fileOrNull(STAGING_PATH), candidate)) {
              throw new Error("staged-byte verification failed");
            }
            injectFault("after-stage-verify");
            try { saveModule.FS.unlink(PREVIOUS_PATH); } catch (_) {}
            if (before) {
              saveModule.FS.rename(SAVE_PATH, PREVIOUS_PATH);
              movedCurrent = true;
            }
            injectFault("after-backup");
            saveModule.FS.rename(STAGING_PATH, SAVE_PATH);
            installed = true;
            injectFault("after-install");
            await syncFs(false);
            injectFault("after-persist");
            await syncFs(true);
            injectFault("after-reload");
            if (!bytesEqual(fileOrNull(SAVE_PATH), candidate)) {
              throw new Error("persisted-byte verification failed");
            }
          } catch (error) {
            try {
              if (installed) {
                try { saveModule.FS.unlink(SAVE_PATH); } catch (_) {}
              }
              if (movedCurrent) {
                saveModule.FS.rename(PREVIOUS_PATH, SAVE_PATH);
              } else if (before) {
                saveModule.FS.writeFile(SAVE_PATH, before);
              } else {
                try { saveModule.FS.unlink(SAVE_PATH); } catch (_) {}
              }
              if (priorRollback) {
                saveModule.FS.writeFile(PREVIOUS_PATH, priorRollback);
              } else if (!movedCurrent) {
                try { saveModule.FS.unlink(PREVIOUS_PATH); } catch (_) {}
              }
              try { saveModule.FS.unlink(STAGING_PATH); } catch (_) {}
              await syncFs(false);
            } catch (rollbackError) {
              throw new Error(
                `${error.message || error}; rollback also failed: ` +
                (rollbackError.message || rollbackError));
            }
            throw error;
          }
        });
      },

      async clear() {
        return serialize(async () => {
          if (released) throw new Error("save manager was released to the game");
          await syncFs(true);
          for (const name of saveModule.FS.readdir("/save")) {
            if (name === "." || name === "..") continue;
            if ("/save/" + name === VIDEO_CONFIG_PATH) continue;
            try { saveModule.FS.unlink("/save/" + name); } catch (_) {}
          }
          await syncFs(false);
          await syncFs(true);
          const remaining = saveModule.FS.readdir("/save").filter(
            (name) => name !== "." && name !== ".." &&
                      "/save/" + name !== VIDEO_CONFIG_PATH);
          if (remaining.length !== 0) {
            throw new Error(
              "stored data remains after erase: " + remaining.join(", "));
          }
        });
      },

      async release() {
        return serialize(async () => {
          if (released) return;
          await syncFs(false);
          released = true;
          setControlsDisabled(true);
        });
      },

      async resume() {
        return serialize(async () => {
          await syncFs(true);
          released = false;
          setControlsDisabled(false);
        });
      },
    };
  }

  function allocateBytes(bytes) {
    const pointer = saveModule._malloc(Math.max(1, bytes.length));
    if (!pointer) throw new Error("save-tools memory allocation failed");
    saveModule.HEAPU8.set(bytes, pointer);
    return pointer;
  }

  function allocateString(value) {
    const encoded = textEncoder.encode(String(value));
    const pointer = saveModule._malloc(encoded.length + 1);
    if (!pointer) throw new Error("save-tools string allocation failed");
    saveModule.HEAPU8.set(encoded, pointer);
    saveModule.HEAPU8[pointer + encoded.length] = 0;
    return pointer;
  }

  function codecError(code) {
    const messages = {
      101: "invalid save-tools argument",
      102: "unsupported file size",
      103: "malformed backup container",
      104: "unsupported backup version or payload format",
      105: "invalid backup payload",
      106: "backup digest does not match its payload",
      107: "save-tools output buffer was too small",
      200: "EEPROM codec rejected the payload",
      "-1": "invalid edit",
      "-2": "EEPROM codec rejected the image",
      "-3": "save-tools output buffer was too small",
      "-4": "field value is outside the game's safe range",
      "-5": "this corrupt block must be reset before it can be edited",
    };
    return messages[code] || `save-tools error ${code}`;
  }

  const codec = {
    load(bytes) {
      const pointer = allocateBytes(bytes);
      try {
        const result = saveModule._mdkr_save_tools_load(pointer, bytes.length);
        if (result !== 0) throw new Error(codecError(result));
      } finally {
        saveModule._free(pointer);
      }
    },

    blank() {
      saveModule._mdkr_save_tools_blank();
    },

    summary() {
      const required = saveModule._mdkr_save_tools_summary_size();
      if (!required) throw new Error("could not size save summary");
      const pointer = saveModule._malloc(required);
      if (!pointer) throw new Error("save-tools memory allocation failed");
      try {
        const result = saveModule._mdkr_save_tools_summary(pointer, required);
        if (result !== 0) throw new Error(codecError(result));
        const bytes = saveModule.HEAPU8.slice(pointer, pointer + required - 1);
        return JSON.parse(textDecoder.decode(bytes));
      } finally {
        saveModule._free(pointer);
      }
    },

    raw() {
      const pointer = saveModule._malloc(IMAGE_SIZE);
      if (!pointer) throw new Error("save-tools memory allocation failed");
      try {
        const result = saveModule._mdkr_save_tools_copy_raw(pointer, IMAGE_SIZE);
        if (result !== 0) throw new Error(codecError(result));
        return saveModule.HEAPU8.slice(pointer, pointer + IMAGE_SIZE);
      } finally {
        saveModule._free(pointer);
      }
    },

    inputMetadata() {
      const format = saveModule._mdkr_save_tools_input_format() >>> 0;
      if (format === 0) return null;
      if (format !== 1) {
        throw new Error("save-tools returned an invalid input format");
      }
      const readField = (field) => {
        const required = saveModule._mdkr_save_tools_metadata(field, 0, 0);
        if (!required) throw new Error("could not read backup metadata");
        const pointer = saveModule._malloc(required);
        if (!pointer) throw new Error("save-tools memory allocation failed");
        try {
          const written = saveModule._mdkr_save_tools_metadata(
            field, pointer, required);
          if (written !== required) {
            throw new Error("save-tools returned truncated backup metadata");
          }
          return textDecoder.decode(
            saveModule.HEAPU8.slice(pointer, pointer + required - 1));
        } finally {
          saveModule._free(pointer);
        }
      };
      return {
        createdAt: readField(0),
        appVersion: readField(1),
        source: readField(2),
      };
    },

    container() {
      const values = [
        new Date().toISOString(),
        document.documentElement.dataset.buildVersion || "browser",
        "web",
      ];
      const pointers = values.map(allocateString);
      try {
        const required = saveModule._mdkr_save_tools_container_size(...pointers);
        if (!required) throw new Error("could not size backup container");
        const output = saveModule._malloc(required);
        if (!output) throw new Error("save-tools memory allocation failed");
        try {
          const result = saveModule._mdkr_save_tools_export_container(
            ...pointers, output, required, 0);
          if (result !== 0) throw new Error(codecError(result));
          return saveModule.HEAPU8.slice(output, output + required - 1);
        } finally {
          saveModule._free(output);
        }
      } finally {
        pointers.forEach((pointer) => saveModule._free(pointer));
      }
    },

    corruptMask() {
      return saveModule._mdkr_save_tools_corrupt_mask() >>> 0;
    },

    diffCount() {
      return saveModule._mdkr_save_tools_diff_count() >>> 0;
    },

    acceptBaseline() {
      saveModule._mdkr_save_tools_accept_baseline();
    },

    call(name, ...values) {
      const result = saveModule[name](...values);
      if (result !== 0) throw new Error(codecError(result));
    },

    name(slot, value) {
      const pointer = allocateString(value);
      try {
        this.call("_mdkr_save_tools_slot_name", slot, pointer);
      } finally {
        saveModule._free(pointer);
      }
    },

    copyBlocks(source, blockMask) {
      const pointer = allocateBytes(source);
      try {
        this.call(
          "_mdkr_save_tools_copy_blocks",
          pointer,
          source.length,
          blockMask >>> 0);
      } finally {
        saveModule._free(pointer);
      }
    },

    validatePak(bytes) {
      if (!(bytes instanceof Uint8Array) ||
          bytes.length !== CONTROLLER_PAK_IMAGE_SIZE) {
        throw new Error(
          `Controller Pak image must be exactly ` +
          `${CONTROLLER_PAK_IMAGE_SIZE} bytes`);
      }
      const pointer = allocateBytes(bytes);
      try {
        const result = saveModule._mdkr_save_tools_validate_pak(
          pointer, bytes.length);
        if (result !== 0) {
          const reasons = {
            1: "invalid argument",
            2: "invalid format or directory extents",
            3: "SHA-256 digest mismatch",
          };
          throw new Error(
            "Controller Pak validator rejected the image: " +
            (reasons[result] || `error ${result}`));
        }
      } finally {
        saveModule._free(pointer);
      }
    },
  };

  function timestampForFile() {
    return new Date().toISOString().replace(/[-:]/g, "").replace(/\..+/, "")
      .replace("T", "-");
  }

  function downloadBytes(bytes, name, type) {
    const blob = new Blob([bytes], { type });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = name;
    anchor.hidden = true;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }

  function bytesToBase64(bytes) {
    let binary = "";
    const chunk = 8192;
    for (let offset = 0; offset < bytes.length; offset += chunk) {
      binary += String.fromCharCode(
        ...bytes.subarray(offset, Math.min(offset + chunk, bytes.length)));
    }
    return btoa(binary);
  }

  function base64ToBytes(value) {
    if (typeof value !== "string" || value.length === 0) {
      throw new Error("Controller Pak bundle contains an empty image");
    }
    let binary;
    try {
      binary = atob(value);
    } catch (_) {
      throw new Error("Controller Pak bundle contains invalid base64");
    }
    if (btoa(binary) !== value) {
      throw new Error("Controller Pak bundle uses a non-canonical base64 image");
    }
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    return bytes;
  }

  function summaryLine(summary) {
    const slots = summary.slots.map((slot, index) => {
      if (slot.status === "empty") return `File ${index + 1}: empty`;
      return `File ${index + 1}: ${slot.name.trim() || "(unnamed)"}, ` +
        `${slot.balloons[0]} balloons, ${statusLabels[slot.status]}`;
    });
    return [...slots,
      `Fastest laps: ${statusLabels[summary.records.fastLaps.status]} ` +
        `(${summary.records.fastLaps.count})`,
      `Course times: ${statusLabels[summary.records.courseTimes.status]} ` +
        `(${summary.records.courseTimes.count})`,
    ];
  }

  function appendSummary(parent, title, summary) {
    const section = document.createElement("section");
    section.className = "save-summary";
    const heading = document.createElement("h3");
    heading.textContent = title;
    section.appendChild(heading);
    const digest = document.createElement("p");
    digest.className = "save-digest";
    digest.textContent = `SHA-256 ${summary.sha256}`;
    section.appendChild(digest);
    const list = document.createElement("ul");
    for (const line of summaryLine(summary)) {
      const item = document.createElement("li");
      item.textContent = line;
      list.appendChild(item);
    }
    section.appendChild(list);
    parent.appendChild(section);
  }

  function appendImportMetadata(parent, metadata) {
    if (!metadata) return;
    const section = document.createElement("section");
    section.className = "save-summary";
    const heading = document.createElement("h3");
    heading.textContent = "Backup details (informational)";
    const list = document.createElement("dl");
    for (const [label, value] of [
      ["Created", metadata.createdAt],
      ["App version", metadata.appVersion],
      ["Source", metadata.source],
    ]) {
      const term = document.createElement("dt");
      const description = document.createElement("dd");
      term.textContent = label;
      description.textContent = value || "Not provided";
      list.append(term, description);
    }
    section.append(heading, list);
    parent.appendChild(section);
  }

  function openDialog(title) {
    const dialog = byId("save-dialog");
    dialogReturnFocus = document.activeElement instanceof HTMLElement
      ? document.activeElement : null;
    byId("save-dialog-title").textContent = title;
    byId("save-dialog-body").replaceChildren();
    byId("save-dialog-actions").replaceChildren();
    byId("save-dialog-status").textContent = "";
    byId("save-dialog-status").className = "";
    dialog.showModal();
    byId("save-dialog-close").focus();
    return dialog;
  }

  function dialogStatus(message, kind = "") {
    const status = byId("save-dialog-status");
    status.className = kind;
    status.textContent = message;
  }

  function actionButton(label, className = "btn btn-ghost") {
    const button = document.createElement("button");
    button.type = "button";
    button.className = className;
    button.textContent = label;
    return button;
  }

  async function downloadPortable() {
    try {
      const bytes = await repository.snapshot();
      if (!bytes) {
        if (!confirm(
          "There is no saved progress yet. Download a canonical blank backup?")) {
          return;
        }
        codec.blank();
      } else {
        codec.load(bytes);
      }
      const summary = codec.summary();
      if (codec.corruptMask() !== 0) {
        setStatus(
          "The stored save has corrupt blocks. Download the raw image or open " +
          "the editor's recovery preview before making a portable backup.", "err");
        return;
      }
      const container = codec.container();
      const name = `mdkr64-save-${timestampForFile()}.mdkr-save`;
      downloadBytes(container, name, "application/json");
      setStatus(`Downloaded ${name} (${container.length} bytes, ` +
                `${summary.sha256.slice(0, 12)}…).`, "ok");
    } catch (error) {
      setStatus("Backup failed: " + (error.message || error), "err");
    }
  }

  async function downloadRaw() {
    try {
      const bytes = await repository.snapshot();
      if (!bytes) {
        setStatus("There is no saved progress stored for this site.");
        return;
      }
      codec.load(bytes);
      const summary = codec.summary();
      const name = `dkr-eeprom-${timestampForFile()}.eep`;
      downloadBytes(codec.raw(), name, "application/octet-stream");
      setStatus(`Downloaded ${name} (512 bytes, ` +
                `${summary.sha256.slice(0, 12)}…).`, "ok");
    } catch (error) {
      setStatus("Raw export failed: " + (error.message || error), "err");
    }
  }

  async function downloadControllerPaks() {
    try {
      const packs = await repository.controllerPaks();
      if (!packs.length) {
        setStatus("No Controller Pak ghosts are stored for this site.");
        return;
      }
      const documentBytes = textEncoder.encode(JSON.stringify({
        format: "mdkr64-controller-paks",
        version: 1,
        createdAt: new Date().toISOString(),
        packs: packs.map(({ port, bytes }) => ({
          port,
          byteLength: bytes.length,
          image: bytesToBase64(bytes),
        })),
      }) + "\n");
      const name = `mdkr64-paks-${timestampForFile()}.mdkr-paks`;
      downloadBytes(documentBytes, name, "application/json");
      setStatus(
        `Downloaded ${name} with ${packs.length} Controller Pak` +
        `${packs.length === 1 ? "" : "s"}.`, "ok");
    } catch (error) {
      setStatus(
        "Controller Pak backup failed: " + (error.message || error), "err");
    }
  }

  async function acceptControllerPakBundle(file) {
    if (!file) return;
    if (released) {
      setStatus("Reload the launcher before managing saves.", "err");
      return;
    }
    if (file.size > MAX_PAK_INPUT) {
      setStatus("That Controller Pak bundle exceeds the 256 KiB limit.", "err");
      return;
    }
    try {
      const source = new Uint8Array(await file.arrayBuffer());
      const bundle = JSON.parse(textDecoder.decode(source));
      if (!bundle || bundle.format !== "mdkr64-controller-paks" ||
          bundle.version !== 1 || !Array.isArray(bundle.packs) ||
          bundle.packs.length < 1 ||
          bundle.packs.length > CONTROLLER_PAK_PATHS.length) {
        throw new Error("unsupported Controller Pak bundle");
      }
      const candidates = CONTROLLER_PAK_PATHS.map(() => null);
      const seen = new Set();
      for (const entry of bundle.packs) {
        if (!entry || !Number.isInteger(entry.port) ||
            entry.port < 1 || entry.port > CONTROLLER_PAK_PATHS.length ||
            seen.has(entry.port)) {
          throw new Error("bundle has an invalid or duplicate controller port");
        }
        seen.add(entry.port);
        const bytes = base64ToBytes(entry.image);
        if (entry.byteLength !== bytes.length ||
            bytes.length !== CONTROLLER_PAK_IMAGE_SIZE) {
          throw new Error(
            `Controller Pak ${entry.port} has an invalid declared size`);
        }
        codec.validatePak(bytes);
        candidates[entry.port - 1] = bytes;
      }
      const current = await repository.controllerPaks();
      const ports = [...seen].sort((a, b) => a - b).join(", ");
      const removed = current
        .map(({ port }) => port)
        .filter((port) => !seen.has(port));
      const removalText = removed.length
        ? `\n\nStored Pak${removed.length === 1 ? "" : "s"} ` +
          `${removed.join(", ")} will be removed because the bundle does not ` +
          "contain them."
        : "";
      if (!confirm(
        `Import ${file.name || "this bundle"} for controller ` +
        `port${seen.size === 1 ? "" : "s"} ${ports}?` +
        removalText +
        "\n\nCurrent images are retained as local .previous rollback files.")) {
        return;
      }
      setControlsDisabled(true);
      setStatus("Staging and verifying Controller Pak bundle…");
      await repository.replaceControllerPaks(candidates);
      setStatus(
        `Imported and verified ${seen.size} Controller Pak` +
        `${seen.size === 1 ? "" : "s"}. Reload or press Play to use them.`,
        "ok");
    } catch (error) {
      setStatus(
        `Could not import ${file.name || "Controller Pak bundle"}: ` +
        (error.message || error), "err");
    } finally {
      const input = byId("import-paks-input");
      if (input) input.value = "";
      if (!released) setControlsDisabled(false);
    }
  }

  function setImportPreview(
    candidateBytes, candidateSummary, currentSummary, currentBytes,
    candidateMetadata) {
    const body = byId("save-dialog-body");
    body.replaceChildren();
    appendSummary(body, "Import candidate", candidateSummary);
    appendImportMetadata(body, candidateMetadata);
    if (currentSummary) appendSummary(body, "Currently stored", currentSummary);
    const note = document.createElement("p");
    note.className = "fine";
    note.textContent =
      "Import replaces the whole 512-byte EEPROM. A local rollback image is " +
      "kept, and the new bytes are persisted, reloaded, and verified before " +
      "success is reported.";
    body.appendChild(note);

    if (currentSummary && currentBytes &&
        !bytesEqual(candidateBytes, currentBytes)) {
      const merge = document.createElement("section");
      merge.className = "save-editor-card";
      const heading = document.createElement("h3");
      heading.textContent = "Copy selected data into the current save";
      const explanation = document.createElement("p");
      explanation.className = "fine";
      explanation.textContent =
        "Select complete, independently checksummed blocks from the import " +
        "candidate. Unselected current data remains byte-for-byte unchanged. " +
        "This creates another preview; it never writes immediately.";
      const choices = document.createElement("div");
      choices.className = "save-check-grid";
      const inputs = blockLabels.map((label, index) => {
        const control = checkbox(
          `${label} — ${statusLabels[candidateSummary.blocks[index]]}`,
          false);
        control.input.disabled =
          candidateSummary.blocks[index] === "corrupt";
        choices.appendChild(control.label);
        return control.input;
      });
      const build = actionButton("Build merged preview");
      build.disabled = true;
      const refreshButton = () => {
        build.disabled = !inputs.some((input) =>
          input.checked && !input.disabled);
      };
      inputs.forEach((input) =>
        input.addEventListener("change", refreshButton));
      build.addEventListener("click", () => {
        try {
          let mask = 0;
          inputs.forEach((input, index) => {
            if (input.checked && !input.disabled) mask |= 1 << index;
          });
          codec.load(currentBytes);
          codec.copyBlocks(candidateBytes, mask);
          const mergedBytes = codec.raw();
          const mergedSummary = codec.summary();
          setImportPreview(
            mergedBytes, mergedSummary, currentSummary, currentBytes,
            candidateMetadata);
          dialogStatus(
            "Merged draft created. Review the complete summary before " +
            "replacing the stored save.", "ok");
        } catch (error) {
          dialogStatus(
            "Could not build merged preview: " +
            (error.message || error), "err");
        }
      });
      merge.append(heading, explanation, choices, build);
      body.appendChild(merge);
    }

    const actions = byId("save-dialog-actions");
    actions.replaceChildren();
    const mask = codec.corruptMask();
    if (mask !== 0) {
      const recover = actionButton("Reset corrupt blocks");
      recover.addEventListener("click", () => {
        try {
          codec.call("_mdkr_save_tools_recover", mask);
          setImportPreview(
            codec.raw(), codec.summary(), currentSummary, currentBytes,
            candidateMetadata);
          dialogStatus(
            "Recovery draft created. Review the exact blocks listed above " +
            "before replacing your save.", "ok");
        } catch (error) {
          dialogStatus("Recovery failed: " + (error.message || error), "err");
        }
      });
      actions.appendChild(recover);
    }
    const replace = actionButton("Replace saved progress", "btn btn-primary");
    const span = document.createElement("span");
    span.textContent = "Replace saved progress";
    replace.replaceChildren(span);
    replace.disabled = mask !== 0;
    replace.addEventListener("click", async () => {
      if (!confirm(
        "Replace the browser's saved progress with this previewed file?\n\n" +
        "The existing image will be retained locally as eeprom.bin.previous.")) {
        return;
      }
      replace.disabled = true;
      dialogStatus("Staging, persisting, and verifying…");
      try {
        await repository.replace(candidateBytes);
        codec.acceptBaseline();
        dialogStatus("Import verified. Reload or press Play to use it.", "ok");
        setStatus(`Imported ${candidateFileName || "backup"} successfully.`, "ok");
      } catch (error) {
        replace.disabled = false;
        dialogStatus(
          "Import failed; the prior save was restored: " +
          (error.message || error), "err");
      }
    });
    actions.appendChild(replace);
  }

  async function acceptImportFile(file) {
    if (!file) return;
    if (released) {
      setStatus("Reload the launcher before managing saves.", "err");
      return;
    }
    candidateFileName = file.name || "selected file";
    if (file.size > MAX_INPUT) {
      setStatus("That file exceeds the 64 KiB save-import limit.", "err");
      return;
    }
    try {
      const input = new Uint8Array(await file.arrayBuffer());
      codec.load(input);
      const candidateMetadata = codec.inputMetadata();
      const candidateSummary = codec.summary();
      const candidate = codec.raw();
      const current = await repository.snapshot();
      let currentSummary = null;
      if (current) {
        codec.load(current);
        currentSummary = codec.summary();
        codec.load(input);
      }
      openDialog("Import save backup");
      setImportPreview(
        candidate, candidateSummary, currentSummary, current,
        candidateMetadata);
      if (codec.corruptMask() !== 0) {
        dialogStatus(
          "This file contains corrupt blocks. Ordinary import is disabled; " +
          "use the explicit recovery preview or cancel.", "err");
      }
    } catch (error) {
      setStatus(
        `Could not import ${candidateFileName}: ${error.message || error}`,
        "err");
    } finally {
      const input = byId("import-save-input");
      if (input) input.value = "";
    }
  }

  function labeledInput(labelText, input) {
    const label = document.createElement("label");
    label.className = "save-field";
    const text = document.createElement("span");
    text.textContent = labelText;
    label.append(text, input);
    return label;
  }

  function numberInput(value, min, max) {
    const input = document.createElement("input");
    input.type = "number";
    input.min = String(min);
    input.max = String(max);
    input.step = "1";
    input.value = String(value);
    return input;
  }

  function checkbox(labelText, checked) {
    const label = document.createElement("label");
    label.className = "save-check";
    const input = document.createElement("input");
    input.type = "checkbox";
    input.checked = Boolean(checked);
    const text = document.createElement("span");
    text.textContent = labelText;
    label.append(input, text);
    return { label, input };
  }

  function selectInput(value, labels) {
    const select = document.createElement("select");
    labels.forEach((label, index) => {
      const option = document.createElement("option");
      option.value = String(index);
      option.textContent = label;
      select.appendChild(option);
    });
    select.value = String(value);
    return select;
  }

  function editSafely(action) {
    try {
      action();
      updateEditorReview();
    } catch (error) {
      dialogStatus("Draft edit rejected: " + (error.message || error), "err");
      renderEditor();
    }
  }

  function slotField(slot, field, value) {
    codec.call("_mdkr_save_tools_slot_field", slot, field, value >>> 0);
  }

  function renderSlot(parent, slot, slotIndex) {
    const card = document.createElement("section");
    card.className = "save-editor-card";
    const heading = document.createElement("h3");
    heading.textContent = `Adventure file ${slotIndex + 1} — ` +
      statusLabels[slot.status];
    card.appendChild(heading);
    if (slot.status === "corrupt") {
      const warning = document.createElement("p");
      warning.className = "err";
      warning.textContent =
        "This slot is corrupt and cannot be edited. Use recovery to reset it.";
      card.appendChild(warning);
      parent.appendChild(card);
      return;
    }
    if (slot.status === "empty") {
      const create = actionButton("Create fresh slot");
      create.addEventListener("click", () => editSafely(() => {
        codec.call("_mdkr_save_tools_slot_state", slotIndex, 1, 0);
        renderEditor();
      }));
      card.appendChild(create);
      parent.appendChild(card);
      return;
    }

    const basic = document.createElement("div");
    basic.className = "save-fields";
    const name = document.createElement("input");
    name.type = "text";
    name.maxLength = 3;
    name.pattern = "[A-Z.? ]{0,3}";
    name.autocomplete = "off";
    name.value = slot.name.trimEnd();
    name.addEventListener("change", () => editSafely(() => {
      const normalized = name.value.toUpperCase();
      if (!/^[A-Z.? ]{0,3}$/.test(normalized)) {
        throw new Error("names use at most three letters, period, question mark, or space");
      }
      codec.name(slotIndex, normalized);
      name.value = normalized;
    }));
    basic.appendChild(labeledInput("Name", name));
    worldNames.forEach((world, index) => {
      const input = numberInput(slot.balloons[index], 0, 127);
      input.addEventListener("change", () => editSafely(() =>
        codec.call("_mdkr_save_tools_balloon", slotIndex, index,
                   Number(input.value))));
      basic.appendChild(labeledInput(`${world} balloons`, input));
    });
    for (const [label, field, value] of [
      ["T.T. amulet pieces", 3, slot.ttAmulet],
      ["Wizpig amulet pieces", 4, slot.wizpigAmulet],
    ]) {
      const input = numberInput(value, 0, 4);
      input.addEventListener("change", () => editSafely(() =>
        slotField(slotIndex, field, Number(input.value))));
      basic.appendChild(labeledInput(label, input));
    }
    card.appendChild(basic);

    const keys = document.createElement("fieldset");
    keys.className = "save-check-grid";
    const keysLegend = document.createElement("legend");
    keysLegend.textContent = "Keys";
    keys.appendChild(keysLegend);
    [
      ["Dino Domain", 1], ["Snowflake Mountain", 2],
      ["Sherbet Island", 3], ["Dragon Forest", 4],
    ].forEach(([label, bit]) => {
      const control = checkbox(label, slot.keys & (1 << bit));
      control.input.addEventListener("change", () => editSafely(() => {
        const next = control.input.checked
          ? slot.keys | (1 << bit) : slot.keys & ~(1 << bit);
        slotField(slotIndex, 5, next);
        slot.keys = next;
      }));
      keys.appendChild(control.label);
    });
    card.appendChild(keys);

    const taj = document.createElement("fieldset");
    taj.className = "save-check-grid";
    const tajLegend = document.createElement("legend");
    tajLegend.textContent = "Taj vehicle challenges";
    taj.appendChild(tajLegend);
    ["Car", "Hovercraft", "Plane"].forEach((vehicle, index) => {
      const offered = checkbox(`${vehicle} offered`, slot.tajFlags & (1 << index));
      const beaten = checkbox(`${vehicle} beaten`, slot.tajFlags & (1 << (index + 3)));
      const commit = () => editSafely(() => {
        if (beaten.input.checked) offered.input.checked = true;
        if (!offered.input.checked) beaten.input.checked = false;
        let flags = slot.tajFlags & ~((1 << index) | (1 << (index + 3)));
        if (offered.input.checked) flags |= 1 << index;
        if (beaten.input.checked) flags |= 1 << (index + 3);
        slotField(slotIndex, 0, flags);
        slot.tajFlags = flags;
      });
      offered.input.addEventListener("change", commit);
      beaten.input.addEventListener("change", commit);
      taj.append(offered.label, beaten.label);
    });
    card.appendChild(taj);

    const trophies = document.createElement("fieldset");
    trophies.className = "save-fields";
    const trophyLegend = document.createElement("legend");
    trophyLegend.textContent = "Trophy progress";
    trophies.appendChild(trophyLegend);
    ["Dino Domain", "Sherbet Island", "Snowflake Mountain", "Dragon Forest"]
      .forEach((world, index) => {
        const select = selectInput((slot.trophies >> (index * 2)) & 3,
          ["None", "Bronze", "Silver", "Gold"]);
        select.addEventListener("change", () => editSafely(() => {
          const shift = index * 2;
          const value = (slot.trophies & ~(3 << shift)) |
            (Number(select.value) << shift);
          slotField(slotIndex, 1, value);
          slot.trophies = value;
        }));
        trophies.appendChild(labeledInput(world, select));
      });
    card.appendChild(trophies);

    const bosses = document.createElement("fieldset");
    bosses.className = "save-check-grid";
    const bossLegend = document.createElement("legend");
    bossLegend.textContent = "Boss completion";
    bosses.appendChild(bossLegend);
    [
      "Wizpig I", "Tricky I", "Bubbler I", "Bluey I", "Smokey I",
      "Wizpig II", "Tricky II", "Bubbler II", "Bluey II", "Smokey II",
    ].forEach((label, bit) => {
      const control = checkbox(label, slot.bosses & (1 << bit));
      control.input.addEventListener("change", () => editSafely(() => {
        const value = control.input.checked
          ? slot.bosses | (1 << bit) : slot.bosses & ~(1 << bit);
        slotField(slotIndex, 2, value);
        slot.bosses = value;
      }));
      bosses.appendChild(control.label);
    });
    card.appendChild(bosses);

    const courses = document.createElement("details");
    const coursesSummary = document.createElement("summary");
    coursesSummary.textContent = "Course progress (34 entries)";
    courses.appendChild(coursesSummary);
    const courseGrid = document.createElement("div");
    courseGrid.className = "save-fields";
    courseNames.forEach((course, index) => {
      const select = selectInput(slot.courses[index],
        ["Not visited", "Visited", "Cleared", "Silver challenge cleared"]);
      select.addEventListener("change", () => editSafely(() => {
        codec.call("_mdkr_save_tools_course", slotIndex, index,
                   Number(select.value));
        slot.courses[index] = Number(select.value);
      }));
      courseGrid.appendChild(labeledInput(course, select));
    });
    courses.appendChild(courseGrid);
    card.appendChild(courses);

    const advanced = document.createElement("details");
    const advancedSummary = document.createElement("summary");
    advancedSummary.textContent = "Advanced flags";
    advanced.appendChild(advancedSummary);
    const advancedFields = document.createElement("div");
    advancedFields.className = "save-fields";
    const cutscenes = numberInput(slot.cutscenes, 0, 4294967295);
    cutscenes.addEventListener("change", () => editSafely(() =>
      slotField(slotIndex, 6, Number(cutscenes.value))));
    advancedFields.appendChild(labeledInput("Cutscene flag word", cutscenes));
    worldNames.forEach((world, index) => {
      const input = numberInput(slot.worldFlags[index], 0, 65535);
      input.addEventListener("change", () => editSafely(() =>
        codec.call("_mdkr_save_tools_world_flags", slotIndex, index,
                   Number(input.value))));
      advancedFields.appendChild(labeledInput(`${world} flags`, input));
    });
    advanced.appendChild(advancedFields);
    card.appendChild(advanced);

    const erase = actionButton("Erase this slot", "btn btn-danger");
    erase.addEventListener("click", () => {
      if (!confirm(`Erase Adventure file ${slotIndex + 1} in this draft?`)) return;
      editSafely(() => {
        codec.call("_mdkr_save_tools_slot_state", slotIndex, 0, 1);
        renderEditor();
      });
    });
    card.appendChild(erase);
    parent.appendChild(card);
  }

  function renderGlobal(parent, config, records) {
    const card = document.createElement("section");
    card.className = "save-editor-card";
    const heading = document.createElement("h3");
    heading.textContent = `Global settings — ${statusLabels[config.status]}`;
    card.appendChild(heading);
    if (config.status === "corrupt") {
      const warning = document.createElement("p");
      warning.className = "err";
      warning.textContent =
        "The global settings block is corrupt. Reset it in recovery first.";
      card.appendChild(warning);
      parent.appendChild(card);
      return;
    }
    for (const [label, field, value] of [
      ["Adventure Two unlocked", 0, config.adventureTwo],
      ["Drumstick unlocked", 1, config.drumstick],
      ["Subtitles enabled", 5, config.subtitles],
    ]) {
      const control = checkbox(label, value);
      control.input.addEventListener("change", () => editSafely(() =>
        codec.call("_mdkr_save_tools_config", field,
                   control.input.checked ? 1 : 0)));
      card.appendChild(control.label);
    }
    const language = selectInput(config.language,
      ["English", "German", "French", "Japanese"]);
    language.addEventListener("change", () => editSafely(() =>
      codec.call("_mdkr_save_tools_config", 2, Number(language.value))));
    card.appendChild(labeledInput("Language", language));

    const tt = document.createElement("details");
    const ttSummary = document.createElement("summary");
    ttSummary.textContent = "T.T. course unlocks";
    tt.appendChild(ttSummary);
    const grid = document.createElement("div");
    grid.className = "save-check-grid";
    ttCourseNames.forEach((name, bit) => {
      const control = checkbox(name, config.ttCourses & (1 << bit));
      control.input.addEventListener("change", () => editSafely(() => {
        const current = codec.summary().config.ttCourses;
        const value = control.input.checked
          ? current | (1 << bit) : current & ~(1 << bit);
        codec.call("_mdkr_save_tools_config", 3, value);
      }));
      grid.appendChild(control.label);
    });
    tt.appendChild(grid);
    card.appendChild(tt);

    const recordActions = document.createElement("div");
    recordActions.className = "actions";
    const resetLaps = actionButton(
      `Reset fastest laps (${records.fastLaps.count})`);
    resetLaps.addEventListener("click", () => editSafely(() =>
      codec.call("_mdkr_save_tools_reset_records", 1)));
    const resetTimes = actionButton(
      `Reset course times (${records.courseTimes.count})`);
    resetTimes.addEventListener("click", () => editSafely(() =>
      codec.call("_mdkr_save_tools_reset_records", 2)));
    recordActions.append(resetLaps, resetTimes);
    card.appendChild(recordActions);
    parent.appendChild(card);
  }

  function semanticDiff(before, after) {
    const changes = [];
    before.slots.forEach((oldSlot, index) => {
      const next = after.slots[index];
      if (oldSlot.status !== next.status) {
        changes.push(`File ${index + 1}: ${oldSlot.status} → ${next.status}`);
      }
      for (const [field, label] of [
        ["name", "name"], ["tajFlags", "Taj flags"],
        ["trophies", "trophies"], ["bosses", "bosses"],
        ["ttAmulet", "T.T. amulet"], ["wizpigAmulet", "Wizpig amulet"],
        ["keys", "keys"], ["cutscenes", "cutscenes"],
      ]) {
        if (oldSlot[field] !== next[field]) {
          changes.push(`File ${index + 1} ${label}: ${oldSlot[field]} → ${next[field]}`);
        }
      }
      if (JSON.stringify(oldSlot.balloons) !== JSON.stringify(next.balloons)) {
        changes.push(`File ${index + 1}: balloon totals changed`);
      }
      if (JSON.stringify(oldSlot.courses) !== JSON.stringify(next.courses)) {
        const count = next.courses.filter((value, course) =>
          value !== oldSlot.courses[course]).length;
        changes.push(`File ${index + 1}: ${count} course entr${count === 1 ? "y" : "ies"} changed`);
      }
      if (JSON.stringify(oldSlot.worldFlags) !== JSON.stringify(next.worldFlags)) {
        changes.push(`File ${index + 1}: world flags changed`);
      }
    });
    for (const [field, label] of [
      ["adventureTwo", "Adventure Two"], ["drumstick", "Drumstick"],
      ["language", "language"], ["ttCourses", "T.T. courses"],
      ["subtitles", "subtitles"],
    ]) {
      if (before.config[field] !== after.config[field]) {
        changes.push(`${label}: ${before.config[field]} → ${after.config[field]}`);
      }
    }
    if (before.records.fastLaps.count !== after.records.fastLaps.count) {
      changes.push("Fastest-lap records reset");
    }
    if (before.records.courseTimes.count !== after.records.courseTimes.count) {
      changes.push("Course-time records reset");
    }
    return changes;
  }

  let editorBaselineSummary = null;

  function updateEditorReview() {
    const count = codec.diffCount();
    const element = byId("save-edit-change-count");
    if (element) {
      element.textContent = count === 0
        ? "No bytes changed."
        : `${count} of 512 raw bytes will change (including checksums).`;
    }
    const apply = byId("save-edit-apply");
    if (apply) apply.disabled = count === 0 || codec.corruptMask() !== 0;
  }

  function renderEditor() {
    const body = byId("save-dialog-body");
    const actions = byId("save-dialog-actions");
    const summary = codec.summary();
    body.replaceChildren();
    summary.slots.forEach((slot, index) => renderSlot(body, slot, index));
    renderGlobal(body, summary.config, summary.records);

    const presets = document.createElement("section");
    presets.className = "save-editor-card";
    const heading = document.createElement("h3");
    heading.textContent = "Validated presets";
    presets.appendChild(heading);
    const presetActions = document.createElement("div");
    presetActions.className = "actions";
    const adv2 = actionButton("Unlock Adventure Two");
    adv2.addEventListener("click", () => editSafely(() =>
      codec.call("_mdkr_save_tools_config", 0, 1)));
    const characters = actionButton("Unlock playable characters");
    characters.addEventListener("click", () => editSafely(() => {
      codec.call("_mdkr_save_tools_config", 1, 1);
      codec.call("_mdkr_save_tools_config", 3, 0xFFFFF);
    }));
    const normal = actionButton("Complete normal races");
    normal.addEventListener("click", () => {
      const current = codec.summary();
      const slot = current.slots.findIndex((value) => value.status !== "empty" &&
        value.status !== "corrupt");
      if (slot < 0) {
        dialogStatus("Create an Adventure slot before applying this preset.", "err");
        return;
      }
      editSafely(() => {
        normalRaceCourseIndices.forEach((course) =>
          codec.call("_mdkr_save_tools_course", slot, course, 2));
      });
    });
    const records = actionButton("Clear all time records");
    records.addEventListener("click", () => editSafely(() =>
      codec.call("_mdkr_save_tools_reset_records", 3)));
    presetActions.append(adv2, characters, normal, records);
    presets.appendChild(presetActions);
    const presetNote = document.createElement("p");
    presetNote.className = "fine";
    presetNote.textContent =
      "A 100% preset is intentionally withheld until a reference-generated " +
      "golden save defines every boss, cutscene, world, and course relationship. " +
      "The editor will not invent a checksum-valid but impossible completion state.";
    presets.appendChild(presetNote);
    body.appendChild(presets);

    const review = document.createElement("section");
    review.className = "save-editor-review";
    const count = document.createElement("p");
    count.id = "save-edit-change-count";
    review.appendChild(count);
    const diffList = document.createElement("ul");
    diffList.id = "save-edit-diff";
    diffList.hidden = true;
    review.appendChild(diffList);
    body.appendChild(review);

    actions.replaceChildren();
    const corruptMask = codec.corruptMask();
    if (corruptMask !== 0) {
      const recover = actionButton("Reset corrupt blocks");
      recover.addEventListener("click", () => editSafely(() => {
        codec.call("_mdkr_save_tools_recover", corruptMask);
        renderEditor();
      }));
      actions.appendChild(recover);
    }
    const reviewButton = actionButton("Review changes");
    reviewButton.addEventListener("click", () => {
      const changes = semanticDiff(editorBaselineSummary, codec.summary());
      diffList.replaceChildren();
      if (changes.length === 0) {
        const item = document.createElement("li");
        item.textContent = "No semantic changes.";
        diffList.appendChild(item);
      } else {
        changes.forEach((change) => {
          const item = document.createElement("li");
          item.textContent = change;
          diffList.appendChild(item);
        });
      }
      diffList.hidden = false;
    });
    const apply = actionButton("Apply changes", "btn btn-primary");
    apply.id = "save-edit-apply";
    const span = document.createElement("span");
    span.textContent = "Apply changes";
    apply.appendChild(span);
    apply.addEventListener("click", async () => {
      const bytes = codec.raw();
      apply.disabled = true;
      dialogStatus("Staging, persisting, and verifying editor changes…");
      try {
        await repository.replace(bytes);
        codec.acceptBaseline();
        editorBaselineSummary = codec.summary();
        updateEditorReview();
        dialogStatus("Changes verified. Reload or press Play to use them.", "ok");
        setStatus("Saved-progress edits applied successfully.", "ok");
      } catch (error) {
        updateEditorReview();
        dialogStatus(
          "Edit transaction failed; the prior save was restored: " +
          (error.message || error), "err");
      }
    });
    actions.append(reviewButton, apply);
    updateEditorReview();
  }

  async function openEditor() {
    try {
      const bytes = await repository.snapshot();
      if (bytes) codec.load(bytes);
      else codec.blank();
      editorBaselineSummary = codec.summary();
      openDialog("Edit saved progress");
      if (codec.corruptMask() !== 0) {
        dialogStatus(
          "Corrupt blocks are read-only. Reset them explicitly before editing " +
          "or close without applying.", "err");
      }
      renderEditor();
    } catch (error) {
      setStatus("Could not open the editor: " + (error.message || error), "err");
    }
  }

  async function openAutosaves() {
    try {
      const automatic = await repository.autosaves();
      const previous = await repository.previous();
      const candidates = automatic.flatMap((bytes, index) => bytes ? [{
        bytes,
        label: `Automatic snapshot ${index + 1}`,
      }] : []);
      if (previous) {
        candidates.push({
          bytes: previous,
          label: "Previous import/edit rollback",
        });
      }
      if (candidates.length === 0) {
        setStatus(
          "No recovery points exist yet. Automatic snapshots appear after the " +
          "game replaces an existing save generation.");
        return;
      }
      const current = await repository.snapshot();
      let currentSummary = null;
      if (current) {
        codec.load(current);
        currentSummary = codec.summary();
      }
      openDialog("Restore automatic snapshot");
      const body = byId("save-dialog-body");
      const intro = document.createElement("p");
      intro.className = "fine";
      intro.textContent =
        "Snapshot 1 is the newest prior checksum-safe generation. Review one " +
        "against the current EEPROM before replacing anything.";
      body.appendChild(intro);
      candidates.forEach((point, index) => {
        const candidate = point.bytes;
        codec.load(candidate);
        const summary = codec.summary();
        const section = document.createElement("section");
        section.className = "save-editor-card";
        appendSummary(section, point.label, summary);
        const review = actionButton(`Review ${point.label.toLowerCase()}`);
        review.addEventListener("click", () => {
          codec.load(candidate);
          candidateFileName = point.label.toLowerCase();
          byId("save-dialog-title").textContent =
            `Review ${point.label.toLowerCase()}`;
          setImportPreview(
            candidate, summary, currentSummary, current, null);
          dialogStatus(
            "Nothing has been written. Compare the complete summaries, then " +
            "choose Replace saved progress only if this is the generation " +
            "you want.", "ok");
        });
        section.appendChild(review);
        body.appendChild(section);
      });
      byId("save-dialog-actions").replaceChildren();
    } catch (error) {
      setStatus(
        "Could not inspect automatic snapshots: " +
        (error.message || error), "err");
    }
  }

  async function eraseSave() {
    try {
      if (!await repository.hasAnyData()) {
        setStatus("There is no saved progress or recovery point stored for this site.");
        return;
      }
      if (!confirm(
        "Erase all saved progress for this page?\n\n" +
        "This deletes Adventure files, lap records, options, Controller Pak " +
        "ghosts, the local " +
        "rollback copy, and all automatic recovery points. Download a backup " +
        "first if the progress matters.")) {
        return;
      }
      setControlsDisabled(true);
      setStatus("Erasing…");
      await repository.clear();
      setStatus("Saved progress erased. Reload or press Play to start fresh.", "ok");
    } catch (error) {
      setStatus("Could not erase saved progress: " + (error.message || error), "err");
    } finally {
      if (!released) setControlsDisabled(false);
    }
  }

  function wireUi() {
    byId("download-save")?.addEventListener("click", downloadPortable);
    byId("download-save-raw")?.addEventListener("click", downloadRaw);
    byId("download-paks")?.addEventListener("click", downloadControllerPaks);
    byId("import-paks-input")?.addEventListener("change", (event) =>
      acceptControllerPakBundle(event.target.files && event.target.files[0]));
    byId("import-paks-button")?.addEventListener("click", () =>
      byId("import-paks-input").click());
    byId("edit-save")?.addEventListener("click", openEditor);
    byId("restore-save-snapshot")?.addEventListener("click", openAutosaves);
    byId("clear-save")?.addEventListener("click", eraseSave);
    byId("import-save-input")?.addEventListener("change", (event) =>
      acceptImportFile(event.target.files && event.target.files[0]));
    byId("import-save-button")?.addEventListener("click", () =>
      byId("import-save-input").click());
    const dialog = byId("save-dialog");
    byId("save-dialog-close")?.addEventListener("click", () => dialog.close());
    dialog?.addEventListener("close", () => {
      const target = dialogReturnFocus;
      dialogReturnFocus = null;
      if (target && target.isConnected && typeof target.focus === "function") {
        target.focus();
      }
    });

    const drop = byId("save-drop");
    if (drop) {
      drop.addEventListener("keydown", (event) => {
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          byId("import-save-input").click();
        }
      });
      ["dragenter", "dragover"].forEach((name) =>
        drop.addEventListener(name, (event) => {
          event.preventDefault();
          event.stopPropagation();
          drop.classList.add("over");
        }));
      ["dragleave", "dragend"].forEach((name) =>
        drop.addEventListener(name, () => drop.classList.remove("over")));
      drop.addEventListener("drop", (event) => {
        event.preventDefault();
        event.stopPropagation();
        drop.classList.remove("over");
        const file = event.dataTransfer?.files?.[0];
        if (file && /\.mdkr-paks$/i.test(file.name || "")) {
          acceptControllerPakBundle(file);
        } else {
          acceptImportFile(file);
        }
      });
    }
  }

  async function init() {
    if (readyPromise) return readyPromise;
    setControlsDisabled(true);
    wireUi();
    readyPromise = (async () => {
      try {
        repository = await createRepository();
        setControlsDisabled(false);
        const bytes = await repository.snapshot();
        if (!bytes) {
          setStatus(
            "No saved progress is stored yet. Save tools are ready without a ROM.");
        } else {
          codec.load(bytes);
          const summary = codec.summary();
          const mask = codec.corruptMask();
          setStatus(mask
            ? "Stored progress contains corrupt blocks; raw export and recovery are available."
            : `Save tools ready — ${summary.sha256.slice(0, 12)}…`, mask ? "err" : "ok");
        }
      } catch (error) {
        setStatus(
          "Save tools could not start. Import/export is unavailable: " +
          (error.message || error), "err");
        setControlsDisabled(true);
        throw error;
      }
    })();
    return readyPromise;
  }

  async function release() {
    if (!readyPromise) return;
    try {
      await readyPromise;
      await repository.release();
    } catch (_) {
      // Engine boot will perform its own IDBFS mount and surface storage errors.
    }
  }

  async function resume() {
    if (!readyPromise) return;
    try {
      await readyPromise;
      await repository.resume();
    } catch (error) {
      setStatus(
        "Save tools could not reacquire browser storage: " +
        (error.message || error), "err");
    }
  }

  const publicApi = {
    init,
    release,
    resume,
  };
  if (globalThis.__mdkrTestConfig) {
    // Runtime checks receive these through the same transaction and codec paths
    // as the UI. Production pages do not expose mutation hooks.
    publicApi.testApi = {
      acceptImportFile,
      acceptControllerPakBundle,
      downloadPortable,
      faultPoints: SAVE_FAULT_POINTS,
      pakFaultPoints: PAK_FAULT_POINTS,
      snapshot: async () => {
        await init();
        return repository.snapshot();
      },
      controllerPaks: async () => {
        await init();
        return repository.controllerPaks();
      },
      replaceControllerPaks: async (candidates) => {
        await init();
        return repository.replaceControllerPaks(candidates.map((bytes) =>
          bytes === null || bytes instanceof Uint8Array
            ? bytes : new Uint8Array(bytes)));
      },
      autosaves: async () => {
        await init();
        return repository.autosaves();
      },
      previous: async () => {
        await init();
        return repository.previous();
      },
      seedAutosave: async (index, bytes) => {
        await init();
        const candidate = bytes instanceof Uint8Array
          ? bytes : new Uint8Array(bytes);
        codec.load(candidate);
        if (codec.corruptMask() !== 0) {
          throw new Error("automatic snapshot fixture is corrupt");
        }
        return repository.seedAutosaveForTest(index, codec.raw());
      },
      replace: async (bytes) => {
        await init();
        codec.load(bytes);
        if (codec.corruptMask() !== 0) throw new Error("candidate is corrupt");
        return repository.replace(codec.raw());
      },
      seedRaw: async (bytes) => {
        await init();
        const candidate = bytes instanceof Uint8Array
          ? bytes : new Uint8Array(bytes);
        return repository.replace(candidate);
      },
      summary: async () => {
        await init();
        const bytes = await repository.snapshot();
        if (!bytes) return null;
        codec.load(bytes);
        return codec.summary();
      },
    };
  }
  return Object.freeze(publicApi);
})();
