// sw.js — offline cache for the published Golden Balloon shell.
//
// The page advertises itself as installable (fullscreen display mode, Add to
// Home Screen). An installed app that cannot start without the network is a
// broken promise, so the shell + engine are cached; nothing else about the
// page's behaviour changes.
//
// THE ONE INVARIANT: a cached JS/wasm pair must always come from ONE build.
// Mixing a new mdkr64_web.js against an old mdkr64_web.wasm (or an old shell
// against a new engine) is the exact failure the publisher's ?v=<commit> stamp
// exists to prevent, and a cache is the classic way to reintroduce it. Three
// rules together make that impossible:
//
//   1. This worker is registered as sw.js?v=<commit>, so its own identity IS
//      the build. BUILD below is read from that URL and names the cache.
//   2. A request is only served from (or written to) the cache when its ?v=
//      matches BUILD exactly. A request stamped for a different build is passed
//      straight to the network — this worker will not answer for it.
//   3. There is no skipWaiting() and no clients.claim(). A newer worker installs
//      alongside and takes over only once every document using this one has
//      gone, so a live page keeps the worker whose cache matches the HTML it
//      actually loaded. Activation then deletes every other build's cache.
//
// The document itself carries no stamp (it is what CONTAINS the stamps), so it
// is network-first: a publish is picked up on the next load, and the cached copy
// is only a fallback for being offline.
//
// An unstamped registration (a local dev page) caches nothing and removes
// itself; a worker must never pin a half-edited working copy.

"use strict";

const BUILD = new URL(self.location.href).searchParams.get("v") || "";
const CACHE = "mdkr64-shell-" + BUILD;
const DOCUMENT_KEY = "./?document=" + BUILD;

// Everything a home-screen launch needs before the player supplies a ROM.
// The whole set is ~2.5 MB, so it is precached at install: the worker is
// registered after the page's own fetches (which bypass it -- no
// clients.claim), so install-time is the only moment the first visit can be
// made launchable offline.
const PRECACHE = [
  "style.css?v=" + BUILD,
  "manifest.webmanifest?v=" + BUILD,
  "mdkr64-shell.js?v=" + BUILD,
  "mdkr-save-ui.js?v=" + BUILD,
  "rom-id.js?v=" + BUILD,
  "mdkr64_web.js?v=" + BUILD,
  "mdkr64_web.wasm?v=" + BUILD,
  "mdkr-save-tools.js?v=" + BUILD,
  "mdkr-save-tools.wasm?v=" + BUILD,
  "assets/favicon.png",
  "assets/favicon-32.png",
  "assets/apple-touch-icon.png",
];

self.addEventListener("install", (event) => {
  if (!BUILD) return;
  event.waitUntil((async () => {
    try {
      const cache = await caches.open(CACHE);
      const response = await fetch("./", { cache: "reload" });
      if (response.ok) await cache.put(DOCUMENT_KEY, response);
      // Fetch each asset individually so one miss does not abandon the rest;
      // anything missed here is still captured by the runtime fetch path.
      await Promise.all(PRECACHE.map(async (url) => {
        try {
          const r = await fetch(url, { cache: "reload" });
          if (r.ok) await cache.put(url, r);
        } catch (_) { /* runtime path fills in */ }
      }));
    } catch (_) { /* offline at install time: the runtime path still fills in */ }
  })());
});

self.addEventListener("activate", (event) => {
  event.waitUntil((async () => {
    if (!BUILD) {
      await self.registration.unregister();
      return;
    }
    const names = await caches.keys();
    await Promise.all(names
      .filter((name) => name.startsWith("mdkr64-shell-") && name !== CACHE)
      .map((name) => caches.delete(name)));
  })());
});

async function cacheFirst(request, key) {
  const cache = await caches.open(CACHE);
  const hit = await cache.match(key);
  if (hit) return hit;
  const response = await fetch(request);
  // Only a same-origin success is storable: an opaque or error response cached
  // here would be served as the engine on the next load.
  if (response.ok && response.type === "basic") {
    await cache.put(key, response.clone());
  }
  return response;
}

async function networkFirstDocument(request) {
  const cache = await caches.open(CACHE);
  try {
    const response = await fetch(request);
    if (response.ok && response.type === "basic") {
      await cache.put(DOCUMENT_KEY, response.clone());
    }
    return response;
  } catch (error) {
    const hit = await cache.match(DOCUMENT_KEY);
    if (hit) return hit;
    throw error;
  }
}

self.addEventListener("fetch", (event) => {
  if (!BUILD || event.request.method !== "GET") return;
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin) return;

  if (event.request.mode === "navigate") {
    event.respondWith(networkFirstDocument(event.request));
    return;
  }

  const stamp = url.searchParams.get("v");
  if (stamp !== null) {
    // Stamped for some other build: not ours to answer, at all.
    if (stamp !== BUILD) return;
    event.respondWith(cacheFirst(event.request, url.href));
    return;
  }

  // Unstamped same-origin assets (the brand art). The cache itself is
  // build-scoped and wiped on activation, so cache-first here cannot outlive
  // its build either.
  event.respondWith(cacheFirst(event.request, url.href));
});
