// Publisher-owned Online Room activation policy.
//
// This file is intentionally static, tiny, and loaded before online-room.js.
// A release may change `enabled` to true only after the multiplayer admission
// checklist has a written GO. The launcher still requires a clean build and a
// locally verified supported ROM before it will load the room model or perform
// any room-service request.
"use strict";

globalThis.__mdkrOnlineControlReleasePolicy = Object.freeze({
  enabled: false,
  serviceOrigin: location.origin,
});
