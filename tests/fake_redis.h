// tests/fake_redis.h — a Redis that lives in the test binary, so the client suite needs no docker.
//
// It speaks enough RESP to be a server: HELLO 3 (and a RESP2 fallback when asked for RESP2), AUTH,
// SELECT, CLIENT SETNAME, PING, GET/SET (with PX and NX), DEL, EXISTS, INCR/INCRBY, PEXPIRE/PTTL,
// MGET, GETDEL, HSET/HGET, SUBSCRIBE/PUBLISH, FLUSHALL. It also has two modes a real server cannot
// offer: a **scripted** mode (the next command gets these exact bytes, so a test can assert on
// reply types no Redis returns on demand — attributes, maps, doubles, big numbers) and a
// `fail_next` counter (drop the connection mid-command, which is how the fail-open path is tested).
//
// The client is the real one: the pool, the parked socket layer, the parser, the handshake.
#ifndef FAKE_REDIS_H
#define FAKE_REDIS_H

// The system headers below declare `select`, `bind` and friends as functions; <webc/task.h> and
// <webc/dom.h> define them as statement macros. This header is therefore included *before* the
// framework ones in a suite, and clears them once — the same dance src/db.c and src/tls.c do.
#ifdef select
#undef select
#endif
#ifdef bind
#undef bind
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <webc/core.h>

#define FR_KEYS 256
#define FR_SUBS 8
#define FR_SCRIPT 32

typedef struct FRKey {
  char key[192];
  char *val;
  size_t n;
  bool is_int;
  i64 i;
  bool used;
} FRKey;

typedef struct FakeRedis {
  int lfd;
  u16 port;
  pthread_t th;
  atomic_bool stop;
  pthread_mutex_t mu;
  FRKey keys[FR_KEYS];
  const char *script[FR_SCRIPT];
  int script_n, script_i;
  int fail_next;
  int connections;
  int commands; // every command served, so a test can count round trips
  int subs[FR_SUBS];
  char sub_chan[FR_SUBS][64];
  int nsubs;
  // Live connections, so `fake_redis_stop()` can hang up on them: a test that kills the server
  // must actually kill the connections the client already holds.
  int conns[64];
  int nconns;
  bool dead;
} FakeRedis;

// ---------------------------------------------------------------- store

static void fr_lock(FakeRedis *f) { pthread_mutex_lock(&f->mu); }
static void fr_unlock(FakeRedis *f) { pthread_mutex_unlock(&f->mu); }

static FRKey *fr_find(FakeRedis *f, const char *key) {
  if (!key) return nullptr; // a command with a missing argument is a client bug, not a crash
  for (int i = 0; i < FR_KEYS; i++)
    if (f->keys[i].used && !strcmp(f->keys[i].key, key)) return &f->keys[i];
  return nullptr;
}

static FRKey *fr_slot(FakeRedis *f, const char *key) {
  FRKey *k = fr_find(f, key);
  if (k) return k;
  for (int i = 0; i < FR_KEYS; i++) {
    if (!f->keys[i].used) {
      memset(&f->keys[i], 0, sizeof f->keys[i]);
      snprintf(f->keys[i].key, sizeof f->keys[i].key, "%s", key);
      f->keys[i].used = true;
      return &f->keys[i];
    }
  }
  return nullptr;
}

static void fr_put(FakeRedis *f, const char *key, const char *val, size_t n) {
  FRKey *k = fr_slot(f, key);
  if (!k) return;
  free(k->val);
  k->val = malloc(n + 1);
  memcpy(k->val, val, n);
  k->val[n] = 0;
  k->n = n;
  k->is_int = false;
}

static void fr_incr(FakeRedis *f, const char *key, i64 by, i64 *out) {
  FRKey *k = fr_slot(f, key);
  if (!k) return;
  i64 base = 0;
  if (k->val && !k->is_int) base = strtoll(k->val, nullptr, 10);
  else if (k->is_int) base = k->i;
  free(k->val);
  k->val = nullptr;
  k->is_int = true;
  k->i = base + by;
  k->n = 0;
  if (out) *out = k->i;
}

// Read the store from the test (for asserting what the cache wrote).
static bool fake_redis_get_key(FakeRedis *f, const char *key, char *out, size_t cap) {
  bool ok = false;
  fr_lock(f);
  FRKey *k = fr_find(f, key);
  if (k) {
    if (k->is_int) snprintf(out, cap, "%lld", (long long)k->i);
    else snprintf(out, cap, "%.*s", (int)(k->n < cap - 1 ? k->n : cap - 1), k->val ? k->val : "");
    ok = true;
  }
  fr_unlock(f);
  return ok;
}

static int fake_redis_key_count(FakeRedis *f, const char *prefix) {
  int n = 0;
  fr_lock(f);
  for (int i = 0; i < FR_KEYS; i++)
    if (f->keys[i].used && !strncmp(f->keys[i].key, prefix, strlen(prefix))) n++;
  fr_unlock(f);
  return n;
}

// ---------------------------------------------------------------- wire (server side)

static bool fr_send(int fd, const void *p, size_t n) {
  const char *at = p;
  while (n) {
    ssize_t w = send(fd, at, n, 0);
    if (w <= 0) return false;
    at += w;
    n -= (size_t)w;
  }
  return true;
}

static bool fr_send_str(int fd, Buf *b) { return fr_send(fd, b->p, b->n); }

static void fr_bulk(Buf *b, const char *p, size_t n) {
  buf_putc(b, '$');
  buf_puti64(b, (i64)n);
  buf_write(b, "\r\n", 2);
  if (n) buf_write(b, p, n);
  buf_write(b, "\r\n", 2);
}

static bool fr_read_line(int fd, char *out, size_t cap) {
  size_t i = 0;
  for (;;) {
    char c;
    ssize_t r = recv(fd, &c, 1, 0);
    if (r <= 0) return false;
    if (c == '\r') continue;
    if (c == '\n') {
      out[i] = 0;
      return true;
    }
    if (i + 1 < cap) out[i++] = c;
  }
}

static bool fr_read_exact(int fd, char *out, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, out + got, n - got, 0);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

// One command: up to 16 arguments, each up to 1 KB (the cache envelope is bigger, so `arg` for a
// SET value can grow — this is a test server, not a Redis).
typedef struct FrCmd {
  char *argv[16];
  size_t len[16];
  int argc;
  char *heap;
} FrCmd;

static bool fr_read_cmd(int fd, FrCmd *c) {
  char line[64];
  memset(c, 0, sizeof *c);
  if (!fr_read_line(fd, line, sizeof line)) return false;
  if (line[0] != '*') return false;
  int n = atoi(line + 1);
  if (n <= 0 || n > 16) return false;
  c->argc = n;
  for (int i = 0; i < n; i++) {
    char hdr[64];
    if (!fr_read_line(fd, hdr, sizeof hdr)) return false;
    if (hdr[0] != '$') return false;
    size_t len = (size_t)atol(hdr + 1);
    char *p = malloc(len + 1);
    if (!p) return false;
    if (!fr_read_exact(fd, p, len)) {
      free(p);
      return false;
    }
    char crlf[2];
    if (!fr_read_exact(fd, crlf, 2)) {
      free(p);
      return false;
    }
    p[len] = 0;
    c->argv[i] = p;
    c->len[i] = len;
    // one allocation per argument is simplest to free: chain them through `heap` is not possible,
    // so each is freed by fr_cmd_free() walking argv.
  }
  return true;
}

static void fr_cmd_release(FrCmd *c) {
  for (int i = 0; i < c->argc; i++) free(c->argv[i]);
  free(c->heap);
  memset(c, 0, sizeof *c);
}

// ---------------------------------------------------------------- command handling

static bool fr_dispatch(FakeRedis *f, int fd, FrCmd *c) {
  const char *verb = c->argv[0];
#ifdef FR_DEBUG
  fprintf(stderr, "[fake] %s argc=%d", verb, c->argc);
  for (int i = 1; i < c->argc; i++) fprintf(stderr, " %s", c->argv[i]);
  fprintf(stderr, "\n");
#endif
  Buf out;
  buf_init(&out, nullptr, 256);

  if (!strcasecmp(verb, "HELLO")) {
    const char *hello_pass = nullptr;
    for (int i = 2; i < c->argc; i++)
      if (!strcasecmp(c->argv[i], "AUTH") && i + 2 < c->argc + 1 && i + 1 < c->argc)
        hello_pass = (i + 2 < c->argc) ? c->argv[i + 2] : c->argv[i + 1];
    if (hello_pass && !strcmp(hello_pass, "bad")) {
      // A real Redis rejects the handshake itself, and the message names neither the password
      // nor the user: the client must not print it either.
      buf_puts(&out, "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
    } else if (c->argc >= 2 && !strcmp(c->argv[1], "2")) {
      buf_puts(&out, "-ERR unknown command 'HELLO'\r\n"); // force the RESP2 fallback
    } else {
      buf_puts(&out, "%2\r\n$6\r\nserver\r\n$10\r\nfake-redis\r\n$5\r\nproto\r\n:3\r\n");
    }
  } else if (!strcasecmp(verb, "AUTH")) {
    const char *pass = c->argc >= 3 ? c->argv[2] : (c->argc >= 2 ? c->argv[1] : "");
    if (!strcmp(pass, "bad")) buf_puts(&out, "-WRONGPASS invalid username-password pair\r\n");
    else buf_puts(&out, "+OK\r\n");
  } else if (!strcasecmp(verb, "SELECT") || !strcasecmp(verb, "CLIENT")) {
    buf_puts(&out, "+OK\r\n");
  } else if (!strcasecmp(verb, "PING")) {
    buf_puts(&out, "+PONG\r\n");
  } else if (!strcasecmp(verb, "SET")) {
    bool nx = false;
    size_t ttl_ms = 0;
    for (int i = 3; i < c->argc; i++) {
      if (!strcasecmp(c->argv[i], "NX")) nx = true;
      if (!strcasecmp(c->argv[i], "PX") && i + 1 < c->argc) ttl_ms = (size_t)atol(c->argv[i + 1]);
    }
    (void)ttl_ms;
    fr_lock(f);
    bool exists = fr_find(f, c->argv[1]) != nullptr;
    if (nx && exists) {
      fr_unlock(f);
      buf_puts(&out, "_\r\n");
    } else {
      fr_put(f, c->argv[1], c->argv[2], c->len[2]);
      fr_unlock(f);
      buf_puts(&out, "+OK\r\n");
    }
  } else if (!strcasecmp(verb, "GET")) {
    fr_lock(f);
    FRKey *k = fr_find(f, c->argv[1]);
    if (!k) buf_puts(&out, "_\r\n");
    else if (k->is_int) {
      char tmp[32];
      int n = snprintf(tmp, sizeof tmp, "%lld", (long long)k->i);
      fr_bulk(&out, tmp, (size_t)n);
    } else fr_bulk(&out, k->val, k->n);
    fr_unlock(f);
  } else if (!strcasecmp(verb, "GETDEL")) {
    fr_lock(f);
    FRKey *k = fr_find(f, c->argv[1]);
    if (!k) buf_puts(&out, "_\r\n");
    else {
      fr_bulk(&out, k->val ? k->val : "", k->n);
      k->used = false;
      free(k->val);
      k->val = nullptr;
    }
    fr_unlock(f);
  } else if (!strcasecmp(verb, "DEL")) {
    int n = 0;
    fr_lock(f);
    for (int i = 1; i < c->argc; i++) {
      FRKey *k = fr_find(f, c->argv[i]);
      if (k) {
        k->used = false;
        free(k->val);
        k->val = nullptr;
        n++;
      }
    }
    fr_unlock(f);
    buf_printf(&out, ":%d\r\n", n);
  } else if (!strcasecmp(verb, "EXISTS")) {
    int n = 0;
    fr_lock(f);
    for (int i = 1; i < c->argc; i++)
      if (fr_find(f, c->argv[i])) n++;
    fr_unlock(f);
    buf_printf(&out, ":%d\r\n", n);
  } else if (!strcasecmp(verb, "INCR") || !strcasecmp(verb, "INCRBY")) {
    i64 by = 1;
    if (c->argc >= 3) by = strtoll(c->argv[2], nullptr, 10);
    i64 now = 0;
    fr_lock(f);
    fr_incr(f, c->argv[1], by, &now);
    fr_unlock(f);
    buf_printf(&out, ":%lld\r\n", (long long)now);
  } else if (!strcasecmp(verb, "PEXPIRE") || !strcasecmp(verb, "EXPIRE")) {
    fr_lock(f);
    bool had = fr_find(f, c->argv[1]) != nullptr;
    fr_unlock(f);
    buf_printf(&out, ":%d\r\n", had ? 1 : 0);
  } else if (!strcasecmp(verb, "PTTL")) {
    fr_lock(f);
    bool had = fr_find(f, c->argv[1]) != nullptr;
    fr_unlock(f);
    buf_printf(&out, ":%d\r\n", had ? 30000 : -2);
  } else if (!strcasecmp(verb, "MGET")) {
    buf_printf(&out, "*%d\r\n", c->argc - 1);
    fr_lock(f);
    for (int i = 1; i < c->argc; i++) {
      FRKey *k = fr_find(f, c->argv[i]);
      if (!k) buf_puts(&out, "_\r\n");
      else if (k->is_int) {
        char tmp[32];
        int n = snprintf(tmp, sizeof tmp, "%lld", (long long)k->i);
        fr_bulk(&out, tmp, (size_t)n);
      } else fr_bulk(&out, k->val, k->n);
    }
    fr_unlock(f);
  } else if (!strcasecmp(verb, "HSET")) {
    char hk[400];
    snprintf(hk, sizeof hk, "%s\x1f%s", c->argv[1], c->argv[2]);
    fr_lock(f);
    fr_put(f, hk, c->argv[3], c->len[3]);
    fr_unlock(f);
    buf_puts(&out, ":1\r\n");
  } else if (!strcasecmp(verb, "HGET")) {
    char hk[400];
    snprintf(hk, sizeof hk, "%s\x1f%s", c->argv[1], c->argv[2]);
    fr_lock(f);
    FRKey *k = fr_find(f, hk);
    if (!k) buf_puts(&out, "_\r\n");
    else fr_bulk(&out, k->val, k->n);
    fr_unlock(f);
  } else if (!strcasecmp(verb, "LPUSH") || !strcasecmp(verb, "RPUSH")) {
    fr_lock(f);
    fr_put(f, c->argv[1], c->argv[2], c->len[2]);
    fr_unlock(f);
    buf_puts(&out, ":1\r\n");
  } else if (!strcasecmp(verb, "LPOP")) {
    fr_lock(f);
    FRKey *k = fr_find(f, c->argv[1]);
    if (!k) buf_puts(&out, "_\r\n");
    else {
      fr_bulk(&out, k->val, k->n);
      k->used = false;
      free(k->val);
      k->val = nullptr;
    }
    fr_unlock(f);
  } else if (!strcasecmp(verb, "SADD") || !strcasecmp(verb, "SREM")) {
    bool add = !strcasecmp(verb, "SADD");
    char sk[400];
    snprintf(sk, sizeof sk, "%s\x1f%s", c->argv[1], c->argv[2]);
    fr_lock(f);
    if (add) fr_put(f, sk, "1", 1);
    else {
      FRKey *k = fr_find(f, sk);
      if (k) {
        k->used = false;
        free(k->val);
        k->val = nullptr;
      }
    }
    fr_unlock(f);
    buf_puts(&out, ":1\r\n");
  } else if (!strcasecmp(verb, "SISMEMBER")) {
    char sk[400];
    snprintf(sk, sizeof sk, "%s\x1f%s", c->argv[1], c->argv[2]);
    fr_lock(f);
    bool has = fr_find(f, sk) != nullptr;
    fr_unlock(f);
    buf_printf(&out, ":%d\r\n", has ? 1 : 0);
  } else if (!strcasecmp(verb, "SUBSCRIBE")) {
    fr_lock(f);
    if (f->nsubs < FR_SUBS) {
      f->subs[f->nsubs] = fd;
      snprintf(f->sub_chan[f->nsubs], sizeof f->sub_chan[f->nsubs], "%s",
               c->argc >= 2 ? c->argv[1] : "");
      f->nsubs++;
    }
    fr_unlock(f);
    buf_puts(&out, "*3\r\n$9\r\nsubscribe\r\n");
    fr_bulk(&out, c->argc >= 2 ? c->argv[1] : "", c->argc >= 2 ? c->len[1] : 0);
    buf_puts(&out, ":1\r\n");
  } else if (!strcasecmp(verb, "PUBLISH")) {
    int got = 0;
    fr_lock(f);
    for (int i = 0; i < f->nsubs; i++) {
      if (!strcmp(f->sub_chan[i], c->argv[1])) {
        Buf push;
        buf_init(&push, nullptr, 128);
        buf_puts(&push, ">3\r\n$7\r\nmessage\r\n");
        fr_bulk(&push, c->argv[1], c->len[1]);
        fr_bulk(&push, c->argv[2], c->len[2]);
        fr_send_str(f->subs[i], &push);
        buf_free(&push);
        got++;
      }
    }
    fr_unlock(f);
    buf_printf(&out, ":%d\r\n", got);
  } else if (!strcasecmp(verb, "FLUSHALL")) {
    fr_lock(f);
    for (int i = 0; i < FR_KEYS; i++) {
      free(f->keys[i].val);
      memset(&f->keys[i], 0, sizeof f->keys[i]);
    }
    fr_unlock(f);
    buf_puts(&out, "+OK\r\n");
  } else {
    buf_printf(&out, "-ERR unknown command '%s'\r\n", verb);
  }

  bool ok = fr_send_str(fd, &out);
  buf_free(&out);
  return ok;
}

// ---------------------------------------------------------------- connection + accept

typedef struct FrConn {
  FakeRedis *f;
  int fd;
} FrConn;

static void fr_conn_register(FakeRedis *f, int fd) {
  fr_lock(f);
  if (f->nconns < 64) f->conns[f->nconns++] = fd;
  fr_unlock(f);
}

static void fr_conn_unregister(FakeRedis *f, int fd) {
  fr_lock(f);
  for (int i = 0; i < f->nconns; i++)
    if (f->conns[i] == fd) {
      f->conns[i] = f->conns[--f->nconns];
      break;
    }
  fr_unlock(f);
}

static void *fr_conn_main(void *arg) {
  FrConn *cn = arg;
  FakeRedis *f = cn->f;
  int fd = cn->fd;
  for (;;) {
    if (f->dead) break;
    FrCmd cmd;
    if (!fr_read_cmd(fd, &cmd)) break;
    fr_lock(f);
    f->commands++;
    fr_unlock(f);
    // scripted mode: the next command gets exactly these bytes
    fr_lock(f);
    if (f->fail_next > 0) {
      f->fail_next--;
      fr_unlock(f);
      fr_cmd_release(&cmd);
      break; // drop the connection mid-command: the client must mark it broken and recover
    }
    const char *scripted = nullptr;
    if (f->script_i < f->script_n) scripted = f->script[f->script_i++];
    fr_unlock(f);
    if (scripted) {
      bool ok = fr_send(fd, scripted, strlen(scripted));
      fr_cmd_release(&cmd);
      if (!ok) break;
      continue;
    }
    bool ok = fr_dispatch(f, fd, &cmd);
    fr_cmd_release(&cmd);
    if (!ok) break;
  }
  fr_lock(f);
  for (int i = 0; i < f->nsubs; i++)
    if (f->subs[i] == fd) {
      f->subs[i] = f->subs[--f->nsubs];
      break;
    }
  fr_unlock(f);
  fr_conn_unregister(f, fd);
  close(fd);
  free(cn);
  return nullptr;
}

static void *fr_accept_main(void *arg) {
  FakeRedis *f = arg;
  while (!atomic_load(&f->stop)) {
    struct sockaddr_in in;
    socklen_t il = sizeof in;
    int fd = accept(f->lfd, (struct sockaddr *)&in, &il);
    if (fd < 0) {
      if (atomic_load(&f->stop)) break;
      continue;
    }
    f->connections++;
    fr_conn_register(f, fd);
    FrConn *cn = calloc(1, sizeof *cn);
    cn->f = f;
    cn->fd = fd;
    pthread_t th;
    if (pthread_create(&th, nullptr, fr_conn_main, cn) == 0) pthread_detach(th);
    else {
      close(fd);
      free(cn);
    }
  }
  return nullptr;
}

static FakeRedis *fake_redis_start(void) {
  FakeRedis *f = calloc(1, sizeof *f);
  pthread_mutex_init(&f->mu, nullptr);
  f->lfd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(f->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in in = {0};
  in.sin_family = AF_INET;
  in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  in.sin_port = 0;
  if (bind(f->lfd, (struct sockaddr *)&in, sizeof in) != 0) {
    close(f->lfd);
    free(f);
    return nullptr;
  }
  socklen_t il = sizeof in;
  getsockname(f->lfd, (struct sockaddr *)&in, &il);
  f->port = ntohs(in.sin_port);
  listen(f->lfd, 64);
  pthread_create(&f->th, nullptr, fr_accept_main, f);
  return f;
}

// Hangs up on everything — the listener and every connection the client already holds — so a test
// can assert what the app does when Redis is *gone* rather than merely slow. The struct stays
// allocated: the connection threads are detached, and freeing it under them is the test server's
// one chance to crash the suite with its own bug instead of the client's.
static void fake_redis_stop(FakeRedis *f) {
  if (!f || f->dead) return;
  atomic_store(&f->stop, true);
  shutdown(f->lfd, SHUT_RDWR);
  close(f->lfd);
  pthread_join(f->th, nullptr);
  fr_lock(f);
  f->dead = true;
  for (int i = 0; i < f->nconns; i++) shutdown(f->conns[i], SHUT_RDWR);
  fr_unlock(f);
}

// The next `n` commands are answered with this exact text, in order.
static void fake_redis_script(FakeRedis *f, const char *const *replies, int n) {
  fr_lock(f);
  f->script_n = n < FR_SCRIPT ? n : FR_SCRIPT;
  f->script_i = 0;
  for (int i = 0; i < f->script_n; i++) f->script[i] = replies[i];
  fr_unlock(f);
}

#endif // FAKE_REDIS_H