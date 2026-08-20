// Which pairing backend served this page — declared by the server, never
// guessed from the URL. The cloud (Cloudflare Worker / Pages) ships this file
// verbatim, so a cloud page is in cloud mode even when it is served over plain
// http (the loopback E2E lane). The embedded LAN server (Task 5 manifest
// builder) overrides just this one asset with the `true` form at serve time, so
// a page a phone loaded from the game's own LAN server is in local-play mode.
// controller.js reads globalThis.__MDKR_LAN_PARTY__ to pick ws-redeem (LAN) vs
// POST-redeem (cloud); nothing else is protocol-guessed.
window.__MDKR_LAN_PARTY__ = false;
