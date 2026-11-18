// SPDX-License-Identifier: Apache-2.0
// WebC — passwords and signed session cookies (SPEC §6.4 accounts).
//
//   str hash = auth_hash_password(a->arena, password);
//   if (!auth_verify_password(account.password_hash, password)) return 401;
//   auth_set_session(&res, secret, account.id);
#ifndef WEBC_AUTH_H
#define WEBC_AUTH_H

#include <webc/core.h>
#include <webc/http.h>

// PBKDF2-SHA256, 210k iterations, 16-byte salt, stored as
// "pbkdf2-sha256$<iters>$<salt-b64>$<hash-b64>".
// The Text flavour: an application has `const char *` password fields, not str.
// Allocates in the current scope, like fmt().
Text auth_hash(const char *password);
bool auth_verify(const char *stored, const char *password);

str  auth_hash_password(Arena *a, str password);
str  auth_hash_password_iters(Arena *a, str password, u32 iters);
bool auth_verify_password(str stored, str password);

// A session cookie is "<uid>.<expiry>.<mac>" — signed, never encrypted, so the
// server can trust it without a session table.
void auth_set_session(Response *res, str secret, i64 uid);
void auth_clear_session(Response *res);
bool auth_get_session(Request *req, str secret, i64 *uid_out);
i64  auth_require_session(Request *req, str secret);   // 0 when absent/invalid

// The session secret: WEBC_SECRET when it is set, otherwise a random one generated
// for this process (sessions then do not survive a restart — which is the honest
// behaviour for a missing secret, and the log says so once).
str  auth_secret(void);
void auth_secret_set(str secret);

#define WEBC_SESSION_COOKIE "session"
#define WEBC_SESSION_TTL_S  604800   // one week

#endif // WEBC_AUTH_H