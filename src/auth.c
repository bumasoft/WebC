// SPDX-License-Identifier: Apache-2.0
// WebC — password hashing and signed sessions.
#include <webc/auth.h>
#include <webc/crypto.h>
#include <webc/log.h>

str auth_hash_password_iters(Arena *a, str password, u32 iters) {
  u8 salt[16];
  str hexsalt = crypto_random_hex(a, sizeof salt);
  for (size_t i = 0; i < sizeof salt; i++) {
    char hi = hexsalt.p[i * 2], lo = hexsalt.p[i * 2 + 1];
    u8 h = (u8)((hi <= '9' ? hi - '0' : hi - 'a' + 10) << 4);
    u8 l = (u8)((lo <= '9' ? lo - '0' : lo - 'a' + 10));
    salt[i] = (u8)(h | l);
  }
  u8 hash[32];
  pbkdf2_sha256(password.p, password.n, salt, sizeof salt, iters, hash, sizeof hash);
  str salt_b64 = {0}, hash_b64 = {0};
  base64_encode(a, (str){(char *)salt, sizeof salt}, &salt_b64);
  base64_encode(a, (str){(char *)hash, sizeof hash}, &hash_b64);
  return str_fmt(a, "pbkdf2-sha256$%u$%.*s$%.*s", iters, (int)salt_b64.n, salt_b64.p,
                 (int)hash_b64.n, hash_b64.p);
}

str auth_hash_password(Arena *a, str password) {
  return auth_hash_password_iters(a, password, 210000);
}

static i64 split_dollar(str s, str *parts, i32 max) {
  i32 n = 0;
  size_t start = 0;
  for (size_t i = 0; i <= s.n && n < max; i++) {
    if (i == s.n || s.p[i] == '$') {
      parts[n++] = str_sub(s, start, i);
      start = i + 1;
    }
  }
  return n;
}

bool auth_verify_password(str stored, str password) {
  str parts[4];
  if (split_dollar(stored, parts, 4) != 4) return false;
  if (!str_eq_c(parts[0], "pbkdf2-sha256")) return false;
  i64 iters = 0;
  if (!str_to_i64(parts[1], &iters) || iters <= 0 || iters > 10000000) return false;
  Arena *a = arena_new(512);
  str salt = {0}, want = {0};
  bool ok = base64_decode(a, parts[2], &salt) && base64_decode(a, parts[3], &want);
  u8 got[64];
  if (ok && want.n <= sizeof got) {
    u8 wide[64] = {0};
    pbkdf2_sha256(password.p, password.n, salt.p, salt.n, (u32)iters, wide, want.n);
    ok = true;
    u8 diff = 0;
    for (size_t i = 0; i < want.n; i++)
      diff |= (u8)(wide[i] ^ (u8)want.p[i]);
    ok = diff == 0;
  } else {
    ok = false;
  }
  (void)got;
  arena_destroy(a);
  return ok;
}

// ---------------------------------------------------------------- sessions

void auth_set_session(Response *res, str secret, i64 uid) {
  Arena *a = res->arena;
  u64 exp = wc_wall_ms() / 1000 + WEBC_SESSION_TTL_S;
  str payload = str_fmt(a, "%lld.%llu", (long long)uid, (unsigned long long)exp);
  str signed_value = crypto_sign(a, secret, payload);
  res_cookie(res, WEBC_SESSION_COOKIE, str_cstr(a, signed_value), "HttpOnly; SameSite=Lax");
}

void auth_clear_session(Response *res) {
  res_delete_cookie(res, WEBC_SESSION_COOKIE);
}

bool auth_get_session(Request *req, str secret, i64 *uid_out) {
  str v = req_cookie(req, WEBC_SESSION_COOKIE);
  if (v.n == 0) return false;
  str payload = {0};
  if (!crypto_verify(req->arena, secret, v, &payload)) return false;
  i64 dot = str_index(payload, '.');
  if (dot <= 0) return false;
  i64 uid = 0, exp = 0;
  if (!str_to_i64(str_sub(payload, 0, (size_t)dot), &uid)) return false;
  if (!str_to_i64(str_sub(payload, (size_t)dot + 1, payload.n), &exp)) return false;
  if ((u64)exp < wc_wall_ms() / 1000) return false;
  if (uid_out) *uid_out = uid;
  return true;
}

i64 auth_require_session(Request *req, str secret) {
  i64 uid = 0;
  return auth_get_session(req, secret, &uid) ? uid : 0;
}
Text auth_hash(const char *password) {
  str s = password ? (str){password, strlen(password)} : (str){"", 0};
  return str_cdup(wc_arena_get(), auth_hash_password(wc_arena_get(), s));
}

bool auth_verify(const char *stored, const char *password) {
  if (!stored || !password) return false;
  return auth_verify_password((str){stored, strlen(stored)}, (str){password, strlen(password)});
}

// ---------------------------------------------------------------- the secret

static str g_secret;
static WMutex g_secret_mu = WEBC_MUTEX_INIT;

void auth_secret_set(str secret) {
  wmutex_lock(&g_secret_mu);
  g_secret = secret;
  wmutex_unlock(&g_secret_mu);
}

str auth_secret(void) {
  wmutex_lock(&g_secret_mu);
  str s = g_secret;
  wmutex_unlock(&g_secret_mu);
  if (s.n) return s;
  // Not a crash: a dev server should start without configuration. But a random
  // secret per boot quietly invalidates every session cookie on each restart,
  // which during development looks like the auth is broken (an hour was lost to
  // exactly that while building a real app). So the fallback, in this order:
  // WEBC_SECRET, then a per-project secret file the dev loop keeps stable,
  // then — only in production, where writing secrets to disk is a decision the
  // operator should make — a random one, with the log line that says so.
  const char *env = getenv("WEBC_SECRET");
  const char *dev = getenv("WEBC_DEV");
  wmutex_lock(&g_secret_mu);
  if (g_secret.n == 0) {
    if (env && *env) {
      g_secret = str_lit(env);
    } else {
      bool from_file = false;
      // A secret file that exists is an explicit choice in any mode (the dev loop
      // wrote it, or the operator did); creating one silently in production would
      // be our decision to make for them, so writing stays behind WEBC_DEV.
      FILE *f = fopen("data/.webc-secret", "r");
      char buf[80];
      if (f && fgets(buf, sizeof buf, f) && *buf && *buf != '\n') {
        buf[strcspn(buf, "\r\n")] = 0;
        g_secret = str_lit(persist(buf));
        from_file = true;
      }
      if (f) fclose(f);
      if (!from_file && dev && *dev) {
        str fresh = crypto_random_hex(wc_app_arena(), 32);
        FILE *out = fopen("data/.webc-secret", "w");
        if (out) {
          fprintf(out, "%.*s\n", (int)fresh.n, fresh.p);
          fclose(out);
          g_secret = fresh;
          from_file = true;
        }
      }
      if (!from_file) {
        g_secret = crypto_random_hex(wc_app_arena(), 32);
        log_write(LOG_WARN,
                  "WEBC_SECRET is not set: sessions will not survive a restart", nullptr, 0);
      }
    }
  }
  s = g_secret;
  wmutex_unlock(&g_secret_mu);
  return s;
}
