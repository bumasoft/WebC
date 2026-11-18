// SPDX-License-Identifier: Apache-2.0
// webc/pwa.h — installable, and it opens with the wifi off.
//
// A WebC app is already a small static module plus a JSON seed, which is exactly what a service
// worker wants to cache. What is missing is three files nobody wants to write by hand: the
// manifest, the worker, and the offline page. Two of them are facts the app already declared —
// its name, its colours, its icon — and the third is a view it writes anyway.
//
//   pwa_init(&(WPwa){
//       .name = "Meridian", .short_name = "Meridian", .theme = "#111312",
//       .icon = "/icon-192.png", .mask_icon = "/icon-512.png",
//       .start = "/", .offline = "/offline", .scope = "/",
//   });
//
//   GET("/manifest.webmanifest") { return pwa_manifest(); }
//   GET("/sw.js")                { return pwa_worker(); }
//
//   view(Offline) { el("main") { h1("You are offline"); p("This page will load when you are back."); } }
//   GET("/offline") { return render_page(Offline, ); }
//
// **The cache name is the build's hash.** `pwa_worker()` writes `webc-<hash>` where the hash comes
// from `WEBC_BUILD_ID` — the version string `webc build` stamps into the binary — so a deploy
// invalidates every old cache on the next visit and a stale module can never be served against a
// fresh seed. That is the one bug a hand-written worker always has.
//
// **The strategy is two rules, not a framework.** Navigation requests are network-first with the
// offline page as the fallback (a page you can read beats a cached page that lies); everything
// under /webc.js, /app.wasm and the icon set is cache-first (they are immutable for a build).
//
// **Refuses:** push notifications (they need a bridge verb, and the bridge is 20 KB every app
// pays), background sync, an install-prompt UI of its own (`ui.h` has the button and three lines
// of `enhance`), app-store packaging, and a workbox-shaped plugin system. A worker that needs
// more than these two rules is an app that should write its own `sw.js` — and it can: this module
// is a header and a string, so replacing it is not a migration.
#pragma once

#include <webc/core.h>
#include <webc/http.h>

typedef struct WPwa {
  const char *name;        // "Meridian" — the install sheet's title
  const char *short_name;  // the home-screen label; falls back to `name`
  const char *theme;       // "#111312": the browser chrome and the splash
  const char *background;  // the splash's colour; falls back to `theme`
  const char *icon;        // 192×192 png
  const char *mask_icon;   // 512×512 png, maskable
  const char *start;       // "/" — where the installed app opens
  const char *scope;       // "/" — what it owns
  const char *offline;     // "/offline" — the page a navigation falls back to
  bool standalone;         // true: no address bar once installed
} WPwa;

void pwa_init(const WPwa *cfg);
bool pwa_configured(void);
const WPwa *pwa_config(void);

// application/manifest+json from the config.
Response pwa_manifest(void);

// text/javascript: the worker, with this build's cache name.
Response pwa_worker(void);

// The two lines a page's head needs: the manifest link and the theme colour. Called inside
// `head()`, so it works in every view without the app repeating them.
void pwa_head(void);

// The build's version string: `WEBC_BUILD_ID` when the build stamped one, otherwise the module's
// own size and mtime, so a worker can never claim two different builds are the same.
str pwa_build_id(void);

// The cache name the worker uses ("webc-<build>"), for a test or a header that wants to say which
// build a page is from. A `Text`, the shelf's usual spelling for "a string you print".
Text pwa_cache_name(void);