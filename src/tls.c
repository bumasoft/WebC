// SPDX-License-Identifier: Apache-2.0
// webc — TLS, behind a build feature (SPEC §11).
//
// The server's I/O goes through four functions (accept handshake, read, write,
// close). With -DWEBC_TLS=openssl they are OpenSSL; without it they are stubs that
// say so. No macro leaks into the HTTP layer, so the server code is identical in
// both builds — which is the only way a TLS path stays tested.
//
// Handshakes park the task instead of blocking the worker: SSL_accept on a
// non-blocking socket returns WANT_READ/WANT_WRITE, and wfd_wait() is where the
// green thread waits for the socket (the same primitive every handler uses).
// The system headers come first: <webc/task.h> defines `select` as a statement
// macro, and sys/select.h (pulled in by OpenSSL) declares select() as a function.
#include <webc/tls.h>

#ifdef WEBC_TLS_OPENSSL

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <unistd.h>

#define WEBC_NO_SELECT_MACRO 1
#include <webc/task.h>

static SSL_CTX *g_ctx;
static WMutex g_mu = WEBC_MUTEX_INIT;

static void tls_err(char *err, size_t cap, const char *what) {
  if (!err || cap == 0) return;
  unsigned long e = ERR_get_error();
  char buf[256] = {0};
  if (e) ERR_error_string_n(e, buf, sizeof buf);
  snprintf(err, cap, "%s: %s", what, buf[0] ? buf : "unknown TLS error");
}

bool tls_available(void) {
  return true;
}

bool tls_init(const char *cert_path, const char *key_path, char *err, size_t errcap) {
  if (!cert_path || !key_path) {
    if (err) snprintf(err, errcap, "TLS needs both a certificate and a key");
    return false;
  }
  wmutex_lock(&g_mu);
  if (g_ctx) {
    wmutex_unlock(&g_mu);
    return true;
  }
  SSL_library_init();
  SSL_load_error_strings();
  g_ctx = SSL_CTX_new(TLS_server_method());
  if (!g_ctx) {
    tls_err(err, errcap, "SSL_CTX_new");
    wmutex_unlock(&g_mu);
    return false;
  }
  SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(g_ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  SSL_CTX_set_mode(g_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  // a session cache costs us nothing and saves a full handshake per reconnect
  SSL_CTX_set_session_cache_mode(g_ctx, SSL_SESS_CACHE_SERVER);
  if (SSL_CTX_use_certificate_chain_file(g_ctx, cert_path) != 1) {
    tls_err(err, errcap, "loading the certificate");
    SSL_CTX_free(g_ctx);
    g_ctx = nullptr;
    wmutex_unlock(&g_mu);
    return false;
  }
  if (SSL_CTX_use_PrivateKey_file(g_ctx, key_path, SSL_FILETYPE_PEM) != 1) {
    tls_err(err, errcap, "loading the private key");
    SSL_CTX_free(g_ctx);
    g_ctx = nullptr;
    wmutex_unlock(&g_mu);
    return false;
  }
  if (SSL_CTX_check_private_key(g_ctx) != 1) {
    tls_err(err, errcap, "certificate and key do not match");
    SSL_CTX_free(g_ctx);
    g_ctx = nullptr;
    wmutex_unlock(&g_mu);
    return false;
  }
  wmutex_unlock(&g_mu);
  return true;
}

void *tls_accept(i32 fd) {
  if (!g_ctx) return nullptr;
  SSL *ssl = SSL_new(g_ctx);
  if (!ssl) return nullptr;
  if (SSL_set_fd(ssl, fd) != 1) {
    SSL_free(ssl);
    return nullptr;
  }
  for (;;) {
    int r = SSL_accept(ssl);
    if (r == 1) return ssl;
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ) {
      if (!wfd_wait(fd, false, 5000)) break;
      continue;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
      if (!wfd_wait(fd, true, 5000)) break;
      continue;
    }
    break;
  }
  SSL_free(ssl);
  return nullptr;
}

void *tls_connect(i32 fd, const char *hostname, const char *ca_file, bool insecure, char *err,
                  size_t errcap) {
  return tls_connect_opt(fd, hostname, ca_file, !insecure, insecure, err, errcap);
}

// The client handshake. A stub that always fails ("this build has no TLS") makes an
// https:// URL and a database that requires SSL impossible in *every* build, including a TLS
// one. `verify_host` is the difference between libpq's verify-ca and
// verify-full: the chain is checked either way, and the name on the certificate is checked
// only when the caller asks (a managed database whose hostname is a proxy alias needs
// verify-ca, and pretending otherwise makes TLS unusable rather than safe).
void *tls_connect_opt(i32 fd, const char *hostname, const char *ca_file, bool verify_host,
                      bool insecure, char *err, size_t errcap) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    tls_err(err, errcap, "creating a TLS context");
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  if (insecure) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  } else {
    if (ca_file && *ca_file) {
      if (SSL_CTX_load_verify_locations(ctx, ca_file, nullptr) != 1) {
        tls_err(err, errcap, "loading the CA bundle");
        SSL_CTX_free(ctx);
        return nullptr;
      }
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      tls_err(err, errcap, "loading the system CA store");
      SSL_CTX_free(ctx);
      return nullptr;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  }
  SSL *ssl = SSL_new(ctx);
  SSL_CTX_free(ctx); // the SSL holds its own reference
  if (!ssl) {
    tls_err(err, errcap, "creating a TLS session");
    return nullptr;
  }
  if (SSL_set_fd(ssl, fd) != 1) {
    tls_err(err, errcap, "binding the socket");
    SSL_free(ssl);
    return nullptr;
  }
  if (hostname && *hostname) {
    SSL_set_tlsext_host_name(ssl, hostname); // SNI: one IP, many databases
    if (verify_host) SSL_set1_host(ssl, hostname);
  }
  for (;;) {
    int r = SSL_connect(ssl);
    if (r == 1) return ssl;
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_READ) {
      if (!wfd_wait(fd, false, 30000)) break;
      continue;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
      if (!wfd_wait(fd, true, 30000)) break;
      continue;
    }
    long vr = SSL_get_verify_result(ssl);
    if (!insecure && vr != X509_V_OK)
      snprintf(err, errcap, "certificate verification failed: %s",
               X509_verify_cert_error_string(vr));
    else
      tls_err(err, errcap, "the TLS handshake");
    break;
  }
  SSL_free(ssl);
  return nullptr;
}
i64 tls_read(void *s, void *buf, size_t n) {
  SSL *ssl = s;
  if (!ssl) return -1;
  for (;;) {
    int r = SSL_read(ssl, buf, (int)n);
    if (r > 0) return r;
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_ZERO_RETURN) return 0; // clean close_notify
    if (e == SSL_ERROR_WANT_READ) {
      if (!wfd_wait(SSL_get_fd(ssl), false, 30000)) return -1;
      continue;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
      if (!wfd_wait(SSL_get_fd(ssl), true, 30000)) return -1;
      continue;
    }
    if (e == SSL_ERROR_SYSCALL && r == 0) return 0; // peer closed without close_notify
    return -1;
  }
}

i64 tls_write(void *s, const void *buf, size_t n) {
  SSL *ssl = s;
  if (!ssl) return -1;
  size_t off = 0;
  while (off < n) {
    int r = SSL_write(ssl, (const char *)buf + off, (int)(n - off));
    if (r > 0) {
      off += (size_t)r;
      continue;
    }
    int e = SSL_get_error(ssl, r);
    if (e == SSL_ERROR_WANT_WRITE) {
      if (!wfd_wait(SSL_get_fd(ssl), true, 30000)) return -1;
      continue;
    }
    if (e == SSL_ERROR_WANT_READ) {
      if (!wfd_wait(SSL_get_fd(ssl), false, 30000)) return -1;
      continue;
    }
    return -1;
  }
  return (i64)off;
}

void tls_close(void *s) {
  SSL *ssl = s;
  if (!ssl) return;
  SSL_shutdown(ssl); // best effort: one close_notify, do not wait for the peer's
  SSL_free(ssl);
}

void tls_shutdown_all(void) {
  wmutex_lock(&g_mu);
  if (g_ctx) SSL_CTX_free(g_ctx);
  g_ctx = nullptr;
  wmutex_unlock(&g_mu);
}

#else // no TLS in this build

bool tls_available(void) {
  return false;
}

bool tls_init(const char *cert_path, const char *key_path, char *err, size_t errcap) {
  (void)cert_path;
  (void)key_path;
  if (err && errcap)
    snprintf(err, errcap,
             "this build has no TLS: rebuild with -DWEBC_TLS=openssl (or terminate TLS at a "
             "reverse proxy)");
  return false;
}

void *tls_accept(i32 fd) {
  (void)fd;
  return nullptr;
}
void *tls_connect(i32 fd, const char *hostname, const char *ca_file, bool insecure, char *err,
                  size_t errcap) {
  (void)fd;
  (void)hostname;
  (void)ca_file;
  (void)insecure;
  if (err && errcap)
    snprintf(err, errcap, "this build has no TLS: rebuild with -DWEBC_TLS=openssl");
  return nullptr;
}

void *tls_connect_opt(i32 fd, const char *hostname, const char *ca_file, bool verify_host,
                      bool insecure, char *err, size_t errcap) {
  return tls_connect(fd, hostname, ca_file, insecure, err, errcap);
}

i64 tls_read(void *s, void *buf, size_t n) {
  (void)s;
  (void)buf;
  (void)n;
  return -1;
}
i64 tls_write(void *s, const void *buf, size_t n) {
  (void)s;
  (void)buf;
  (void)n;
  return -1;
}
void tls_close(void *s) {
  (void)s;
}
void tls_shutdown_all(void) {}

#endif