// SPDX-License-Identifier: Apache-2.0
// WebC — the shared half of an app: everything both sides may use, and nothing
// that only one of them can.
//
//   src/view.c    includes <webc/shared.h>   the view; rendered by the server,
//                                            hydrated by the browser, same markup
//   src/server.c  includes <webc/server.h>   routes, database, auth, HTTP
//   src/client.c  includes <webc/web.h>      signals, events, fetch
//
// A shared file that tries `db_save()` or `webc_use()` does not compile, which is
// the difference between "do not mix the two halves" as a convention and as a
// fact. `webc/webc.h` still includes everything, for code that predates the split.
#ifndef WEBC_SHARED_H
#define WEBC_SHARED_H

#include <webc/core.h>
#include <webc/vec.h>
#include <webc/map.h>
#include <webc/kinds.h>
#include <webc/url.h>
#include <webc/model.h>
#include <webc/relations.h>
#include <webc/page.h>
#include <webc/json.h>
#include <webc/react.h>
#include <webc/dom.h>
#include <webc/fe.h>

#endif // WEBC_SHARED_H