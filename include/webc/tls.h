// SPDX-License-Identifier: Apache-2.0
// webc/tls.h — TLS as a build feature (SPEC §11).
//
//   cc -std=c23 -DWEBC_TLS=openssl -I include app.c -lwebc -lssl -lcrypto
//   WServer o = { .addr = ":8443", .tls_cert = "cert.pem", .tls_key = "key.pem" };
//
// Off by default on purpose: "lightweight" has to be true of the binary an app
// ships, and most deployments terminate TLS at a reverse proxy. When it is on, the
// server speaks TLS on the same socket it would otherwise speak plain HTTP on, and
// a build without it refuses to start rather than serving plaintext to a client
// that asked for TLS.
#ifndef WEBC_TLS_H
#define WEBC_TLS_H

#include <webc/core.h>

bool tls_available(void);
// Load the certificate and key once per process. `err` gets a human sentence.
bool tls_init(const char *cert_path, const char *key_path, char *err, size_t errcap);
// Handshake on an accepted socket, parking the task while the peer thinks.
void *tls_accept(i32 fd);
// Client side: verify the peer against `ca_file` (null = the system store) unless
// `insecure`. Returns null and fills `err` when the handshake or verification fails
// — a client that silently accepts any certificate is worse than no TLS at all.
void *tls_connect(i32 fd, const char *hostname, const char *ca_file, bool insecure, char *err,
                  size_t errcap);
// The same, with the two checks separated: `verify_host` is libpq's verify-full (the
// certificate names the host) versus verify-ca (the chain is trusted, the name is not
// checked). `insecure` skips verification altogether and is for `sslmode=require` and
// tests only.
void *tls_connect_opt(i32 fd, const char *hostname, const char *ca_file, bool verify_host,
                      bool insecure, char *err, size_t errcap);
i64   tls_read(void *s, void *buf, size_t n); // -1 error, 0 EOF, >0 bytes
i64   tls_write(void *s, const void *buf, size_t n);
void  tls_close(void *s);
void  tls_shutdown_all(void);

#endif // WEBC_TLS_H