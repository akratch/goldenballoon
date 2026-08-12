"use strict";

(() => {
  const title = document.getElementById("room-entry-title");
  const status = document.getElementById("room-entry-status");
  const capability = new URLSearchParams(location.hash.slice(1)).get("match") || "";
  history.replaceState(null, "", location.pathname + location.search);
  if (!/^[A-Za-z0-9_-]{43}$/.test(capability)) {
    title.textContent = "Check the Room Invitation";
    status.textContent = "This is not a current private-room link. Ask for a new link or enter the six-digit room code.";
    title.focus();
    return;
  }
  try {
    sessionStorage.setItem("mdkr-online-room-entry-v1", JSON.stringify({
      capability, createdAt: Date.now(),
    }));
    location.replace("../");
  } catch (_) {
    // Fragments are not sent in HTTP requests or referrers. This fallback is
    // still secret-safe when browser storage is unavailable; the launcher
    // erases it before joining.
    location.replace(`../#match=${capability}`);
  }
})();
