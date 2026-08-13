"use strict";

(() => {
  const title = document.getElementById("room-entry-title");
  const status = document.getElementById("room-entry-status");
  const fragment = new URLSearchParams(location.hash.slice(1));
  const capability = [...fragment].length === 1 ? fragment.get("match") || "" : "";
  const exactFragment = location.hash === `#match=${capability}`;
  if (location.hash) {
    try { history.replaceState(null, "", location.pathname + location.search); }
    catch (_) {
      // Never carry the capability forward when the role page cannot first erase
      // it. A clean navigation is the only recovery; the next page can offer code
      // entry without retaining or redeeming this secret.
      location.replace(location.pathname + location.search);
      return;
    }
  }
  if (!exactFragment || !/^[A-Za-z0-9_-]{43}$/.test(capability)) {
    title.textContent = "Check the Room Invitation";
    status.textContent = "This is not a current private-room link. Ask for a new link or enter the six-digit room code.";
    title.focus();
    return;
  }
  // Keep the capability in fragment custody across this same-origin redirect.
  // The always-loaded launcher captures it into memory and erases the URL
  // synchronously. Session storage would outlive the handoff when Online Room
  // is disabled and would expose the secret to unrelated same-tab reloads.
  location.replace(`../#match=${capability}`);
})();
