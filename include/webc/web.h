// SPDX-License-Identifier: Apache-2.0
// WebC — the browser half: signals, the component DSL, events, fetch.
//
//   src/client.c  #include <webc/web.h>
//
// Includes the shared half plus the pieces that only exist in a browser module.
// No sockets, no SQL, no scheduler: this is what a wasm build links.
#ifndef WEBC_WEB_H
#define WEBC_WEB_H

#include <webc/shared.h>
#include <webc/js.h>
#include <webc/webc_dev.h>

// `WebC.start({data: '/api/state'})` fetches that URL and hands the JSON to the module
// before the first render. Define this in your client file to seed a mounted view from
// data the server already had, instead of a fetch-then-render flash.
void wc_app_data(str json);

#endif // WEBC_WEB_H
