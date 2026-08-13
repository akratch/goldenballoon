// Pure semantic presentation for the launcher-owned Online Room.
//
// This module deliberately knows nothing about the DOM, Wasm, networking or
// the game.  The browser renderer and the Node conformance test both consume
// it, so action order, safety copy and assistive announcements cannot quietly
// diverge between runtime behavior and browser-free evidence.
"use strict";

((root, factory) => {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.MDKROnlineRoomPresenter = api;
})(typeof globalThis === "object" ? globalThis : null, () => {
  const MAX_ACTION = 27;
  const CONTROL_COUNT = 4;
  const TIMEOUT_SLOT = 3;
  const ACTION_CHOOSE_CHARACTER = 6;
  const ACTION_CHOOSE_VEHICLE = 7;
  const ACTION_CHOOSE_TRACK = 8;
  const ACTION_CHOOSE_ROM = 17;
  const ACTION_PLAY_HERE = 21;
  const DESTRUCTIVE_ACTIONS = new Set([24, 25]);
  const characters = Object.freeze(["Diddy", "Timber", "Pipsy", "Tiptup",
    "Conker", "Bumper", "Banjo", "Krunch", "Drumstick", "T.T."]);
  const vehicles = Object.freeze(["Car", "Hovercraft", "Plane"]);
  const tracks = Object.freeze([
    Object.freeze({label: "Ancient Lake", value: 5}),
    Object.freeze({label: "Fossil Canyon", value: 3}),
    Object.freeze({label: "Jungle Falls", value: 29}),
  ]);
  const LIVE_ROUTES = Object.freeze([
    null,
    "create", "join", "share_invite", "check_setup", "connection_details",
    "choose_character", "choose_vehicle", "choose_track", "ready",
    "change_selection", "race_locked", "race_locked", "race_locked", "retry",
    "join", "update", "choose_rom", "setup_locked", "setup_controller",
    "connection_doctor", "play_here", "race_locked", "return_home",
    "leave_room", "race_locked", "carrier_locked", "carrier_locked",
  ]);

  function boundedText(value, name, required = false) {
    if (typeof value !== "string" || value.length > 511 ||
        value.includes("\0") || (required && value.length === 0)) {
      throw new TypeError(`invalid Online Room ${name}`);
    }
    return value;
  }

  function boundedInteger(value, name, minimum, maximum) {
    if (!Number.isInteger(value) || value < minimum || value > maximum) {
      throw new TypeError(`invalid Online Room ${name}`);
    }
    return value;
  }

  function selectionFor(action) {
    let label = "";
    let options = null;
    if (action === ACTION_CHOOSE_CHARACTER) {
      label = "Choose Character";
      options = characters.map((item, value) => Object.freeze({label: item, value}));
    } else if (action === ACTION_CHOOSE_VEHICLE) {
      label = "Choose Vehicle";
      options = vehicles.map((item, value) => Object.freeze({label: item, value}));
    } else if (action === ACTION_CHOOSE_TRACK) {
      label = "Choose Track";
      options = tracks;
    }
    return options ? Object.freeze({label, options: Object.freeze(options)}) : null;
  }

  function normalizeControl(value, slot) {
    if (!value || typeof value !== "object" || value.slot !== slot) {
      throw new TypeError(`invalid Online Room control slot ${slot}`);
    }
    const action = boundedInteger(value.action, `control ${slot} action`, 0, MAX_ACTION);
    const label = boundedText(value.label, `control ${slot} label`, action !== 0);
    if (action === 0 && (label !== "" || value.enabled)) {
      throw new TypeError(`inert Online Room control ${slot} carries state`);
    }
    if (typeof value.enabled !== "boolean") {
      throw new TypeError(`invalid Online Room control ${slot} enabled state`);
    }
    return Object.freeze({slot, action, label, enabled: value.enabled});
  }

  function countLabel(value, singular, plural) {
    return `${value} ${value === 1 ? singular : plural}`;
  }

  function project(model, options = {}) {
    if (!model || typeof model !== "object" || !Array.isArray(model.controls) ||
        model.controls.length !== CONTROL_COUNT) {
      throw new TypeError("invalid Online Room presentation model");
    }
    const slug = boundedText(model.slug, "slug", true);
    const title = boundedText(model.title, "title", true);
    const explanation = boundedText(model.explanation, "explanation", true);
    const status = boundedText(model.status, "status") || "Private Room Preview";
    const verificationPhrase = boundedText(
      model.verificationPhrase, "verification phrase");
    if (verificationPhrase && (verificationPhrase.length > 63 ||
        !/^[A-Z][a-z]+-[A-Z][a-z]+ [A-Z][a-z]+-[A-Z][a-z]+ [A-Z][a-z]+-[A-Z][a-z]+$/.test(
          verificationPhrase))) {
      throw new TypeError("invalid Online Room verification phrase shape");
    }
    const timeoutTitle = boundedText(model.timeoutTitle, "timeout title");
    const timeoutCopy = boundedText(model.timeoutCopy, "timeout copy");
    const kind = boundedInteger(model.kind, "view kind", 1, 10);
    const failure = boundedInteger(model.failure, "failure", 0, 18);
    const announcement = boundedInteger(model.announcement, "announcement", 0, 2);
    const members = boundedInteger(model.members, "member count", 0, 4);
    const seats = boundedInteger(model.seats, "seat count", 0, 4);
    const ready = boundedInteger(model.ready, "ready count", 0, members);
    for (const name of ["admission", "localPlay", "timeoutVisible"]) {
      if (typeof model[name] !== "boolean") {
        throw new TypeError(`invalid Online Room ${name}`);
      }
    }
    if (model.timeoutVisible !== Boolean(timeoutTitle && timeoutCopy) ||
        (!model.timeoutVisible && (timeoutTitle || timeoutCopy))) {
      throw new TypeError("Online Room timeout visibility and copy disagree");
    }
    const controls = Object.freeze(model.controls.map(normalizeControl));
    const phraseDecision = controls.some(({action}) => action === 26 || action === 27);
    if (Boolean(verificationPhrase) !== phraseDecision ||
        (verificationPhrase && (controls[0].action !== 26 ||
          controls[0].label !== "Words Match" || controls[1].action !== 27 ||
          controls[1].label !== "Words Differ"))) {
      throw new TypeError("Online Room phrase and explicit decisions disagree");
    }
    const live = options.live === true;
    const localPlayable = options.localPlayable === true;
    const actions = [];
    const seen = new Set();

    function add(control, primary) {
      if (!control.action || seen.has(control.action)) return;
      let displayed = control;
      if (live && control.action === ACTION_PLAY_HERE && !localPlayable) {
        displayed = Object.freeze({...control, action: ACTION_CHOOSE_ROM,
          label: "Choose ROM"});
      }
      if (seen.has(displayed.action)) return;
      const selection = primary ? selectionFor(displayed.action) : null;
      actions.push(Object.freeze({
        action: displayed.action,
        label: displayed.label,
        enabled: displayed.enabled,
        primary,
        destructive: DESTRUCTIVE_ACTIONS.has(displayed.action),
        selection,
      }));
      seen.add(displayed.action);
    }

    if (model.timeoutVisible) add(controls[TIMEOUT_SLOT], true);
    for (const control of controls.slice(0, TIMEOUT_SLOT)) {
      add(control, !model.timeoutVisible && control.slot === 0);
    }
    if (live && model.localPlay) {
      const action = localPlayable ? ACTION_PLAY_HERE : ACTION_CHOOSE_ROM;
      if (!seen.has(action)) add(Object.freeze({slot: -1, action,
        label: localPlayable ? "Play Here" : "Choose ROM", enabled: true}), false);
    }

    const phraseWarning = "Do not continue if even 1 word differs.";
    const announcementText = verificationPhrase
      ? `${title}. ${explanation} Verification phrase: ${verificationPhrase}. ` +
        phraseWarning
      : `${title}. ${explanation}`;
    return Object.freeze({
      slug, title, explanation, status, kind, failure,
      countsText: `${countLabel(members, "member", "members")} · ` +
        `${countLabel(seats, "racer seat", "racer seats")} · ${ready} ready`,
      verification: Object.freeze({
        visible: verificationPhrase.length !== 0,
        phrase: verificationPhrase,
        warning: phraseWarning,
      }),
      timeout: Object.freeze({
        visible: model.timeoutVisible,
        title: timeoutTitle,
        copy: timeoutCopy,
      }),
      actions: Object.freeze(actions),
      announcement: Object.freeze({
        key: `${slug}:${kind}:${failure}:${verificationPhrase}`,
        priority: announcement === 2 ? "assertive" : "polite",
        text: announcementText,
      }),
    });
  }

  function liveAction(presentation, action, options = {}) {
    if (!presentation || typeof presentation !== "object" ||
        !Array.isArray(presentation.actions) ||
        !Number.isInteger(action) || action < 1 || action > MAX_ACTION) return null;
    const item = presentation.actions.find((candidate) =>
      candidate.action === action && candidate.enabled === true);
    if (!item) return null;
    let route = LIVE_ROUTES[action];
    if (route === "check_setup" && options.fixture !== true) route = "setup_locked";
    if (!route) return null;
    return Object.freeze({action, route,
      available: route !== "race_locked" && route !== "carrier_locked" &&
        route !== "setup_locked"});
  }

  return Object.freeze({version: 1, project, liveAction});
});
