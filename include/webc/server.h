// SPDX-License-Identifier: Apache-2.0
// WebC — the server half: routes, database, sessions, HTTP, tasks, the boot.
//
//   src/server.c  #include <webc/server.h>
//
// Includes the shared half plus everything that only exists on a server. A view
// file cannot reach any of it, on purpose.
#ifndef WEBC_SERVER_H
#define WEBC_SERVER_H

#include <webc/shared.h>
#include <webc/app.h>
#include <webc/ctx.h>
#include <webc/multipart.h>
#include <webc/crypto.h>
#include <webc/log.h>
#include <webc/task.h>
#include <webc/http.h>
#include <webc/tls.h>
#include <webc/auth.h>
#include <webc/db.h>
#include <webc/redis.h>
#include <webc/qcache.h>
#include <webc/config.h>
#include <webc/client.h>

#endif // WEBC_SERVER_H
