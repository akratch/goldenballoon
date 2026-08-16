// Publisher-owned cloud-feature activation policy.
//
// This file is intentionally static, tiny, and loaded before the Phone Party
// and Online Room hosts. A release may enable either surface only after its
// deployment and admission checklist has a written GO. Online Room still
// requires a clean build and locally verified supported ROM before it loads
// the room model or performs any room-service request.
"use strict";

globalThis.__mdkrOnlineControlReleasePolicy = Object.freeze({
  enabled: false,
  phonePartyEnabled: false,
  serviceOrigin: location.origin,
});
